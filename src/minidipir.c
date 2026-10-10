/*
 * opera-boot (GPL-2.0-only): clean-room minimal 3DO bootstrapper.
 *
 * minidipir.c: the dipir() boot attempt and the kernel-visible dipir
 * service stubs.
 *
 * Boot flow (mirrors the retail boot contract, all RSA/crypto stages
 * deliberately dropped):
 *   1. gate the video timing chain on the splash-chain enable bits so
 *      the launch ROM's banner stays visible while we work
 *   2. attach to the MEI CD unit; bail to the banner unless a ready,
 *      door-closed, disc-in drive answers
 *   3. disc-shape checks: single digital data track, single session
 *   4. probe the volume label (avatars at first-block +0/+225/+33011, i.e.
 *      blocks 150 / 375 / 33161 for FB=150) and read the RomTag table that
 *      follows it
 *   5. splash image (if any) is read into the staging area and shown
 *   6. misc code (or the C&B-era old-misc rebuild), then the OS
 *      component, relocated into the component area on page rules
 *   7. zero the inter-component gaps, publish the kernel address in
 *      [0x28], and jump with r6=0x101, r7=&BootData, r8=misc addr
 *
 * The version race is intentionally absent: this ROM exists to always
 * run the OS found on the CD.
 */
#include "opera-boot.h"

// Scalar query ABI from the sysinfo.h and super.h interface contracts.
#define SYSINFO_TAG_FIELDFREQ     0x10001
#define SYSINFO_TAG_GRAPHDISPSUPP 0x20004
#define SYSINFO_NTSC_SUPPORTED    0x00000001
#define SYSINFO_PAL_SUPPORTED     0x00000002
#define SYSINFO_NTSC_DFLT         0x00000100
#define SYSINFO_PAL_DFLT          0x00000200
#define SYSINFO_NTSC_CURDISP      0x00010000
#define SYSINFO_PAL_CURDISP       0x00020000
#define SYSINFO_FREQ_50HZ         50
#define SYSINFO_FREQ_60HZ         60
#define SYSINFO_UNSUPPORTEDTAG   0xFFFFFFFE

#define BOOT_LOG_DECIMAL_BASE    10
#define BOOT_LOG_U32_DIGITS      10
#define BOOT_LOG_TEXT_BYTES      32
#define BOOT_LOG_ASCII_FIRST     0x20
#define BOOT_LOG_ASCII_LAST      0x7E
#define BOOT_LOG_HEX_SHIFT       4
#define BOOT_LOG_HEX_MASK        0x0F
#define BOOT_LOG_BOOTDATA_VERSION 24

BootData g_bootdata;

/* Data-abort net landing pad: 0 while disarmed.  vectors.s reads this
 * pointer from the dabt handler, so it must stay a link-visible global. */
u32 *g_dabt_pad;
static ob_jmpbuf g_jb;

/* Boot-null marker: launch plants this ROM address in [0x28] at cold
 * start; the first successful boot replaces it with the kernel image. */
#define FAKEHDR_ADDR 0x0301F000

/* Identity of the disc we booted, for the runtime same-disc probe:
 * TOC-derived first data block plus the label's generic content
 * fields.  dl_VolumeUniqueIdentifier is diagnostic-only -
 * no boot policy may depend on mastering-rolled ids, only on what the
 * disc says it contains. */
static u32 g_booted_first_block;
static u32 g_booted_volblocks;
/* dl_VolumeIdentifier as words: the same-disc compare's alignment is a
 * type property, not a link-placement accident. */
static u32 g_booted_volid[32 / sizeof(u32)];

/* Same-disc test: the label re-probed from the drive matches the
 * booted disc's generic content fields (volume identifier + block
 * count).  Label side is 4-aligned by DiscLabel layout, saved side by
 * type. */
static int same_disc_label(const DiscLabel *l)
{
	const u32 *a = (const u32 *)l->dl_VolumeIdentifier;
	const u32 *b = g_booted_volid;
	int i;

	for (i = 0; i < (int)(32 / sizeof(u32)); i++) {
		if (a[i] != b[i])
			return 0;
	}
	return l->dl_VolumeBlockCount == g_booted_volblocks;
}

