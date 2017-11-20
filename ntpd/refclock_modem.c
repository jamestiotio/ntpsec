/*
 * refclock_modem - clock driver for the NIST/USNO/PTB/NPL Computer Time
 *	Services
 */
#include "config.h"

#include "ntpd.h"
#include "ntp_io.h"
#include "ntp_refclock.h"
#include "ntp_stdlib.h"

#include <stdio.h>
#include <ctype.h>
#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif /* HAVE_SYS_IOCTL_H */

/*
 * This driver supports the US (NIST, USNO) and European (PTB, NPL,
 * etc.) modem time services, as well as Spectracom GPS
 * receivers connected via a modem. The driver periodically dials a
 * number from a telephone list, receives the timecode data and
 * calculates the local clock correction. It is designed primarily for
 * use as backup when neither a radio clock nor connectivity to Internet
 * time servers is available.
 *
 * WARNING: The ACTS mode of this driver depends on the system clock
 * for year disambiguation.  It will thus not be usable for recovery
 * if the system clock is trashed.
 *
 * This driver requires a modem with a Hayes-compatible command set
 * and control over the modem data terminal ready (DTR) control
 * line. The default modem setup string is hard-coded in the driver
 * and may require changes for nonstandard modems or special
 * circumstances.  It can be overridden by setting the extended system
 * variable "modem_setup" via ntpq.
 *
 * When enabled, the calling program dials the first number in the
 * phones file. If that call fails, it dials the second number and
 * so on. The phone number is specified by the Hayes ATDT prefix
 * followed by the number itself, including the long-distance prefix
 * and delay code, if necessary. The calling program is enabled
 * when (a) option flag1 is set by ntpq, (b) at each poll interval
 * when no other synchronization sources are present, and (c) at each
 * poll interval whether or not other synchronization sources are
 * present. The calling program disconnects if (a) the called party
 * is busy or does not answer, (b) the called party disconnects
 * before a sufficient number of timecodes have been received.
 *
 * The driver is transparent to each of the modem time services and
 * Spectracom receivers. It selects the parsing algorithm depending on the
 * message length. There is some hazard should the message be corrupted.
 * However, the data format is checked carefully and only if all checks
 * succeed is the message accepted. Corrupted lines are discarded
 * without complaint.
 *
 * Options:
 *
 * flag1	force a call in manual mode
 * flag2	enable port locking (not verified)
 * flag3	not used
 * flag4	not used
 *
 * time1	offset adjustment (s)
 *
 * Ordinarily, the serial port is connected to a modem and the phones
 * list is defined. If no phones list is defined, the port can be
 * connected directly to a device or another computer. In this case the
 * driver will send a single character 'T' at each poll event. If
 * option flag2 is enabled, port locking allows the modem to be shared
 * when not in use by this driver.
 */
/*
 * National Institute of Science and Technology (NIST)
 *
 * Phone: (303) 494-4774 (Boulder, CO); (808) 335-4721 (Hawaii)
 *
 * Data Format
 *
 * National Institute of Standards and Technology
 * Telephone Time Service, Generator 3B
 * Enter question mark "?" for HELP
 *                         D  L D
 *  MJD  YR MO DA H  M  S  ST S UT1 msADV        <OTM>
 * 47999 90-04-18 21:39:15 50 0 +.1 045.0 UTC(NIST) *<CR><LF>
 * ...
 *
 * MJD, DST, DUT1 and UTC are not used by this driver. The "*" or "#" is
 * the on-time markers echoed by the driver and used by NIST to measure
 * and correct for the propagation delay. Note: the ACTS timecode has
 * recently been changed to eliminate the * on-time indicator. The
 * reason for this and the long term implications are not clear.
 *
 * US Naval Observatory (USNO)
 *
 * Phone: (202) 762-1594 (Washington, DC); (719) 567-6743 (Colorado Springs, CO)
 *
 * Data Format (two lines, repeating at one-second intervals)
 *
 * jjjjj nnn hhmmss UTC<CR><LF>
 * *<CR><LF>
 *
 * jjjjj	modified Julian day number (not used)
 * nnn		day of year
 * hhmmss	second of day
 * *		on-time marker for previous timecode
 * ...
 *
 * USNO does not correct for the propagation delay. A fudge time1 of
 * about .06 s is advisable.
 *
 * European Services (PTB, NPL, etc.)
 *
 * PTB: +49 531 512038 (Germany)
 * NPL: 0906 851 6333 (UK only)
 *
 * Data format (see the documentation for phone numbers and formats.)
 *
 * 1995-01-23 20:58:51 MEZ  10402303260219950123195849740+40000500<CR><LF>
 *
 * Spectracom GPS Receivers
 *
 * If a modem is connected to a Spectracom receiver, this driver will
 * call it up and retrieve the time in one of two formats. As this
 * driver does not send anything, the radio will have to either be
 * configured in continuous mode or be polled by another local driver.
 */
