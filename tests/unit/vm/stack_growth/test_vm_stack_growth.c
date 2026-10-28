/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_vm_stack_growth.c - Coroutine storage identity and growth invariants
 */
#include "vm/xvm_stack.h"
#include "vm/xvm_coro_state.h"
#include "base/xmalloc.h"
#include <stdio.h>

extern int stack_growth_faults(void);

static const XrCoroBackendVTable vm_identity = {.kind = XR_CORO_BACKEND_VM};
static const XrCoroBackendVTable foreign_identity = {.kind = XR_CORO_BACKEND_AOT};

static void storage_identity(void) {
    XrCoroutine coro = {0};
    XrVmCoroState state = {0};
    XrVMContext *ctx = &state.ctx;
    coro.backend = &vm_identity;
    coro.backend_state = &state;
    ctx->current_coro = &coro;
    ctx->stack_capacity = 8;
    ctx->frame_capacity = 16;
    ctx->stack = xr_malloc(8u * sizeof(XrValue));
    ctx->frames = xr_malloc(16u * sizeof(XrBcCallFrame));
    XR_CHECK(ctx->stack && ctx->frames, "fixture allocation failed");
    memset(ctx->stack, 0, 8u * sizeof(XrValue));
    memset(ctx->frames, 0, 16u * sizeof(XrBcCallFrame));
    ctx->stack_top = ctx->stack + 3;
    ctx->stack[1] = xr_int(91);
    XR_CHECK(xr_vm_prepare_entry(ctx, 1), "existing capacity rejected");
    coro.backend = &foreign_identity;
    XR_CHECK(!xr_coro_grow_stack(&coro, 8), "foreign backend admitted");
    coro.backend = &vm_identity;
    XrVMContext copy = *ctx;
    XR_CHECK(!xr_vm_prepare_entry(&copy, 12), "foreign context admitted");
    ctx->current_coro = NULL;
    XR_CHECK(!xr_vm_prepare_entry(ctx, 12), "static context grew");
    XR_CHECK(!xr_coro_grow_stack(&coro, 8), "unbound owner grew");
    ctx->current_coro = &coro;
    XR_CHECK(xr_vm_prepare_entry(ctx, 12), "authenticated growth failed");
    XR_CHECK(ctx->stack_top == ctx->stack + 3, "top was not relocated");
    XR_CHECK(XR_TO_INT(ctx->stack[1]) == 91, "stack data changed");
    XR_CHECK(!xr_coro_grow_stack(&coro, 1024 * 1024), "stack limit bypassed");
    XR_CHECK(ctx->stack_top == ctx->stack + 3, "limit rejection changed top");
    xr_free(ctx->frames);
    xr_free(ctx->stack);
}

int main(void) {
    storage_identity();
    int result = stack_growth_faults();
    if (!result)
        puts("VM stack identity, partial growth, rollback cursors and reuse: PASS");
    return result;
}
