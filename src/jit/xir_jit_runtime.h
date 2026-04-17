/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_jit_runtime.h - JIT runtime bridge function declarations
 *
 * KEY CONCEPT:
 *   C bridge functions callable from JIT-compiled ARM64 code.
 *   Unified calling convention:
 *
 *   1. Value-returning helpers: XrJitResult fn(XrCoroutine*, int64_t)
 *      ARM64 ABI: struct{int64_t, uint64_t} <= 16B -> x0=payload, x1=tag
 *      Codegen reads x0 (payload) into dst vreg, x1 (tag) stored via
 *      call_c_stub into jit_ctx->call_result_tag. No manual tag side-effect.
 *
 *   2. GC helpers (barrier, alloc, safepoint): custom signatures with
 *      precise register assignments.
 */

#ifndef XIR_JIT_RUNTIME_H
#define XIR_JIT_RUNTIME_H

#include <stdint.h>
#include "../base/xdefs.h"
#include "../runtime/value/xvalue.h"  /* XR_TAG_*, XrValue */

struct XrCoroutine;
struct XrGCHeader;

/* ========== Value-returning helper result type ========== */

// ARM64 ABI: struct { int64_t; uint64_t } -> x0=payload, x1=tag
// All CALL_C value-returning helpers return this instead of int64_t,
// eliminating the global jit_ctx->return_tag side-effect channel.
typedef struct {
    int64_t  payload;  // raw 64-bit value (int / float bits / pointer)
    uint64_t tag;      // XrValue.tag (lower byte valid, upper bytes zero)
} XrJitResult;

/* ========== Convenience macros for constructing XrJitResult ========== */

#define XR_JIT_OK()       ((XrJitResult){ 0, 0 })
#define XR_JIT_NULL()     ((XrJitResult){ 0, (uint64_t)XR_TAG_NULL })
#define XR_JIT_INT(v)     ((XrJitResult){ (int64_t)(v), (uint64_t)XR_TAG_I64 })
#define XR_JIT_BOOL(v)    ((XrJitResult){ (int64_t)(v), (uint64_t)XR_TAG_BOOL })
#define XR_JIT_PTR(p)     ((XrJitResult){ (int64_t)(uintptr_t)(p), (uint64_t)XR_TAG_PTR })
#define XR_JIT_VAL(v)     ((XrJitResult){ (v).i, (uint64_t)(v).tag })

static inline XrJitResult xr_jit_float_result(double v) {
    union { double f; int64_t i; } u = { .f = v };
    return (XrJitResult){ u.i, (uint64_t)XR_TAG_F64 };
}
#define XR_JIT_FLOAT(v)   xr_jit_float_result(v)

/* ========== GC Helpers (defined in coro/xcoro.c, gc/xcoro_gc.c) ========== */

XR_FUNC int xr_coro_gc_safepoint(struct XrCoroutine *coro);
XR_FUNC void xr_jit_barrier_fwd(struct XrCoroutine *coro, void *parent, void *child);
XR_FUNC void xr_jit_barrier_back(struct XrCoroutine *coro, void *container);
XR_FUNC struct XrGCHeader* xr_jit_alloc(struct XrCoroutine *coro, uint64_t type_and_size);
XR_FUNC void xr_jit_alloc_post(struct XrCoroutine *coro, void *obj_ptr);
XR_FUNC void xr_jit_mark_lines(struct XrCoroutine *coro, uint64_t obj_ptr);

/* ========== Call / Invoke ========== */

