/*
 * opera-boot (GPL-2.0-only): clean-room minimal 3DO bootstrapper.
 *
 * label.c: Opera volume-label probe and disc RomTag table handling.
 *
 * The boot probe follows vendor FindVolumeLabelBlock exactly: track-1
 * first block, +DISC_LABEL_OFFSET (225), first block again for a
 * transient retry, then +225+DISC_LABEL_AVATAR_DELTA (33011).  This is
 * distinct from the filesystem's later directory-avatar walk.  Every
 * retail disc in the validated library uses the first label slot.
 */
#include "opera-boot.h"

static int label_valid(const DiscLabel *dl)
{
	static const u8 sync[VOLUME_SYNC_LEN] =
		{ 0x5A, 0x5A, 0x5A, 0x5A, 0x5A };
	u32 i;

	if (dl->dl_RecordType != 1)
		return 0;
	for (i = 0; i < VOLUME_SYNC_LEN; i++)
		if (dl->dl_VolumeSyncBytes[i] != sync[i])
			return 0;
	/* Vendor VerifyLabel (cdipir.c) checks exactly the record type and
	 * the sync bytes; discdata.h says StructVer "should contain 1" but
	 * dipir never enforces it, so a disc mastered with 0 must boot. */
	return 1;
}

/* Read and validate the volume label.  Returns 0 with *label_block set
 * on success.  Buffers are caller-owned (must hold DISC_BLOCK_SIZE). */
int disc_probe_label(DiscLabel *dl, u32 first_block, u32 *label_block)
{
	static const u32 avatar_off[4] = {
		LABEL_OFF0, LABEL_OFF1,
		LABEL_OFF0,		/* vendor re-probes avatar 0 (transients) */
		LABEL_OFF2		/* = LABEL_OFF1 + LABEL_DELTA = 33011 */
	};
	u32 i;

	for (i = 0; i < 4; i++) {
		u32 blk = first_block + avatar_off[i];
		if (mei_read_block(blk, (u8 *)dl) < 0)
			continue;
		if (label_valid(dl)) {
			*label_block = blk;
			return 0;
		}
	}
	return -1;
}

/* Read the RomTag table block; return the number of tags, -1 if no
 * terminating row is found.  Vendor rule (FindRT): a zero SubSysType
 * alone terminates the table. */
int disc_read_rtt(RomTag *rtt, u32 rtt_block)
{
	u32 i;

	if (mei_read_block(rtt_block, (u8 *)rtt) < 0)
		return -1;
	for (i = 0; i < DISC_BLOCK_SIZE / sizeof(RomTag); i++) {
		if (rtt[i].rt_SubSysType == 0)
			return (int)i;
	}
	return -1;
}

const RomTag *disc_find_rt(const RomTag *rtt, u8 subsys, u8 type)
{
	u32 i;

	for (i = 0; i < DISC_BLOCK_SIZE / sizeof(RomTag); i++) {
		if (rtt[i].rt_SubSysType == 0)
			break;
		if (rtt[i].rt_SubSysType == subsys && rtt[i].rt_Type == type)
			return &rtt[i];
	}
	return NULL;
}
