/*
 * opera-boot (GPL-2.0-only): clean-room minimal 3DO bootstrapper.
 *
 * Shared types, hardware addresses, and wire-protocol structures for the
 * mini dipir blob (resident at DRAM 0x200).  No Portfolio sources.
 *
 * All on-disc structures are big-endian ARM wire format; the struct
 * layouts below are the immutable boot ABI between dipir, Sherry and
 * the MEI CD drive, and are dictated by the hardware/ROM interface.
 */
#ifndef OPERA_BOOT_H
#define OPERA_BOOT_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;
typedef signed long    s32;

#define NULL ((void*)0)
#define VOLUME_SYNC_LEN 5

#define OPERA_BOOT_LOG_PREFIX "opera-boot: "

/* ------------------------------------------------------------------ */
/* Hardware                                                           */
/* ------------------------------------------------------------------ */

#define MADAM_BASE     0x03300000
#define MADAM_KPRINT      (*(volatile u32*)(MADAM_BASE + 0x00))
#define MADAM_MSYS        (*(volatile u32*)(MADAM_BASE + 0x04))
#define MADAM_MCTL        (*(volatile u32*)(MADAM_BASE + 0x08))

#define MCTL_VSCTXEN   0x00004000
#define MCTL_CLUTXEN   0x00002000
#define MCTL_OP_SET    3
#define MCTL_OP_BITSET 1

#define CLIO_BASE      0x03400000
#define CLIO_CSTAT        (*(volatile u32*)(CLIO_BASE + 0x28))
#define CLIO_VCNT         (*(volatile u32*)(CLIO_BASE + 0x34))
/* ClioInterrupts layout: SetInt +0, ClrInt +4, SetIntEn +8, ClrIntEn +C;
 * bank0 at +0x40, bank1 at +0x60. */
#define CLIO_CLRINT0      (*(volatile u32*)(CLIO_BASE + 0x44))
#define CLIO_CLRINT0EN    (*(volatile u32*)(CLIO_BASE + 0x4C))
#define CLIO_CLRINT1      (*(volatile u32*)(CLIO_BASE + 0x64))
#define CLIO_CLRINT1EN    (*(volatile u32*)(CLIO_BASE + 0x6C))
#define CLIO_ADBIO        (*(volatile u32*)(CLIO_BASE + 0x84))

#define VCNT_MASK      0x000007FF

/* Expansion bus (CLIO + 0x400), unit slots at +0x100/+0x140/... */
#define XBUS_BASE      0x03400400
#define XB_SETEXPCTL      (*(volatile u32*)(XBUS_BASE + 0x000))
#define XB_CLREXPCTL      (*(volatile u32*)(XBUS_BASE + 0x004))
#define XB_EXPTYPE        (*(volatile u32*)(XBUS_BASE + 0x008))
#define XB_DIPIR1         (*(volatile u32*)(XBUS_BASE + 0x010))
#define XB_DIPIR2         (*(volatile u32*)(XBUS_BASE + 0x014))
#define XB_SEL            (*(volatile u32*)(XBUS_BASE + 0x100))
#define XB_POLL           (*(volatile u32*)(XBUS_BASE + 0x140))
#define XB_CMDSTAT        (*(volatile u32*)(XBUS_BASE + 0x180))
#define XB_DATA           (*(volatile u32*)(XBUS_BASE + 0x1C0))

#define XB_CPUHASXBUS     (1u << 7)
#define XBUS_DIPIR_CURRENT 0x00008000

/* Data-abort net: pad armed by ob_setjmp during a boot attempt;
 * vectors.s reads g_dabt_pad directly (0 = disarmed, fault is fatal). */
typedef u32 ob_jmpbuf[12];
extern u32 *g_dabt_pad;
int  ob_setjmp(ob_jmpbuf env);
void ob_longjmp(ob_jmpbuf env, int val);
void ob_arm_dabt_net(void);
void ob_disarm_dabt_net(void);

