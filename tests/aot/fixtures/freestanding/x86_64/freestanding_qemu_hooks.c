#include <stddef.h>
#include <stdint.h>

#define COM1 0x3f8u

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init_once(void) {
    static int initialized;
    if (initialized)
        return;
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x80u);
    outb(COM1 + 0u, 0x03u);
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x03u);
    outb(COM1 + 2u, 0xc7u);
    outb(COM1 + 4u, 0x0bu);
    initialized = 1;
}

void xr_hook_write(const char *bytes, size_t len) {
    serial_init_once();
    for (size_t i = 0; i < len; i++) {
        while ((inb(COM1 + 5u) & 0x20u) == 0u) {
        }
        outb(COM1, (uint8_t) bytes[i]);
    }
}

__attribute__((noreturn)) void xr_hook_panic(const char *message, size_t len) {
    xr_hook_write("PANIC:", 6);
    xr_hook_write(message, len);
    for (;;) {
    }
}
