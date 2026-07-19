#if defined(__aarch64__)

/*
 * AArch64 kernel handoff, derived directly from XNU's own
 * entry convention in osfmk/arm64/start.s:
 *   EXT(_start): b start_first_cpu
 *   start_first_cpu:
 *       mov x20, x0                    // boot_args ptr, everything else
 *       ldr x22, [x20, BA_VIRT_BASE]    // (virt/phys base, mem size, boot
 *       ldr x23, [x20, BA_PHYS_BASE]    // flags, page tables) is derived
 *       ...                            // FROM the boot_args struct and
 *                                       // symbols baked into the kernel
 *                                       // image itself.
 *
 * So the handoff contract is: x0 = physical address of boot_args, branch
 * to the kernel's _start, AT EL1. That last part turned out to matter in
 * practice: under QEMU's raspi3b + u-boot, the firmware runs at EL2 (not
 * EL1) the whole time - confirmed by hitting ESR_EL2 == a translation
 * fault (data abort) immediately after jumping to the kernel entry with
 * only SCTLR_EL1 touched, landing back in u-boot's own EL2 exception
 * vectors. Real Apple Silicon iBoot always drops to EL1 before jumping to
 * XNU (start_first_cpu's own register/MMU setup is written assuming EL1),
 * so an earlier version of this code that only cleared SCTLR_EL1.M/C/I
 * did nothing useful when actually running at EL2: EL2's own MMU
 * (SCTLR_EL2.M) was still on and still faulting.
 *
 * Fixed by checking CurrentEL and, if at EL2, doing the standard ARM
 * bootloader EL2->EL1 drop (HCR_EL2 configured for a normal 64-bit EL1,
 * SPSR_EL2/ELR_EL2 set up, then `eret`) instead of a plain branch. If
 * already at EL1 (e.g. a firmware that already dropped down, or future
 * real hardware), falls through to the original plain-branch path.
 */
.text
.globl jump_to_xnu
.type jump_to_xnu, @function

jump_to_xnu:
    /* AAPCS64: x0=entry, x1=boot_args_phys, x2=stack_top. */
    mov x9, x0      /* stash entry (branch target) - x0 is about to become boot_args */
    mov x10, x1     /* stash boot_args_phys the same way, ahead of clobbering x1 below */
    mov x11, x2     /* stash stack_top */

    /* Mask all exceptions before touching system control state -
     * start_first_cpu does this itself too (DAIFSet, #0xF), but we
     * don't want an interrupt landing us back in UEFI/firmware code
     * mid-transition. */
    msr daifset, #0xf

    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.ne 1f

    /*
     * At EL2: configure HCR_EL2 for a plain 64-bit, non-virtualized EL1
     * (RW=1 selects AArch64 EL1; everything else off - no VM, no traps),
     * disable EL2's own MMU/caches (same reasoning as the EL1 case below:
     * identity-mapped, so turning it off doesn't move our own PC), then
     * ERET into EL1 at the kernel's entry point. ERET (unlike a plain
     * branch) is what actually performs the exception-level change -
     * SPSR_EL2/ELR_EL2 tell it "return to EL1h resuming at this address,
     * with these interrupt masks."
     */
    mov x0, #(1 << 31) /* HCR_EL2.RW = 1 (EL1 is AArch64) */
    msr hcr_el2, x0

    mrs x0, sctlr_el2
    bic x0, x0, #1         /* M  */
    bic x0, x0, #(1 << 2)  /* C  */
    bic x0, x0, #(1 << 12) /* I  */
    msr sctlr_el2, x0
    isb

    /* Give EL1 a clean (MMU/cache off) starting SCTLR_EL1 too, matching
     * what start_first_cpu assumes - EL1 has never run yet on this path
     * (u-boot ran entirely at EL2), so its reset-state assumption may not
     * hold otherwise. */
    msr sctlr_el1, xzr
    isb

    mov x0, #0x3c5      /* SPSR: EL1h, all interrupts masked (DAIF=1111) */
    msr spsr_el2, x0
    msr elr_el2, x9
    mov sp, x11
    mov x0, x10         /* boot_args_phys, in the register XNU's entry expects it in */
    eret

1:
    /* Already at EL1 (or lower - shouldn't happen for a bootloader, but
     * there's nothing more privileged left to drop from either way):
     * same MMU/cache teardown as before, plain branch. */
    mrs x1, sctlr_el1
    bic x1, x1, #1        /* M  */
    bic x1, x1, #(1 << 2) /* C  */
    bic x1, x1, #(1 << 12) /* I */
    msr sctlr_el1, x1
    isb

    mov sp, x11
    mov x0, x10     /* boot_args_phys, in the register XNU's entry expects it in */
    br x9

