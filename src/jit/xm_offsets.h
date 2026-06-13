/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_offsets.h - Unified struct field offsets for JIT/AOT codegen
 *
 * KEY CONCEPT:
 *   All hardcoded byte offsets used by JIT code generation are defined
 *   here with compile-time assertions to catch layout changes.
 *
 * WHY THIS DESIGN:
 *   - Single source of truth for all struct offsets
 *   - _Static_assert catches silent breakage when fields are added/removed
 *   - Shared between xm_codegen.c and xi_to_xm.c
 */

#ifndef XM_OFFSETS_H
#define XM_OFFSETS_H

#include <stddef.h>
#include "../coro/xcoroutine.h"
#include "xjit_scratch.h"
#include "xjit_coro_state.h"
#include "../vm/xvm_coro_state.h"

/* ========== XrCoroutine field offsets ========== */

#define XM_CORO_REDUCTIONS_OFFSET offsetof(XrCoroutine, reductions)
#define XM_CORO_GC_OFFSET offsetof(XrCoroutine, coro_gc)
#define XM_CORO_BACKEND_STATE_OFFSET offsetof(XrCoroutine, backend_state)
#define XM_VM_STATE_JIT_STATE_OFFSET offsetof(XrVmCoroState, jit_state)

/* ========== XrJitCoroState fields ========== */

#define XM_JIT_STATE_SCRATCH_OFFSET offsetof(XrJitCoroState, scratch)
#define XM_JIT_STATE_RESUME_ENTRY_OFFSET offsetof(XrJitCoroState, resume_entry)
#define XM_JIT_STATE_RESUME_PROTO_OFFSET offsetof(XrJitCoroState, resume_proto)
#define XM_JIT_STATE_SUSPEND_ID_OFFSET offsetof(XrJitCoroState, suspend_id)
#define XM_JIT_STATE_SUSPEND_SMAP_OFFSET offsetof(XrJitCoroState, suspend_smap_id)
#define XM_JIT_STATE_SUSPEND_PTR_OFFSET offsetof(XrJitCoroState, suspend)
// Sub-field offsets within XrJitSuspendState (for ARM64 codegen addressing)
#define XM_SUSPEND_CALLER_SAVED_OFF offsetof(XrJitSuspendState, caller_saved)
#define XM_SUSPEND_CALLEE_SAVED_OFF offsetof(XrJitSuspendState, callee_saved)
#define XM_SUSPEND_RESULT_OFF offsetof(XrJitSuspendState, result)
#define XM_SUSPEND_RESULT_TAG_OFF offsetof(XrJitSuspendState, result_tag)
#define XM_SUSPEND_SPILL_OFF offsetof(XrJitSuspendState, spill)

/* ========== XrJitScratch field offsets (relative to jit_ctx base) ========== */

#define XM_JIT_CALL_ARGS_OFFSET offsetof(XrJitScratch, call_args)
#define XM_JIT_CALL_ARG_TAGS_OFFSET offsetof(XrJitScratch, call_arg_tags)
#define XM_JIT_CALL_PROTO_OFFSET offsetof(XrJitScratch, call_proto)
#define XM_JIT_CALL_CLOSURE_OFFSET offsetof(XrJitScratch, call_closure)
#define XM_JIT_EXCEPTION_OFFSET offsetof(XrJitScratch, exception)
#define XM_JIT_DEOPT_ID_OFFSET offsetof(XrJitScratch, deopt_id)
#define XM_JIT_DEOPT_REGS_OFFSET offsetof(XrJitScratch, deopt_regs)
#define XM_JIT_DEOPT_FP_REGS_OFFSET offsetof(XrJitScratch, deopt_fp_regs)
#define XM_JIT_DEOPT_SPILL_BASE_OFFSET offsetof(XrJitScratch, deopt_spill_base)
#define XM_JIT_DEOPT_SPILL_SAVE_OFFSET offsetof(XrJitScratch, deopt_spill_save)
#define XM_JIT_PARAM_TAGS_OFFSET offsetof(XrJitScratch, param_tags)
#define XM_JIT_RET_COUNT_OFFSET offsetof(XrJitScratch, ret_count)
#define XM_JIT_RET_VALS_OFFSET offsetof(XrJitScratch, ret_vals)
#define XM_JIT_RET_TAGS_OFFSET offsetof(XrJitScratch, ret_tags)
// GC stack map fields
#define XM_JIT_ACTIVE_SMAP_ID_OFFSET offsetof(XrJitScratch, active_safepoint_id)
#define XM_JIT_ACTIVE_SMAP_OFFSET offsetof(XrJitScratch, active_stack_map)
#define XM_JIT_FRAME_SP_OFFSET offsetof(XrJitScratch, jit_frame_sp)
#define XM_JIT_SAFEPOINT_SAVED_SP_OFFSET offsetof(XrJitScratch, safepoint_saved_sp)
// JIT frame stack for GC caller-frame scanning
#define XM_JIT_FRAME_DEPTH_OFFSET offsetof(XrJitScratch, jit_frame_depth)
#define XM_JIT_FRAME_STACK_OFFSET offsetof(XrJitScratch, jit_frame_stack)
// Per-vreg runtime tags (written by CALL_C codegen from XrJitResult.tag).
// Indexed by vreg index (no bc_slot indirection).
#define XM_JIT_VREG_RUNTIME_TAGS_OFFSET offsetof(XrJitScratch, vreg_runtime_tags)
// Tag from last call_c_stub: stored here instead of x1 to avoid clobbering alloc_regs[0]
#define XM_JIT_CALL_RESULT_TAG_OFFSET offsetof(XrJitScratch, call_result_tag)
// Scratch slot reusing call_args[15] for temporary tag save/restore
// during field load/store codegen. Not a tag bitmap channel.
#define XM_JIT_TAG_SCRATCH_OFFSET (XM_JIT_CALL_ARGS_OFFSET + 15 * 8)
// Guard page safepoint fields
#define XM_JIT_SAFEPOINT_PAGE_OFFSET offsetof(XrJitScratch, safepoint_page)
#define XM_JIT_SAFEPOINT_RETURN_PC_OFFSET offsetof(XrJitScratch, safepoint_return_pc)

