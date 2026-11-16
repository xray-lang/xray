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
#include "xlir_map.h"
#include "xlir_callable_facts.h"

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
    XR_LIR_OWNERSHIP_ARRAY_OPERATION,
    XR_LIR_OWNERSHIP_BINDING_NEW,
    XR_LIR_OWNERSHIP_BINDING_RELEASE,
    XR_LIR_OWNERSHIP_BINDING_BORROW,
    XR_LIR_OWNERSHIP_BINDING_READ,
    XR_LIR_OWNERSHIP_BINDING_WRITE,
    XR_LIR_OWNERSHIP_MAP_OPERATION,
    XR_LIR_OWNERSHIP_CALLABLE_VALUE,
    XR_LIR_OWNERSHIP_CALLABLE_MATERIALIZE,
    XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT,
    XR_LIR_OWNERSHIP_VIEW_VALUE,
    XR_LIR_OWNERSHIP_VIEW_END
} XrLirOwnershipFactKind;

/* Transfer semantics belong to the verified event, not to a type's possible
 * owner leaves or a receiving home's slot count. BORROW copies the complete
 * borrowed representation without acquiring or releasing any owner. */
typedef enum XrLirOwnershipTransferMode {
    XR_LIR_OWNERSHIP_TRANSFER_NONE,
    XR_LIR_OWNERSHIP_TRANSFER_BORROW,
    XR_LIR_OWNERSHIP_TRANSFER_COPY,
    XR_LIR_OWNERSHIP_TRANSFER_MOVE
} XrLirOwnershipTransferMode;

/* Positions and SSA IDs belong to the exact input module: function, block,
 * instruction and edge are one-based; argument is a zero-based parameter or
 * operand ordinal. Facts are grouped by function, not an execution schedule.
 * Zero means inapplicable for all other IDs. An edge fact
 * describes that edge only, not simultaneous transfers along both branches.
 * ARRAY_OPERATION records a verified catalog operation and exact element.
 * NEW and REPEAT have no receiver; their value/type fields are zero.
 * MAP_OPERATION uses map_operation instead of array_operation. BINDING facts
 * name the actual Cell/Ref in value. target_value is the initializer, snapshot,
 * replacement, or BORROW source; RELEASE has no target. These are positions
 * already checked by the independent Cell/Ref CFG verifier, not permissions.
 * CALLABLE_VALUE copies one constructor transport row; target_value is its
 * alias source and callable_function is only an exact FUNCTION_REF target.
 * MATERIALIZE requests a real zero-capture environment at that constructor.
 * SCOPE_EXIT names the kept source/result in value/target_value and the scope
 * in callable_scope. Publish that keep before the following same-position
 * RELEASE facts. Selected callable edges COPY owned sources, which can remain
 * live or feed multiple parameters; other owned-value edges MOVE. A borrowed
 * callable return COPYs into the caller's owning landing. Transfer modes do
 * not replace scope or escaping validation. */
typedef struct XrLirOwnershipFact {
    XrLirOwnershipFactKind kind;
    uint32_t function, block, instruction, value, argument, edge;
    uint32_t target_block, target_value;
    XrLirType type, array_element;
    XrLirArrayOperation array_operation;
    XrLirMapOperation map_operation;
    XrLirCallableTransport callable_transport;
    XrLirOp callable_operation;
    uint32_t callable_function, callable_scope;
    bool callable_materialize;
    XrLirOwnershipTransferMode transfer_mode;
    /* A natural Slice has no owner leaf. This separate mode describes only
     * its private physical keeper; it cannot create or extend a logical loan. */
    uint32_t view_operation;
    XrLirOwnershipTransferMode hold_transfer_mode;
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

/* The common receiving value-layout producer supplies these exact final-module
 * Record/Nullable facts from its sealed ValuePlan owned-leaf inventory. Core
 * ownership collection does not infer physical layouts or count child leaves.
 * Rows are strictly ordered by type; every used Record/Nullable requires a row,
 * including POD rows whose has_owned is false. */
typedef struct XrLirOwnershipTypeFact {
    XrLirType type;
    bool has_owned;
} XrLirOwnershipTypeFact;
typedef struct XrLirOwnershipRequest {
    const XrLirModule *module;
    const XrLirOwnershipTypeFact *types;
    uint32_t type_count;
} XrLirOwnershipRequest;

/* Direct String, Array, Map and described Record/Nullable owners are modeled.
 * Known POD values emit no RC events. The current boxed IR's explicit value
 * lifetime is still verified even for a target inline POD representation.
 * Other lifetime
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
XR_FUNC XrLirStatus xr_lir_ownership_plan_create(const XrLirOwnershipRequest *,
    XrLirOwnershipPlan **, XrLirDiagnostic *);
XR_FUNC void xr_lir_ownership_plan_destroy(XrLirOwnershipPlan *);
XR_FUNC uint32_t xr_lir_ownership_function_count(const XrLirOwnershipPlan *);
XR_FUNC uint32_t xr_lir_ownership_fact_count(const XrLirOwnershipPlan *);
XR_FUNC XrLirStatus xr_lir_ownership_function_at(const XrLirOwnershipPlan *,
    uint32_t function, XrLirOwnershipFunction *);
XR_FUNC XrLirStatus xr_lir_ownership_fact_at(const XrLirOwnershipPlan *,
    uint32_t ordinal, XrLirOwnershipFact *);

#endif // XLIR_OWNERSHIP_H