/* Fixed-size read targets: every read writes a full 2048-byte block. */
typedef union { DiscLabel l; u8 pad[DISC_BLOCK_SIZE]; } LabelBuf;
static LabelBuf  g_label_buf;
#define g_label  (g_label_buf.l)
static RomTag    g_rtt[DISC_BLOCK_SIZE / sizeof(RomTag)];
/* Runtime-poll probes must NOT target g_label: it is published to the
 * kernel at boot (r7 / bd_BootVolumeLabel) and must stay frozen across
 * polls even when a probe fails (the vendor likewise never rewrites
 * its published label).  Nor may the probe write upper DRAM (the
 * running game owns it).  g_rtt is exactly one block, blob-resident
 * and dead once boot completes, so it is the probe scratch. */
#define g_probe  (*(DiscLabel *)g_rtt)

/* ------------------------------------------------------------------ */
/* Boot handoff and fault diagnostics via the Opera kprint channel.    */
/* ------------------------------------------------------------------ */
void kprint(const char *s)
{
	while (*s)
		MADAM_KPRINT = (u8)*s++;
}

void khex8(u32 v)
{
	static const char hx[] = "0123456789ABCDEF";
	int i;
	for (i = 28; i >= 0; i -= 4)
		MADAM_KPRINT = hx[(v >> i) & 15];
}


static
void
_log_decimal(u32 value_)
{
  char digits[(BOOT_LOG_U32_DIGITS + 1)];
  u32 quotient;
  u32 pos = BOOT_LOG_U32_DIGITS;

  digits[pos] = '\0';
  do
    {
      quotient = ob_udiv(value_, BOOT_LOG_DECIMAL_BASE);
      --pos;
      digits[pos] = (char)('0' + (value_ - (quotient * BOOT_LOG_DECIMAL_BASE)));
      value_ = quotient;
    }
  while((value_ != 0) && (pos != 0));

  kprint(&digits[pos]);
}


static
void
_log_decimal_field(const char *label_,
                   u32         value_)
{
  kprint(label_);
  _log_decimal(value_);
}


static
void
_log_hex_field(const char *label_,
               u32         value_)
{
  kprint(label_);
  kprint("0x");
  khex8(value_);
}


static
void
_log_version(u8 version_,
             u8 revision_)
{
  _log_decimal(version_);
  kprint(".");
  _log_decimal(revision_);
}


static
void
_log_text_byte(u8 byte_)
{
  static const char hex[] = "0123456789ABCDEF";
  char text[] = "\\x00";

  if((byte_ >= BOOT_LOG_ASCII_FIRST) &&
     (byte_ <= BOOT_LOG_ASCII_LAST) &&
     (byte_ != '"') &&
     (byte_ != '\\'))
    {
      text[0] = (char)byte_;
      text[1] = '\0';
      kprint(text);
      return;
    }

  text[2] = hex[(byte_ >> BOOT_LOG_HEX_SHIFT)];
  text[3] = hex[(byte_ & BOOT_LOG_HEX_MASK)];
  kprint(text);
}


// Trim NUL padding only; escape embedded controls within the 32-byte field.
static
void
_log_text32(const char *label_,
            const u8   *text_)
{
  u32 i;
  u32 bytes = BOOT_LOG_TEXT_BYTES;

  while((bytes != 0) && (text_[(bytes - 1)] == '\0'))
    --bytes;
  kprint(label_);
  kprint("\"");
  for(i = 0; i < bytes; ++i)
    _log_text_byte(text_[i]);

  kprint("\"\r\n");
}


static
void
_log_disc(u32 first_block_,
          u32 label_block_,
          u32 rtt_block_)
{
  _log_text32(OPERA_BOOT_LOG_PREFIX "disc identifier=", g_label.dl_VolumeIdentifier);
  _log_text32(OPERA_BOOT_LOG_PREFIX "disc commentary=", g_label.dl_VolumeCommentary);
  _log_hex_field(OPERA_BOOT_LOG_PREFIX "disc unique-id=", g_label.dl_VolumeUniqueIdentifier);
  _log_decimal_field(" structure-version=", g_label.dl_VolumeStructureVersion);
  _log_hex_field(" flags=", g_label.dl_VolumeFlags);
  _log_decimal_field(" block-count=", g_label.dl_VolumeBlockCount);
  _log_decimal_field(" block-size=", g_label.dl_VolumeBlockSize);
  kprint("\r\n");
  _log_decimal_field(OPERA_BOOT_LOG_PREFIX "disc track-first-block=", first_block_);
  _log_decimal_field(" label-block=", label_block_);
  _log_decimal_field(" table-block=", rtt_block_);
  kprint("\r\n");
}


