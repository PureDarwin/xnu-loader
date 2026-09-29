OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  . = 0x80400000;
  .text : { KEEP(*(.text.head)) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  . = ALIGN(8);
  .data : { *(.data .data.* .sdata .sdata.*) }
  __global_pointer$ = . + 0x800;
  . = ALIGN(8);
  __image_end = .;
  .bss (NOLOAD) : {
    __bss_start = .;
    *(.sbss .sbss.* .bss .bss.* COMMON)
    . = ALIGN(16);
    . += 0x8000;
    __stack_top = .;
    __bss_end = .;
  }
  __image_size = __bss_end - 0x80400000;
  /DISCARD/ : { *(.comment) *(.note*) *(.riscv.attributes) *(.eh_frame*) }
}
