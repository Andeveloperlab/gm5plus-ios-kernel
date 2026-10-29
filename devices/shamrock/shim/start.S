/*
 * sweet boot shim entry: arm64 Linux Image header so Qualcomm ABL treats
 * us like a kernel. ABL enters at EL1, MMU/D-cache off, x0 = DTB pointer.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB pointer from ABL */
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	bl	shim_main
3:	wfe
	b	3b

/* PSCI CPU_ON target. x0 = context id = cpu index (1..7). */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x0, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	bl	secondary_main
4:	wfe
	b	4b

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number
 * and jumps to the common path, which saves x0-x28 and calls
 * exc_handler(slot, regs). IRQs return; everything else ends in the
 * on-screen panic.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret

/*
 * Return stub from XNU early exception or orderly return.
 * Entered from loader_jump with MMU either on (mapped via TTBR0) or off.
 * Safely turn off MMU & caches before executing any C code or accessing MMIO/FB.
 */
	.globl shim_return_stub
shim_return_stub:
	msr	daifset, #0xf
	msr	spsel, #1

	/* Turn off MMU & caches safely */
	mrs	x9, sctlr_el1
	tbz	x9, #0, 1f		/* if MMU already off, skip */
	bic	x9, x9, #(1 << 0)	/* M = 0 (disable MMU) */
	bic	x9, x9, #(1 << 2)	/* C = 0 (disable D-cache) */
	bic	x9, x9, #(1 << 12)	/* I = 0 (disable I-cache) */
	dsb	sy
	msr	sctlr_el1, x9
	isb
	tlbi	vmalle1
	dsb	sy
	isb
1:
	adr	x9, _stack_top
	mov	sp, x9
	adr	x9, vectors
	msr	vbar_el1, x9
	isb
	/*
	 * Arguments from XNU:
	 *   x0 = log PA
	 *   x1 = log len
	 *   x2 = reason (1 = early exception, 0 = orderly, 2 = checkpoint)
	 *   x3 = ESR_EL1
	 *   x4 = ELR_EL1
	 *   x5 = FAR_EL1
	 *   x6 = LR
	 *   x7 = SP
	 */
	mov	x9, x0		/* log PA */
	mov	x10, x1		/* log len */
	mov	x0, x2		/* arg0: reason */
	mov	x1, x3		/* arg1: esr */
	mov	x2, x4		/* arg2: elr */
	mov	x3, x5		/* arg3: far */
	mov	x4, x9		/* arg4: log_pa */
	mov	x5, x10		/* arg5: log_len */
	bl	shim_on_xnu_return
1:	wfe
	b	1b

