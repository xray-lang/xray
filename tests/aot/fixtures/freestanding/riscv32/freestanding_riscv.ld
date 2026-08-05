EXTERN(xray_kernel_entry)
ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text ALIGN(4) : { KEEP(*(.text.start)) KEEP(*(".text.entry")) *(.text*) }
  .rodata ALIGN(4) : { *(.rodata*) }
  .sdata ALIGN(4) : { *(.sdata*) }
  .data ALIGN(4) : { *(.data*) }
  .bss ALIGN(8) : { *(.bss*) *(COMMON) }
  _stack_top = 0x87fff000;
}
