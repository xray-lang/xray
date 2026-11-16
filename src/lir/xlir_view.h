/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_view.h - Closed library roles on actual typed activation resources
 */
#ifndef XLIR_VIEW_H
#define XLIR_VIEW_H
#include "xlir_builder.h"

typedef enum XrLirViewOperation {
    XR_LIR_VIEW_OP_NONE,
#define XR_LIR_VIEW_OP(name, symbol, result, count, a, b, c, d, e, f) XR_LIR_VIEW_OP_##name,
#include "xlir_view_ops.def"
#undef XR_LIR_VIEW_OP
    XR_LIR_VIEW_OP_COUNT
} XrLirViewOperation;

/* The receiving registry owns signatures and numeric roles. Activation slot
 * constants remain real call operands and are checked against their producer. */
XR_FUNC XrLirStatus xr_lir_view_catalog_install(XrLirModule *);
XR_FUNC XrLirStatus xr_lir_view_operation(const XrLirModule *, XrLirViewOperation, uint32_t *);
XR_FUNC const char *xr_lir_view_native_symbol(XrLirViewOperation);

/* Natural readonly Array operations use no ViewFrame or ViewCell operand.
 * These closed roles are distinct from the activation catalog above;
 * only the shared verifier may turn their instructions into loan facts. */
typedef enum XrLirSliceOperation {
    XR_LIR_SLICE_OP_NONE,
    XR_LIR_SLICE_OP_NEW,
    XR_LIR_SLICE_OP_REBORROW,
    XR_LIR_SLICE_OP_BIND,
    XR_LIR_SLICE_OP_LEN,
    XR_LIR_SLICE_OP_GET,
    XR_LIR_SLICE_OP_COPY,
    XR_LIR_SLICE_OP_END,
    XR_LIR_SLICE_OP_COUNT
} XrLirSliceOperation;
typedef struct XrLirSliceIntrinsic {
    uint32_t id;
    XrLirSliceOperation operation;
    XrLirType element;
    /* NEW consumes the actual ARRAY_CELL or ARRAY_REF binding. Other roles
     * consume a Slice value and require TYPE_INVALID here. */
    XrLirType binding_type;
} XrLirSliceIntrinsic;
/* NEW(binding,start,limit) receives Source-normalized Array bounds.
 * REBORROW(parent,start,limit,open) keeps strict View-window semantics.
 * BIND(parent), LEN(view), GET(view,index), COPY(view), END(view) have their
 * natural typed signatures. No explicit slot is a language operand. */
XR_FUNC XrLirStatus xr_lir_slice_catalog_install(XrLirModule *);
enum XrLirSliceLibraryFlags {
    XR_LIR_SLICE_LIBRARY_DISPLAY = 1u, XR_LIR_SLICE_LIBRARY_STRING = 2u,
    XR_LIR_SLICE_LIBRARY_RANGE = 4u, XR_LIR_SLICE_LIBRARY_MAP = 8u
};
XR_FUNC XrLirStatus xr_lir_slice_library_catalog_install(XrLirModule *, uint32_t flags);
XR_FUNC XrLirStatus xr_lir_slice_operation(const XrLirModule *, XrLirSliceOperation,
    XrLirType element, XrLirType binding_type, uint32_t *id);
/* This is a cold copied role/element projection. Equality of numeric ids, type
 * rows or Header addresses grants neither a live loan nor a keeper reference. */
XR_FUNC XrLirStatus xr_lir_slice_intrinsic_view(const XrLirModule *, uint32_t id,
    XrLirSliceIntrinsic *);
#endif // XLIR_VIEW_H