/*
 * Interface definitions
 */
#ifndef ENABLE_CLASSIC_MODE
#define	DEVICE		"/dev/modem%d" /* device name and unit */
#else
#define	DEVICE		"/dev/acts%d" /* device name and unit */
#endif
#define	SPEED232	B19200	/* uart speed (19200 bps) */
#define	PRECISION	(-10)	/* precision assumed (about 1 ms) */
#define LOCKFILE	"/var/spool/lock/LCK..cua%d"
#define NAME		"MODEM"	/* shortname */
#define DESCRIPTION	"Automated Computer Time Service" /* WRU */
#define REFID		"NONE"	/* default reference ID */
/* #define MSGCNT	20	* max message count UNUSED */
/* #define MAXPHONE	10	* max number of phone numbers UNUSED */

/*
 * Calling program modes (mode)
 */
#define MODE_BACKUP	0	/* backup mode */
#define MODE_AUTO	1	/* automatic mode */
#define MODE_MANUAL	2	/* manual mode */

/*
 * Service identifiers (message length)
 */
#define REFACTS		"NIST"	/* NIST reference ID */
#define LENACTS		50	/* NIST format A */
#define REFUSNO		"USNO"	/* USNO reference ID */
#define LENUSNO		20	/* USNO */
#define REFPTB		"PTB\0"	/* PTB/NPL reference ID */
#define LENPTB		78	/* PTB/NPL format */
#define REFTYPE		"GPS\0"	/* Spectracom reference ID */
#define	LENTYPE0	22	/* SPECTRACOM format 0 */
#define	LENTYPE2	24	/* SPECTRACOM format 2 */
#define LF		0x0a	/* ASCII LF */

/*
 * Modem setup strings. These may have to be changed for
 * some modems.
 *
 * AT	command prefix
 * B1	US answer tone
 * &C0	disable carrier detect
 * &D2	hang up and return to command mode on DTR transition
 * E0	modem command echo disabled
 * L1	set modem speaker volume to low level
 * M1	speaker enabled until carrier detect
 * Q0	return result codes
 * V1	return result codes as English words
 * Y1	enable long-space disconnect
 */
static const char def_modem_setup[] = "ATB1&C0&D2E0L1M1Q0V1Y1";
static const char *modem_setup = def_modem_setup;

/*
 * Timeouts (all in seconds)
 */
#define SETUP		3	/* setup timeout */
#define	REDIAL		30	/* redial timeout */
#define ANSWER		60	/* answer timeout */
#define TIMECODE	60	/* message timeout */
#define	MAXCODE		20	/* max timecodes */

/*
 * State machine codes
 */
typedef enum {
	S_IDLE,			/* wait for poll */
	S_SETUP,		/* send modem setup */
	S_CONNECT,		/* wait for answer */
	S_MSG			/* wait for timecode */
} teModemState;

/*
 * Unit control structure
 */
struct modemunit {
	int	unit;		/* unit number */
	int	state;		/* the first one was Delaware */
	int	timer;		/* timeout counter */
	int	retry;		/* retry index */
	int	msgcnt;		/* count of messages received */
	l_fp	tstamp;		/* on-time timestamp */
	char	*bufptr;	/* next incoming char stored here */
	char	buf[BMAX];	/* bufptr roams within buf[] */
};

/*
 * Function prototypes
 */
static	bool	modem_start	(int, struct peer *);
static	void	modem_shutdown	(int, struct refclockproc *);
static	void	modem_receive	(struct recvbuf *);
static	void	modem_message	(struct peer *, const char *);
static	void	modem_timecode	(struct peer *, const char *);
static	void	modem_poll	(int, struct peer *);
static	void	modem_timeout	(struct peer *, teModemState);
static	void	modem_timer	(int, struct peer *);
static	void	modem_close	(struct refclockproc *);