static
void
_log_tag(const char   *label_,
         const RomTag *tag_,
         u32           rtt_block_)
{
  kprint(OPERA_BOOT_LOG_PREFIX "tag ");
  kprint(label_);
  if(tag_ == NULL)
    {
      kprint(" missing (optional)\r\n");
      return;
    }

  _log_hex_field(" subsystem=", tag_->rt_SubSysType);
  _log_hex_field(" type=", tag_->rt_Type);
  kprint(" version=");
  _log_version(tag_->rt_Version, tag_->rt_Revision);
  _log_hex_field(" flags=", tag_->rt_Flags);
  _log_hex_field(" type-specific=", tag_->rt_TypeSpecific);
  _log_decimal_field(" absolute-sector=", (rtt_block_ + tag_->rt_Offset));
  _log_decimal_field(" size-bytes=", tag_->rt_Size);
  kprint("\r\n");
}


static
void
_log_components(u32 kernel_index_,
                int legacy_misc_)
{
  u32 i;

  if(legacy_misc_)
    kprint(OPERA_BOOT_LOG_PREFIX "misc source=resident replacement (legacy misc tag)\r\n");
  for(i = 0; i < g_num_components; ++i)
    {
      _log_decimal_field(OPERA_BOOT_LOG_PREFIX "image index=", i);
      if(i == kernel_index_)
        kprint(" role=kernel");
      else if(i < kernel_index_)
        kprint(" role=misc");
      else
        kprint(" role=OS executable");
      _log_hex_field(" destination=", g_components[i].comp_Address);
      _log_decimal_field(" size-bytes=", g_components[i].comp_Size);
      kprint("\r\n");
    }
}


static
void
_log_kernel_aif(const Component *kernel_)
{
  const u32 *aif = (const u32 *)kernel_->comp_Address;

  if(kernel_->comp_Size < AIF_SIZEOF)
    {
      kprint(OPERA_BOOT_LOG_PREFIX "kernel AIF metadata unavailable (short image)\r\n");
      return;
    }

  _log_decimal_field(OPERA_BOOT_LOG_PREFIX "kernel AIF RO-bytes=", aif[AIF_RO_SIZE]);
  _log_decimal_field(" RW-bytes=", aif[AIF_RW_SIZE]);
  _log_decimal_field(" ZI-bytes=", aif[AIF_ZI_SIZE]);
  kprint("\r\n");
}


static
void
_log_kernel_version(const Component *kernel_)
{
  const Bin3DOHeader *header;

  if(kernel_->comp_Size < (AIF_SIZEOF + sizeof(Bin3DOHeader)))
    {
      kprint(OPERA_BOOT_LOG_PREFIX "kernel version metadata unavailable (short image)\r\n");
      kprint(OPERA_BOOT_LOG_PREFIX "boot-args ABI unavailable (short kernel header)\r\n");
      return;
    }

  header = (const Bin3DOHeader *)(kernel_->comp_Address + AIF_SIZEOF);
  kprint(OPERA_BOOT_LOG_PREFIX "kernel version=");
  _log_version(header->b3_nVersion, header->b3_nRevision);
  kprint("\r\n");
  if(header->b3_nVersion >= BOOT_LOG_BOOTDATA_VERSION)
    kprint(OPERA_BOOT_LOG_PREFIX "boot-args ABI=BootData r6=0x00000101\r\n");
  else
    kprint(OPERA_BOOT_LOG_PREFIX "boot-args ABI=DiscLabel r6=0x00000100\r\n");
}

/* ------------------------------------------------------------------ */
/* Video: enable the pixel pipeline if it is off (ModMCTL BITSET       */
/* semantics: touch VSCTXEN first, wait a frame, then the rest,        */
/* inside the safe CLUT-DMA window between scan lines 10..13).         */
/* ------------------------------------------------------------------ */
#define MCTL_SAFE_MIN 10
#define MCTL_SAFE_MAX 13

