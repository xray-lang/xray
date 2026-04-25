/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_offsets.h - Unified struct field offsets for JIT/AOT codegen
 *
 * KEY CONCEPT:
 *   All hardcoded byte offsets used by JIT code generation are defined
 *   here with compile-time assertions to catch layout changes.
 *
 * WHY THIS DESIGN:
 *   - Single source of truth for all struct offsets
 *   - _Static_assert catches silent breakage when fields are added/removed
 *   - Shared between xir_codegen.c, xir_codegen_aot.c, and xir_builder.c
 */

#ifndef XIR_OFFSETS_H
#define XIR_OFFSETS_H

#include <stddef.h>
#include "../coro/xcoroutine.h"

/* ========== XrCoroutine field offsets ========== */

#define XIR_CORO_REDUCTIONS_OFFSET   offsetof(XrCoroutine, reductions)
#define XIR_CORO_GC_OFFSET           offsetof(XrCoroutine, coro_gc)
#define XIR_CORO_JIT_CTX_OFFSET      offsetof(XrCoroutine, jit_ctx)

/* ========== XrCoroutine JIT suspend/resume fields ========== */

#define XIR_CORO_RESUME_ENTRY_OFFSET   offsetof(XrCoroutine, jit_resume_entry)
#define XIR_CORO_RESUME_PROTO_OFFSET   offsetof(XrCoroutine, jit_resume_proto)
#define XIR_CORO_SUSPEND_ID_OFFSET     offsetof(XrCoroutine, jit_suspend_id)
#define XIR_CORO_SUSPEND_SMAP_OFFSET   offsetof(XrCoroutine, jit_suspend_smap_id)
#define XIR_CORO_SUSPEND_PTR_OFFSET    offsetof(XrCoroutine, jit_suspend)
// Sub-field offsets within XrJitSuspendState (for ARM64 codegen addressing)
#define XIR_SUSPEND_CALLEE_SAVED_OFF   offsetof(XrJitSuspendState, callee_saved)
#define XIR_SUSPEND_RESULT_OFF         offsetof(XrJitSuspendState, result)
#define XIR_SUSPEND_RESULT_TAG_OFF     offsetof(XrJitSuspendState, result_tag)
#define XIR_SUSPEND_SPILL_OFF          offsetof(XrJitSuspendState, spill)

/* ========== XrJitScratch field offsets (relative to jit_ctx base) ========== */

#define XIR_JIT_CALL_ARGS_OFFSET     offsetof(XrJitScratch, call_args)
#define XIR_JIT_CALL_ARG_TAGS_OFFSET offsetof(XrJitScratch, call_arg_tags)
#define XIR_JIT_CALL_PROTO_OFFSET    offsetof(XrJitScratch, call_proto)
#define XIR_JIT_CALL_CLOSURE_OFFSET  offsetof(XrJitScratch, call_closure)
#define XIR_JIT_EXCEPTION_OFFSET     offsetof(XrJitScratch, exception)
#define XIR_JIT_DEOPT_ID_OFFSET      offsetof(XrJitScratch, deopt_id)
#define XIR_JIT_DEOPT_REGS_OFFSET    offsetof(XrJitScratch, deopt_regs)
#define XIR_JIT_DEOPT_FP_REGS_OFFSET offsetof(XrJitScratch, deopt_fp_regs)
#define XIR_JIT_DEOPT_SPILL_BASE_OFFSET offsetof(XrJitScratch, deopt_spill_base)
#define XIR_JIT_DEOPT_SPILL_SAVE_OFFSET offsetof(XrJitScratch, deopt_spill_save)
#define XIR_JIT_PARAM_TAGS_OFFSET    offsetof(XrJitScratch, param_tags)
#define XIR_JIT_RET_COUNT_OFFSET     offsetof(XrJitScratch, ret_count)
#define XIR_JIT_RET_VALS_OFFSET      offsetof(XrJitScratch, ret_vals)
#define XIR_JIT_RET_TAGS_OFFSET      offsetof(XrJitScratch, ret_tags)
// GC stack map fields
#define XIR_JIT_ACTIVE_SMAP_ID_OFFSET  offsetof(XrJitScratch, active_safepoint_id)
#define XIR_JIT_ACTIVE_SMAP_OFFSET     offsetof(XrJitScratch, active_stack_map)
#define XIR_JIT_FRAME_SP_OFFSET        offsetof(XrJitScratch, jit_frame_sp)
#define XIR_JIT_SAFEPOINT_SAVED_SP_OFFSET offsetof(XrJitScratch, safepoint_saved_sp)
// JIT frame stack for GC caller-frame scanning
#define XIR_JIT_FRAME_DEPTH_OFFSET     offsetof(XrJitScratch, jit_frame_depth)
#define XIR_JIT_FRAME_STACK_OFFSET     offsetof(XrJitScratch, jit_frame_stack)
// Per-slot runtime tags (written by CALL_C codegen from XrJitResult.tag)
#define XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET offsetof(XrJitScratch, slot_runtime_tags)
// Tag from last call_c_stub: stored here instead of x1 to avoid clobbering alloc_regs[0]
#define XIR_JIT_CALL_RESULT_TAG_OFFSET   offsetof(XrJitScratch, call_result_tag)
// Scratch slot for saving/loading runtime tags (call_args[15])
#define XIR_JIT_LOAD_TAG_SCRATCH         (XIR_JIT_CALL_ARGS_OFFSET + 15 * 8)
// Guard page safepoint fields
#define XIR_JIT_SAFEPOINT_PAGE_OFFSET    offsetof(XrJitScratch, safepoint_page)
#define XIR_JIT_SAFEPOINT_RETURN_PC_OFFSET offsetof(XrJitScratch, safepoint_return_pc)