/* ========== XrClosure field offsets ========== */

#define XM_CLOSURE_PROTO_OFFSET XM_GC_HEADER_SIZE  // offsetof(XrClosure, proto) = header

/* ========== XrCell field offsets (header + value) ========== */

#define XM_CELL_VALUE_OFFSET XM_GC_HEADER_SIZE  // offsetof(XrCell, value) = header

/* ========== XrProto field offsets ========== */

/*
 * Hardcoded XrProto byte offsets consumed by JIT codegen; static_asserts
 * below validate them against the live struct layout. Update these in
 * lockstep whenever XrProto fields are added/removed (most recently the
 * raw constant pool fields were removed, shifting everything by -16).
 */
#define XM_PROTO_JIT_ENTRY_OFFSET 328
#define XM_PROTO_JIT_FAST_ENTRY_OFFSET 336
#define XM_PROTO_JIT_RESUME_ENTRY_OFFSET 344
#define XM_PROTO_STACK_MAP_OFFSET 424

/* ========== Object layout constants ========== */

#define XM_GC_HEADER_SIZE 16           // sizeof(XrGCHeader)
#define XM_XRVALUE_SIZE 16             // sizeof(XrValue)
#define XM_XRVALUE_TAG_OFFSET 0        // offsetof(XrValue, tag) — uint8_t at byte 0
#define XM_XRVALUE_HEAP_TYPE_OFFSET 2  // offsetof(XrValue, heap_type) — uint16_t at byte 2
#define XM_XRVALUE_PAYLOAD_OFFSET 8    // offsetof(XrValue, i/f/ptr) — at byte 8
#define XM_GC_TYPE_OFFSET 0            // offsetof(XrGCHeader, type) — uint16_t
#define XM_GC_EXTRA_OFFSET 2           // offsetof(XrGCHeader, extra)

// XrInstance: XrGCHeader(16) + klass*(8) + XrValue fields[]
#define XM_INSTANCE_KLASS_OFFSET XM_GC_HEADER_SIZE
#define XM_INSTANCE_FIELDS_OFFSET (XM_GC_HEADER_SIZE + 8)
// XrJson: XrGCHeader(16) + overflow*(8) + XrValue fields[]
#define XM_JSON_FIELDS_OFFSET (XM_GC_HEADER_SIZE + 8)

/* ========== XrArray field offsets ========== */

/* ========== XrArray field offsets (header + fields) ========== */

#define XM_ARRAY_DATA_OFFSET XM_GC_HEADER_SIZE              // offsetof(XrArray, data)
#define XM_ARRAY_LENGTH_OFFSET (XM_GC_HEADER_SIZE + 8)      // offsetof(XrArray, length)
#define XM_ARRAY_ELEM_TYPE_OFFSET (XM_GC_HEADER_SIZE + 24)  // offsetof(XrArray, elem_type)
#define XM_ARRAY_ELEM_SIZE_OFFSET (XM_GC_HEADER_SIZE + 25)  // offsetof(XrArray, elem_size)

/* ========== GC / Allocation offsets ========== */

