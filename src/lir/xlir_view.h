/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_view.h - Closed roles for natural readonly windows
 */
#ifndef XLIR_VIEW_H
#define XLIR_VIEW_H
#include "xlir_builder.h"

/* Natural readonly Slice operations use no ViewFrame or ViewCell operand.
 * Only the shared verifier may turn these closed instructions into loan facts. */
typedef enum XrLirSliceOperation {
    XR_LIR_SLICE_OP_NONE,
    XR_LIR_SLICE_OP_NEW,
    XR_LIR_SLICE_OP_REBORROW,
    XR_LIR_SLICE_OP_BIND,
    XR_LIR_SLICE_OP_LEN,
    XR_LIR_SLICE_OP_GET,
    XR_LIR_SLICE_OP_COPY,
    XR_LIR_SLICE_OP_END,
    /* Resource cleanup is a zero-operand LIR effect, never a callable role. */
    XR_LIR_SLICE_OP_CLEANUP,
    XR_LIR_SLICE_OP_FORMAL_CLEANUP,
    /* Call and return hooks are common events, not intrinsic catalog rows. */
    XR_LIR_SLICE_OP_CALL,
    XR_LIR_SLICE_OP_RETURN,
    XR_LIR_SLICE_OP_STRICT_UTF8,
    XR_LIR_SLICE_OP_LOSSY_UTF8,
    XR_LIR_SLICE_OP_COUNT
} XrLirSliceOperation;
typedef struct XrLirSliceIntrinsic {
    uint32_t id;
    XrLirSliceOperation operation;
    XrLirType element;
    /* NEW reads an actual String Cell or Array binding.
     * State reads require the exact slot payload and element, not write access.
     * Other roles consume a Slice and require TYPE_INVALID here. */
    XrLirType binding_type;
} XrLirSliceIntrinsic;
/* NEW(binding,start,limit) receives Source-normalized Array bounds.
 * NEW(String Cell) borrows the complete code-unit window.
 * REBORROW(parent,start,limit,open) keeps strict View-window semantics.
 * BIND(parent), LEN(view), GET(view,index), COPY(view), END(view) have their
 * natural typed signatures. No explicit slot is a language operand. */
XR_FUNC XrLirStatus xr_lir_slice_catalog_install(XrLirModule *);
enum XrLirSliceLibraryFlags {
    XR_LIR_SLICE_LIBRARY_DISPLAY = 1u, XR_LIR_SLICE_LIBRARY_STRING = 2u,
    XR_LIR_SLICE_LIBRARY_RANGE = 4u, XR_LIR_SLICE_LIBRARY_MAP = 8u,
    XR_LIR_SLICE_LIBRARY_STRICT_UTF8 = 16u, XR_LIR_SLICE_LIBRARY_LOSSY_UTF8 = 32u
};
XR_FUNC XrLirStatus xr_lir_slice_library_catalog_install(XrLirModule *, uint32_t flags);
XR_FUNC XrLirStatus xr_lir_slice_operation(const XrLirModule *, XrLirSliceOperation,
    XrLirType element, XrLirType binding_type, uint32_t *id);
/* This is a cold copied role/element projection. Equality of numeric ids, type
 * rows or Header addresses grants neither a live loan nor a keeper reference. */
XR_FUNC XrLirStatus xr_lir_slice_intrinsic_view(const XrLirModule *, uint32_t id,
    XrLirSliceIntrinsic *);
/* SLICE_CLEANUP has Unit type and no SSA operands. Its bits name one actual
 * local Slice value in the same function, solely as a keeper target. The
 * target may be unconstructed, non-dominating or already ended on this path.
 * Finish validates its producer and home; cleanup never reads its natural
 * bytes and leaves it logically dead. Strict END and reads still require a
 * live loan. A not-yet-allocated Frame has no published keeper to retire. */
/* SLICE_FORMAL_CLEANUP is a separate Unit/zero-operand/zero-edge effect whose
 * bits name one READ Slice formal in this function. It only ends that formal's
 * logical loan, including conditional or repeated cleanup. Its physical action
 * is NONE: no bytes, owner, slot or caller keeper is read or retired. */
#endif // XLIR_VIEW_H
