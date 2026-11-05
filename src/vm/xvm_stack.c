/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_stack.c - Coroutine stack and frame growth with stable top offsets
 */
#include "xvm_stack.h"
#include "xvm_coro_state.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include <string.h>

bool xr_coro_grow_stack(XrCoroutine *coro, int extra_slots) {
    if (!coro)
        return false;
    XrVMContext *ctx = xr_coro_vm_ctx(coro);
    if (!ctx->stack)
        return false;
    XR_DCHECK(extra_slots > 0, "grow_stack: non-positive extra_slots");
    XR_DCHECK(ctx->stack_capacity > 0, "grow_stack: zero stack_capacity");

    int stack_used = (int) (ctx->stack_top - ctx->stack);
    int new_capacity = ctx->stack_capacity + extra_slots;
    if (new_capacity > 1024 * 1024)
        return false;

    char *stack_end = (char *) ctx->stack + sizeof(XrValue) * ctx->stack_capacity;
    bool combined = ((char *) ctx->frames == stack_end);

    if (combined) {
        XrValue *new_stack = (XrValue *) xr_malloc(sizeof(XrValue) * new_capacity);
        if (!new_stack)
            return false;
        memcpy(new_stack, ctx->stack, sizeof(XrValue) * ctx->stack_capacity);
        memset(new_stack + ctx->stack_capacity, 0, sizeof(XrValue) * extra_slots);

        XrBcCallFrame *new_frames =
            (XrBcCallFrame *) xr_malloc(sizeof(XrBcCallFrame) * ctx->frame_capacity);
        if (!new_frames) {
            xr_free(new_stack);
            return false;
        }
        memcpy(new_frames, ctx->frames, sizeof(XrBcCallFrame) * ctx->frame_count);

        xr_free(ctx->stack);
        ctx->stack = new_stack;
        ctx->stack_capacity = new_capacity;
        ctx->stack_top = ctx->stack + stack_used;
        ctx->frames = new_frames;
    } else {
        XrValue *new_stack = (XrValue *) xr_realloc(ctx->stack, sizeof(XrValue) * new_capacity);
        if (!new_stack)
            return false;
        memset(new_stack + ctx->stack_capacity, 0, sizeof(XrValue) * extra_slots);
        ctx->stack = new_stack;
        ctx->stack_capacity = new_capacity;
        ctx->stack_top = ctx->stack + stack_used;
    }

    if (ctx->frame_count + 8 >= ctx->frame_capacity) {
        int new_frame_cap = ctx->frame_capacity * 2;
        XrBcCallFrame *new_frames =
            (XrBcCallFrame *) xr_realloc(ctx->frames, sizeof(XrBcCallFrame) * new_frame_cap);
        if (!new_frames)
            return false;
        ctx->frames = new_frames;

        ctx->frame_capacity = new_frame_cap;
    }

    return true;
}

/*
** Ensure ctx has room for one new entry frame plus extra_stack slots.
** See xvm_internal.h for contract.
**
** Growth preserves stack_top as an offset even when stack relocation
** succeeds before frame allocation fails. Entry preparation also restores
** the offset so callers receive a valid context on every outcome.
*/
bool xr_vm_prepare_entry(XrVMContext *ctx, int extra_stack) {
    XR_DCHECK(ctx != NULL, "vm_prepare_entry: NULL ctx");
    XR_DCHECK(extra_stack >= 0, "vm_prepare_entry: negative extra_stack");

    int stack_used = (int) (ctx->stack_top - ctx->stack);
    int stack_needed = stack_used + extra_stack;
    int frames_needed = ctx->frame_count + 1;

    bool need_stack = stack_needed > ctx->stack_capacity;
    bool need_frames = frames_needed >= ctx->frame_capacity;

    if (!need_stack && !need_frames) {
        return true;
    }

    XrCoroutine *coro = (XrCoroutine *) ctx->current_coro;
    if (!coro) {
        // Static fallback ctx (isolate->vm_ctx with no main coro): cannot grow.
        // Caller must surface XR_VM_RUNTIME_ERROR; we never silently truncate.
        return false;
    }

    // xr_coro_grow_stack(coro, n>0) grows stack and (when frames near full)
    // also doubles frame capacity. Use a small bump (64) when only frames
    // need to grow so the underlying API contract (extra_slots > 0) holds.
    int extra = need_stack ? (stack_needed - ctx->stack_capacity + 64) : 64;
    bool grown = xr_coro_grow_stack(coro, extra);
    // Stack relocation can succeed before a later frame allocation fails.
    ctx->stack_top = ctx->stack + stack_used;
    return grown;
}