#define XM_IMMIX_CURSOR_OFFSET 0     // offsetof(XrImmixHeap, cursor)
#define XM_IMMIX_LIMIT_OFFSET 8      // offsetof(XrImmixHeap, limit)
#define XM_GC_HDR_TYPE_OFFSET 0      // offsetof(XrGCHeader, type)
#define XM_GC_HDR_EXTRA_OFFSET 2     // offsetof(XrGCHeader, extra)
#define XM_GC_HDR_REFCOUNT_OFFSET 4  // offsetof(XrGCHeader, refcount)
#define XM_GC_HDR_OBJSIZE_OFFSET 8   // offsetof(XrGCHeader, objsize)
#define XM_GC_HDR_RSV_OFFSET 12      // offsetof(XrGCHeader, _rsv)

/* ========== GC bookkeeping offsets (for inline alloc_post) ========== */
/* Inline allocation bumps the Immix cursor, updates block accounting, and
 * adds to totalbytes. */

#define XM_GC_TOTALBYTES_OFFSET 96            // offsetof(XrCoroGC, totalbytes)
#define XM_IMMIX_BLOCK_ALLOC_MARKS_OFFSET 8   // offsetof(XrImmixBlock, alloc_marks)
#define XM_IMMIX_BLOCK_ALLOC_COUNT_OFFSET 28  // offsetof(XrImmixBlock, alloc_count)
#define XM_IMMIX_BLOCK_ALLOC_BYTES_OFFSET 32  // offsetof(XrImmixBlock, alloc_bytes)
#define XM_IMMIX_BLOCK_SIZE_MASK 0x3FFF       // XR_IMMIX_BLOCK_SIZE - 1
#define XM_IMMIX_LINE_SIZE_SHIFT 7            // log2(128) = 7

/* ========== Compile-time offset verification ========== */

// Include struct definitions only when verifying (not in codegen hot path)
#ifdef XM_VERIFY_OFFSETS

#include "../coro/xcoroutine.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/gc/xgc_header.h"

_Static_assert(offsetof(XrCoroutine, reductions) == XM_CORO_REDUCTIONS_OFFSET,
               "reductions offset mismatch");
_Static_assert(offsetof(XrCoroutine, coro_gc) == XM_CORO_GC_OFFSET, "coro_gc offset mismatch");
_Static_assert(offsetof(XrCoroutine, backend_state) == XM_CORO_BACKEND_STATE_OFFSET,
               "backend_state offset mismatch");
_Static_assert(offsetof(XrVmCoroState, jit_state) == XM_VM_STATE_JIT_STATE_OFFSET,
               "VM jit_state offset mismatch");
_Static_assert(sizeof(XrValue) == XM_XRVALUE_SIZE, "XrValue size mismatch");
_Static_assert(offsetof(XrValue, tag) == XM_XRVALUE_TAG_OFFSET, "XrValue.tag offset mismatch");
_Static_assert(sizeof(XrGCHeader) == XM_GC_HEADER_SIZE, "GCHeader size mismatch");
_Static_assert(offsetof(XrGCHeader, type) == XM_GC_TYPE_OFFSET, "GCHeader.type offset mismatch");

/* call_arg_tags[] must immediately follow call_args[] in XrJitScratch.
 * Codegen writes per-byte XR_TAG_* here; runtime reads from the same offset.
 * Tag scratch slot reuses call_args[15] and must not alias call_arg_tags. */
_Static_assert(XM_JIT_CALL_ARGS_OFFSET + 16 * 8 == XM_JIT_CALL_ARG_TAGS_OFFSET,
               "call_arg_tags must immediately follow call_args[16]");
_Static_assert(sizeof(((XrJitScratch *) 0)->call_arg_tags) == 16,
               "call_arg_tags must be 16 bytes (one tag per call_arg slot)");

/* JIT multi-return scratch: ret_vals[] and ret_tags[] must be 8-byte aligned
 * for ARM64 STR/LDR instructions. Using int64_t elements guarantees this. */
_Static_assert(XM_JIT_RET_COUNT_OFFSET % 8 == 0, "ret_count must be 8-byte aligned for ARM64 STR");
_Static_assert(XM_JIT_RET_VALS_OFFSET % 8 == 0, "ret_vals must be 8-byte aligned for ARM64");
_Static_assert(XM_JIT_RET_TAGS_OFFSET % 8 == 0, "ret_tags must be 8-byte aligned for ARM64");
_Static_assert(sizeof(((XrJitScratch *) 0)->ret_tags[0]) == 8,
               "ret_tags elements must be 8 bytes for ARM64 alignment");
_Static_assert(sizeof(((XrJitScratch *) 0)->vreg_runtime_tags) == XR_JIT_MAX_VREG_TAGS,
               "vreg_runtime_tags size must match XR_JIT_MAX_VREG_TAGS");

#include "../runtime/value/xchunk.h"
_Static_assert(offsetof(XrProto, jit_entry) == XM_PROTO_JIT_ENTRY_OFFSET,
               "jit_entry offset mismatch");