/* Build + arm the ROM-provided startup VDL (VRAM 0xB0000, MADAM+0x580);
 * full-boot paths only, never during a same-disc runtime re-entry. */
void install_startup_vdl(void);

#define XBUS_POLL_STAT_INT_EN   0x01
#define XBUS_POLL_READ_INT_EN   0x02
#define XBUS_POLL_WRIT_INT_EN   0x04
#define XBUS_POLL_RESET         0x08
#define XBUS_POLL_STAT_VALID    0x10
#define XBUS_POLL_READ_VALID    0x20
#define XBUS_POLL_WRIT_VALID    0x40
#define XBUS_POLL_MEDIA_ACCESS  0x80

/* ------------------------------------------------------------------ */
/* Magic DRAM cells (dipir <-> launch <-> kernel contract)            */
/* ------------------------------------------------------------------ */

#define MAGIC_KERNELADR   0x28   /* staged kernel AIF image pointer    */
#define MAGIC_ROMTAGS     0x2C   /* ROM RomTag table address           */
#define MAGIC_RESET       0x30   /* warm entry (launch)                */
#define MAGIC_SENTINEL    0x34
#define MAGIC_DISPTYPE    0x38
#define MAGIC_DIPIR_RESUME 0x40   // Pending resident reset continuation; 0 when idle.

// Launch caches the timing family, not the graphics folio's PAL1/PAL2 width.
// Values match launch.s; a stopped counter retains the NTSC boot default.
#define BOOT_DISPTYPE_NTSC 0
#define BOOT_DISPTYPE_PAL  1

/* ------------------------------------------------------------------ */
/* Disc / MEI protocol                                                */
/* ------------------------------------------------------------------ */

#define CDMEI_READY      0x01
#define CDMEI_ERROR      0x10
#define CDMEI_SPINNING   0x20
#define CDMEI_DISCIN     0x40
#define CDMEI_DOORCLOSED 0x80

#define MEI_DISCINFO     0x8B
#define MEI_SESSIONINFO  0x8D
#define MEI_READTOC      0x8C
#define MEI_READBLOCK    0x10
#define MEI_DEVERROR     0x82
#define MEI_SPINUP       0x02

#define FRAMEperSEC      75
#define SECperMIN        60

#define DISC_BLOCK_SIZE  2048
/* Vendor avatar geometry (dipir's FindVolumeLabelBlock + the filesystem's
 * avatar walk in MountFileSystem.c): avatars live at +0, +DISC_LABEL_OFFSET,
 * then successive +DISC_LABEL_AVATAR_DELTA. */
#define LABEL_OFF0       0        /* avatar 0: TOC track-1 first block */
#define LABEL_OFF1       225      /* DISC_LABEL_OFFSET */
#define LABEL_DELTA      32786    /* DISC_LABEL_AVATAR_DELTA */
#define LABEL_OFF2       (LABEL_OFF1 + LABEL_DELTA)  /* avatar 2: +33011 */

/* Disc label (132 bytes, block 150 usually) */
typedef struct DiscLabel {
	u8  dl_RecordType;                 /* 1 */
	u8  dl_VolumeSyncBytes[5];         /* 0x5A x5 */
	u8  dl_VolumeStructureVersion;     /* 1 */
	u8  dl_VolumeFlags;                /* 1 = data disc */
	u8  dl_VolumeCommentary[32];
	u8  dl_VolumeIdentifier[32];
	u32 dl_VolumeUniqueIdentifier;
	u32 dl_VolumeBlockSize;
	u32 dl_VolumeBlockCount;
	u32 dl_RootUniqueIdentifier;
	u32 dl_RootDirectoryBlockCount;
	u32 dl_RootDirectoryBlockSize;
	u32 dl_RootDirectoryLastAvatarIndex;
	u32 dl_RootDirectoryAvatarList[8];
} DiscLabel;

