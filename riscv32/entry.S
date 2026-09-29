/* riscv linux Image header (Documentation/arch/riscv/boot-image-header.rst): OpenSBI
 * enters in s-mode, paging off, a0 = hart id, a1 = FDT. Linked where qemu and booti
 * put an rv32 Image, RAM base plus 4 MB, so nothing needs relocating */
	.section .text.head, "ax"
	.globl _start
_start:
	j	1f			/* code0 */
	.word	0			/* code1 */
	.dword	0x400000		/* text_offset */
	.dword	__image_size		/* image_size */
	.dword	0			/* flags: little-endian */
	.word	2			/* version 0.2 */
	.word	0
	.dword	0
	.ascii	"RISCV\0\0\0"		/* magic */
	.ascii	"RSC\x05"		/* magic2 */
	.word	0
1:
	csrw	sie, zero
	csrw	sip, zero
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
	call	boot_rv32_main
4:	wfi
	j	4b

/* void boot_rv32_jump(uint32_t entry, uint32_t boot_args, uint32_t hartid): paging off */
	.text
	.globl boot_rv32_jump
boot_rv32_jump:
	mv	t0, a0
	mv	a0, a1
	mv	a1, a2
	fence.i
	jr	t0
