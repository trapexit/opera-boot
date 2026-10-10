; opera-boot clean-room launch ROM
;
; Copyright (c) 2026 opera-boot project. GPL-2.0-only (see docs/LICENSE).
;
; Reset starts at PC 0 with ROM aliased over DRAM.  Transfer immediately
; to the permanent ROM window at 0x03000000 before any DRAM write.
;
; Contract with the resident clean-room dipir blob:
;   [0x28] = current kernel AIF image pointer (fake zero header until boot)
;   [0x2C] = ROM RomTag table address
;   [0x30] = warm-reset entry (launch_start)
;   [0x34] = sentinel 0xB00710AD ("BOOTLOAD")
;   [0x38] = timing family: 0 NTSC, 1 PAL
;   [0x3C] = reserved scratch word used to disable the reset ROM overlay
;   [0x40] = pending resident DIPIR-reset continuation (0 when idle)
;   dipir blob resident at DRAM 0x200; region 0x200..0x10000 kept intact
;   across dipir calls (dipir.s entry contract); launch zeroes the whole
;   span so resident BSS, the private stack, and other low-memory state
;   start clean independently of prior RAM contents.
;
; [0x30] is the warm-reset entry used by the staged kernel.  Audio/photo
; discs are outside this ROM's CD-OS boot contract.