XR_FUNC XrJitResult xr_jit_call_self(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_call_func(struct XrCoroutine *coro, int64_t nargs_encoded);
XR_FUNC XrJitResult xr_jit_invoke_method(struct XrCoroutine *coro, int64_t encoded);
XR_FUNC XrJitResult xr_jit_invoke_direct(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_closure_new(struct XrCoroutine *coro, int64_t proto_raw);
XR_FUNC XrJitResult xr_jit_closure_set_upval(struct XrCoroutine *coro, int64_t encoded);

// Flat upvalue read from current closure
XR_FUNC XrJitResult xr_jit_upval_get(struct XrCoroutine *coro, int64_t upval_index);

/* ========== Arithmetic (mixed-type fallback) ========== */

XR_FUNC XrJitResult xr_jit_rt_add(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_rt_sub(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_rt_mul(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_rt_div(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_rt_mod(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_rt_eq(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_eq_value(struct XrCoroutine *coro, int64_t unused);
/* ========== Property Access ========== */

XR_FUNC XrJitResult xr_jit_getprop(struct XrCoroutine *coro, int64_t symbol_id);
XR_FUNC XrJitResult xr_jit_setprop(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_getfield_ic(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_getbuiltin(struct XrCoroutine *coro, int64_t extra_arg);

/* ========== Index / Container Access ========== */

XR_FUNC XrJitResult xr_jit_index_get(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_index_set(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_tarray_get(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_tarray_set(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_map_get(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_map_set(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_map_increment(struct XrCoroutine *coro, int64_t extra_arg);

/* ========== Shared Variables ========== */

XR_FUNC XrJitResult xr_jit_get_shared(struct XrCoroutine *coro, int64_t shared_index);
XR_FUNC XrJitResult xr_jit_set_shared(struct XrCoroutine *coro, int64_t extra_arg);

/* ========== Exception ========== */

XR_FUNC XrJitResult xr_jit_throw(struct XrCoroutine *coro, int64_t exception_raw);

/* ========== Type Operations ========== */

XR_FUNC XrJitResult xr_jit_is_type(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_checktype(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_typename(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_deep_copy(struct XrCoroutine *coro, int64_t extra_arg);

/* ========== String Operations ========== */

XR_FUNC XrJitResult xr_jit_chr(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_substring(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_str_repeat(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_tostring(struct XrCoroutine *coro, int64_t slot_hint);
XR_FUNC XrJitResult xr_jit_strbuf_new(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_strbuf_append(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_strbuf_finish(struct XrCoroutine *coro, int64_t unused);

/* ========== Struct Native Storage ========== */

XR_FUNC XrJitResult xr_jit_new_struct(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_struct_get(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_struct_set(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_struct_copy(struct XrCoroutine *coro, int64_t extra_arg);

/* ========== Container Construction ========== */

XR_FUNC XrJitResult xr_jit_rt_array_new(struct XrCoroutine *coro, int64_t capacity);
XR_FUNC XrJitResult xr_jit_rt_array_push(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_rt_array_len(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_rt_map_new(struct XrCoroutine *coro, int64_t capacity);
XR_FUNC XrJitResult xr_jit_newrange(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_range_unpack(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_newset(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_slice(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_bytes_new(struct XrCoroutine *coro, int64_t extra_arg);

/* ========== Enum ========== */

XR_FUNC XrJitResult xr_jit_enum_access(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_enum_name(struct XrCoroutine *coro, int64_t unused);
XR_FUNC XrJitResult xr_jit_enum_convert(struct XrCoroutine *coro, int64_t extra_arg);

/* ========== IO / Debug ========== */

XR_FUNC XrJitResult xr_jit_typeof(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_print(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_dump(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_assert(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_assert_eq(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_assert_ne(struct XrCoroutine *coro, int64_t extra_arg);

/* ========== Channel / Concurrency ========== */

XR_FUNC XrJitResult xr_jit_chan_new(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_chan_close(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_chan_is_closed(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_chan_try_send(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_chan_try_recv(struct XrCoroutine *coro, int64_t extra_arg);

// Blocking channel send/recv (JIT CPS via XIR_SUSPEND)
XR_FUNC XrJitResult xr_jit_chan_send(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_chan_send_block(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_chan_recv(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_chan_recv_block(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_scope_enter(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_scope_exit(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_spawn_cont(struct XrCoroutine *coro, int64_t extra_arg);
XR_FUNC XrJitResult xr_jit_await(struct XrCoroutine *coro, int64_t extra_arg);

/* JIT suspend helper for AWAIT blocking path.
 * Sets up waiter + CAS state machine. Called from JIT suspend block
 * after live registers are saved to coro->jit_suspend_state.
 * Returns 0 if successfully blocked (JIT returns SUSPEND_MARKER).
 * Returns 1 if child completed during CAS race (JIT continues). */
XR_FUNC XrJitResult xr_jit_await_block(struct XrCoroutine *coro, int64_t extra_arg);

/* JIT resume entry — called by worker to re-enter JIT after suspend.
 * Reloads saved registers from coro->jit_suspend_state and jumps to
 * the continuation point via jit_suspend_id jump table. */
XR_FUNC int xir_jit_resume(struct XrCoroutine *coro, struct XrValue *result);

#endif // XIR_JIT_RUNTIME_H
