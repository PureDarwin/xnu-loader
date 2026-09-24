/* arm64 Linux Image entry (Documentation/arch/arm64/booting.rst): x0 = FDT,
 * MMU off, at EL2 or EL1, anywhere 2 MiB-aligned. Drops to EL1, relocates the
 * position-independent image, then kernel_arm64_main builds an identity map. */
.section .text.linux_entry, "ax"
.globl _start
_start:
  b primary_entry                 /* code0 */
  .long 0                         /* code1 */
  .quad 0                         /* text_offset */
  .quad __image_size              /* image_size, including .bss */
  .quad 0xa                       /* little-endian, 4K pages, place anywhere */
  .quad 0
  .quad 0
  .quad 0
  .ascii "ARM\x64"                /* magic */
  .long 0

primary_entry:
  mov x19, x0
  msr daifset, #0xf
  mrs x0, CurrentEL
  cmp x0, #(2 << 2)
  b.ne at_el1

  /* EL2: plain AArch64 EL1 below, timers and GICv3 system registers usable there */
  mov x0, #(1 << 31)
  msr hcr_el2, x0
  mrs x0, cnthctl_el2
  orr x0, x0, #3
  msr cnthctl_el2, x0
  msr cntvoff_el2, xzr
  mrs x0, id_aa64pfr0_el1
  ubfx x0, x0, #24, #4
  cbz x0, 1f
  mrs x0, S3_4_C12_C9_5           /* ICC_SRE_EL2 */
  orr x0, x0, #0x1                /* SRE */
  orr x0, x0, #0x8                /* Enable */
  msr S3_4_C12_C9_5, x0
  isb
1:
  mrs x0, midr_el1
  msr vpidr_el2, x0
  mrs x0, mpidr_el1
  msr vmpidr_el2, x0
  mov x0, #0x33ff
  msr cptr_el2, x0
  msr hstr_el2, xzr
  ldr x0, =0x30d00800             /* SCTLR_EL1 RES1 bits, MMU and caches off */
  msr sctlr_el1, x0
  mov x0, #0x3c5                  /* EL1h, DAIF masked */
  msr spsr_el2, x0
  adr x0, at_el1
  msr elr_el2, x0
  eret

at_el1:
  mrs x0, sctlr_el1
  bic x0, x0, #1
  bic x0, x0, #(1 << 2)
  msr sctlr_el1, x0
  isb
  mov x0, #(3 << 20)              /* CPACR_EL1.FPEN: the loader may use FP */
  msr cpacr_el1, x0
  isb
  adrp x0, __stack_top
  add x0, x0, :lo12:__stack_top
  mov sp, x0

  adrp x0, __bss_start
  add x0, x0, :lo12:__bss_start
  adrp x1, __bss_end
  add x1, x1, :lo12:__bss_end
2:
  cmp x0, x1
  b.hs 3f
  str xzr, [x0], #8
  b 2b
3:
  /* Linked at 0: every R_AARCH64_RELATIVE slot gets the load address added */
  adr x0, _start
  adrp x1, __rela_start
  add x1, x1, :lo12:__rela_start
  adrp x2, __rela_end
  add x2, x2, :lo12:__rela_end
4:
  cmp x1, x2
  b.hs 5f
  ldp x3, x4, [x1], #16
  ldr x5, [x1], #8
  cmp w4, #1027
  b.ne 4b
  add x5, x5, x0
  str x5, [x0, x3]
  b 4b
5:
  mov x0, x19
  bl kernel_arm64_main
6:
  wfi
  b 6b

.ltorg
