/*
 * Linux x86 boot protocol, 32-bit entry. This shit is EVIL
 */

#define KERNEL_LINUX_MAGIC 0x584e554c /* "LUNX" */

.section .text.linux_entry,"ax"
.code32
.global linux_entry32
linux_entry32:
  cli
  cld
  mov %esi, %ebx

  /* ebp = where __kernel_start actually is. */
  call 1f
1:
  pop %ebp
  sub $1b, %ebp
  add $__kernel_start, %ebp

  mov $__kernel_start, %edi
  cmp %edi, %ebp
  je 3f

  mov $__file_end, %ecx
  sub $__kernel_start, %ecx
  mov %ebp, %esi
  cmp %edi, %esi
  ja 2f
  /* Destination above the source: copy backwards in case they overlap. */
  lea -1(%esi, %ecx), %esi
  lea -1(%edi, %ecx), %edi
  std
  rep movsb
  cld
  jmp 3f
2:
  rep movsb
3:
  mov $KERNEL_LINUX_MAGIC, %edi
  mov %ebx, %esi
  mov $kernel_boot32, %eax
  jmp *%eax

.section .note.GNU-stack,"",@progbits
