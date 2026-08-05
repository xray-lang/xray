EXTERN(xray_kernel_entry)
ENTRY(_start)
SECTIONS {
  . = 0x100000;
  _start = xray_kernel_entry;
  .xray_multiboot ALIGN(4) : { KEEP(*("__DATA,.xray_multiboot")) }
  .text ALIGN(16) : { KEEP(*("__TEXT,.xray_boot")) *(.text*) }
  .rodata ALIGN(16) : { *(.rodata*) }
  .eh_frame_hdr ALIGN(4) : { *(.eh_frame_hdr) }
  .eh_frame ALIGN(8) : { *(.eh_frame) }
  .data ALIGN(16) : { *(.data*) }
  .bss ALIGN(16) : { *(.bss*) *(COMMON) }
}
