/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_provenance_internal.h - Owned finite cell origin and scope proofs
 *
 * KEY CONCEPT:
 *   Actual producer and capture edges establish ownership; types never do.
 */
#ifndef XXIR_CELL_PROVENANCE_INTERNAL_H
#define XXIR_CELL_PROVENANCE_INTERNAL_H
#include "xxir.h"

#define XR_XIR_CELL_ORIGIN_OWNED 1u
#define XR_XIR_CELL_ORIGIN_SCOPED 2u
#define XR_XIR_CELL_ORIGIN_MODULE 4u
#define XR_XIR_CELL_ORIGIN_UNKNOWN 8u
/* An authenticated scoped owner can retain an unresolved execution origin. */
#define XR_XIR_CELL_ORIGIN_ROOT_UNRESOLVED 16u
#define XR_XIR_CELL_ACCESS_ROOT 1u
#define XR_XIR_CELL_ACCESS_UNKNOWN 2u
typedef enum XrXirCellProofRole {
    XR_XIR_CELL_PROOF_UNKNOWN, XR_XIR_CELL_PROOF_OWNED_CAPTURE,
    XR_XIR_CELL_PROOF_SCOPED_REF, XR_XIR_CELL_PROOF_LEXICAL_CLEANUP
} XrXirCellProofRole;
typedef struct XrXirCellProvenance XrXirCellProvenance;
typedef struct XrXirCellAccessRequest {
    uint32_t function, value;
    const uint8_t *parameter_owners;
    uint32_t parameter_count;
} XrXirCellAccessRequest;
typedef struct XrXirCellOriginView {
    uint32_t intrinsic_mask;
    const uint64_t *dependencies;
    uint32_t word_count;
    const uint32_t *parameters;
    uint32_t parameter_count;
} XrXirCellOriginView;
/* Requires full structural/type/SSA/initialization checks; does not re-enter them.
 * Output owns all facts and never borrows the module, declarations or context. */
XR_FUNC XrXirStatus xr_xir_compile_cell_provenance_verified(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirCellProvenance **output, XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_cell_provenance_free(XrXirCellProvenance *proof);
XR_FUNC uint32_t xr_xir_cell_provenance_origin(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t value);
XR_FUNC XrXirCellProofRole xr_xir_cell_provenance_role(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t parameter);
/* Borrowed immutable compact dependency bits map through physical parameter IDs. */
XR_FUNC XrXirStatus xr_xir_compile_cell_origin_view(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof, uint32_t function, uint32_t value, XrXirCellOriginView *output);
/* NULL owners is the open definition context; every actual parameter dependency
 * then contributes UNKNOWN. Explicit owners cover the complete physical prefix. */
XR_FUNC XrXirStatus xr_xir_compile_cell_access(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof, const XrXirCellAccessRequest *request, uint32_t *output);
/* Offsets and byte roles are caller-owned outputs, published only after success. */
XR_FUNC XrXirStatus xr_xir_compile_cell_roles_verified(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t *offsets, uint8_t *roles, XrXirDiagnostic *diagnostic);
#endif // XXIR_CELL_PROVENANCE_INTERNAL_H
