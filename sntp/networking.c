#include <config.h>
#include "networking.h"
#include "ntp_debug.h"


/* Send a packet */
void
sendpkt (
	SOCKET rsock,
	sockaddr_u *dest,
	struct pkt *pkt,
	int len
	)
{
	int cc;

#ifdef DEBUG
	if (debug > 2) {
		printf("sntp sendpkt: Packet data:\n");
		pkt_output(pkt, len, stdout);
	}

	if (debug) {
		printf("sntp sendpkt: Sending packet to %s ...\n", sptoa(dest));
	}
#endif

	cc = sendto(rsock, (void *)pkt, len, 0, &dest->sa, SOCKLEN(dest));
	if (cc == SOCKET_ERROR) {
#ifdef DEBUG
		printf("sntp sendpkt: Socket error: %i. Couldn't send packet!\n", cc);
#endif
		if (errno != EWOULDBLOCK && errno != ENOBUFS) {
			/* oh well */
		}
	} else {
		DPRINTF(3, ("Packet sent.\n"));
	}
}


/*
** Fetch data, check if it's data for us and whether it's useable or not.
**
** If not, return a failure code so we can delete this server from our list
** and continue with another one.
*/
int
recvpkt (
	SOCKET rsock,
	struct pkt *rpkt,    /* received packet (response) */
	unsigned int rsize,  /* size of rpkt buffer */
	struct pkt *spkt     /* sent     packet (request) */
	)
{
	int pkt_len;
	sockaddr_u sender;

	pkt_len = recvdata(rsock, &sender, (char *)rpkt, rsize);
	if (pkt_len > 0)
		pkt_len = process_pkt(rpkt, &sender, pkt_len, MODE_SERVER, spkt, "recvpkt");

	return pkt_len;
}


/* Receive raw data */
int
recvdata(
	SOCKET rsock,
	sockaddr_u *sender,
	char *rdata,
	int rdata_length
	)
{
	GETSOCKNAME_SOCKLEN_TYPE slen;
	int recvc;

	slen = sizeof(*sender);
	recvc = recvfrom(rsock, rdata, rdata_length, 0, 
			 &sender->sa, &slen);
	if (-1 == recvc) {
		msyslog(LOG_ERR, "recvdata(%d) failed: %m", rsock);
		return recvc;
	}
#ifdef DEBUG
	if (debug > 2) {
		printf("Received %d bytes from %s:\n", recvc, sptoa(sender));
		pkt_output((struct pkt *)rdata, recvc, stdout);
	}
#endif
	return recvc;
}