SENTINEL	EQU	0xB00710AD
OVERLAY_SCRATCH_ADDR EQU	0x3C	; safe first DRAM write on cold/warm entry
MAGIC_DIPIR_RESUME EQU	0x40	; matches opera-boot.h and vectors.s
CLIO_CSTAT_ADDR	EQU	0x03400028
CSTAT_CLRDIP	EQU	0x20
CSTAT_DIPIR_RESET EQU	0x40	; reset reason, not the SOFTRES request bit
BANNER_LINES	EQU	3150		; ~200ms at ~63.6us/line
VCNT_MASK	EQU	0x7FF
NTSC_LINE_LIMIT	EQU	264		; NTSC never reaches this scan line
VIDEO_POLL_LIMIT EQU	0x100000	; finite guard for a stopped VCNT
BOOT_DISPTYPE_NTSC EQU	0		; matches opera-boot.h
BOOT_DISPTYPE_PAL EQU	1

	GET	launch_defs.i		; static layout constants

	IMPORT	BannerLogo

	AREA	|ASMCODE|, CODE, READONLY
	ENTRY

	; Absolute transfer also works when this instruction runs at PC 0.
	; The adjacent literal contains the linked high-ROM launch_start.
	ldr	pc, [pc, #-4]
	DCD	launch_start

launch_start
	; SVC mode, IRQ+FIQ masked
	mov	r1, #0xD3
	msr	CPSR_c, r1

	; Execute from high ROM before exposing retained DRAM.  No stack,
	; vectors, contract cells, or reboot arguments r6-r8 are touched.
	mov	r0, #OVERLAY_SCRATCH_ADDR
	mov	r1, #0
	str	r1, [r0]			; disable reset ROM overlay

	; Warm-vs-cold check BEFORE touching r6-r8
	mov	r0, #0x34
	ldr	r2, [r0]
	ldr	r3, =SENTINEL
	cmp	r2, r3
	bne	cold_boot

	; A resident media-ack reset resumes its interrupted C invocation even
	; with a staged OS.  Sentinel + hardware reset reason guard the cell.
	; No stack, timing, vectors, tables, or kernel state are changed here.
	ldr	r0, =CLIO_CSTAT_ADDR
	ldr	r1, [r0]
	tst	r1, #CSTAT_DIPIR_RESET
	beq	check_staged_kernel
	mov	r0, #MAGIC_DIPIR_RESUME
	ldr	r2, [r0]
	cmp	r2, #0
	beq	check_staged_kernel
	movs	pc, r2				; restore mode/masks/flags from SPSR_svc
check_staged_kernel
	mov	r0, #0x28
	ldr	r4, [r0]			; staged OS image (or fake hdr)
	ldr	r3, =FAKEHDR_ADDR
	cmp	r4, r3
	beq	cold_boot

	; Warm: an OS is staged.  SVC stack from MSYS, then re-enter.
	; NOTE: only the SVC stack is re-armed here; the banked
	; FIQ/IRQ/ABT/UND tops are whatever the dying kernel left.
	; Assumed dead: the staged kernel re-installs its own stacks on
	; entry (v24 startup.s does; the cold-path arming exists for
	; v0.16's early run).
	bl	read_dram_size			; r9 = dram size
	mov	sp, r9
	sub	sp, sp, #16
	mov	pc, r4				; r6/r7/r8 from REBOOT caller

cold_boot
	; kprint the CLIO reset reason like the retail launch does.
	ldr	r1, =CLIO_CSTAT_ADDR
	ldr	r0, [r1]
	ldr	r2, =0x03300000
	str	r0, [r2]			; opera_kprint channel
	mov	r0, #CSTAT_CLRDIP		; clear reset-reason bit without reset
	str	r0, [r1]

	; Zero DRAM 0x0000..0x10000, including resident BSS, stack, and
	; contract cells, before copying the blob.  VRAM is not zeroed:
	; the clean-room dipir installs its startup VDL before painting
	; or loading a splash.
	mov	r0, #0
	mov	r1, #0
	mov	r2, #0
	mov	r3, #0
	mov	r12, #0
	mov	r4, #0x10000
zero_dram
	stmia	r0!, {r1-r3, r12}
	cmp	r0, r4
	blt	zero_dram

	; Install DRAM vectors 0x00..0x1F:
	; reset/undef/pabt/dabt/addr26 -> self loop, SWI -> movs pc,lr,
	; IRQ/FIQ -> subs pc,lr,#4
	mov	r0, #0
	ldr	r1, =0xEAFFFFFE			; b .
	ldr	r2, =0xE1B0F00E			; movs pc,lr
	ldr	r3, =0xE24EF004			; subs pc,lr,#4
	str	r1, [r0], #4			; 0x00 reset
	str	r1, [r0], #4			; 0x04 undef
	str	r2, [r0], #4			; 0x08 SWI
	str	r1, [r0], #4			; 0x0C prefetch abort
	str	r1, [r0], #4			; 0x10 data abort
	str	r1, [r0], #4			; 0x14 addr26
	str	r3, [r0], #4			; 0x18 IRQ
	str	r3, [r0], #4			; 0x1C FIQ

	; Copy resident dipir blob ROM -> DRAM 0x200
	mov	r1, #0x200
	ldr	r2, =DIPIR_BLOB_ADDR
	ldr	r3, =DIPIR_BLOB_LEN
copy_blob
	ldr	r0, [r2], #4
	str	r0, [r1], #4
	subs	r3, r3, #4
	bgt	copy_blob

	; Magic cells
	mov	r0, #0x28
	ldr	r1, =FAKEHDR_ADDR
	str	r1, [r0]			; current-kernel pointer
	mov	r0, #0x2C
	ldr	r1, =ROMTAGS_ADDR
	str	r1, [r0]			; ROM RomTag table
	mov	r0, #0x30
	ldr	r1, =launch_start
	str	r1, [r0]			; warm-reset entry
	mov	r0, #0x34
	ldr	r1, =SENTINEL
	str	r1, [r0]			; sentinel
	bl	detect_video_timing
	mov	r0, #0x38
	str	r1, [r0]			; cached for scalar sysinfo queries

	; SVC stack at DRAM top - 16
	bl	read_dram_size			; r9 = dram size
	mov	sp, r9
	sub	sp, sp, #16

	; The startup.s contract: "on entry all nonuser stacks have been
	; set up by the launch rom... to the end of the memory space".
	; v24 Sherry re-installs them below the kernel image, but TE's
	; OS v0.16 (1993) still runs on these pointers long enough to
	; matter.  Give FIQ/IRQ/ABT/UND valid tops just below SVC's.
	mrs	r1, CPSR
	mov	r5, r9
	sub	r5, r5, #0x10000		; r5 = dram top - 64KB
	; FIQ (mode 0x11)
	bic	r0, r1, #0x0F
	orr	r0, r0, #0x11
	msr	CPSR_c, r0
	mov	sp, r5
	; IRQ (mode 0x12)
	bic	r0, r1, #0x0F
	orr	r0, r0, #0x12
	msr	CPSR_c, r0
	sub	r5, r5, #0x1000
	mov	sp, r5
	; SVC (mode 0x13)
	bic	r0, r1, #0x0F
	orr	r0, r0, #0x13
	msr	CPSR_c, r0
	sub	r5, r5, #0x1000
	mov	sp, r5
	; ABT (mode 0x17)
	bic	r0, r1, #0x0F
	orr	r0, r0, #0x17
	msr	CPSR_c, r0
	sub	r5, r5, #0x1000
	mov	sp, r5
	; UND (mode 0x1B)
	bic	r0, r1, #0x0F
	orr	r0, r0, #0x1B
	msr	CPSR_c, r0
	sub	r5, r5, #0x1000
	mov	sp, r5
	; back to SVC
	msr	CPSR_c, r1
	mov	sp, r9
	sub	sp, sp, #16

	; dipir() turns video on (VSCTXEN|CLUTXEN) mid-pass, restoring the
	; "static screen" - paint the banner first so it shows at once.
	bl	paint_banner

retry_loop
	mov	lr, pc				; return point after next insn
	ldr	pc, =0x200			; dipir vector 0 = b dipir_start
	; Control returns here only if no kernel was staged.  The banner was
	; painted once before the first pass; retry paths do not touch VRAM,
	; so repainting would only introduce visible tearing.
	bl	wait_200ms
	b	retry_loop

; ----------------------------------------------------------------------
; read_dram_size: r9 = DRAM size in bytes (dipirutils.c FindMemSize math)
; stock 2MB machine: MSYS=0x29 -> 1MB(bank0) + 1MB(bank1).
; Clobbers r0,r1,r2; result in r9.
read_dram_size
	ldr	r0, =0x03300004			; MSYSBits
	ldr	r0, [r0]
	mov	r1, r0, LSR #4			; >> (SET0SHIFT-1)
	and	r1, r1, #6			; & (SETMASK<<1)
	mov	r2, #1
	mov	r1, r2, LSL r1
	mov	r9, r1, LSR #2			; bank0 megabytes
	mov	r1, r0, LSR #2			; >> (SET1SHIFT-1)
	and	r1, r1, #6
	mov	r1, r2, LSL r1
	add	r9, r9, r1, LSR #2		; + bank1 megabytes
	mov	r9, r9, LSL #20			; * 1MB
	mov	pc, lr

; ----------------------------------------------------------------------
; detect_video_timing: r1 = timing family; stopped VCNT falls back to NTSC.
; CLIO VCNT[10:0] is available on FZ-1; the ANVIL video-mode straps are not.
; PAL1 and PAL2 share 312-line timing; NTSC has at most 263 lines.
; Two wraps cover a complete field even if entry happens near its end.
; Read-only, no timers or video registers changed; cold launch only, so
; runtime dipir and warm kernel re-entry reuse the result without polling.
; Clobbers r0-r4,r12.  No stack needed at this stage of cold launch.
detect_video_timing
	ldr	r0, =0x03400034
	ldr	r3, =VCNT_MASK
	ldr	r2, =VIDEO_POLL_LIMIT
	ldr	r1, [r0]
	and	r1, r1, r3
	mov	r4, #2
detect_video_loop
	ldr	r12, [r0]
	and	r12, r12, r3
	cmp	r12, #NTSC_LINE_LIMIT
	bhs	detect_video_pal
	cmp	r12, r1
	bhs	detect_video_next
	subs	r4, r4, #1
	beq	detect_video_ntsc
detect_video_next
	mov	r1, r12
	subs	r2, r2, #1
	bne	detect_video_loop
	b	detect_video_ntsc	; retain the conservative cold-boot default
detect_video_pal
	mov	r1, #BOOT_DISPTYPE_PAL
	mov	pc, lr
detect_video_ntsc
	mov	r1, #BOOT_DISPTYPE_NTSC
	mov	pc, lr

; ----------------------------------------------------------------------
; paint_banner: original no-disc screen from src/banner.c.
; Fills the framebuffer at dramSize+vramSize-256K == 0x2C0000 on stock
; hardware, which is exactly what the startup VDL scans out.
paint_banner
	stmfd	sp!, {r0, lr}
	bl	BannerLogo
	ldmfd	sp!, {r0, pc}

; ----------------------------------------------------------------------
; wait_200ms: poll CLIO VCNT (0x03400034) for ~200ms of scanline ticks.
wait_200ms
	ldr	r0, =0x03400034
	ldr	r3, =VCNT_MASK
	ldr	r1, [r0]
	and	r1, r1, r3
	ldr	r2, =BANNER_LINES
wait_200ms_loop
	ldr	r12, [r0]
	and	r12, r12, r3
	cmp	r12, r1
	beq	wait_200ms_loop
	mov	r1, r12
	subs	r2, r2, #1
	bne	wait_200ms_loop
	mov	pc, lr

	END
