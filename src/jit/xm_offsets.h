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

/* ========== XrCoroutine field offsets ========== */

#define XM_CORO_REDUCTIONS_OFFSET offsetof(XrCoroutine, reductions)
#define XM_CORO_GC_OFFSET offsetof(XrCoroutine, coro_gc)
#define XM_CORO_JIT_CTX_OFFSET offsetof(XrCoroutine, jit_ctx)

/* ========== XrCoroutine JIT suspend/resume fields ========== */

#define XM_CORO_RESUME_ENTRY_OFFSET offsetof(XrCoroutine, jit_resume_entry)
#define XM_CORO_RESUME_PROTO_OFFSET offsetof(XrCoroutine, jit_resume_proto)
#define XM_CORO_SUSPEND_ID_OFFSET offsetof(XrCoroutine, jit_suspend_id)
#define XM_CORO_SUSPEND_SMAP_OFFSET offsetof(XrCoroutine, jit_suspend_smap_id)
#define XM_CORO_SUSPEND_PTR_OFFSET offsetof(XrCoroutine, jit_suspend)
// Sub-field offsets within XrJitSuspendState (for ARM64 codegen addressing)
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

#define XM_CLOSURE_PROTO_OFFSET 16

/* ========== XrCell field offsets (32B lightweight cell) ========== */

#define XM_CELL_VALUE_OFFSET 16  // offsetof(XrCell, value)

/* ========== XrProto field offsets ========== */

/*
 * Offsets shifted by -16 bytes after the IC tables (XrICMethodTable*,
 * XrICFieldTable*) were removed from XrProto and the smaller proto_id
 * field was added; static_asserts below validate against the live
 * struct layout.
 */
#define XM_PROTO_JIT_ENTRY_OFFSET 344
#define XM_PROTO_JIT_FAST_ENTRY_OFFSET 352
#define XM_PROTO_JIT_RESUME_ENTRY_OFFSET 360

/* ========== Object layout constants ========== */

#define XM_GC_HEADER_SIZE 16           // sizeof(XrGCHeader)
#define XM_XRVALUE_SIZE 16             // sizeof(XrValue)
#define XM_XRVALUE_TAG_OFFSET 0        // offsetof(XrValue, tag) — uint8_t at byte 0
#define XM_XRVALUE_HEAP_TYPE_OFFSET 2  // offsetof(XrValue, heap_type) — uint16_t at byte 2
#define XM_XRVALUE_PAYLOAD_OFFSET 8    // offsetof(XrValue, i/f/ptr) — at byte 8
#define XM_GC_TYPE_OFFSET 8            // offsetof(XrGCHeader, type) — uint8_t
#define XM_GC_EXTRA_OFFSET 10          // offsetof(XrGCHeader, extra)

// XrInstance: XrGCHeader(16) + klass*(8) + XrValue fields[]
#define XM_INSTANCE_KLASS_OFFSET XM_GC_HEADER_SIZE         // 16
#define XM_INSTANCE_FIELDS_OFFSET (XM_GC_HEADER_SIZE + 8)  // 24
// XrJson: XrGCHeader(16) + overflow*(8) + XrValue fields[]
#define XM_JSON_FIELDS_OFFSET (XM_GC_HEADER_SIZE + 8)  // 24

/* ========== XrArray field offsets ========== */

#define XM_ARRAY_DATA_OFFSET 16       // offsetof(XrArray, data)
#define XM_ARRAY_LENGTH_OFFSET 24     // offsetof(XrArray, length)
#define XM_ARRAY_ELEM_TYPE_OFFSET 40  // offsetof(XrArray, elem_type)
#define XM_ARRAY_ELEM_SIZE_OFFSET 41  // offsetof(XrArray, elem_size)

/* ========== GC / Allocation offsets ========== */

