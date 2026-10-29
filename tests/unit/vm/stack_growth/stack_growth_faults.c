/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * stack_growth_faults.c - Allocation-only failures in production storage bodies
 */
#define xr_coro_grow_stack fault_coro_grow_stack
#define xr_vm_prepare_entry fault_vm_prepare_entry
#define xr_vm_ensure_call_stack fault_vm_ensure_call_stack
#define xr_vm_push_frame fault_vm_push_frame
#include "vm/xvm_stack.h"
#include "vm/xvm_coro_state.h"
#include "base/xmalloc.h"

static void *raw_allocate(size_t bytes) { return xr_malloc(bytes); }
static void raw_release(void *pointer) { xr_free(pointer); }

static struct { void *pointer; size_t bytes; } allocations[8];
static unsigned live_count, moves, refusals, allocation_calls, fail_at;
static void *refused_frames;
static bool refuse_frames;

static unsigned allocation_index(void *pointer) {
    for (unsigned i = 0; i < live_count; ++i)
        if (allocations[i].pointer == pointer)
            return i;
    XR_CHECK(false, "allocation owner not found");
    return 0;
}

static void *fault_allocate(size_t bytes) {
    if (++allocation_calls == fail_at) { ++refusals; return NULL; }
    void *pointer = raw_allocate(bytes);
    if (!pointer) return NULL;
    XR_CHECK(live_count < 8, "allocation inventory full");
    allocations[live_count].pointer = pointer;
    allocations[live_count++].bytes = bytes;
    return pointer;
}

static void fault_release(void *pointer) {
    if (!pointer) return;
    unsigned index = allocation_index(pointer);
    allocations[index] = allocations[--live_count];
    raw_release(pointer);
}

static void *fault_resize(void *pointer, size_t bytes) {
    if (pointer == refused_frames && refuse_frames) {
        ++refusals;
        return NULL;
    }
    if (!pointer) return fault_allocate(bytes);
    size_t old_bytes = allocations[allocation_index(pointer)].bytes;
    void *replacement = fault_allocate(bytes);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, bytes < old_bytes ? bytes : old_bytes);
    fault_release(pointer);
    ++moves;
    return replacement;
}

#undef xr_malloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(bytes) fault_allocate(bytes)
#define xr_free(pointer) fault_release(pointer)
#define xr_realloc(pointer, bytes) fault_resize(pointer, bytes)
#ifndef XVM_STACK_SOURCE
#define XVM_STACK_SOURCE "../../../../src/vm/xvm_stack.c"
#endif
/* Compile the real storage implementation; only its physical allocator is
 * intercepted. The dispatch helper above calls this exact faulted owner. */
#include XVM_STACK_SOURCE
#undef xr_malloc
#undef xr_free
#undef xr_realloc
#undef xr_coro_grow_stack
#undef xr_vm_prepare_entry
#undef xr_vm_ensure_call_stack
#undef xr_vm_push_frame

static const XrCoroBackendVTable vm_identity = {.kind = XR_CORO_BACKEND_VM};

typedef struct {
    XrCoroutine coro;
    XrVmCoroState state;
} Fixture;

static void fixture_open(Fixture *f, bool combined) {
    XR_CHECK(live_count == 0, "previous owner leaked");
    memset(f, 0, sizeof(*f));
    moves = refusals = allocation_calls = fail_at = 0;
    refuse_frames = false;
    refused_frames = NULL;
    f->coro.backend = &vm_identity;
    f->coro.backend_state = &f->state;
    XrVMContext *ctx = &f->state.ctx;
    ctx->current_coro = &f->coro;
    ctx->stack_capacity = 8;
    ctx->frame_capacity = 4;
    ctx->frame_count = 1;
    size_t values = 8u * sizeof(XrValue), frames = 4u * sizeof(XrBcCallFrame);
    ctx->stack = fault_allocate(values + (combined ? frames : 0u));
    XR_CHECK(ctx->stack, "fixture stack allocation failed");
    ctx->frames = combined ? (XrBcCallFrame *) ((char *) ctx->stack + values)
                           : fault_allocate(frames);
    XR_CHECK(ctx->frames, "fixture frames allocation failed");
    memset(ctx->stack, 0, values);
    memset(ctx->frames, 0, frames);
    ctx->stack_top = ctx->stack + 3;
    ctx->stack[1] = xr_int(91);
    ctx->frames[0].base_offset = 1;
    ctx->pending_error = xr_int(119);
}

static void fixture_close(Fixture *f) {
    XrVMContext *ctx = &f->state.ctx;
    bool combined = (char *) ctx->frames == (char *) (ctx->stack + ctx->stack_capacity);
    if (!combined) fault_release(ctx->frames);
    fault_release(ctx->stack);
    XR_CHECK(live_count == 0, "physical storage leaked");
}

