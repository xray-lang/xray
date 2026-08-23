#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static unsigned char arena[4096];
static size_t arena_used;

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

_Noreturn void xr_hook_panic(const char *message, size_t length) {
    (void) message;
    (void) length;
    for (;;) {
    }
}

void xr_hook_write(const char *bytes, size_t length) {
    (void) bytes;
    (void) length;
}

bool xr_hook_assertion_report(void *context, const char *bytes,
                              size_t length) {
    return context == NULL && bytes != NULL && length != 0;
}
