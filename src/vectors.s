; opera-boot (GPL-2.0-only): clean-room dipir vector table + entry shim.
;
; Resident at DRAM 0x200 (copied there by the launch ROM).  Slot layout
; is the immutable ABI consumed by the kernel and the launch ROM.
;
;   [0x200] +00  b dipir_entry          full boot attempt (cold + runtime)
;   [0x204] +04  b stub_rsa_ok          RSACheck      -> 1 (no crypto)
;   [0x208] +08  b stub_rsa_ok          GenRSACheck   -> 1
;   [0x20c] +0c  b ob_query_rom_sysinfo  QueryROMSysInfo -> display timing or -2
;   [0x210] +10  b stub_sysinfo_unsupported  SetROMSysInfo   -> -2
;   [0x214] +14  b stub_zero            SectorECC       -> 0
;   [0x218] +18  DCD 11                 table entry count
;   [0x21c] +1c  b stub_zero            OpenRomFile   -> NULL
;   [0x220] +20  b stub_zero            ReadRomFile   -> 0
;   [0x224] +24  b stub_zero            CloseRomFile  -> 0
;   [0x228] +28  b get_disc_os_version  from staged kernel header

; Reset continuation ABI shared with launch.s and opera-boot.h.
MAGIC_DIPIR_RESUME EQU	0x40
CLIO_CSTAT_ADDR	EQU	0x03400028
CLIO_ADBIO_ADDR	EQU	0x03400084
ADBIO_ROM2_SELECT EQU	0x04
CSTAT_SOFTRES	EQU	0x10
CSTAT_CLRDIP	EQU	0x20

	IMPORT	dipir_c
	IMPORT	stub_rsa_ok
	IMPORT	stub_zero
	IMPORT	stub_sysinfo_unsupported
	IMPORT	ob_query_rom_sysinfo
	IMPORT	get_disc_os_version
	IMPORT	ob_trap_c
	IMPORT	g_dabt_pad

	AREA	|ASMCODE|, CODE, READONLY
	ENTRY

vec_table
	b	dipir_entry		; 0x200
	b	stub_rsa_ok		; 0x204
	b	stub_rsa_ok		; 0x208
	b	ob_query_rom_sysinfo	; 0x20c QueryROMSysInfo
	b	stub_sysinfo_unsupported	; 0x210 SetROMSysInfo
	b	stub_zero		; 0x214
	DCD	11			; 0x218
	b	stub_zero		; 0x21c
	b	stub_zero		; 0x220
	b	stub_zero		; 0x224
	b	get_disc_os_version	; 0x228

; ----------------------------------------------------------------------
; Full boot attempt.  Called with IRQ/FIQ masked, caller lr = return
; point.  Runs C on our own stack so the caller's stack stays intact.
dipir_entry
	ldr	r3, =saved_sp
	stmia	r3, {sp, lr}
	ldr	sp, =stack_top
	mov	fp, #0
	bl	dipir_c
	ldr	r3, =saved_sp
	ldmia	r3, {sp, lr}
	mov	pc, lr

; ----------------------------------------------------------------------
; ob_dipir_reset: acknowledge CURRENT through the CPU/controller reset.
; Caller is resident C with IRQ/FIQ masked.  The core preserves its stack
; and r6-r11, captures the pre-reset CPSR in SPSR_svc, and replaces LR_svc.
; Save every register touched by the stackless launch continuation check,
; plus the original return address, before publishing.
	EXPORT	ob_dipir_reset
ob_dipir_reset
	stmdb	sp!, {r0-r5, r12, lr}
	; A running OS can leave the font ROM selected.  Reset must fetch
	; the boot ROM, then restore the caller's selection on resumption.
	ldr	r2, =CLIO_ADBIO_ADDR
	ldr	r3, [r2]
	stmdb	sp!, {r3}
	bic	r3, r3, #ADBIO_ROM2_SELECT
	str	r3, [r2]
	adr	r0, dipir_reset_resume
	mov	r1, #MAGIC_DIPIR_RESUME
	str	r0, [r1]
	ldr	r0, =CLIO_CSTAT_ADDR
	mov	r1, #(CSTAT_SOFTRES :OR: CSTAT_CLRDIP)
	str	r1, [r0]			; only the controller, not the drive
