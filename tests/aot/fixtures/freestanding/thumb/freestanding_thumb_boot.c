#include <stdint.h>

extern int32_t xray_kernel_entry(void);
extern uint32_t _stack_top;

void Reset_Handler(void);

__attribute__((noreturn)) void Reset_Handler(void) {
    (void) xray_kernel_entry();
    for (;;) {
    }
}

__attribute__((section(".vectors"), used)) void (*const vector_table[])(void) = {
    (void (*)(void))(&_stack_top),
    Reset_Handler,
};
