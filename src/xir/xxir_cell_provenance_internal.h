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
#include "xxir_metadata_domain_internal.h"

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
typedef struct XrXirCellProofBodyHead {
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
    uint32_t instruction_count;
    const uint32_t *operands;
    uint32_t operand_count;
} XrXirCellProofBodyHead;
typedef struct XrXirCellProofInstructionShape {
    XrXirOp op;
    XrXirType type;
} XrXirCellProofInstructionShape;
/* A construction-local stream cursor contains no permission or completed
 * proof. Its owner validates every preceding canonical run before advancing. */
typedef struct XrXirCellProofSpanCursor {
    uint32_t function,next,offset,run;
    XrXirCellProofInstructionShape shape;
} XrXirCellProofSpanCursor;

/* This cursor belongs to one complete physical walk. It contains numeric
 * positions only; no borrowed reference or permission survives the request. */
typedef struct XrXirCellProofRunCursor {
    uint32_t function,next,offset,run;
} XrXirCellProofRunCursor;
typedef struct XrXirCellProofRun {
    uint32_t first,count,varying;
    XrXirInstruction operation;
} XrXirCellProofRun;

static inline bool xir_cell_proof_scalar_shape(XrXirOp opcode,XrXirType type) {
    switch (opcode) {
    case XR_XIR_CONST_BOOL:return type==XR_XIR_BOOL;
    case XR_XIR_CONST_INT:return type==XR_XIR_I64;
    case XR_XIR_CONST_FLOAT:return type==XR_XIR_F64;
    case XR_XIR_CONST_RUNE:return type==XR_XIR_RUNE;
    case XR_XIR_JUMP:return type==XR_XIR_UNIT;
    default:return false;
    }
}
/* This metadata view grants no body admission. Ordinary callbacks read one
 * complete generation. The optional span reader consumes only its owner's
 * unpublished construction prefix; the builder checks the real complete head
 * and canonical EOF before publication. The complete range reader is independent of this prefix. All requests charge the live ledger. */
typedef struct XrXirCellProofBodyRead XrXirCellProofBodyRead;
struct XrXirCellProofBodyRead {
    XirMetadataDomain domain;
    XrCompileResources *resources;
    const void *owner;
    XrXirStatus (*head)(const XrXirCompileContext *,const void *,uint32_t,XrXirCellProofBodyHead *);
    /* The returned row lives only in this call: genuine raw storage returns its
     * actual row; owned storage decodes into the caller-provided stack. */
    XrXirStatus (*instruction)(const XrXirCompileContext *,const void *,uint32_t,uint32_t,
        XrXirInstruction *,const XrXirInstruction **);
    XrXirStatus (*type)(const XrXirCompileContext *,const void *,uint32_t,uint32_t,XrXirType *);
    XrXirStatus (*shape)(const XrXirCompileContext *,const void *,uint32_t,uint32_t,XrXirCellProofInstructionShape *);
    XrXirStatus (*span)(const XrXirCompileContext *,const void *,uint32_t,uint32_t,uint32_t,XrXirCellProofSpanCursor *);
    /* Complete owned runs retain every physical word in the canonical codec.
     * The callback reads the actual first row, never a semantic permission. */
    XrXirStatus (*range)(const XrXirCompileContext *,const XrXirCellProofBodyRead *,
        XrXirCellProofRunCursor *,XrXirCellProofRun *);
};

/* One construction-local provider builder consumes actual writer rows. It
 * holds no authority; complete role/edge/solve/validation follows inventory.
 * The verified caller must own the full same-generation body at finish. */
typedef struct XrXirCellProofBuilder XrXirCellProofBuilder;
XR_FUNC XrXirStatus xir_cell_proof_builder_begin(const XrXirCompileContext *context,
    const XrXirCellProofBodyRead *body,XrXirCellProofBuilder **output);
XR_FUNC XrXirStatus xir_cell_proof_builder_head(XrXirCellProofBuilder *builder,
    uint32_t function,const XrXirCellProofBodyHead *head);
XR_FUNC XrXirStatus xir_cell_proof_builder_parameter(XrXirCellProofBuilder *builder,
    uint32_t function,uint32_t value,XrXirType type);
XR_FUNC XrXirStatus xir_cell_proof_builder_instruction(XrXirCellProofBuilder *builder,
    uint32_t function,uint32_t value,XrXirType type,XrXirOp opcode,XrXirType actual_type);
/* Only a genuine optional reader can submit a constant physical span.
 * The ordinary function end still validates the complete stream and head. */
XR_FUNC XrXirStatus xir_cell_proof_builder_span(XrXirCellProofBuilder *builder,
    uint32_t function,uint32_t first,uint32_t count,const XrXirCellProofInstructionShape *source);
XR_FUNC XrXirStatus xir_cell_proof_builder_function(XrXirCellProofBuilder *builder,uint32_t function);
XR_FUNC XrXirStatus xir_cell_proof_builder_finish(XrXirCellProofBuilder **builder,
    XrXirCellProvenance **output);
XR_FUNC void xir_cell_proof_builder_free(XrXirCellProofBuilder *builder);

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
typedef struct XrXirCellClassRoot {
    uint32_t place,value;
} XrXirCellClassRoot;
typedef struct XrXirCellClassOriginView {
    XrXirCellOriginView cells;
    uint32_t residual_intrinsic_mask;
    const uint64_t *dependencies;
    uint32_t word_count;
    const XrXirCellClassRoot *roots;
    uint32_t root_count;
} XrXirCellClassOriginView;
/* Requires full structural/type/SSA/initialization checks; does not re-enter them.
 * Output owns all facts and never borrows the module, declarations or context. */
XR_FUNC XrXirStatus xr_xir_compile_cell_provenance_verified(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirCellProvenance **output, XrXirDiagnostic *diagnostic);
/* Replays the same provider core against a fully admitted body generation.
 * No permission or second proof is published; every owned fact must match. */
XR_FUNC XrXirStatus xr_xir_compile_cell_provenance_body_matches(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof,const XrXirCellProofBodyRead *body,XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_cell_provenance_free(XrXirCellProvenance *proof);
XR_FUNC uint32_t xr_xir_cell_provenance_origin(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t value);
XR_FUNC XrXirCellProofRole xr_xir_cell_provenance_role(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t parameter);
/* Borrowed immutable compact dependency bits map through physical parameter IDs. */
XR_FUNC XrXirStatus xr_xir_compile_cell_origin_view(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof, uint32_t function, uint32_t value, XrXirCellOriginView *output);
/* Empty output only. The original cells view preserves all conservative
 * UNKNOWN bits. Substituting Class dependencies requires every true subject's
 * current equation mask/rank; these facts alone never grant permission. */
XR_FUNC XrXirStatus xr_xir_compile_cell_class_origin_view(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof,uint32_t function,uint32_t value,XrXirCellClassOriginView *output);
/* NULL owners is the open definition context; every actual parameter dependency
 * then contributes UNKNOWN. Explicit owners cover the complete physical prefix. */
XR_FUNC XrXirStatus xr_xir_compile_cell_access(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof, const XrXirCellAccessRequest *request, uint32_t *output);
/* Offsets and byte roles are caller-owned outputs, published only after success. */
XR_FUNC XrXirStatus xr_xir_compile_cell_roles_verified(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t *offsets, uint8_t *roles, XrXirDiagnostic *diagnostic);
#endif // XXIR_CELL_PROVENANCE_INTERNAL_H