/*
 * Transfer vector (conditional structure name)
 */
struct refclock refclock_modem = {
	NAME,			/* basename of driver */
	modem_start,		/* start up driver */
	modem_shutdown,		/* shut down driver */
	modem_poll,		/* transmit poll message */
	NULL,			/* control - not used */
	NULL,			/* init - not used */
	modem_timer		/* housekeeping timer */
};

/*
 * Initialize data for processing
 */
static bool
modem_start(
	int	unit,
	struct peer *peer
	)
{
	struct modemunit *up;
	struct refclockproc *pp;
	const char *setup;

	/*
	 * Allocate and initialize unit structure
	 */
	up = emalloc_zero(sizeof(struct modemunit));
	up->unit = unit;
	pp = peer->procptr;
	pp->unitptr = up;
	pp->io.clock_recv = modem_receive;
	pp->io.srcclock = peer;
	pp->io.datalen = 0;
	pp->io.fd = -1;

	/*
	 * Initialize miscellaneous variables
	 */
	peer->precision = PRECISION;
	pp->clockname = NAME;
	pp->clockdesc = DESCRIPTION;
	memcpy(&pp->refid, REFID, REFIDLEN);
	peer->sstclktype = CTL_SST_TS_TELEPHONE;
	up->bufptr = up->buf;
	if (def_modem_setup == modem_setup) {
		setup = get_ext_sys_var("modemsetup");
		if (setup != NULL)
			modem_setup = estrdup(setup);
	}

	return true;
}


/*
 * modem_shutdown - shut down the clock
 */
static void
modem_shutdown(
	int	unit,
	struct refclockproc *pp
	)
{
	UNUSED_ARG(unit);

	/*
	 * Warning: do this only when a call is not in progress.
	 */
	modem_close(pp);
	free(pp->unitptr);
}


/*
 * modem_receive - receive data from the serial interface
 */
static void
modem_receive(
	struct recvbuf *rbufp
	)
{
	struct modemunit *up;
	struct refclockproc *pp;
	struct peer *peer;
	char	tbuf[sizeof(up->buf)];
	char *	tptr;
	size_t	octets;

	/*
	 * Initialize pointers and read the timecode and timestamp. Note
	 * we are in raw mode and victim of whatever the terminal
	 * interface kicks up; so, we have to reassemble messages from
	 * arbitrary fragments. Capture the timecode at the beginning of
	 * the message and at the '*' and '#' on-time characters.
	 */
	peer = rbufp->recv_peer;
	pp = peer->procptr;
	up = pp->unitptr;
	octets = sizeof(up->buf) - (size_t)(up->bufptr - up->buf);
	refclock_gtraw(rbufp, tbuf, octets, &pp->lastrec);
	for (tptr = tbuf; *tptr != '\0'; tptr++) {
		if (*tptr == LF) {
			if (up->bufptr == up->buf) {
				up->tstamp = pp->lastrec;
				continue;
			} else {
				*up->bufptr = '\0';
				up->bufptr = up->buf;
				modem_message(peer, up->buf);
			}
		} else if (!iscntrl((unsigned char)*tptr)) {
			*up->bufptr++ = *tptr;
			if (*tptr == '*' || *tptr == '#') {
				up->tstamp = pp->lastrec;
				if (write(pp->io.fd, tptr, 1) < 0)
					msyslog(LOG_ERR, "REFCLOCK: modem: write echo fails %m");
			}
		}
	}
}


/*
 * modem_message - process message
 */