static int split_entry_failure(void) {
    Fixture f;
    fixture_open(&f, false);
    XrVMContext *ctx = &f.state.ctx;
    refused_frames = ctx->frames;
    refuse_frames = true;
    bool grown = fault_vm_prepare_entry(ctx, 9);
    if (grown || moves != 1 || refusals != 1 || ctx->stack_top != ctx->stack + 3) {
        fprintf(stderr, "split prepare failed invariant: ok=%d moves=%u refusals=%u top_matches=%d\n",
                grown, moves, refusals, ctx->stack_top == ctx->stack + 3);
        fixture_close(&f);
        return 1;
    }
    XR_CHECK(ctx->frames == refused_frames && ctx->frame_capacity == 4, "failed frame mutated");
    XR_CHECK(ctx->frame_count == 1 && XR_TO_INT(ctx->pending_error) == 119, "failure cause changed");
    XR_CHECK(XR_TO_INT(ctx->stack[1]) == 91, "moved stack payload changed");
    refuse_frames = false;
    ctx->frame_count = 3;
    XR_CHECK(fault_vm_prepare_entry(ctx, 0), "same context could not be reused");
    XR_CHECK(ctx->stack_top == ctx->stack + 3 && ctx->frame_capacity == 8, "reuse corrupted cursors");
    fixture_close(&f);
    return 0;
}

static int dispatch_failure(bool combined) {
    Fixture f;
    fixture_open(&f, combined);
    XrVMContext *ctx = &f.state.ctx;
    XrValue *base = ctx->stack + 1;
    XrBcCallFrame *frame = &ctx->frames[0];
    uintptr_t stack_address = (uintptr_t) ctx->stack;
    uintptr_t frame_address = (uintptr_t) ctx->frames;
    XrInstruction code[1] = {0};
    XrProto proto = {0};
    XrClosure closure = {0};
    proto.maxstacksize = 12;
    closure.proto = &proto;
    refused_frames = ctx->frames;
    refuse_frames = !combined;
    if (combined) fail_at = allocation_calls + 3;
    XrVMStackCursor cursor = {base, frame, code};
    XrBcCallFrame *next = fault_vm_push_frame(ctx, &f.coro, &closure, 2, &cursor);
    base = cursor.base;
    frame = cursor.frame;
    if (next || moves != (combined ? 0u : 1u) || refusals != 1 || base != ctx->stack + 1 || frame != ctx->frames ||
        ctx->stack_top != ctx->stack + 3) {
        fprintf(stderr, "dispatch failure cursors: null=%d moves=%u refusals=%u base=%d frame=%d top=%d\n",
                next == NULL, moves, refusals, base == ctx->stack + 1, frame == ctx->frames,
                ctx->stack_top == ctx->stack + 3);
        fixture_close(&f);
        return 1;
    }
    XR_CHECK(frame->pc == code && ctx->frame_count == 1, "failed dispatch installed frame");
    XR_CHECK((uintptr_t) ctx->stack != stack_address, "failure did not relocate stack");
    if (combined) {
        XR_CHECK((uintptr_t) ctx->frames != frame_address, "third allocation failed before frame move");
        XR_CHECK(ctx->frame_capacity == 4, "failed frame expansion changed capacity");
        XR_CHECK(live_count == 2, "combined slab ownership was not transferred");
    }
    XR_CHECK(XR_TO_INT(ctx->pending_error) == 119, "dispatch replaced original error");
    refuse_frames = false;
    fail_at = 0;
    next = fault_vm_push_frame(ctx, &f.coro, &closure, 2, &cursor);
    base = cursor.base;
    frame = cursor.frame;
    XR_CHECK(next == &ctx->frames[1] && ctx->frame_count == 2, "same dispatch owner could not be reused");
    XR_CHECK(base == ctx->stack + 1 && frame == &ctx->frames[0], "reuse cursors stale");
    XR_CHECK(next->base_offset == 3 && next->closure == &closure, "reused frame identity changed");
    fixture_close(&f);
    return 0;
}

static void combined_failures(void) {
    for (unsigned point = 1; point <= 3; ++point) {
        Fixture f;
        fixture_open(&f, true);
        XrVMContext *ctx = &f.state.ctx;
        unsigned initial = live_count;
        fail_at = allocation_calls + point;
        XR_CHECK(!fault_vm_prepare_entry(ctx, 9), "fault point not reached");
        XR_CHECK(refusals == 1 && ctx->stack_top == ctx->stack + 3, "combined top changed on failure");
        XR_CHECK(ctx->frame_count == 1 && XR_TO_INT(ctx->pending_error) == 119, "combined failure changed state");
        XR_CHECK(XR_TO_INT(ctx->stack[1]) == 91 && ctx->frames[0].base_offset == 1, "combined payload changed");
        XR_CHECK(live_count == (point == 3 ? 2u : initial), "partial allocation not reclaimed");
        fail_at = 0;
        ctx->frame_count = 3;
        XR_CHECK(fault_vm_prepare_entry(ctx, 9), "combined owner not reusable");
        XR_CHECK(ctx->stack_top == ctx->stack + 3 && ctx->frame_capacity == 8, "combined reuse changed state");
        fixture_close(&f);
    }
}

int stack_growth_faults(void) {
    if (split_entry_failure() || dispatch_failure(false) || dispatch_failure(true)) return 1;
    combined_failures();
    return 0;
}
