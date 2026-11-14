/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_ownership.h - Owned snapshots of verified local reference transfers
 */
#ifndef XLIR_OWNERSHIP_H
#define XLIR_OWNERSHIP_H
#include "xlir_array.h"

typedef struct XrLirOwnershipPlan XrLirOwnershipPlan;

typedef enum XrLirOwnershipFactKind {
    XR_LIR_OWNERSHIP_PARAMETER_BORROW = 1,
    XR_LIR_OWNERSHIP_BLOCK_OWNER,
    XR_LIR_OWNERSHIP_RESULT_OWNER,
    XR_LIR_OWNERSHIP_OPERAND_BORROW,
    XR_LIR_OWNERSHIP_RELEASE,
    XR_LIR_OWNERSHIP_APPEND_TRANSFER,
    XR_LIR_OWNERSHIP_RETURN_TRANSFER,
    XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER,
    XR_LIR_OWNERSHIP_ARRAY_OPERATION
} XrLirOwnershipFactKind;

/* Positions and SSA IDs belong to the exact input module: function, block,
 * instruction and edge are one-based; argument is a zero-based parameter or
 * operand ordinal. Facts are grouped by function, not an execution schedule.
 * Zero means inapplicable for all other IDs. An edge fact
 * describes that edge only, not simultaneous transfers along both branches.
 * ARRAY_OPERATION records a verified catalog operation and exact element.
 * NEW and REPEAT have no receiver; their value/type fields are zero. */
typedef struct XrLirOwnershipFact {
    XrLirOwnershipFactKind kind;
    uint32_t function, block, instruction, value, argument, edge;
    uint32_t target_block, target_value;
    XrLirType type, array_element;
    XrLirArrayOperation array_operation;
} XrLirOwnershipFact;

enum XrLirOwnershipUnmodeled {
    XR_LIR_OWNERSHIP_UNMODELED_CELL = 1u << 0,
    XR_LIR_OWNERSHIP_UNMODELED_REFERENCE = 1u << 1,
    XR_LIR_OWNERSHIP_UNMODELED_STATE = 1u << 2,
    XR_LIR_OWNERSHIP_UNMODELED_VIEW = 1u << 3,
    XR_LIR_OWNERSHIP_UNMODELED_CALLABLE = 1u << 4,
    XR_LIR_OWNERSHIP_UNMODELED_CONSTRUCTED = 1u << 5
};

typedef struct XrLirOwnershipFunction {
    uint32_t fact_offset, fact_count, unmodeled;
    bool external;
} XrLirOwnershipFunction;

/* Only direct String, Array and Map references are modeled. Other lifetime
 * verifiers still run, and their presence is reported per function. Facts do
 * not establish owner origins, runtime slots, coalescing, disjoint parameters,
 * mutation invalidation or cross-function summaries. An operand borrow is an
 * instruction-local use, not permission to extend a borrow or remove a copy.
 *
 * Creation requires a finished module and revalidates the complete final IR.
 * Call after all rebuilding transforms, including String elision. The plan
 * owns every byte, survives module retirement and has no executor dependency.
 * A later consumer must bind these IDs to that exact final module; this API
 * does not authorize applying a plan to another or rebuilt module.
 * Output must be NULL. Failure publishes nothing and preserves input/output.
 * Diagnostics borrow the input's source paths, as in xr_lir_verify. */
XR_FUNC XrLirStatus xr_lir_ownership_plan_create(const XrLirModule *,
    XrLirOwnershipPlan **, XrLirDiagnostic *);
XR_FUNC void xr_lir_ownership_plan_destroy(XrLirOwnershipPlan *);
XR_FUNC uint32_t xr_lir_ownership_function_count(const XrLirOwnershipPlan *);
XR_FUNC uint32_t xr_lir_ownership_fact_count(const XrLirOwnershipPlan *);
XR_FUNC XrLirStatus xr_lir_ownership_function_at(const XrLirOwnershipPlan *,
    uint32_t function, XrLirOwnershipFunction *);
XR_FUNC XrLirStatus xr_lir_ownership_fact_at(const XrLirOwnershipPlan *,
    uint32_t ordinal, XrLirOwnershipFact *);

#endif // XLIR_OWNERSHIP_H