/* RomTag: 32-byte entry in the block after the label */
typedef struct RomTag {
	u8  rt_SubSysType;
	u8  rt_Type;
	u8  rt_Version;
	u8  rt_Revision;
	u8  rt_Flags;
	u8  rt_TypeSpecific;
	u8  rt_Reserved1;
	u8  rt_Reserved2;
	u32 rt_Offset;        /* block offset from the RTT's block */
	u32 rt_Size;          /* bytes, incl. 64-byte RSA signature */
	u32 rt_Reserved3[4];
} RomTag;

#define RSANODE              0x0F   /* rt_SubSysType for CD RSA family */
#define RSA_BLOCKS_ALWAYS    0x02
#define RSA_SIGNATURE_BLOCK  0x05
#define RSA_BOOT             0x06
#define RSA_OS               0x07
#define RSA_NEWBOOT          0x09
#define RSA_NEWKNEWNEWGNUBOOT 0x0D
#define RSA_OLD_MISCCODE     0x0F
#define RSA_MISCCODE         0x10
#define RSA_APP              0x11
#define RSA_APPSPLASH        0x14
#define RSA_DEV_PERMS        0x17
#define RSA_BOOT_OVERLAY     0x18

/* MEI ReadDiscInfo status reply (7 bytes) */
typedef struct DiscInfo {
	u8 di_cmd;
	u8 di_DiscId;
	u8 di_FirstTrackNumber;
	u8 di_LastTrackNumber;
	u8 di_MSFEndAddr_Min;
	u8 di_MSFEndAddr_Sec;
	u8 di_MSFEndAddr_Frm;
} DiscInfo;

/* MEI ReadTOC status reply (9 bytes) */
typedef struct TOCInfo {
	u8 toc_cmd;
	u8 toc_reserved1;
	u8 toc_AddrCntrl;
	u8 toc_TrackNumber;
	u8 toc_reserved2;
	u8 toc_CDROMAddr_Min;
	u8 toc_CDROMAddr_Sec;
	u8 toc_CDROMAddr_Frm;
	u8 toc_reserved3;
} TOCInfo;

#define ACB_AUDIO_PREEMPH   1
#define ACB_DIGITAL_COPIABLE 2
#define ACB_DIGITAL_TRACK   4
#define ACB_FOUR_CHANNEL    8

/* ------------------------------------------------------------------ */
/* AIF / 3DO binary headers (kernel image layout)                     */
/* ------------------------------------------------------------------ */

#define AIF_BL_DECOMPRESS   0
#define AIF_BL_SELFRELOC    1
#define AIF_BL_ZEROINIT     2
#define AIF_BL_ENTRY        3
#define AIF_SWI_EXIT        4
#define AIF_RO_SIZE         5   /* offsets in u32 words */
#define AIF_RW_SIZE         6
#define AIF_DEBUG_SIZE      7
#define AIF_ZI_SIZE         8
#define AIF_SIZEOF          128 /* bytes (32 words) */

/* _3DOBinHeader (starts at AIF + 0x80) */
typedef struct Bin3DOHeader {
	/* _3DOBinHeader as aif.h + nodes.h lay it out (36-byte
	 * ItemNode, then the dipir-era fields).  b3_Stack lives at
	 * byte 40.  Note: a disc's file-record load address already
	 * folds the kernel stack region in (return-fire's v23 kernel
	 * masters sherry at 0x18000 with b3_Stack=0x2800 and boots
	 * without any bump), so relocate_component applies no reserve
	 * at all; b3_Stack is kept here for layout fidelity only. */
	u32 b3_nNext;             /* ItemNode */
	u32 b3_nPrev;
	u8  b3_nSubsysType;
	u8  b3_nType;
	u8  b3_nPriority;
	u8  b3_nFlags;
	u32 b3_nSize;
	u32 b3_nName;
	u8  b3_nVersion;
	u8  b3_nRevision;
	u8  b3_nFolioFlags;
	u8  b3_nItemFlags;
	u32 b3_nItem;
	u32 b3_nOwner;
	u32 b3_nReservedP;
	u8  b3_Flags;
	u8  b3_OS_Version;
	u8  b3_OS_Revision;
	u8  b3_Reserved;
	u32 b3_Stack;
	u32 b3_FreeSpace;
	/* rest unused by the boot contract */
} Bin3DOHeader;