static void wait_vcnt_window(void)
{
	u32 v;
	do {
		v = CLIO_VCNT & VCNT_MASK;
	} while (v < MCTL_SAFE_MIN || v >= MCTL_SAFE_MAX);
}

static void wait_vcnt_past(void)
{
	u32 v;
	do {
		v = CLIO_VCNT & VCNT_MASK;
	} while (v <= MCTL_SAFE_MAX);
}

/* One-time hardware init the booted OS may rely on (retail-equivalent):
 * timer 14 cascades into 15 at 1ms cadence, timer slack = 336, and the
 * ADBIO bus is forced to the primary ROM while dipir runs. */
#define CLIO_TIM14_CNT     0x170
#define CLIO_TIM14_CNTBACK 0x174
#define CLIO_TM1_SET       0x208
#define CLIO_TM1_CLR       0x20C
#define CLIO_SLACK         0x220

static u32 g_saved_adbio;
static u32 g_saved_slack;
static int g_hw_active;

static void hw_init(void)
{
	g_saved_slack = *(volatile u32 *)(CLIO_BASE + CLIO_SLACK);
	g_saved_adbio = CLIO_ADBIO;
	g_hw_active = 1;

	*(volatile u32 *)(CLIO_BASE + CLIO_TIM14_CNT) = 61;
	*(volatile u32 *)(CLIO_BASE + CLIO_TIM14_CNTBACK) = 61;
	*(volatile u32 *)(CLIO_BASE + CLIO_TM1_CLR) = 0xFF000000;
	*(volatile u32 *)(CLIO_BASE + CLIO_TM1_SET) = 0x73000000;
	*(volatile u32 *)(CLIO_BASE + CLIO_SLACK) = 336;

	/* Vendor entry: raise OTHERROM_EN and force the primary ROM in one
	 * state; the enable bit must stay up for the whole dipir pass. */
	CLIO_ADBIO = (g_saved_adbio | 0x40) & ~0x04;
}

static void hw_done(void)
{
	if (!g_hw_active)
		return;

	/* Vendor dipir exit state: OTHERROM_EN stays raised together with
	 * the saved OTHERROM select, and Slack goes back to the caller's
	 * value. */
	CLIO_ADBIO = g_saved_adbio | 0x40;
	*(volatile u32 *)(CLIO_BASE + CLIO_SLACK) = g_saved_slack;
	g_hw_active = 0;
}

static void video_on(void)
{
again:
	wait_vcnt_window();
	if ((MADAM_MCTL & MCTL_VSCTXEN) == 0) {
		MADAM_MCTL |= MCTL_VSCTXEN;
		wait_vcnt_past();
		goto again;
	}
	wait_vcnt_window();
	MADAM_MCTL |= MCTL_CLUTXEN;
}

/* ------------------------------------------------------------------ */
/* Dipir service-vector payload stubs (kernel calls into these).      */
/* ------------------------------------------------------------------ */
int  stub_rsa_ok(void)          { return 1; }
int  stub_zero(void)            { return 0; }

// SetROMSysInfo and unimplemented queries retain the unsupported result.
s32
stub_sysinfo_unsupported(void)
{
  return (s32)SYSINFO_UNSUPPORTEDTAG;
}


u32
ob_query_rom_sysinfo(u32   tag_,
                    void *info_,
                    u32   info_size_)
{
  u32 mode;

  // These tags return scalars in r0, not in the optional info buffer.
  (void)info_;
  (void)info_size_;
  if((tag_ != SYSINFO_TAG_FIELDFREQ) && (tag_ != SYSINFO_TAG_GRAPHDISPSUPP))
    return SYSINFO_UNSUPPORTEDTAG;

  mode = *(volatile u32 *)MAGIC_DISPTYPE;
  if((mode != BOOT_DISPTYPE_NTSC) && (mode != BOOT_DISPTYPE_PAL))
    return SYSINFO_UNSUPPORTEDTAG;

  if(tag_ == SYSINFO_TAG_FIELDFREQ)
    return ((mode == BOOT_DISPTYPE_PAL) ? SYSINFO_FREQ_50HZ : SYSINFO_FREQ_60HZ);

  if(mode == BOOT_DISPTYPE_PAL)
    return ((SYSINFO_PAL_SUPPORTED | SYSINFO_PAL_DFLT) | SYSINFO_PAL_CURDISP);

  return ((SYSINFO_NTSC_SUPPORTED | SYSINFO_NTSC_DFLT) | SYSINFO_NTSC_CURDISP);
}