void
modem_message(
	struct peer *peer,
	const char *msg
	)
{
	struct modemunit *up;
	struct refclockproc *pp;
	char	tbuf[BMAX], *cp;
	int		dtr = TIOCM_DTR;

	DPRINT(1, ("modem: %d %s\n", (int)strlen(msg), msg));

	/*
	 * What to do depends on the state and the first token in the
	 * message.
	 */
	pp = peer->procptr;
	up = pp->unitptr;

	/*
	 * Extract the first token in the line.
	 */
	strlcpy(tbuf, msg, sizeof(tbuf));
	for (cp = tbuf; cp < tbuf + sizeof(tbuf); cp++)
	    if (isspace((int)*cp))
		*cp = '\0';

	switch (up->state) {

	/*
	 * We are waiting for the OK response to the modem setup
	 * command. When this happens, dial the number followed.
	 * If anything other than OK is received, just ignore it
	 * and wait for timeoue.
	 */
	case S_SETUP:
		if (strcmp(tbuf, "OK") != 0) {
			/*
			 * We disable echo with MODEM_SETUP's E0 but
			 * if the modem was previously E1, we will
			 * see MODEM_SETUP echoed before the OK/ERROR.
			 * Ignore it.
			 */
			if (!strcmp(tbuf, modem_setup))
				return;
			break;
		}

		mprintf_event(PEVNT_CLOCK, peer, "DIAL #%d %s",
			      up->retry, sys_phone[up->retry]);
		if (ioctl(pp->io.fd, TIOCMBIS, &dtr) < 0)
			msyslog(LOG_ERR, "REFCLOCK: modem: ioctl(TIOCMBIS) failed: %m");
		if (write(pp->io.fd, sys_phone[up->retry],
		    strlen(sys_phone[up->retry])) < 0)
			msyslog(LOG_ERR, "REFCLOCK: modem: write DIAL fails %m");
		IGNORE(write(pp->io.fd, "\r", 1));
		up->retry++;
		up->state = S_CONNECT;
		up->timer = ANSWER;
		return;

	/*
	 * We are waiting for the CONNECT response to the dial
	 * command. When this happens, listen for timecodes. If
	 * something other than CONNECT is received, like BUSY
	 * or NO CARRIER, abort the call.
	 */
	case S_CONNECT:
		if (strcmp(tbuf, "CONNECT") != 0)
			break;

		report_event(PEVNT_CLOCK, peer, msg);
		up->state = S_MSG;
		up->timer = TIMECODE;
		return;

	/*
	 * We are waiting for a timecode response. Pass it to
	 * the parser. If NO CARRIER is received, save the
	 * messages and abort the call.
	 */
	case S_MSG:
		if (strcmp(tbuf, "NO") == 0)
			report_event(PEVNT_CLOCK, peer, msg);
		if (up->msgcnt < MAXCODE)
			modem_timecode(peer, msg);
		else
			modem_timeout(peer, S_MSG);
		return;

        default:
                /* huh? */
                break;
	}

	/*
	 * Other response. Tell us about it.
	 */
	report_event(PEVNT_CLOCK, peer, msg);
	modem_close(peer->procptr);
}


/*
 * modem_timeout - called on timeout
 */