int
process_pkt (
	struct pkt *rpkt,
	sockaddr_u *sas,
	int pkt_len,
	int mode,
	struct pkt *spkt,
	const char * func_name
	)
{
	unsigned int key_id = 0;
	struct key *pkt_key = NULL;
	int is_authentic = -1;
	unsigned int exten_words, exten_words_used = 0;
	int mac_size;

	/*
	 * Parse the extension field if present. We figure out whether
	 * an extension field is present by measuring the MAC size. If
	 * the number of words following the packet header is 0, no MAC
	 * is present and the packet is not authenticated. If 1, the
	 * packet is a crypto-NAK; if 3, the packet is authenticated
	 * with DES; if 5, the packet is authenticated with MD5; if 6,
	 * the packet is authenticated with SHA. If 2 or 4, the packet
	 * is a runt and discarded forthwith. If greater than 6, an
	 * extension field is present, so we subtract the length of the
	 * field and go around again.
	 */
	if (pkt_len < LEN_PKT_NOMAC || (pkt_len & 3) != 0) {
unusable:
		msyslog(LOG_ERR,
			"%s: Funny packet length: %i. Discarding packet.",
			func_name, pkt_len);
		return PACKET_UNUSEABLE;
	}
	/* skip past the extensions, if any */
	exten_words = ((unsigned)pkt_len - LEN_PKT_NOMAC) >> 2;
	while (exten_words > 6) {
		unsigned int exten_len;

		exten_len = ntohl(rpkt->exten[exten_words_used]) & 0xffff;
		exten_len = (exten_len + 7) >> 2; /* convert to words, add 1 */
		if (exten_len > exten_words || exten_len < 5)
			goto unusable;
		exten_words -= exten_len;
		exten_words_used += exten_len;
	}

	switch (exten_words) {
	    case 0:
		break;
	    case 1:
		key_id = ntohl(rpkt->exten[exten_words_used]);
		printf("Crypto NAK = 0x%08x\n", key_id);
		break;
	    case 5:
	    case 6:
		/*
		** Look for the key used by the server in the specified
		** keyfile and if existent, fetch it or else leave the
		** pointer untouched
		*/
		key_id = ntohl(rpkt->exten[exten_words_used]);
		get_key(key_id, &pkt_key);
		if (!pkt_key) {
			printf("unrecognized key ID = 0x%08x\n", key_id);
			break;
		}
		/*
		** Seems like we've got a key with matching keyid.
		**
		** Generate a md5sum of the packet with the key from our
		** keyfile and compare those md5sums.
		*/
		mac_size = exten_words << 2;
		if (!auth_md5((char *)rpkt, pkt_len - mac_size, mac_size - 4, pkt_key)) {
			is_authentic = 0;
			break;
		}
		/* Yay! Things worked out! */
		if (debug) {
			char *hostname = ss_to_str(sas);

			printf("sntp %s: packet received from %s successfully authenticated using key id %i.\n",
				func_name, hostname, key_id);
			free(hostname);
		}
		is_authentic = 1;
		break;
	    default:
		goto unusable;
		break;
	}

	switch (is_authentic) {
	case -1:	/* unknown */
		break;
	case 0:		/* not authentic */
		return SERVER_AUTH_FAIL;
		break;
	case 1:		/* authentic */
		break;
	default:	/* error */
		break;
	}

	/* Check for server's ntp version */
	if (PKT_VERSION(rpkt->li_vn_mode) < NTP_OLDVERSION ||
		PKT_VERSION(rpkt->li_vn_mode) > NTP_VERSION) {
		msyslog(LOG_ERR,
			"%s: Packet shows wrong version (%i)",
			func_name, PKT_VERSION(rpkt->li_vn_mode));
		return SERVER_UNUSEABLE;
	} 
	/* We want a server to sync with */
	if (PKT_MODE(rpkt->li_vn_mode) != mode &&
	    PKT_MODE(rpkt->li_vn_mode) != MODE_PASSIVE) {
		msyslog(LOG_ERR,
			"%s: mode %d stratum %i", func_name, 
			PKT_MODE(rpkt->li_vn_mode), rpkt->stratum);
		return SERVER_UNUSEABLE;
	}
	/* Stratum is unspecified (0) check what's going on */
	if (STRATUM_PKT_UNSPEC == rpkt->stratum) {
		char *ref_char;

		DPRINTF(1, ("%s: Stratum unspecified, going to check for KOD (stratum: %i)\n", 
				func_name, rpkt->stratum));
		ref_char = (char *) &rpkt->refid;
		DPRINTF(1, ("%s: Packet refid: %c%c%c%c\n", func_name,
			       ref_char[0], ref_char[1], ref_char[2], ref_char[3]));
		/* If it's a KOD packet we'll just use the KOD information */
		if (ref_char[0] != 'X') {
			if (strncmp(ref_char, "DENY", 4) == 0)
				return KOD_DEMOBILIZE;
			if (strncmp(ref_char, "RSTR", 4) == 0)
				return KOD_DEMOBILIZE;
			if (strncmp(ref_char, "RATE", 4) == 0)
				return KOD_RATE;
			/*
			** There are other interesting kiss codes which
			** might be interesting for authentication.
			*/
		}
	}
	/* If the server is not synced it's not really useable for us */
	if (LEAP_NOTINSYNC == PKT_LEAP(rpkt->li_vn_mode)) {
		msyslog(LOG_ERR,
			"%s: Server not in sync, skipping this server", func_name);
		return SERVER_UNUSEABLE;
	}

	/*
	 * Decode the org timestamp and make sure we're getting a response
	 * to our last request, but only if we're not in broadcast mode.
	 */
#ifdef DEBUG
	if (debug > 2) {
		printf("rpkt->org:\n");
		l_fp_output(&rpkt->org, stdout);
		printf("spkt->xmt:\n");
		l_fp_output(&spkt->xmt, stdout);
	}
#endif
	if (mode != MODE_BROADCAST && !L_ISEQU(&rpkt->org, &spkt->xmt)) {
		msyslog(LOG_ERR,
			"process_pkt: pkt.org and peer.xmt differ");
		return PACKET_UNUSEABLE;
	}

	return pkt_len;
}
