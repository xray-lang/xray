#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char arena[4096];
static size_t arena_used;
static char captured[1024];
static size_t captured_length;

void *xr_hook_alloc(size_t size, size_t align) {
    if (align == 0 || (align & (align - 1u)) != 0)
        return NULL;
    size_t begin = (arena_used + align - 1u) & ~(align - 1u);
    if (begin > sizeof(arena) || size > sizeof(arena) - begin)
        return NULL;
    arena_used = begin + size;
    return &arena[begin];
}

void xr_hook_free(void *pointer) {
    (void) pointer;
}

void xr_hook_write(const char *bytes, size_t length) {
    (void) bytes;
    (void) length;
}

bool xr_hook_assertion_report(void *context, const char *bytes,
                              size_t length) {
    if (context != NULL || !bytes || length >= sizeof(captured))
        return false;
    memcpy(captured, bytes, length);
    captured[length] = '\0';
    captured_length = length;
    return false;
}

_Noreturn void xr_hook_panic(const char *message, size_t length) {
    static const char expected[] =
        "freestanding assertion provider rejected failure bytes";
    if (!message || length != sizeof(expected) - 1u ||
        memcmp(message, expected, sizeof(expected) - 1u) != 0 ||
        captured_length == 0)
        exit(91);
    fwrite(captured, 1, captured_length, stdout);
    fwrite("\nTRAP:", 1, 6, stdout);
    fwrite(message, 1, length, stdout);
    fflush(stdout);
    exit(73);
}