static void
modem_timeout(
	struct peer *peer,
	teModemState	dstate
	)
{
	struct modemunit *up;
	struct refclockproc *pp;
	int	fd;
	ssize_t	rc;
	char	device[20];
	char	lockfile[128], pidbuf[8];

	/*
	 * The state machine is driven by messages from the modem,
	 * when first started and at timeout.
	 */
	pp = peer->procptr;
	up = pp->unitptr;
	switch (dstate) {

	/*
	 * System poll event. Lock the modem port, open the device
	 * and send the setup command.
	 */
	case S_IDLE:
		if (-1 != pp->io.fd)
			return;		/* port is already open */

		/*
		 * Lock the modem port. If busy, retry later. Note: if
		 * something fails between here and the close, the lock
		 * file may not be removed.
		 */
		if (pp->sloppyclockflag & CLK_FLAG2) {
			snprintf(lockfile, sizeof(lockfile), LOCKFILE,
			    up->unit);
			fd = open(lockfile, O_WRONLY | O_CREAT | O_EXCL,
			    0644);
			if (fd < 0) {
				report_event(PEVNT_CLOCK, peer, "modem: port busy");
				return;
			}
			snprintf(pidbuf, sizeof(pidbuf), "%u\n",
			    (unsigned int)getpid());
			if (write(fd, pidbuf, strlen(pidbuf)) < 0)
				msyslog(LOG_ERR, "REFCLOCK: modem: write lock fails %m");
			close(fd);
		}

		/*
		 * Open the device in raw mode and link the I/O.
		 */
		snprintf(device, sizeof(device), DEVICE, up->unit);
		fd = refclock_open(peer->cfg.path ? peer->cfg.path : device,
				   peer->cfg.baud ? peer->cfg.baud : SPEED232,
				   LDISC_ACTS | LDISC_RAW | LDISC_REMOTE);
		if (fd < 0) {
			msyslog(LOG_ERR, "REFCLOCK: modem: open fails %m");
			return;
		}
		pp->io.fd = fd;
		if (!io_addclock(&pp->io)) {
			msyslog(LOG_ERR, "REFCLOCK: modem: addclock fails");
			close(fd);
			pp->io.fd = -1;
			return;
		}
		up->msgcnt = 0;
		up->bufptr = up->buf;

		/*
		 * If the port is directly connected to the device, skip
		 * the modem business and send 'T' for Spectrabum.
		 */
		if (sys_phone[up->retry] == NULL) {
			if (write(pp->io.fd, "T", 1) < 0)
				msyslog(LOG_ERR, "REFCLOCK: modem: write T fails %m");
			up->state = S_MSG;
			up->timer = TIMECODE;
			return;
		}

		/*
		 * Initialize the modem. This works with Hayes-
		 * compatible modems.
		 */
		mprintf_event(PEVNT_CLOCK, peer, "SETUP %s",
			      modem_setup);
		rc = write(pp->io.fd, modem_setup, strlen(modem_setup));
		if (rc < 0)
			msyslog(LOG_ERR, "REFCLOCK: modem: write SETUP fails %m");
		IGNORE(write(pp->io.fd, "\r", 1));
		up->state = S_SETUP;
		up->timer = SETUP;
		return;

	/*
	 * In SETUP state the modem did not respond OK to setup string.
	 */
	case S_SETUP:
		report_event(PEVNT_CLOCK, peer, "no modem");
		break;

	/*
	 * In CONNECT state the call did not complete. Abort the call.
	 */
	case S_CONNECT:
		report_event(PEVNT_CLOCK, peer, "no answer");
		break;

	/*
	 * In MSG states no further timecodes are expected. If any
	 * timecodes have arrived, update the clock. In any case,
	 * terminate the call.
	 */
	case S_MSG:
		if (up->msgcnt == 0) {
			report_event(PEVNT_CLOCK, peer, "no timecodes");
		} else {
			pp->lastref = pp->lastrec;
			record_clock_stats(peer, pp->a_lastcode);
			refclock_receive(peer);
		}
		break;

        default:
                /* huh? */
                break;
	}
	modem_close(peer->procptr);
}


/*
 * modem_close - close and prepare for next call.
 *
 * In ClOSE state no further protocol actions are required
 * other than to close and release the device and prepare to
 * dial the next number if necessary.
 */
void
modem_close(
	struct refclockproc *pp
	)
{
	struct modemunit *up;
	char	lockfile[128];
	int	dtr;

	up = pp->unitptr;
	if (pp->io.fd != -1) {
		//report_event(PEVNT_CLOCK, peer, "close");
		dtr = TIOCM_DTR;
		if (ioctl(pp->io.fd, TIOCMBIC, &dtr) < 0)
			msyslog(LOG_ERR, "REFCLOCK: modem: ioctl(TIOCMBIC) failed: %m");
		io_closeclock(&pp->io);
		pp->io.fd = -1;
	}
	if (pp->sloppyclockflag & CLK_FLAG2) {
		snprintf(lockfile, sizeof(lockfile),
		    LOCKFILE, up->unit);
		unlink(lockfile);
	}
	if (up->msgcnt == 0 && up->retry > 0) {
		if (sys_phone[up->retry] != NULL) {
			up->state = S_IDLE;
			up->timer = REDIAL;
			return;
		}
	}
	up->state = S_IDLE;
	up->timer = 0;
}


/*
 * modem_poll - called by the transmit routine
 */
static void
modem_poll(
	int	unit,
	struct peer *peer
	)
{
	struct modemunit *up;
	struct refclockproc *pp;

	UNUSED_ARG(unit);

	/*
	 * This routine is called at every system poll. All it does is
	 * set flag1 under certain conditions. The real work is done by
	 * the timeout routine and state machine.
	 */
	pp = peer->procptr;
	up = pp->unitptr;
	switch (peer->cfg.ttl) {

	/*
	 * In manual mode the calling program is activated by the ntpq
	 * program using the enable flag (flag1 option), either manually
	 * or by a cron job.
	 */
	case MODE_MANUAL:
		return;

	/*
	 * In automatic mode the calling program runs continuously at
	 * intervals determined by the poll event or specified timeout.
	 */
	case MODE_AUTO:
		break;

	/*
	 * In backup mode the calling program runs continuously as long
	 * as either no peers are available or this peer is selected.
	 */
	case MODE_BACKUP:
		if (!(sys_peer == NULL || sys_peer == peer))
			return;

		break;

        default:
                /* huh? */
                break;
	}
	pp->polls++;
	if (S_IDLE == up->state) {
		up->retry = 0;
		modem_timeout(peer, S_IDLE);
	}
}