/* ------------------------------------------------------------------ */
/* App splash image header                                            */
/* ------------------------------------------------------------------ */

// Pixels follow this complete on-disc header. Each interleaved pair holds
// 2 * width pixels; its byte stride is 2 * floor(width * depth / 8).
// Odd heights require a complete final pair. rt_Size bounds loaded bytes;
// vi_Size is image metadata, not authority to read beyond that span.
typedef struct VideoImage {
	u8  vi_Version;
	u8  vi_Pattern[7];        /* "APPSCRN" */
	u32 vi_Size;
	u16 vi_Height;
	u16 vi_Width;
	u8  vi_Depth;
	u8  vi_Type;
	u8  vi_Reserved1;
	u8  vi_Reserved2;
	u32 vi_Reserved3;
} VideoImage;

#define APP_SPLASH_PATTERN  "APPSCRN"

/* ------------------------------------------------------------------ */
/* Boot handoff                                                       */
/* ------------------------------------------------------------------ */

typedef struct BootData {
	u32        bd_Version;          /* 1 */
	DiscLabel *bd_BootVolumeLabel;
	u32        bd_DevicePerms;
} BootData;

/* Portfolio list ABI for the C&B-era misc vtable (nodes.h/list.h field
 * order, big-endian ARM layout).  Anchor invariant: a_first holds the
 * first node or &a_tailmark when empty; a_tailmark doubles as the
 * forward end marker and the Next cell of the last node; a_last holds
 * the last node or &a_first when empty. */
#define OB_NST_KERNEL     1	/* KERNELNODE  */
#define OB_NODETYPE_LIST  2	/* LISTNODE    */
#define OB_NODE_NAMEVALID 0x80

typedef struct ObNode {
	struct ObNode *n_Next;
	struct ObNode *n_Prev;
	u8             n_SubsysType;
	u8             n_Type;
	u8             n_Priority;
	u8             n_Flags;
	s32            n_Size;
	const char    *n_Name;
} ObNode;

typedef struct ObList {
	ObNode l_node;
	ObNode *a_first;
	ObNode *a_tailmark;
	ObNode *a_last;
} ObList;

typedef struct Component {
	u32 comp_Address;
	u32 comp_Size;
} Component;

/* Memory plan (2MB machine, mirroring the retail ROM's geometry):
 *   0x000200  this blob: vectors + code + data + the 2KB private stack,
 *             all inside the area gated < 0x8000 by tools/mapcheck.py
 *             (build/minidipir.map is the source of truth)
 *   0x00C000  old-misc rebuild for C&B-era discs (dead band below
 *             COMPONENT_BASE, so record-honored kernel placement can
 *             never collide with it; era kernels stack 0xE000..0x10000)
 *   0x010000  components land here upward (misc, kernel, operator, fs) -
 *             the kernel parks its 8KB of system stacks immediately
 *             below its image; starting here keeps those stacks clear of
 *             the dipir blob and of the misc component (0x4000 nearly
 *             killed v0.16/v21 boots by stack overlap).
 *   0x0A0000  component staging area (scratch)
 */
#define STAGE_ADDR        0x0A0000
#define COMPONENT_BASE    0x10000
#define OLDMISC_ADDR      0x0C000
#define OLDMISC_SPAN      (3 * 1024)
#define OLDMISC_SLOTS     14
#define OS_SIG_LEN        64
#define MAX_OS_COMPONENTS 8

/* ------------------------------------------------------------------ */
/* Cross-module interface                                             */
/* ------------------------------------------------------------------ */