/* ========== XrClosure field offsets ========== */

#define XIR_CLOSURE_PROTO_OFFSET      16

/* ========== XrCell field offsets (32B lightweight cell) ========== */

#define XIR_CELL_VALUE_OFFSET         16   // offsetof(XrCell, value)

/* ========== XrProto field offsets ========== */

/*
 * Offsets shifted by -16 bytes after the IC tables (XrICMethodTable*,
 * XrICFieldTable*) were removed from XrProto and the smaller proto_id
 * field was added; static_asserts below validate against the live
 * struct layout.
 */
#define XIR_PROTO_JIT_ENTRY_OFFSET       344
#define XIR_PROTO_JIT_FAST_ENTRY_OFFSET  352
#define XIR_PROTO_JIT_RESUME_ENTRY_OFFSET 360

/* ========== Object layout constants ========== */

#define XIR_GC_HEADER_SIZE            16   // sizeof(XrGCHeader)
#define XIR_XRVALUE_SIZE              16   // sizeof(XrValue)
#define XIR_XRVALUE_TAG_OFFSET         0   // offsetof(XrValue, tag) — uint8_t at byte 0
#define XIR_XRVALUE_HEAP_TYPE_OFFSET   2   // offsetof(XrValue, heap_type) — uint16_t at byte 2
#define XIR_XRVALUE_PAYLOAD_OFFSET     8   // offsetof(XrValue, i/f/ptr) — at byte 8
#define XIR_GC_TYPE_OFFSET             8   // offsetof(XrGCHeader, type) — uint8_t
#define XIR_GC_EXTRA_OFFSET           10   // offsetof(XrGCHeader, extra)

// XrInstance: XrGCHeader(16) + klass*(8) + XrValue fields[]
#define XIR_INSTANCE_KLASS_OFFSET     XIR_GC_HEADER_SIZE        // 16
#define XIR_INSTANCE_FIELDS_OFFSET    (XIR_GC_HEADER_SIZE + 8)  // 24
// XrJson: XrGCHeader(16) + overflow*(8) + XrValue fields[]
#define XIR_JSON_FIELDS_OFFSET         (XIR_GC_HEADER_SIZE + 8)  // 24

/* ========== XrArray field offsets ========== */

#define XIR_ARRAY_DATA_OFFSET       16   // offsetof(XrArray, data)
#define XIR_ARRAY_LENGTH_OFFSET     24   // offsetof(XrArray, length)
#define XIR_ARRAY_ELEM_TYPE_OFFSET  40   // offsetof(XrArray, elem_type)
#define XIR_ARRAY_ELEM_SIZE_OFFSET  41   // offsetof(XrArray, elem_size)

/* ========== GC / Allocation offsets ========== */

#define XIR_IMMIX_CURSOR_OFFSET        0   // offsetof(XrImmixHeap, cursor)
#define XIR_IMMIX_LIMIT_OFFSET         8   // offsetof(XrImmixHeap, limit)
#define XIR_GC_CURRENTWHITE_OFFSET   109   // offsetof(XrCoroGC, currentwhite)
#define XIR_GC_HDR_TYPE_OFFSET         8   // offsetof(XrGCHeader, type)
#define XIR_GC_HDR_MARKED_OFFSET       9   // offsetof(XrGCHeader, marked)
#define XIR_GC_HDR_EXTRA_OFFSET       10   // offsetof(XrGCHeader, extra)
#define XIR_GC_HDR_OBJSIZE_OFFSET     12   // offsetof(XrGCHeader, objsize)

/* ========== GC bookkeeping offsets (for inline alloc_post) ========== */