/*
 * modem_timer - called at one-second intervals
 */
static void
modem_timer(
	int	unit,
	struct peer *peer
	)
{
	struct modemunit *up;
	struct refclockproc *pp;

	UNUSED_ARG(unit);

	/*
	 * This routine implements a timeout which runs for a programmed
	 * interval. The counter is initialized by the state machine and
	 * counts down to zero. Upon reaching zero, the state machine is
	 * called. If flag1 is set while timer is zero, force a call.
	 */
	pp = peer->procptr;
	up = pp->unitptr;
	if (up->timer == 0) {
		if (pp->sloppyclockflag & CLK_FLAG1) {
			pp->sloppyclockflag &= ~CLK_FLAG1;
			modem_timeout(peer, S_IDLE);
		}
	} else {
		up->timer--;
		if (up->timer == 0)
			modem_timeout(peer, (teModemState)up->state);
	}
}

/*
 * modem_timecode - identify the service and parse the timecode message
 */
void
modem_timecode(
	struct peer *	peer,	/* peer structure pointer */
	const char *	str	/* timecode string */
	)
{
	struct modemunit *up;
	struct refclockproc *pp;
	int	day;		/* day of the month */
	int	month;		/* month of the year */
	unsigned long	mjd;	/* Modified Julian Day */
	double	dut1;		/* DUT adjustment */

	unsigned int	dst;		/* ACTS daylight/standard time */
	unsigned int	leap;		/* ACTS leap indicator */
	double	msADV;		/* ACTS transmit advance (ms) */
	char	utc[10];	/* ACTS timescale */
	char	flag;		/* ACTS on-time character (* or #) */

	char	synchar;	/* Spectracom synchronized indicator */
	char	qualchar;	/* Spectracom quality indicator */
	char	leapchar;	/* Spectracom leap indicator */
	char	dstchar;	/* Spectracom daylight/savings indicator */
	int	tz;		/* Spectracom timezone */

	int	leapmonth;	/* PTB/NPL month of leap */
	char	leapdir;	/* PTB/NPL leap direction */

	/*
	 * The parser selects the modem format based on the message
	 * length. Since the data are checked carefully, occasional
	 * errors due noise are forgivable.
	 */
	pp = peer->procptr;
	up = pp->unitptr;
	pp->nsec = 0;
	switch (strlen(str)) {

	/*
	 * For USNO format on-time character '*', which is on a line by
	 * itself. Be sure a timecode has been received.
	 */
	case 1:
		if (*str == '*' && up->msgcnt > 0)
			break;

		return;
	
	/*
	 * ACTS format A: "jjjjj yy-mm-dd hh:mm:ss ds l uuu aaaaa
	 * UTC(NIST) *".
	 */
	case LENACTS:
		if (sscanf(str,
		    "%5lu %2d-%2d-%2d %2d:%2d:%2d %2u %1u %3lf %5lf %9s %c",
		    &mjd, &pp->year, &month, &day, &pp->hour,
		    &pp->minute, &pp->second, &dst, &leap, &dut1,
		    &msADV, utc, &flag) != 13) {
			refclock_report(peer, CEVNT_BADREPLY);
			return;
		}
		pp->day = ymd2yd(pp->year, month, day);
		pp->leap = LEAP_NOWARNING;
		if (leap == 1)
			pp->leap = LEAP_ADDSECOND;
		else if (leap == 2)
			pp->leap = LEAP_DELSECOND;
		memcpy(&pp->refid, REFACTS, REFIDLEN);
		peer->sstclktype = CTL_SST_TS_TELEPHONE;
		up->msgcnt++;
		if (flag != '#' && up->msgcnt < 10)
			return;

		break;

	/*
	 * USNO format: "jjjjj nnn hhmmss UTC"
	 */
	case LENUSNO:
		if (sscanf(str, "%5lu %3d %2d%2d%2d %3s",
		    &mjd, &pp->day, &pp->hour, &pp->minute,
		    &pp->second, utc) != 6) {
			refclock_report(peer, CEVNT_BADREPLY);
			return;
		}

		/*
		 * Wait for the on-time character, which follows in a
		 * separate message. There is no provision for leap
		 * warning.
		 */
		pp->leap = LEAP_NOWARNING;
		memcpy(&pp->refid, REFUSNO, REFIDLEN);
		peer->sstclktype = CTL_SST_TS_TELEPHONE;
		up->msgcnt++;
		break;

	/*
	 * PTB/NPL format: "yyyy-mm-dd hh:mm:ss MEZ"
	 */
	case LENPTB:
		if (sscanf(str,
		    "%*4d-%*2d-%*2d %*2d:%*2d:%2d %*5c%*12c%4d%2d%2d%2d%2d%5lu%2lf%c%2d%3lf%*15c%c",
		    &pp->second, &pp->year, &month, &day, &pp->hour,
		    &pp->minute, &mjd, &dut1, &leapdir, &leapmonth,
		    &msADV, &flag) != 12) {
			refclock_report(peer, CEVNT_BADREPLY);
			return;
		}
		pp->leap = LEAP_NOWARNING;
		if (leapmonth == month) {
			if (leapdir == '+')
				pp->leap = LEAP_ADDSECOND;
			else if (leapdir == '-')
				pp->leap = LEAP_DELSECOND;
		}
		pp->day = ymd2yd(pp->year, month, day);
		memcpy(&pp->refid, REFPTB, REFIDLEN);
		peer->sstclktype = CTL_SST_TS_TELEPHONE;
		up->msgcnt++;
		break;


	/*
	 * Spectracom format 0: "I  ddd hh:mm:ss DTZ=nn"
	 */
	case LENTYPE0:
		if (sscanf(str, "%c %3d %2d:%2d:%2d %cTZ=%2d",
		    &synchar, &pp->day, &pp->hour, &pp->minute,
		    &pp->second, &dstchar, &tz) != 7) {
			refclock_report(peer, CEVNT_BADREPLY);
			return;
		}
		pp->leap = LEAP_NOWARNING;
		if (synchar != ' ')
			pp->leap = LEAP_NOTINSYNC;
		memcpy(&pp->refid, REFTYPE, REFIDLEN);
		peer->sstclktype = CTL_SST_TS_TELEPHONE;
		up->msgcnt++;
		break;

	/*
	 * Spectracom format 2: "IQyy ddd hh:mm:ss.mmm LD"
	 */
	case LENTYPE2:
		if (sscanf(str, "%c%c%2d %3d %2d:%2d:%2d.%3ld%c%c%c",
		    &synchar, &qualchar, &pp->year, &pp->day,
		    &pp->hour, &pp->minute, &pp->second, &pp->nsec,
		    &dstchar, &leapchar, &dstchar) != 11) {
			refclock_report(peer, CEVNT_BADREPLY);
			return;
		}
		pp->nsec *= 1000000;
		pp->leap = LEAP_NOWARNING;
		if (synchar != ' ')
			pp->leap = LEAP_NOTINSYNC;
		else if (leapchar == 'L')
			pp->leap = LEAP_ADDSECOND;
		memcpy(&pp->refid, REFTYPE, REFIDLEN);
		peer->sstclktype = CTL_SST_TS_TELEPHONE;
		up->msgcnt++;
		break;

	/*
	 * None of the above. Just forget about it and wait for the next
	 * message or timeout.
	 */
	default:
		return;
	}

	/*
	 * We have a valid timecode. The fudge time1 value is added to
	 * each sample by the main line routines. Note that in current
	 * telephone networks the propatation time can be different for
	 * each call and can reach 200 ms for some calls.
	 */
	peer->refid = pp->refid;
	pp->lastrec = up->tstamp;
	if (up->msgcnt == 0)
		return;

	strlcpy(pp->a_lastcode, str, sizeof(pp->a_lastcode));
	pp->lencode = (int)strlen(pp->a_lastcode);
	if (!refclock_process(pp)) {
		refclock_report(peer, CEVNT_BADTIME);
		return;
	}
	pp->lastref = pp->lastrec;
}