/* mei.c: XBUS PIO + MEI CD commands */
u32   ob_udiv(u32 n, u32 d);              /* clib-free divide */
int   mei_init(void);                /* 0 ok, -1 XBUS attach timeout */
int   mei_dev_error(void);        /* last status byte, -1 on cmd error */
int   mei_disc_info(DiscInfo *di);
int   mei_session_info(u8 *flags);
int   mei_read_toc(u8 track, TOCInfo *toc);
int   mei_read_block(u32 blk, u8 *dst);   /* 0 ok, -1 error */
void  mei_spin_up(void);
void  mei_wait_lines(u32 lines);          /* VCNT-based pacing */

/* label.c */
int   disc_probe_label(DiscLabel *dl, u32 first_block, u32 *label_block);
int   disc_read_rtt(RomTag *rtt, u32 rtt_block);        /* tag count or -1 */
const RomTag *disc_find_rt(const RomTag *rtt, u8 subsys, u8 type);

/* loader.c */
extern Component g_components[MAX_OS_COMPONENTS];
extern u32 g_num_components;
int   load_component(u32 first_block, u32 size); /* read into STAGE_ADDR */
int   relocate_component(void);              /* 0 ok, -1 overflow/bad record */
void  relocate_old_misc(void);                   /* C&B-era misc rebuild  */

/* oldmisc.c: C&B-era RSA_OLD_MISCCODE vtable routines.  The booted OS
 * calls these through the buffer built by ob_build_old_misc; bodies are
 * blob-resident and survive boot in low DRAM. */
void   *ob_memcpy(void *dst, const void *src, u32 len);
void   *ob_memset(void *dst, u32 val, u32 len);
void    ob_list_addhead(ObList *l, ObNode *n);
void    ob_list_addtail(ObList *l, ObNode *n);
ObNode *ob_list_remhead(ObList *l);
ObNode *ob_list_remtail(ObList *l);
void    ob_list_remnode(ObNode *n);
void    ob_list_insert_from_head(ObList *l, ObNode *n);
void    ob_list_insert_from_tail(ObList *l, ObNode *n);
void    ob_list_init(ObList *l, const char *name);
u32     ob_rt_sdiv(u32 d1, u32 d2);          /* rtdiv.s: q r0, r r1 */
u32     ob_rt_udiv(u32 d1, u32 d2);          /* rtdiv.s: q r0, r r1 */
u32     ob_rt_sdiv10(u32 n);                 /* rtdiv.s: q r0, r r1 */
u32     ob_rt_udiv10(u32 n);                 /* rtdiv.s: q r0, r r1 */
void    ob_build_old_misc(u32 miscaddr);     /* prelude + entry table */
void  clear_gaps(void);
void  boot_kernel(u32 kernel_addr, u32 r6, u32 r7, u32 r8); /* no return */
void  ob_kernel_tail(u32 os, u32 r6, u32 r7, u32 r8);       /* vectors.s */
void  ob_install_traps(void);                              /* vectors.s */

/* display.c */
// Borrows a word-aligned loaded image; blacks the screen, then returns 0
// for an invalid header or pixel extent without disrupting the OS boot.
int display_image(const void *image_,
                  u32         image_size_,
                  const u8   *pattern_);

/* minidipir.c */
extern u32      g_component_load_addr;
extern BootData g_bootdata;
// ROM vector 0x20c: scalar result; info_ is untouched for supported tags.
u32 ob_query_rom_sysinfo(u32 tag_, void *info_, u32 info_size_);

// Boot and fault diagnostics on the Opera kprint channel.
void  kprint(const char *s);
void  khex8(u32 v);

/* vectors.s entry */
// Requests CLRDIP CPU/controller reset, then returns on the preserved caller stack.
// Called by resident MEI initialization with IRQ and FIQ masked.
void ob_dipir_reset(void);
void  dipir_c(void);

#endif