/*
 * Entry point for secondary cores (1-3), released via the BCM2837
 * spin-table mechanism (see main.c's post-jump_to_xnu-adjacent release
 * code): a core sitting at its reset-vector spin loop reads a nonzero
 * value from its release-address slot and branches to it directly, with
 * NO register setup at all - unlike jump_to_xnu above, there's no
 * boot_args/entry/stack_top handed to us in x0-x2. Whatever XNU's real
 * entry point and boot_args physical address are, they have to come from
 * fixed memory locations main.c fills in before releasing the cores
 * (secondary_entry_target below), since there's nowhere else to get them
 * from.
 *
 * Each secondary core independently needs the SAME EL2->EL1 drop the
 * boot CPU gets in jump_to_xnu above - real Raspberry Pi firmware never
 * drops secondary cores out of EL2 for you, and XNU's own reset_vector
 * (osfmk/arm64/start.s) expects to already be at EL1 on every core, not
 * just the first one.
 */
.globl secondary_entry_trampoline
.type secondary_entry_trampoline, @function
secondary_entry_trampoline:
    msr daifset, #0xf

    adrp x9, secondary_entry_target
    add x9, x9, :lo12:secondary_entry_target
    ldr x9, [x9]        /* phys_entry */
    adrp x10, secondary_bootargs_phys
    add x10, x10, :lo12:secondary_bootargs_phys
    ldr x10, [x10]      /* boot_args_phys */

    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.ne 1f

    mov x0, #(1 << 31)
    msr hcr_el2, x0

    mrs x0, sctlr_el2
    bic x0, x0, #1
    bic x0, x0, #(1 << 2)
    bic x0, x0, #(1 << 12)
    msr sctlr_el2, x0
    isb

    msr sctlr_el1, xzr
    isb

    mov x0, #0x3c5
    msr spsr_el2, x0
    msr elr_el2, x9
    mov x0, x10
    eret

1:
    mrs x1, sctlr_el1
    bic x1, x1, #1
    bic x1, x1, #(1 << 2)
    bic x1, x1, #(1 << 12)
    msr sctlr_el1, x1
    isb

    mov x0, x10
    br x9

.data
.align 3
.globl secondary_entry_target
secondary_entry_target:
    .quad 0
.globl secondary_bootargs_phys
secondary_bootargs_phys:
    .quad 0
.text

.section .note.GNU-stack,"",%progbits

#else

.text
.code64
.globl jump_to_xnu
.type jump_to_xnu, @function

jump_to_xnu:
    cli

    /* set stack */
    mov %rdx, %rsp
    and $~0xF, %rsp
    xor %rbp, %rbp

    /*
     * Build the 10-byte GDTR on the stack at runtime.
     * This avoids any absolute relocation against .data:
     *   [0..1]  limit  = gdt_end - gdt - 1
     *   [2..9]  base   = &gdt  (filled via RIP-relative lea)
     */
    sub $16, %rsp /* keep 16-byte alignment */
    movw $(gdt_end - gdt - 1), 0(%rsp)
    lea gdt(%rip), %rax
    mov %rax, 2(%rsp)
    lgdt 0(%rsp)
    add $16, %rsp

    /* stash entry + boot_args in callee-saved regs before lretq */
    mov %rdi, %rbx /* entry_phys */
    mov %rsi, %rbp /* boot_args */

    /* load 32-bit flat data segments in 64-bit mode so the descriptor
     * cache picks up base=0 from the GDT before the far jump.
     * In 64-bit mode this does not update GS_BASE MSR, that MSR is
     * cleared separately below so OVMF's value doesn't survive. */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* clear GS_BASE MSR, OVMF leaves a non-zero value */
    xor %eax, %eax
    xor %edx, %edx
    mov $0xC0000101, %ecx
    wrmsr
    mov $0xC0000102, %ecx
    wrmsr

    /* far jump into 32-bit compat CS */
    pushq $0x08
    lea compat32(%rip), %rax
    pushq %rax
    lretq


.code32
compat32:
    /* set data segments now that we're in 32-bit mode (base comes from GDT) */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* disable paging, matches Apple boot.efi trampoline */
    mov %cr0, %eax
    and $0x7fffffff, %eax
    mov %eax, %cr0

    /* clear EFER.LME, exit long mode entirely */
    mov $0xC0000080, %ecx
    rdmsr
    and $0xfffffeff, %eax
    wrmsr

    /* pipeline flush */
    jmp 1f
1:
    mov %ebp, %eax
    jmp *%ebx

.section .data
.align 8

gdt:
    .quad 0 /* null descriptor  */

    /* 0x08, 32-bit code */
    .word 0xffff, 0
    .byte 0, 0x9a, 0xcf, 0

    /* 0x10, 32-bit data */
    .word 0xffff, 0
    .byte 0, 0x92, 0xcf, 0

gdt_end:

.section .note.GNU-stack,"",@progbits

#endif /* __aarch64__ */