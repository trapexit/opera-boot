; opera-boot dipir blob - ARM runtime divide helpers for the misc vtable
;
; Copyright (c) 2026 opera-boot project. GPL-2.0-only (see docs/LICENSE).
;
; rtdiv.s - ARM runtime divide helpers for the C&B-era misc vtable
; (slots 10-13).  The booted kernel and game code call these with the
; compiler's internal-helper convention, which differs from a plain C
; prototype: the FIRST argument is the divisor, the SECOND the dividend,
; the quotient returns in r0 and the remainder in r1.  That register
; pair cannot be produced by armcc-generated C, so these are hand
; written: classic shift-subtract, no early termination, 32 iterations.
;
; Division by zero yields quotient 0 and remainder = dividend (the same
; benign convention as util.c ob_udiv); nothing in the boot path divides
; by zero and games guard their own.
;
; Register discipline is full APCS: only r0-r3/ip are scratched, r4 is
; saved where used, lr preserved, so kernel context survives every call.

	AREA	|ASMCODE|, CODE, READONLY
	EXPORT	ob_rt_sdiv
	EXPORT	ob_rt_udiv
	EXPORT	ob_rt_sdiv10
	EXPORT	ob_rt_udiv10

; ----------------------------------------------------------------------
; _umag_core: unsigned magnitude divide.
; in:  r1 = divisor, r2 = dividend
; out: r0 = quotient, r1 = remainder
; clobbers ip, r3 only.
_umag_core
	mov	ip, r1			; divisor
	mov	r0, #0			; quotient
	mov	r1, #0			; remainder
	mov	r3, #32			; bit count
_uc_loop
	movs	r1, r1, LSL #1		; rem <<= 1 | next dividend bit
	orr	r1, r1, r2, LSR #31
	movs	r2, r2, LSL #1
	cmp	r1, ip			; carry set iff rem >= divisor
	subcs	r1, r1, ip
	adc	r0, r0, r0		; q = q<<1 | carry
	subs	r3, r3, #1
	bne	_uc_loop
	mov	pc, lr

; ----------------------------------------------------------------------
; u32 ob_rt_udiv(u32 d1 /*divisor*/, u32 d2 /*dividend*/)
ob_rt_udiv
	mov	r2, r1			; dividend
	mov	r1, r0			; divisor
	cmp	r1, #0
	beq	_uzero
	b	_umag_core
_uzero
	mov	r0, #0			; q = 0, r = dividend
	mov	r1, r2
	mov	pc, lr

; ----------------------------------------------------------------------
; u32 ob_rt_sdiv(u32 d1 /*divisor*/, u32 d2 /*dividend*/)
; C truncation semantics: quotient rounds toward zero, remainder takes
; the dividend's sign.
ob_rt_sdiv
	stmdb	sp!, {r4, lr}
	mov	r2, r1			; dividend
	mov	r1, r0			; divisor
	cmp	r1, #0
	beq	_szero
	eor	r4, r2, r0		; bit31 = quotient sign
	and	r4, r4, #0x80000000
	orr	r4, r4, r2, LSR #31	; bit0 = dividend/remainder sign
	teq	r2, #0
	rsbmi	r2, r2, #0		; |dividend|
	teq	r1, #0
	rsbmi	r1, r1, #0		; |divisor|
	bl	_umag_core
	teq	r4, #0			; N from bit31: quotient negative?
	rsbmi	r0, r0, #0
	tst	r4, #1			; dividend was negative?
	rsbne	r1, r1, #0
	ldmfd	sp!, {r4, pc}
_szero
	mov	r0, #0			; q = 0, r = original dividend
	mov	r1, r2
	ldmfd	sp!, {r4, pc}

; ----------------------------------------------------------------------
; u32 ob_rt_udiv10(u32 n): divide by ten, q -> r0, r -> r1.
ob_rt_udiv10
	mov	r2, r0			; dividend
	mov	r1, #10
	b	_umag_core

; ----------------------------------------------------------------------
; u32 ob_rt_sdiv10(u32 n): signed divide by ten.
ob_rt_sdiv10
	stmdb	sp!, {r4, lr}
	mov	r2, r0			; dividend
	and	r4, r2, #0x80000000	; sign for both results
	teq	r2, #0
	rsbmi	r2, r2, #0		; |n|
	mov	r1, #10
	bl	_umag_core
	tst	r4, #0x80000000
	rsbne	r0, r0, #0
	rsbne	r1, r1, #0
	ldmfd	sp!, {r4, pc}

	END
