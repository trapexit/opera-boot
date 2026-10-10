; opera-boot (GPL-2.0-only): clean-room ROM RomTag table (subsystem 0x10).
;
; Linked at 0x03001000 (LAYOUT_ROMTAGS), published via [0x2C].  The
; clean-room dipir never reads this table; everything here is for
; software running after boot that walks ROM RomTags (FindSysRomTag).
;
; KEPT:  ROM_DIPIR      -> resident dipir blob (so runtime ROM-probing
;                          code sees a dipir in ROM, RAM-loaded at 0x200)
;        ROM_KERNEL_CD  -> zero-version stub: FindSysRomTag cannot
;                          express "absent" safely; version 0.0 is what
;                          "no OS in ROM" means to the version check.
; No ROM_DIPIR_DRIVERS tag: the public ROM has no driver pack.

	GET	romtags_defs.i

ROMTAGS_FILEOFF	EQU	&1000
; DIPRBIN_FILEOFF comes from generated romtags_defs.i.
FAKEHDR_FILEOFF	EQU	&1F000

	AREA	|ASMCODE|, CODE, READONLY

launch_romtags
; ---- ROM_DIPIR (0x20) -------------------------------------------------
	DCB	&10		; RT_SUBSYS_ROM
	DCB	&20		; ROM_DIPIR
	DCB	0		; rt_Version
	DCB	0		; rt_Revision
	DCB	0, 0, 0, 0
	DCD	DIPRBIN_FILEOFF - ROMTAGS_FILEOFF
	DCD	DIPR_SIZE
	DCD	&200		; rt_Reserved3[0] = RAM download address
	DCD	0, 0, 0

; ---- ROM_KERNEL_CD (0x31): zero-version stub ---------------------------
	DCB	&10
	DCB	&31		; ROM_KERNEL_CD
	DCB	0
	DCB	0
	DCB	0, 0, 0, 0
	DCD	FAKEHDR_FILEOFF - ROMTAGS_FILEOFF
	DCD	256
	DCD	0, 0, 0, 0

; ---- terminator --------------------------------------------------------
	DCD	0
	END
