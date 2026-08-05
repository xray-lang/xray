.section .text.start, "ax", @progbits
.globl _start
.type _start, @function
_start:
    la sp, _stack_top
    call xray_kernel_entry
1:
    wfi
    j 1b
.size _start, . - _start

.globl xray_startup_linked
.type xray_startup_linked, @function
xray_startup_linked:
    ret
.size xray_startup_linked, .-xray_startup_linked