_Static_assert(offsetof(XrProto, jit_fast_entry) == XM_PROTO_JIT_FAST_ENTRY_OFFSET,
               "jit_fast_entry offset mismatch");
_Static_assert(offsetof(XrProto, jit_resume_entry) == XM_PROTO_JIT_RESUME_ENTRY_OFFSET,
               "jit_resume_entry offset mismatch");
_Static_assert(offsetof(XrProto, stack_map) == XM_PROTO_STACK_MAP_OFFSET,
               "stack_map offset mismatch");

#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/ximmix.h"
_Static_assert(offsetof(XrCoroGC, totalbytes) == XM_GC_TOTALBYTES_OFFSET,
               "totalbytes offset mismatch");
_Static_assert(offsetof(XrImmixBlock, alloc_marks) == XM_IMMIX_BLOCK_ALLOC_MARKS_OFFSET,
               "alloc_marks offset mismatch");
_Static_assert(offsetof(XrImmixBlock, alloc_count) == XM_IMMIX_BLOCK_ALLOC_COUNT_OFFSET,
               "alloc_count offset mismatch");
_Static_assert(offsetof(XrImmixBlock, alloc_bytes) == XM_IMMIX_BLOCK_ALLOC_BYTES_OFFSET,
               "alloc_bytes offset mismatch");
_Static_assert(offsetof(XrImmixHeap, cursor) == XM_IMMIX_CURSOR_OFFSET,
               "ImmixHeap.cursor offset mismatch");
_Static_assert(offsetof(XrImmixHeap, limit) == XM_IMMIX_LIMIT_OFFSET,
               "ImmixHeap.limit offset mismatch");

// XrGCHeader detailed field checks
_Static_assert(offsetof(XrGCHeader, extra) == XM_GC_HDR_EXTRA_OFFSET,
               "GCHeader.extra offset mismatch");
_Static_assert(offsetof(XrGCHeader, refcount) == XM_GC_HDR_REFCOUNT_OFFSET,
               "GCHeader.refcount offset mismatch");
_Static_assert(offsetof(XrGCHeader, objsize) == XM_GC_HDR_OBJSIZE_OFFSET,
               "GCHeader.objsize offset mismatch");
_Static_assert(offsetof(XrGCHeader, _rsv) == XM_GC_HDR_RSV_OFFSET, "GCHeader._rsv offset mismatch");

// XrValue detailed field checks
_Static_assert(offsetof(XrValue, heap_type) == XM_XRVALUE_HEAP_TYPE_OFFSET,
               "XrValue.heap_type offset mismatch");
_Static_assert(offsetof(XrValue, i) == XM_XRVALUE_PAYLOAD_OFFSET,
               "XrValue payload offset mismatch");

// XrClosure / XrCell checks
#include "../runtime/xexec_frame.h"
#include "../runtime/closure/xcell.h"
_Static_assert(offsetof(XrClosure, proto) == XM_CLOSURE_PROTO_OFFSET,
               "Closure.proto offset mismatch");

// JIT suspend state struct layout must match the old int64_t[40] layout
_Static_assert(sizeof(XrJitSuspendState) == 40 * sizeof(int64_t),
               "XrJitSuspendState size mismatch");
_Static_assert(XM_SUSPEND_CALLER_SAVED_OFF == 0, "caller_saved offset mismatch");
_Static_assert(XM_SUSPEND_CALLEE_SAVED_OFF == 15 * 8, "callee_saved offset mismatch");
_Static_assert(XM_SUSPEND_RESULT_OFF == 23 * 8, "result offset mismatch");
_Static_assert(XM_SUSPEND_RESULT_TAG_OFF == 24 * 8, "result_tag offset mismatch");
_Static_assert(XM_SUSPEND_SPILL_OFF == 25 * 8, "spill offset mismatch");
_Static_assert(offsetof(XrCell, value) == XM_CELL_VALUE_OFFSET, "Cell.value offset mismatch");

// XrArray checks
#include "../runtime/object/xarray.h"
_Static_assert(offsetof(XrArray, data) == XM_ARRAY_DATA_OFFSET, "Array.data offset mismatch");
_Static_assert(offsetof(XrArray, length) == XM_ARRAY_LENGTH_OFFSET, "Array.length offset mismatch");
_Static_assert(offsetof(XrArray, elem_type) == XM_ARRAY_ELEM_TYPE_OFFSET,
               "Array.elem_type offset mismatch");
_Static_assert(offsetof(XrArray, elem_size) == XM_ARRAY_ELEM_SIZE_OFFSET,
               "Array.elem_size offset mismatch");

#endif  // XM_VERIFY_OFFSETS

#endif  // XM_OFFSETS_H
