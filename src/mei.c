/*
 * opera-boot (GPL-2.0-only): clean-room minimal 3DO bootstrapper.
 *
 * mei.c: CLIO expansion-bus PIO + the MEI CD command wire protocol.
 *
 * Protocol (unit 0):
 *   - select the unit by writing its id to XB_SEL, read XB_POLL.
 *   - squirt a 7-byte command to XB_CMDSTAT.
 *   - collect status bytes from XB_CMDSTAT while STAT_VALID;
 *     while the command runs, when READ_VALID is set drain exactly
 *     `pending` data bytes from XB_DATA into the registered buffer.
 *   - the last status byte carries the error bit (0x10).
 */
#include "opera-boot.h"

#define STATBUF_CAP 16

static u8   g_statbuf[STATBUF_CAP];
static u32  g_statcnt;
static u8  *g_data_dst;
static u32  g_data_pending;

/* Poll iterations before declaring the drive dead.  Conservatively
 * bounded so an empty/absent drive fails fast back to the banner. */
#define POLL_TIMEOUT 4000000L

static u8 sel_poll(void)
{
	XB_SEL = 0;
	return (u8)XB_POLL;
}

/* Pace retries in scan lines (VCNT ticks), like the launch ROM. */
static void wait_lines(u32 lines)
{
	u32 v = CLIO_VCNT & VCNT_MASK;
	while (lines--) {
		u32 nv;
		do {
			nv = CLIO_VCNT & VCNT_MASK;
		} while (nv == v);
		v = nv;
	}
}

static void drain_data(void)
{
	u8 *dst = g_data_dst;
	u32 n = g_data_pending;
	g_data_pending = 0;
	while (n--)
		*dst++ = (u8)XB_DATA;
	g_data_dst = dst;
}

/* Collect the status FIFO after a command has been squirted.
 * Returns 0 on success, -1 on protocol timeout. */
static int collect_status(void)
{
	u8 poll;
	u32 timeout = POLL_TIMEOUT;

	g_statcnt = 0;
	for (;;) {
		poll = sel_poll();
		if (poll & XBUS_POLL_STAT_VALID)
			break;
		if ((poll & XBUS_POLL_READ_VALID) && g_data_pending)
			drain_data();
		if (--timeout == 0)
			return -1;
	}
	if ((poll & XBUS_POLL_READ_VALID) && g_data_pending)
		drain_data();
	while (poll & XBUS_POLL_STAT_VALID) {
		if (g_statcnt < STATBUF_CAP)
			g_statbuf[g_statcnt] = (u8)XB_CMDSTAT;
		g_statcnt++;
		poll = sel_poll();
		if (--timeout == 0)
			return -1;
	}
	return 0;
}

static int squirt7(u8 b0, u8 b1, u8 b2, u8 b3, u8 b4, u8 b5, u8 b6)
{
	XB_CMDSTAT = b0;
	XB_CMDSTAT = b1;
	XB_CMDSTAT = b2;
	XB_CMDSTAT = b3;
	XB_CMDSTAT = b4;
	XB_CMDSTAT = b5;
	XB_CMDSTAT = b6;
	return collect_status();
}

/* Last status byte: carries the CDMEI_* error/door/disc-in bits. */
static int last_stat(void)
{
	if (g_statcnt == 0 || g_statcnt > STATBUF_CAP)
		return -1;
	return g_statbuf[g_statcnt - 1];
}


// Attach to MEI unit 0, acknowledge media access, then flush stale status.
// A CURRENT latch needs a real CLRDIP reset before CMDSTAT becomes accessible.
int
mei_init(void)
{
  u8 poll;
  u32 timeout = POLL_TIMEOUT;

  g_data_pending = 0;
  XB_SEL = 0;
  while((XB_SETEXPCTL & XB_CPUHASXBUS) == 0)
    {
      if(--timeout == 0)
        return -1;
    }

  poll = (u8)XB_POLL;
  XB_POLL = (u32)(poll | XBUS_POLL_MEDIA_ACCESS);
  if(((XB_DIPIR1 | XB_DIPIR2) & XBUS_DIPIR_CURRENT) != 0)
    ob_dipir_reset();
  timeout = POLL_TIMEOUT;
  while((poll & XBUS_POLL_STAT_VALID) != 0)
    {
      (void)XB_CMDSTAT;
      poll = sel_poll();
      if(--timeout == 0)
        return -1;
    }

  return 0;
}


/* 0x82: device error/door/spin status */
int mei_dev_error(void)
{
	if (squirt7(MEI_DEVERROR, 0, 0, 0, 0, 0, 0))
		return -1;
	return last_stat();
}

void mei_spin_up(void)
{
	(void)squirt7(MEI_SPINUP, 0, 0, 0, 0, 0, 0);
}

int mei_disc_info(DiscInfo *di)
{
	u32 i;
	if (squirt7(MEI_DISCINFO, 0, 0, 0, 0, 0, 0))
		return -1;
	if (last_stat() & CDMEI_ERROR)
		return -1;
	for (i = 0; i < sizeof(*di); i++)
		((u8 *)di)[i] = (i < g_statcnt) ? g_statbuf[i] : 0;
	return 0;
}

int mei_session_info(u8 *flags)
{
	if (squirt7(MEI_SESSIONINFO, 0, 0, 0, 0, 0, 0))
		return -1;
	if (last_stat() & CDMEI_ERROR)
		return -1;
	*flags = (g_statcnt > 1) ? g_statbuf[1] : 0;
	return 0;
}

int mei_read_toc(u8 track, TOCInfo *toc)
{
	u32 i;
	if (squirt7(MEI_READTOC, 0, track, 0, 0, 0, 0))
		return -1;
	if (last_stat() & CDMEI_ERROR)
		return -1;
	for (i = 0; i < sizeof(*toc); i++)
		((u8 *)toc)[i] = (i < g_statcnt) ? g_statbuf[i] : 0;
	return 0;
}

/* Single-block read, exactly one "10 msf 00 00 01" on the wire. */
int mei_read_block(u32 blk, u8 *dst)
{
	u32 minutes = ob_udiv(blk, FRAMEperSEC * SECperMIN);
	u32 rem = blk - minutes * FRAMEperSEC * SECperMIN;
	u32 seconds = ob_udiv(rem, FRAMEperSEC);
	u32 frames = rem - seconds * FRAMEperSEC;

	g_data_dst = dst;
	g_data_pending = DISC_BLOCK_SIZE;
	if (squirt7(MEI_READBLOCK, (u8)minutes, (u8)seconds, (u8)frames,
	            0, 0, 1))
		return -1;
	if (last_stat() & CDMEI_ERROR)
		return -1;
	return 0;
}

/* Local helper exposed for the retry pacing in minidipir.c. */
void mei_wait_lines(u32 lines)
{
	wait_lines(lines);
}
