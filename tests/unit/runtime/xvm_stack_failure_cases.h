/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_stack_failure_cases.h - Real partial stack/frame allocation failures
 */
#ifndef XVM_STACK_FAILURE_CASES_H
#define XVM_STACK_FAILURE_CASES_H
static inline void *growth_malloc(size_t size) { return xr_malloc(size); }
static inline void growth_free(void *pointer) { xr_free(pointer); }
static void *growth_frames;
static size_t growth_old_stack_bytes;
static unsigned growth_moves, growth_refusals;
static bool growth_refuse_frames;
static void *growth_realloc(void *pointer, size_t size) {
    if (pointer == growth_frames) {
        if (growth_refuse_frames) { ++growth_refusals; return NULL; }
        return xr_realloc(pointer, size);
    }
    void *replacement = xr_malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, growth_old_stack_bytes);
    xr_free(pointer);
    ++growth_moves;
    return replacement;
}
#undef xr_malloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) growth_malloc(size)
#define xr_free(pointer) growth_free(pointer)
#define xr_realloc(pointer, size) growth_realloc(pointer, size)
#define xr_coro_grow_stack probe_coro_grow_stack
#define xr_vm_prepare_entry probe_vm_prepare_entry
/* Both entry preparation and growth execute their production bodies. Only
 * allocator calls are intercepted to move the stack then reject frame growth. */
#include "../../../src/vm/xvm_stack.c"
#undef xr_coro_grow_stack
#undef xr_vm_prepare_entry
#undef xr_malloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) growth_malloc(size)
#define xr_free(pointer) growth_free(pointer)
#define xr_realloc(pointer, size) growth_realloc(pointer, size)
TEST(stack_growth_partial_failure_restores_top) {
    XrCoroutine coro = {0};
    XrVmCoroState state = {0};
    coro.backend = xr_coro_vm_backend_vtable();
    coro.backend_state = &state;
    XrVMContext *ctx = &state.ctx;
    ctx->current_coro = &coro;
    ctx->stack_capacity = 8;
    ctx->frame_capacity = 4;
    ctx->frame_count = 1;
    ctx->stack = growth_malloc(sizeof(XrValue) * 8u);
    ctx->frames = growth_malloc(sizeof(XrBcCallFrame) * 4u);
    GROW_REQUIRE(ctx->stack && ctx->frames);
    memset(ctx->stack, 0, sizeof(XrValue) * 8u);
    memset(ctx->frames, 0, sizeof(XrBcCallFrame) * 4u);
    ctx->stack[1] = xr_int(91);
    ctx->stack_top = ctx->stack + 3;
    ctx->frames[0].base_offset = 1;
    growth_frames = ctx->frames;
    growth_old_stack_bytes = sizeof(XrValue) * 8u;
    growth_refuse_frames = true;
    growth_moves = growth_refusals = 0u;
    GROW_REQUIRE(!probe_vm_prepare_entry(ctx, 9));
    GROW_REQUIRE(growth_moves == 1u && growth_refusals == 1u);
    GROW_REQUIRE(ctx->stack_capacity > 8 && ctx->frame_capacity == 4);
    GROW_REQUIRE(ctx->stack_top == ctx->stack + 3);
    GROW_REQUIRE(XR_TO_INT(ctx->stack[1]) == 91);
    GROW_REQUIRE(ctx->frames == growth_frames && ctx->frames[0].base_offset == 1);
    growth_refuse_frames = false;
    growth_old_stack_bytes = sizeof(XrValue) * (size_t) ctx->stack_capacity;
    ctx->frame_count = 3;
    GROW_REQUIRE(probe_vm_prepare_entry(ctx, 0));
    GROW_REQUIRE(ctx->stack_top == ctx->stack + 3 && ctx->frame_capacity == 8);
    growth_free(ctx->stack);
    growth_free(ctx->frames);
}
#endif // XVM_STACK_FAILURE_CASES_H