#define XIR_GC_GCDEBT_OFFSET          88   // offsetof(XrCoroGC, GCdebt)
#define XIR_GC_TOTALBYTES_OFFSET      96   // offsetof(XrCoroGC, totalbytes)
#define XIR_GC_GC_REQUESTED_OFFSET   104   // offsetof(XrCoroGC, gc_requested)
#define XIR_GC_IN_GC_OFFSET          110   // offsetof(XrCoroGC, in_gc)
#define XIR_GC_GC_DISABLED_OFFSET    111   // offsetof(XrCoroGC, gc_disabled)
#define XIR_GC_ALLOC_SINCE_GC_OFFSET 120   // offsetof(XrCoroGC, alloc_since_gc)
#define XIR_GC_OBJECT_COUNT_OFFSET   312   // offsetof(XrCoroGC, object_count)
#define XIR_IMMIX_BLOCK_LOCAL_ALLGC_OFFSET  24  // offsetof(XrImmixBlock, local_allgc)
#define XIR_IMMIX_BLOCK_ALLOC_MARKS_OFFSET   8  // offsetof(XrImmixBlock, alloc_marks)
#define XIR_IMMIX_BLOCK_ALLOC_COUNT_OFFSET  40  // offsetof(XrImmixBlock, alloc_count)
#define XIR_IMMIX_BLOCK_ALLOC_BYTES_OFFSET  48  // offsetof(XrImmixBlock, alloc_bytes)
#define XIR_IMMIX_BLOCK_SIZE_MASK    0x3FFF      // XR_IMMIX_BLOCK_SIZE - 1
#define XIR_IMMIX_LINE_SIZE_SHIFT        7       // log2(128) = 7

/* ========== Compile-time offset verification ========== */

// Include struct definitions only when verifying (not in codegen hot path)
#ifdef XIR_VERIFY_OFFSETS

#include "../coro/xcoroutine.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/gc/xgc_header.h"

_Static_assert(offsetof(XrCoroutine, reductions)      == XIR_CORO_REDUCTIONS_OFFSET,   "reductions offset mismatch");
_Static_assert(offsetof(XrCoroutine, coro_gc)          == XIR_CORO_GC_OFFSET,           "coro_gc offset mismatch");
_Static_assert(sizeof(XrValue)                         == XIR_XRVALUE_SIZE,             "XrValue size mismatch");
_Static_assert(offsetof(XrValue, tag)                  == XIR_XRVALUE_TAG_OFFSET,       "XrValue.tag offset mismatch");
_Static_assert(sizeof(XrGCHeader)                      == XIR_GC_HEADER_SIZE,           "GCHeader size mismatch");
_Static_assert(offsetof(XrGCHeader, type)              == XIR_GC_TYPE_OFFSET,           "GCHeader.type offset mismatch");

/* JIT multi-return scratch: ret_vals[] and ret_tags[] must be 8-byte aligned
 * for ARM64 STR/LDR instructions. Using int64_t elements guarantees this. */
_Static_assert(XIR_JIT_RET_COUNT_OFFSET % 8 == 0, "ret_count must be 8-byte aligned for ARM64 STR");
_Static_assert(XIR_JIT_RET_VALS_OFFSET % 8 == 0, "ret_vals must be 8-byte aligned for ARM64");
_Static_assert(XIR_JIT_RET_TAGS_OFFSET % 8 == 0, "ret_tags must be 8-byte aligned for ARM64");
_Static_assert(sizeof(((XrJitScratch*)0)->ret_tags[0]) == 8, "ret_tags elements must be 8 bytes for ARM64 alignment");
_Static_assert(sizeof(((XrJitScratch*)0)->slot_runtime_tags) == 256, "slot_runtime_tags must be 256 bytes");

#include "../runtime/value/xchunk.h"
_Static_assert(offsetof(XrProto, jit_entry)             == XIR_PROTO_JIT_ENTRY_OFFSET,      "jit_entry offset mismatch");
_Static_assert(offsetof(XrProto, jit_fast_entry)        == XIR_PROTO_JIT_FAST_ENTRY_OFFSET, "jit_fast_entry offset mismatch");