dipir_reset_resume
	; launch_start restores CPSR from SPSR_svc before branching here, so
	; SP names the saved caller bank again without touching resident state.
	; Consume the pending cell while masked; return without changing flags.
	mov	r0, #MAGIC_DIPIR_RESUME
	mov	r1, #0
	str	r1, [r0]
	ldmia	sp!, {r3}
	ldr	r2, =CLIO_ADBIO_ADDR
	str	r3, [r2]
	ldmia	sp!, {r0-r5, r12, lr}
	mov	pc, lr


; ----------------------------------------------------------------------
; ob_kernel_tail(os_image, r6, r7, r8)
; Final boot handoff: set the kernel argument registers and jump to the
; staged kernel image.  Never returns.
	EXPORT	ob_kernel_tail
ob_kernel_tail
	mov	r6, r1
	mov	r7, r2
	mov	r8, r3
	mov	pc, r0

; ----------------------------------------------------------------------
; Data-abort net (vendor cdipir parity: dipir.s dataabort + setjmp).
;
; During a boot attempt vector 0x10 points at ob_dabt_net (installed by
; ob_arm_dabt_net).  A dabt mid-transaction means the drive may hold
; half a command, so the recovery is: report nothing, longjmp back to
; the pad armed in dipir_c, which POLL-RESETs the bus and returns to
; the banner/retry loop.  With g_dabt_pad == 0 (outside the attempt)
; the fault is fatal and falls through to the kprint trap.
;
; Stack discipline mirrors the documented handler ABI: none until the
; pad's saved SVC stack is in place.  Probe reads in this blob are never
; deliberately retried after a fault.

	EXPORT	ob_setjmp
ob_setjmp			; r0 = ob_jmpbuf; fills then arms, returns 0
	stmia	r0, {r4-r11, sp, lr}
	ldr	r1, =g_dabt_pad
	str	r0, [r1]
	mov	r0, #0
	mov	pc, lr

	EXPORT	ob_longjmp
ob_longjmp			; r0 = ob_jmpbuf, r1 = nonzero return val
	ldmia	r0, {r4-r11, sp, lr}
	mov	r0, r1
	mov	pc, lr

	EXPORT	ob_dabt_net
ob_dabt_net
	ldr	r2, =g_dabt_pad
	ldr	r2, [r2]
	cmp	r2, #0
	beq	ob_trap_dabt		; disarmed: report lr + spin
	; Read + clear the MADAM abort register (0330'0020), as the
	; vendor dataabort does, so the latch is clean for the retry.
	mov	r0, #0x03300000
	ldr	r1, [r0, #0x20]
	mov	r1, #0
	str	r1, [r0, #0x20]
	; ABT -> SVC (keep the entry-time I/F masks, as vendor).
	mrs	r0, CPSR
	bic	r0, r0, #7
	orr	r0, r0, #3
	msr	CPSR_c, r0
	mov	r0, r2
	mov	r1, #1
	b	ob_longjmp

; ----------------------------------------------------------------------
; Fault traps.  The encodings are written by ob_install_traps (C) since
; armasm cannot fold the address arithmetic; C sees link-time constants.
	EXPORT	ob_trap_undef
	EXPORT	ob_trap_pabt
	EXPORT	ob_trap_dabt
	EXPORT	ob_trap_addr

ob_trap_undef
	mov	r0, #1
	b	ob_trap
ob_trap_addr
	mov	r0, #2
	b	ob_trap
ob_trap_pabt
	mov	r0, #3
	b	ob_trap
ob_trap_dabt
	mov	r0, #4
	b	ob_trap
ob_trap
	mov	r1, lr
	bl	ob_trap_c		; void ob_trap_c(u32 which, u32 lr)
spin_forever
	b	spin_forever


	ALIGN

; ----------------------------------------------------------------------
	AREA	|MINIDATA|, DATA, NOINIT
saved_sp	% 4
saved_lr	% 4
		% 2048			; private dipir stack
stack_top				; (stack grows down from here)

	END
