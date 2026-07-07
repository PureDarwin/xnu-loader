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