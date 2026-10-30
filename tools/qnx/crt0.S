@ Process entry for QNX Neutrino 6.3 on ARM, same steps as QNX's crt1
@ (taken from the disassembly of the 900's own programs):
@ ldqnx.so.2 passes an exit function in r0, argc/argv/envp/auxv on the stack.
@ We call _init_libc(argc, argv, envp, auxv, exitfunc), then exit(main(...)).
	.text
	.arm
	.global	_start
	.type	_start, %function
_start:
	mov	r4, r0			@ exit function from the loader
	ldr	r0, [sp]		@ argc
	add	r1, sp, #4		@ argv
	add	r2, r1, r0, lsl #2
	add	r2, r2, #4		@ envp
	mov	r3, r2
1:	ldr	ip, [r3], #4		@ skip envp up to its NULL -> auxv
	teq	ip, #0
	bne	1b
	str	r4, [sp, #-4]!		@ 5th argument on the stack
	mov	r4, r0
	mov	r5, r1
	mov	r6, r2
	bl	_init_libc
	add	sp, sp, #4
	mov	r0, r4
	mov	r1, r5
	mov	r2, r6
	bl	main
	bl	exit
2:	b	2b
	.size	_start, . - _start
