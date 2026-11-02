@ CPU1 probe stub for IRAM0 at 0x800 (SMP step 0, see vendor-ref/research/smp-report.md 5.1):
@ write MPIDR, CPSR and NSACR to the mailbox at 0x840, then sleep in WFI.
	.arm
	.text
	ldr	r0, =0x840
	mrc	p15, 0, r1, c0, c0, 5	@ MPIDR
	str	r1, [r0]
	mrs	r2, cpsr
	str	r2, [r0, #4]
	mrc	p15, 0, r3, c1, c1, 2	@ NSACR
	str	r3, [r0, #8]
	dsb	sy
1:	wfi
	b	1b
	.ltorg