u32 get_disc_os_version(void)
{
	u32 img = *(volatile u32 *)MAGIC_KERNELADR;
	const Bin3DOHeader *bh =
		(const Bin3DOHeader *)(img + AIF_SIZEOF);
	return ((u32)bh->b3_nVersion << 8) | bh->b3_nRevision;
}

/* Early-kernel fault reporter: called from the trap vectors installed
 * just before handoff.  Never returns. */
void ob_trap_c(u32 which, u32 lr)
{
	static const char names[5][5] = { "", "undf", "addr", "pabt", "dabt" };
	kprint("\r\n" OPERA_BOOT_LOG_PREFIX "TRAP ");
	if (which < 5)
		kprint(names[which]);
	kprint(" lr=");
	khex8(lr);
	for (;;)
		;
}

/* ------------------------------------------------------------------ */
/* Boot attempt.                                                      */
/* ------------------------------------------------------------------ */

/* Wait for "drive ready with a disc in place" (retail InitDisc model:
 * the drive gets up to ~4 s per attempt: 80 tries x ~50 ms pacing). */
static int await_drive(int *errstat)
{
	int e;
	u32 tries;

	for (tries = 0; tries < 80; tries++) {
		e = mei_dev_error();
		if (e < 0)
			return -1;
		*errstat = e;
		if ((e & CDMEI_DOORCLOSED) == 0)
			return -1;          /* door open: banner again */
		if ((e & CDMEI_DISCIN) == 0)
			return -1;          /* empty drive */
		if (e & CDMEI_READY)
			return 0;
		mei_wait_lines(787);        /* ~50 ms */
	}
	return -1;
}

static int disc_shape_ok(u32 *first_block)
{
	DiscInfo di;
	TOCInfo toc;
	u8 sflags;

	if (mei_disc_info(&di) < 0)
		return -1;
	if (di.di_DiscId != 0)
		return -1;
	if (di.di_FirstTrackNumber != 1 || di.di_LastTrackNumber < 1)
		return -1;
	if (mei_session_info(&sflags) < 0)
		return -1;
	if (sflags & 0x80)              /* multisession: not a 3DO disc */
		return -1;
	if (mei_read_toc(1, &toc) < 0)
		return -1;
	if (toc.toc_AddrCntrl & (ACB_AUDIO_PREEMPH | ACB_FOUR_CHANNEL))
		return -1;
	if ((toc.toc_AddrCntrl & ACB_DIGITAL_TRACK) == 0)
		return -1;

	*first_block = (u32)toc.toc_CDROMAddr_Frm +
		       (u32)toc.toc_CDROMAddr_Sec * FRAMEperSEC +
		       (u32)toc.toc_CDROMAddr_Min * FRAMEperSEC * SECperMIN;
	return 0;
}

/* Vendor TRYAGAIN recovery (cdipir.c): a dipir-induced abort can leave
 * the drive holding half a command sequence.  POLL-RESET it to a known
 * state for the next attempt (Expansion Spec Aug-1993 timings: 100 ms
 * asserted, 600 ms released; 15.75 VCNT lines/ms).  Deliberately the
 * ONLY device traffic on this path - a wedged device may not answer. */
static void xbus_poll_reset_settle(void)
{
	XB_POLL = XBUS_POLL_RESET;
	mei_wait_lines(1575);		/* ~100 ms */
	XB_POLL = 0;
	mei_wait_lines(9450);		/* ~600 ms */
}