#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/ximmix.h"
_Static_assert(offsetof(XrCoroGC, GCdebt)              == XIR_GC_GCDEBT_OFFSET,         "GCdebt offset mismatch");
_Static_assert(offsetof(XrCoroGC, totalbytes)           == XIR_GC_TOTALBYTES_OFFSET,     "totalbytes offset mismatch");
_Static_assert(offsetof(XrCoroGC, gc_requested)         == XIR_GC_GC_REQUESTED_OFFSET,   "gc_requested offset mismatch");
_Static_assert(offsetof(XrCoroGC, in_gc)                == XIR_GC_IN_GC_OFFSET,          "in_gc offset mismatch");
_Static_assert(offsetof(XrCoroGC, gc_disabled)          == XIR_GC_GC_DISABLED_OFFSET,    "gc_disabled offset mismatch");
_Static_assert(offsetof(XrCoroGC, alloc_since_gc)       == XIR_GC_ALLOC_SINCE_GC_OFFSET, "alloc_since_gc offset mismatch");
_Static_assert(offsetof(XrCoroGC, object_count)         == XIR_GC_OBJECT_COUNT_OFFSET,   "object_count offset mismatch");
_Static_assert(offsetof(XrCoroGC, currentwhite)         == XIR_GC_CURRENTWHITE_OFFSET,   "currentwhite offset mismatch");
_Static_assert(offsetof(XrImmixBlock, local_allgc)      == XIR_IMMIX_BLOCK_LOCAL_ALLGC_OFFSET, "local_allgc offset mismatch");
_Static_assert(offsetof(XrImmixBlock, alloc_marks)      == XIR_IMMIX_BLOCK_ALLOC_MARKS_OFFSET, "alloc_marks offset mismatch");
_Static_assert(offsetof(XrImmixBlock, alloc_count)      == XIR_IMMIX_BLOCK_ALLOC_COUNT_OFFSET, "alloc_count offset mismatch");
_Static_assert(offsetof(XrImmixBlock, alloc_bytes)      == XIR_IMMIX_BLOCK_ALLOC_BYTES_OFFSET, "alloc_bytes offset mismatch");
_Static_assert(offsetof(XrImmixHeap, cursor)            == XIR_IMMIX_CURSOR_OFFSET,      "ImmixHeap.cursor offset mismatch");
_Static_assert(offsetof(XrImmixHeap, limit)             == XIR_IMMIX_LIMIT_OFFSET,       "ImmixHeap.limit offset mismatch");

// XrGCHeader detailed field checks
_Static_assert(offsetof(XrGCHeader, marked)             == XIR_GC_HDR_MARKED_OFFSET,     "GCHeader.marked offset mismatch");
_Static_assert(offsetof(XrGCHeader, extra)              == XIR_GC_HDR_EXTRA_OFFSET,      "GCHeader.extra offset mismatch");
_Static_assert(offsetof(XrGCHeader, objsize)            == XIR_GC_HDR_OBJSIZE_OFFSET,    "GCHeader.objsize offset mismatch");

// XrValue detailed field checks
_Static_assert(offsetof(XrValue, heap_type)             == XIR_XRVALUE_HEAP_TYPE_OFFSET, "XrValue.heap_type offset mismatch");
_Static_assert(offsetof(XrValue, i)                     == XIR_XRVALUE_PAYLOAD_OFFSET,   "XrValue payload offset mismatch");

// XrClosure / XrCell checks
#include "../runtime/xexec_frame.h"
#include "../runtime/closure/xcell.h"
_Static_assert(offsetof(XrClosure, proto)               == XIR_CLOSURE_PROTO_OFFSET,     "Closure.proto offset mismatch");

// JIT suspend state struct layout must match the old int64_t[40] layout
_Static_assert(sizeof(XrJitSuspendState) == 40 * sizeof(int64_t), "XrJitSuspendState size mismatch");
_Static_assert(XIR_SUSPEND_CALLEE_SAVED_OFF == 15 * 8, "callee_saved offset mismatch");
_Static_assert(XIR_SUSPEND_RESULT_OFF == 23 * 8, "result offset mismatch");
_Static_assert(XIR_SUSPEND_RESULT_TAG_OFF == 24 * 8, "result_tag offset mismatch");
_Static_assert(XIR_SUSPEND_SPILL_OFF == 25 * 8, "spill offset mismatch");
_Static_assert(offsetof(XrCell, value)                  == XIR_CELL_VALUE_OFFSET,        "Cell.value offset mismatch");

// XrArray checks
#include "../runtime/object/xarray.h"
_Static_assert(offsetof(XrArray, data)                  == XIR_ARRAY_DATA_OFFSET,        "Array.data offset mismatch");
_Static_assert(offsetof(XrArray, length)                == XIR_ARRAY_LENGTH_OFFSET,      "Array.length offset mismatch");
_Static_assert(offsetof(XrArray, elem_type)             == XIR_ARRAY_ELEM_TYPE_OFFSET,   "Array.elem_type offset mismatch");
_Static_assert(offsetof(XrArray, elem_size)             == XIR_ARRAY_ELEM_SIZE_OFFSET,   "Array.elem_size offset mismatch");

#endif // XIR_VERIFY_OFFSETS

#endif // XIR_OFFSETS_H
