.text
.global jump_to_xnu
.type jump_to_xnu, @function

jump_to_xnu:
    cli

    /*
     * SysV incoming from C:
     *   rdi = entry
     *   rsi = boot_args_phys
     *   rdx = stack_top
     */

    /* debug marker: J */
    mov $0xE9, %dx
    mov $'J', %al
    out %al, %dx

    mov %rdi, %rax          /* save entry */
    mov %rdx, %rsp          /* switch stack */
    and $~0xF, %rsp         /* keep 16-byte alignment */
    xor %rbp, %rbp
    mov %rsi, %rdi          /* arg0 = boot_args_phys */

    /* debug marker: G */
    mov $0xE9, %dx
    mov $'G', %al
    out %al, %dx

    jmp *%rax

.size jump_to_xnu, .-jump_to_xnu
.section .note.GNU-stack,"",@progbits