/* xnu shim for the esp32-p4: the rom (or the emulator standing in for it) enters here in
 * m-mode on hart 0 from hp sram, interrupts off. The device tree is built in */
	.section .text.head, "ax"
	.globl _start
_start:
	.option push
	.option norelax
	la	gp, __global_pointer$
	.option pop
	la	sp, __stack_top
	la	t0, __bss_start
	la	t1, __bss_end
2:	bgeu	t0, t1, 3f
	sw	zero, 0(t0)
	addi	t0, t0, 4
	j	2b
3:
	call	boot_p4_main
4:	wfi
	j	4b

/* void boot_rv32_jump(uint32_t entry, uint32_t boot_args, uint32_t hartid): m-mode, no mmu */
	.text
	.globl boot_rv32_jump
boot_rv32_jump:
	mv	t0, a0
	mv	a0, a1
	mv	a1, a2
	fence.i
	jr	t0

	.section .rodata
	.balign 8
	.globl p4_dtb
p4_dtb:
	.incbin "esp32p4.dtb"
