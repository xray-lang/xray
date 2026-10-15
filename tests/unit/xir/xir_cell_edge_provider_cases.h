/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_edge_provider_cases.h - Actual provider under a private test namespace
 */
#ifndef XIR_CELL_EDGE_PROVIDER_CASES_H
#define XIR_CELL_EDGE_PROVIDER_CASES_H

/* Exact current verifier opcode metadata used by the private provider TU. */
#include "xir/xxir_operand_roles.h"
typedef enum ResultRule {
    RULE_UNIT, RULE_BOOL, RULE_I64, RULE_INTEGER, RULE_NUMBER, RULE_FLOAT, RULE_SCALAR, RULE_VALUE, RULE_OWNED, RULE_STRING, RULE_RUNE, RULE_CALL, RULE_ATOMIC, RULE_ARRAY, RULE_NOMINAL, RULE_ROOT, RULE_PANIC
} ResultRule;
typedef struct OpRule {
    uint8_t stages;
    ResultRule result;
    uint8_t operands;
    uint8_t edges;
    bool terminal;
} OpRule;

static const OpRule op_rules[XR_XIR_OP_COUNT] = {
    {0, RULE_UNIT, 0, 0, false},
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) \
    {stages, RULE_##rule, operands, edges, terminal != 0},
#include "xir/xxir_ops.def"
#undef XR_XIR_OP
};

static uint32_t operand_count_metadata(const XrXirTypes *types,const XrXirDeclarations *declarations,
    XrXirType result,XrXirType cell,const XrXirInstruction *op) {
    if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_STORE)
        return declarations ? xr_xir_slot_payload_operands(declarations->slots,
            declarations->slot_count,op) : 1u;
    if (op->op == XR_XIR_CELL_NEW || op->op == XR_XIR_CELL_READ ||
        op->op == XR_XIR_CELL_WRITE || op->op == XR_XIR_CELL_LOCAL_WRITE)
        return xr_xir_cell_payload_operands(types,op->op == XR_XIR_CELL_NEW ? op->type : cell,op->op);
    if (xr_xir_op_uses_operand_table(op->op)) return op->args[1];
    return op->op == XR_XIR_RETURN ? (result != XR_XIR_UNIT) : op_rules[op->op].operands;
}

#define xr_xir_compile_cell_provenance_free edge_probe_xr_xir_compile_cell_provenance_free
#define xir_cell_proof_builder_free edge_probe_xir_cell_proof_builder_free
#define xir_cell_proof_builder_begin edge_probe_xir_cell_proof_builder_begin
#define xir_cell_proof_builder_head edge_probe_xir_cell_proof_builder_head
#define xir_cell_proof_builder_parameter edge_probe_xir_cell_proof_builder_parameter
#define xir_cell_proof_builder_instruction edge_probe_xir_cell_proof_builder_instruction
#define xir_cell_proof_builder_span edge_probe_xir_cell_proof_builder_span
#define xir_cell_proof_builder_function edge_probe_xir_cell_proof_builder_function
#define xir_cell_proof_builder_finish edge_probe_xir_cell_proof_builder_finish
#define xr_xir_compile_cell_provenance_verified edge_probe_xr_xir_compile_cell_provenance_verified
#define xr_xir_compile_cell_provenance_body_matches edge_probe_xr_xir_compile_cell_provenance_body_matches
#define xr_xir_cell_provenance_origin edge_probe_xr_xir_cell_provenance_origin
#define xr_xir_cell_provenance_role edge_probe_xr_xir_cell_provenance_role
#define xr_xir_compile_cell_origin_view edge_probe_xr_xir_compile_cell_origin_view
#define xr_xir_compile_cell_class_origin_view edge_probe_xr_xir_compile_cell_class_origin_view
#define xr_xir_compile_cell_access edge_probe_xr_xir_compile_cell_access
#define xr_xir_compile_cell_roles_verified edge_probe_xr_xir_compile_cell_roles_verified
XR_FUNC void xr_xir_compile_cell_provenance_free(XrXirCellProvenance *proof);
XR_FUNC void xir_cell_proof_builder_free(XrXirCellProofBuilder *builder);
XR_FUNC XrXirStatus xir_cell_proof_builder_begin(const XrXirCompileContext *context,
    const XrXirCellProofBodyRead *body,XrXirCellProofBuilder **output);
XR_FUNC XrXirStatus xir_cell_proof_builder_head(XrXirCellProofBuilder *builder,
    uint32_t f,const XrXirCellProofBodyHead *head);
XR_FUNC XrXirStatus xir_cell_proof_builder_parameter(XrXirCellProofBuilder *builder,
    uint32_t f,uint32_t value,XrXirType type);
XR_FUNC XrXirStatus xir_cell_proof_builder_instruction(XrXirCellProofBuilder *builder,
    uint32_t f,uint32_t value,XrXirType type,XrXirOp opcode,XrXirType actual_type);
XR_FUNC XrXirStatus xir_cell_proof_builder_span(XrXirCellProofBuilder *builder,
    uint32_t f,uint32_t first,uint32_t count,const XrXirCellProofInstructionShape *source);
XR_FUNC XrXirStatus xir_cell_proof_builder_function(XrXirCellProofBuilder *builder,uint32_t f);
XR_FUNC XrXirStatus xir_cell_proof_builder_finish(XrXirCellProofBuilder **input,
    XrXirCellProvenance **output);
XR_FUNC XrXirStatus xr_xir_compile_cell_provenance_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirCellProvenance **output,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_cell_provenance_body_matches(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof,const XrXirCellProofBodyRead *body,XrXirDiagnostic *diagnostic);
XR_FUNC uint32_t xr_xir_cell_provenance_origin(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t value);
XR_FUNC XrXirCellProofRole xr_xir_cell_provenance_role(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t parameter);
XR_FUNC XrXirStatus xr_xir_compile_cell_origin_view(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof, uint32_t function, uint32_t value, XrXirCellOriginView *output);
XR_FUNC XrXirStatus xr_xir_compile_cell_class_origin_view(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof,uint32_t function,uint32_t value,XrXirCellClassOriginView *output);
XR_FUNC XrXirStatus xr_xir_compile_cell_access(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof, const XrXirCellAccessRequest *request, uint32_t *output);
XR_FUNC XrXirStatus xr_xir_compile_cell_roles_verified(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t *offsets, uint8_t *roles, XrXirDiagnostic *diagnostic);
#include "xir/xxir_cell_provenance.inc.c"
#undef xr_xir_compile_cell_provenance_free
#undef xir_cell_proof_builder_free
#undef xir_cell_proof_builder_begin
#undef xir_cell_proof_builder_head
#undef xir_cell_proof_builder_parameter
#undef xir_cell_proof_builder_instruction
#undef xir_cell_proof_builder_span
#undef xir_cell_proof_builder_function
#undef xir_cell_proof_builder_finish
#undef xr_xir_compile_cell_provenance_verified
#undef xr_xir_compile_cell_provenance_body_matches
#undef xr_xir_cell_provenance_origin
#undef xr_xir_cell_provenance_role
#undef xr_xir_compile_cell_origin_view
#undef xr_xir_compile_cell_class_origin_view
#undef xr_xir_compile_cell_access
#undef xr_xir_compile_cell_roles_verified

#endif // XIR_CELL_EDGE_PROVIDER_CASES_H
