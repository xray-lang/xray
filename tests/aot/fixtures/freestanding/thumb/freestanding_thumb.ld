EXTERN(Reset_Handler)
EXTERN(xray_kernel_entry)
ENTRY(Reset_Handler)
MEMORY {
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}
_stack_top = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
  .vectors ORIGIN(FLASH) : { KEEP(*(.vectors)) } > FLASH
  .text ALIGN(4) : { KEEP(*(".text.xray")) *(.text*) } > FLASH
  .rodata ALIGN(4) : { *(.rodata*) } > FLASH
  .data ALIGN(4) : { *(.data*) } > RAM AT> FLASH
  .bss ALIGN(8) (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
}