static void dipir_body(void)
{
	static const u8 splash_pattern[7] =
		{ 'A','P','P','S','C','R','N' };
	const RomTag *rt_os, *rt_misc, *rt_splash, *rt_perms;
	u32 first_block, label_block, rtt_block;
	u32 os_addr;
  u32 kern_index;
	int errstat;

	/* Fresh state each entry (the blob is re-entered on disc change). */
	g_num_components = 0;
	g_component_load_addr = COMPONENT_BASE;
	g_bootdata.bd_Version = 1;
	g_bootdata.bd_DevicePerms = 0;
	g_bootdata.bd_BootVolumeLabel = &g_label;

	if (mei_init() < 0) {
		kprint(OPERA_BOOT_LOG_PREFIX "noxbus\r\n");
		return;
	}

	if (*(volatile u32 *)MAGIC_KERNELADR != FAKEHDR_ADDR) {
		/* Runtime re-entry (the OS pokes vector 0 on its own
		 * schedule).  Same disc still in the drive?  One label
		 * read answers it; get out without touching the running
		 * system.  A changed/missing label falls through to the
		 * full boot path (also the multi-disc swap path). */
		int e;
		u32 lb;
		/* Probe at the booted disc's own first block (TOC-derived);
		 * hardcoding 150 would miss every label avatar on a disc
		 * whose data track starts elsewhere and force a full
		 * re-boot of a running system. */
		if (await_drive(&e) == 0 &&
		    disc_probe_label(&g_probe, g_booted_first_block, &lb) == 0 &&
		    same_disc_label(&g_probe))
			return;
	}

	video_on();
	hw_init();

	/* Arm the ROM's own startup VDL before anything paints: real
	 * silicon has no pre-seeded VDL, so without this the banner and
	 * splash are unwatchable there.  Full-boot path only - the
	 * same-disc re-entry above returned already, and rewriting VRAM
	 * 0x2B0000 / the VDL head would trample a live OS. */
	install_startup_vdl();

	if (await_drive(&errstat) < 0) {
		kprint(OPERA_BOOT_LOG_PREFIX "nodisc\r\n");
		return;
	}
	if ((errstat & CDMEI_SPINNING) == 0)
		mei_spin_up();

	if (disc_shape_ok(&first_block) < 0) {
		kprint(OPERA_BOOT_LOG_PREFIX "not3do\r\n");
		return;
	}
	if (disc_probe_label(&g_label, first_block, &label_block) < 0) {
		kprint(OPERA_BOOT_LOG_PREFIX "nolabel\r\n");
		return;
	}
	rtt_block = label_block + 1;
	if (disc_read_rtt(g_rtt, rtt_block) < 2) {
		kprint(OPERA_BOOT_LOG_PREFIX "nortt\r\n");
		return;
	}
	rt_os = disc_find_rt(g_rtt, RSANODE, RSA_OS);
	if (rt_os == NULL) {
		kprint(OPERA_BOOT_LOG_PREFIX "noos\r\n");
		return;
	}
	rt_misc = disc_find_rt(g_rtt, RSANODE, RSA_MISCCODE);
	if (rt_misc == NULL)
		rt_misc = disc_find_rt(g_rtt, RSANODE, RSA_OLD_MISCCODE);
	if (rt_misc == NULL) {
		kprint(OPERA_BOOT_LOG_PREFIX "nomisc\r\n");
		return;
	}

	rt_perms = disc_find_rt(g_rtt, RSANODE, RSA_DEV_PERMS);
	if (rt_perms != NULL)
		g_bootdata.bd_DevicePerms = rt_perms->rt_Reserved3[0];

	if (rt_misc->rt_Type == RSA_OLD_MISCCODE) {
		/* C&B-era: the misc library rides inside the OS image. */
		if (load_component(rt_os->rt_Offset + rtt_block,
				   rt_os->rt_Size) < 0) {
			kprint(OPERA_BOOT_LOG_PREFIX "osread\r\n");
			return;
		}
		relocate_old_misc();
	} else {
		if (load_component(rt_misc->rt_Offset + rtt_block,
				   rt_misc->rt_Size) < 0) {
			kprint(OPERA_BOOT_LOG_PREFIX "miscread\r\n");
			return;
		}
		if (relocate_component() < 0) {
			kprint(OPERA_BOOT_LOG_PREFIX "miscfit\r\n");
			return;
		}
	}

	/* Splash first: it is what the user sees while the OS loads. */
	rt_splash = disc_find_rt(g_rtt, RSANODE, RSA_APPSPLASH);
	if (rt_splash != NULL && rt_splash->rt_Size != 0) {
		if (load_component(rt_splash->rt_Offset + rtt_block,
				   rt_splash->rt_Size) == 0)
      (void)display_image((const void *)STAGE_ADDR,
                         rt_splash->rt_Size,
                         splash_pattern);
	}

	/* OS component: first file on a 1KB boundary.  The kernel's
	 * _3DO_Stack region is NOT added: mastering already folded it into
	 * the recorded load address. */
	{
		if (load_component(rt_os->rt_Offset + rtt_block,
				   rt_os->rt_Size) < 0) {
			kprint(OPERA_BOOT_LOG_PREFIX "osread\r\n");
			return;
		}
		g_component_load_addr =
			(g_component_load_addr + 1023) & ~1023;
		kern_index = g_num_components;
		if (relocate_component() < 0) {
			kprint(OPERA_BOOT_LOG_PREFIX "osfit\r\n");
			return;
		}
		os_addr = g_components[kern_index].comp_Address;
	}

  _log_disc(first_block, label_block, rtt_block);
  _log_tag("OS", rt_os, rtt_block);
  _log_tag("misc", rt_misc, rtt_block);
  _log_tag("splash", rt_splash, rtt_block);
  _log_tag("permissions", rt_perms, rtt_block);
  _log_hex_field(OPERA_BOOT_LOG_PREFIX "device permissions=", g_bootdata.bd_DevicePerms);
  kprint("\r\n");
  _log_components(kern_index, (rt_misc->rt_Type == RSA_OLD_MISCCODE));
  _log_kernel_aif(&g_components[kern_index]);
  _log_kernel_version(&g_components[kern_index]);
  kprint(OPERA_BOOT_LOG_PREFIX "handoff target=disc OS kernel\r\n");

	kprint(OPERA_BOOT_LOG_PREFIX "boot ");
	khex8(os_addr);
	kprint("\r\n");

	/* Boot-argument ABI: two families exist.
	 * Pre-v24 kernels read r7 as a DiscLabel pointer directly
	 * (r6=0x100); passing them a BootData struct is fatal (they parse
	 * it as a label).  v24+ kernels check r6's BOOTDATA bit (r6=0x101,
	 * r7=&BootData) and translate bd_BootVolumeLabel into
	 * kb_BootVolumeName, which the v24 operator turns into the $boot
	 * alias; with the old ABI the volume name collapses to "rom", every
	 * $folios/$boot open fails with ER_Fs_NoFileSystem, and the game
	 * wedges right after its banner.  This is the vendor cdipir's own
	 * per-kernel discrimination, mirrored. */
	clear_gaps();
	hw_done();
	g_booted_first_block = first_block;
	g_booted_volblocks = g_label.dl_VolumeBlockCount;
	{
		const u32 *v = (const u32 *)g_label.dl_VolumeIdentifier;
		u32 *d = g_booted_volid;
		int i;

		for (i = 0; i < (int)(32 / sizeof(u32)); i++)
			d[i] = v[i];
	}
	{
		const Bin3DOHeader *bh =
			(const Bin3DOHeader *)(os_addr + AIF_SIZEOF);
		if (bh->b3_nVersion >= 24) {
			boot_kernel(os_addr, 0x101, (u32)&g_bootdata,
				    g_components[0].comp_Address);
		} else {
			boot_kernel(os_addr, 0x100, (u32)&g_label,
				    g_components[0].comp_Address);
		}
	}
	/* not reached */
}

/* ------------------------------------------------------------------ */
/* dipir vector 0 entry: run the boot attempt under the dabt net.     */
/* A caught abort interrupted an MEI transaction mid-stream; reset    */
/* the bus and hand control back to the launch banner/retry loop.     */
/* ------------------------------------------------------------------ */
void dipir_c(void)
{
	ob_arm_dabt_net();
	if (ob_setjmp(g_jb) != 0) {
		kprint(OPERA_BOOT_LOG_PREFIX "dabt\r\n");
		xbus_poll_reset_settle();
		hw_done();
		ob_disarm_dabt_net();
		return;
	}
	dipir_body();
	hw_done();
	ob_disarm_dabt_net();
}
