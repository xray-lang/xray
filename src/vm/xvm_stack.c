/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_stack.c - Coroutine stack and frame storage growth
 *
 * KEY CONCEPT:
 *   Preserve live stack offsets whenever one allocation moves before another
 *   allocation fails. The coroutine remains usable on every return path.
 */

#include "xvm_stack.h"
#include "xvm_coro_state.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../base/xconstants.h"
#include <string.h>

XR_FUNC bool xr_coro_grow_stack(XrCoroutine *coro, int extra_slots) {
    if (!coro)
        return false;
    XrVmCoroState *state = xr_coro_maybe_vm_state(coro);
    if (!state)
        return false;
    XrVMContext *ctx = &state->ctx;
    if (!ctx->stack || ctx->current_coro != coro)
        return false;
    if (extra_slots <= 0 || ctx->stack_capacity <= 0 ||
        extra_slots > 1024 * 1024 - ctx->stack_capacity)
        return false;
    int stack_used = (int) (ctx->stack_top - ctx->stack);
    int new_capacity = ctx->stack_capacity + extra_slots;

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

XR_FUNC bool xr_vm_prepare_entry(XrVMContext *ctx, int extra_stack) {
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
    if (!coro || xr_coro_maybe_vm_state(coro) == NULL || xr_coro_vm_ctx(coro) != ctx) {
        // Static fallback ctx (isolate->vm_ctx with no main coro): cannot grow.
        // Caller must surface XR_VM_RUNTIME_ERROR; we never silently truncate.
        return false;
    }

    // xr_coro_grow_stack(coro, n>0) grows stack and (when frames near full)
    // also doubles frame capacity. Use a small bump (64) when only frames
    // need to grow so the underlying API contract (extra_slots > 0) holds.
    int extra = need_stack ? (stack_needed - ctx->stack_capacity + 64) : 64;
    bool grown = xr_coro_grow_stack(coro, extra);
    // A later frame allocation can fail after relocating the stack.
    ctx->stack_top = ctx->stack + stack_used;
    return grown;
}


/* A rejected later allocation may leave both storage buffers relocated.
 * Rebuild every recoverable cursor before reporting the allocation result. */
XR_FUNC bool xr_vm_ensure_call_stack(XrVMContext *ctx, XrCoroutine *coro,
                                      int needed, XrVMStackCursor *cursor) {
    bool need_grow = needed > ctx->stack_capacity || ctx->frame_count + 1 >= ctx->frame_capacity;
    if (!need_grow)
        return true;
    XrVmCoroState *state = xr_coro_maybe_vm_state(coro);
    if (!state || &state->ctx != ctx)
        return false;

    int base_offset = (int) (cursor->base - ctx->stack);
    int frame_index = (int) (cursor->frame - ctx->frames);
    cursor->frame->pc = cursor->pc;
    int extra = needed > ctx->stack_capacity ? needed - ctx->stack_capacity + 64 : 64;
    bool grown = xr_coro_grow_stack(coro, extra);
    cursor->base = ctx->stack + base_offset;
    cursor->frame = &ctx->frames[frame_index];
    return grown;
}

XR_FUNC XrBcCallFrame *xr_vm_push_frame(XrVMContext *ctx, XrCoroutine *coro,
                                        XrClosure *closure, int base_delta,
                                        XrVMStackCursor *cursor) {
    if (XR_UNLIKELY(ctx->frame_count >= XR_FRAMES_MAX))
        return NULL;
    XrProto *proto = closure->proto;
    int base_offset = (int) (cursor->base - ctx->stack) + base_delta;
    cursor->frame->pc = cursor->pc;
    if (!xr_vm_ensure_call_stack(ctx, coro, base_offset + proto->maxstacksize, cursor))
        return NULL;

    int index = ctx->frame_count;
    XR_DCHECK(index < ctx->frame_capacity, "push_frame: capacity not prepared");
    XrBcCallFrame *next = &ctx->frames[index];
    memset(next, 0, sizeof(*next));
    ++ctx->frame_count;
    next->closure = closure;
    next->pc = PROTO_CODE_BASE(proto);
    next->base_offset = base_offset;
    return next;
}