#define XM_IMMIX_CURSOR_OFFSET 0       // offsetof(XrImmixHeap, cursor)
#define XM_IMMIX_LIMIT_OFFSET 8        // offsetof(XrImmixHeap, limit)
#define XM_GC_CURRENTWHITE_OFFSET 109  // offsetof(XrCoroGC, currentwhite)
#define XM_GC_HDR_TYPE_OFFSET 8        // offsetof(XrGCHeader, type)
#define XM_GC_HDR_MARKED_OFFSET 9      // offsetof(XrGCHeader, marked)
#define XM_GC_HDR_EXTRA_OFFSET 10      // offsetof(XrGCHeader, extra)
#define XM_GC_HDR_OBJSIZE_OFFSET 12    // offsetof(XrGCHeader, objsize)

/* ========== GC bookkeeping offsets (for inline alloc_post) ========== */

#define XM_GC_GCDEBT_OFFSET 88                // offsetof(XrCoroGC, GCdebt)
#define XM_GC_TOTALBYTES_OFFSET 96            // offsetof(XrCoroGC, totalbytes)
#define XM_GC_GC_REQUESTED_OFFSET 104         // offsetof(XrCoroGC, gc_requested)
#define XM_GC_IN_GC_OFFSET 110                // offsetof(XrCoroGC, in_gc)
#define XM_GC_GC_DISABLED_OFFSET 111          // offsetof(XrCoroGC, gc_disabled)
#define XM_GC_ALLOC_SINCE_GC_OFFSET 120       // offsetof(XrCoroGC, alloc_since_gc)
#define XM_GC_OBJECT_COUNT_OFFSET 312         // offsetof(XrCoroGC, object_count)
#define XM_IMMIX_BLOCK_LOCAL_ALLGC_OFFSET 24  // offsetof(XrImmixBlock, local_allgc)
#define XM_IMMIX_BLOCK_ALLOC_MARKS_OFFSET 8   // offsetof(XrImmixBlock, alloc_marks)
#define XM_IMMIX_BLOCK_ALLOC_COUNT_OFFSET 40  // offsetof(XrImmixBlock, alloc_count)
#define XM_IMMIX_BLOCK_ALLOC_BYTES_OFFSET 48  // offsetof(XrImmixBlock, alloc_bytes)
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

#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/ximmix.h"
_Static_assert(offsetof(XrCoroGC, GCdebt) == XM_GC_GCDEBT_OFFSET, "GCdebt offset mismatch");
_Static_assert(offsetof(XrCoroGC, totalbytes) == XM_GC_TOTALBYTES_OFFSET,
               "totalbytes offset mismatch");
_Static_assert(offsetof(XrCoroGC, gc_requested) == XM_GC_GC_REQUESTED_OFFSET,
               "gc_requested offset mismatch");
_Static_assert(offsetof(XrCoroGC, in_gc) == XM_GC_IN_GC_OFFSET, "in_gc offset mismatch");
_Static_assert(offsetof(XrCoroGC, gc_disabled) == XM_GC_GC_DISABLED_OFFSET,
               "gc_disabled offset mismatch");
_Static_assert(offsetof(XrCoroGC, alloc_since_gc) == XM_GC_ALLOC_SINCE_GC_OFFSET,
               "alloc_since_gc offset mismatch");
_Static_assert(offsetof(XrCoroGC, object_count) == XM_GC_OBJECT_COUNT_OFFSET,
               "object_count offset mismatch");
_Static_assert(offsetof(XrCoroGC, currentwhite) == XM_GC_CURRENTWHITE_OFFSET,
               "currentwhite offset mismatch");
_Static_assert(offsetof(XrImmixBlock, local_allgc) == XM_IMMIX_BLOCK_LOCAL_ALLGC_OFFSET,
               "local_allgc offset mismatch");
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
_Static_assert(offsetof(XrGCHeader, marked) == XM_GC_HDR_MARKED_OFFSET,
               "GCHeader.marked offset mismatch");
_Static_assert(offsetof(XrGCHeader, extra) == XM_GC_HDR_EXTRA_OFFSET,
               "GCHeader.extra offset mismatch");
_Static_assert(offsetof(XrGCHeader, objsize) == XM_GC_HDR_OBJSIZE_OFFSET,
               "GCHeader.objsize offset mismatch");

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
