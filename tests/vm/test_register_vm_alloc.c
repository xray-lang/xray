/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_register_vm_alloc.c - Relocation and allocation-failure regressions.
 *
 * KEY CONCEPT:
 *   Force every successful growth to relocate. Selected failures cover the
 *   initial buffers and both growth protocols without allocator luck.
 */

#include "register_vm_programs.h"
#include "base/xmalloc.h"

static struct {
    void *pointer;
    size_t size;
} blocks[2];
static size_t allocation_calls, fail_at, live_blocks;

static void *moving_realloc(void *old, size_t size) {
    if (++allocation_calls == fail_at)
        return NULL;
    size_t index = 0;
    while (index < 2 && blocks[index].pointer != old)
        ++index;
    XR_CHECK(index < 2, "unknown allocation");
    void *fresh = xr_malloc(size);
    if (!fresh)
        return NULL;
    if (old) {
        XR_CHECK(size >= blocks[index].size, "interpreter only grows buffers");
        memcpy(fresh, old, blocks[index].size);
        xr_free(old);
    } else {
        ++live_blocks;
    }
    blocks[index].pointer = fresh;
    blocks[index].size = size;
    return fresh;
}

static void moving_free(void *pointer) {
    if (!pointer)
        return;
    size_t index = 0;
    while (index < 2 && blocks[index].pointer != pointer)
        ++index;
    XR_CHECK(index < 2, "unknown free");
    xr_free(pointer);
    blocks[index].pointer = NULL;
    blocks[index].size = 0;
    --live_blocks;
}

// Compile the real interpreter with an allocator that always moves buffers.
// The production allocator has already been included for the wrappers above.
#undef xr_realloc
#undef xr_free
#define xr_realloc(pointer, size) moving_realloc(pointer, size)
#define xr_free(pointer) moving_free(pointer)
#include "vm/register/xrbc.c"

int main(void) {
    uint32_t code[11];
    XrRbcFunction function = make_countdown(code);
    XrRbcModule module = {&function, 1, NULL, 0};
    uint64_t argument = 128, result = 999;
    XR_CHECK(xr_rbc_run(&module, 0, &argument, 1, &result) == XR_RBC_OK,
             "relocated stacks must remain usable");
    XR_CHECK(result == argument && live_blocks == 0, "result and cleanup after relocation");
    // The third call grows slots, while the fifth grows the frame buffer.
    const size_t failures[] = {1, 2, 3, 5};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        fail_at = failures[i];
        allocation_calls = 0;
        result = 999;
        XR_CHECK(xr_rbc_run(&module, 0, &argument, 1, &result) == XR_RBC_OUT_OF_MEMORY,
                 "allocation failure must propagate");
        XR_CHECK(result == 999 && live_blocks == 0, "failed run must clean up both buffers");
    }
    puts("register_vm_alloc: passed");
    return 0;
}
