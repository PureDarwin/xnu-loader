OUTPUT_ARCH(riscv)
ENTRY(_start)
/* hp sram, the esp-idf app's usual spot, clear of the rom's stack near the top */
SECTIONS {
  . = 0x4FF20000;
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
  /DISCARD/ : { *(.comment) *(.note*) *(.riscv.attributes) *(.eh_frame*) }
}
