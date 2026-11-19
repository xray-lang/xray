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
#include "xlir_string.h"
#include "xlir_callable_facts.h"
#include "xlir_slice_facts.h"

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
    XR_LIR_OWNERSHIP_VIEW_END,
    XR_LIR_OWNERSHIP_CELL_VALUE,
    XR_LIR_OWNERSHIP_CELL_OPERATION,
    XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT,
    XR_LIR_OWNERSHIP_STRING_OPERATION,
    XR_LIR_OWNERSHIP_SET_OPERATION
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

/* Logical cleanup always ends its loan. The optional physical no-op requires
 * a fresh common CFG proof that this activation never constructed its keeper. */
typedef enum XrLirSliceCleanupMode {
    XR_LIR_SLICE_CLEANUP_CLEAR,
    XR_LIR_SLICE_CLEANUP_UNCONSTRUCTED
} XrLirSliceCleanupMode;

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
    /* The existing dedicated SET_* instruction, not an intrinsic or permission. */
    XrLirOp set_operation;
    XrLirCallableTransport callable_transport;
    XrLirOp callable_operation;
    uint32_t callable_function, callable_scope;
    bool callable_materialize;
    XrLirOwnershipTransferMode transfer_mode;
    /* A natural Slice has no owner leaf. This separate mode describes only
     * its private physical keeper; it cannot create or extend a logical loan. */
    /* VIEW_VALUE: value is the new Slice SSA; target_value is its real Array
     * binding for NEW, or its exact source Slice for BIND/REBORROW. Parameters
     * use argument as their ordinal and have no target or keeper mode.
     * VIEW_END: value is the exact ended SSA. Normalized scope cleanup produces
     * one END for each actual semantic death, never a readable empty value.
     * Selected edges keep value/target_value as source/target Slice SSA ids.
     * Natural transfer_mode is BORROW; hold_transfer_mode is a separate real
     * keeper COPY/MOVE/BORROW fact, not Slice's owned_count. */
    /* CLEANUP is a resource target and has no ordinary SSA operand read. */
    uint32_t view_operation;
    XrLirSliceCleanupMode view_cleanup;
    XrLirOwnershipTransferMode hold_transfer_mode;
    XrLirSliceStorage view_storage;
    /* Actual zero-based formal origin, UINT32_MAX when inapplicable. RETURN
     * names the function's proven origin; CALL names the callee's ordinal. */
    uint32_t view_parameter;
    /* CELL_VALUE describes a real Cell identity and lexical owner; NEW/COPY/
     * BIND keep the source live. A local KEEP moves, a borrowed KEEP copies.
     * CELL_OPERATION value is the actual Cell SSA; target_value is its
     * initializer, read result, replacement or Map payload-ref result.
     * VALUE is metadata except for its explicit alias transfer. OPERATION is
     * the single NEW/READ/WRITE/BORROW_MAP hook. SCOPE_EXIT publishes its keep
     * before following same-position RELEASE events, which clear real slots
     * without reading possibly retired natural values. A normal RET first
     * uses RETURN_TRANSFER; its scope-zero exit has no second kept value.
     * The payload type is exact and does not grant mutation permission. */
    XrLirCellOperation cell_operation;
    XrLirType cell_payload;
    XrLirCellTransport cell_transport;
    uint32_t cell_scope;
    /* Unit String append borrows value's actual binding and target_value's
     * suffix. It neither consumes the binding nor produces an SSA owner.
     * BINDING facts also retain exact String Ref roles when present. */
    XrLirStringOperation string_operation;
    /* A same-block LOCAL Array<i64> NEW/READ is followed
     * immediately by the named source/Cell DROP. MOVE describes the proposed
     * reference forwarding, not the original logical transfer_mode. Consumers
     * retain both original events as the dynamic fallback. This proves neither runtime busy /
     * pending / old-target state nor equivalence of retain-failure behavior,
     * and never authorizes ignoring callbacks or coalescing physical homes. */
    XrLirOwnershipTransferMode forwarding_mode;
    uint32_t forwarding_drop;
    /* Original Array operation SSA identities. Zero means inapplicable;
     * these describe the checked operation and grant no access permission. */
    uint32_t array_result, array_index, array_item;
} XrLirOwnershipFact;

enum XrLirOwnershipUnmodeled {
    XR_LIR_OWNERSHIP_UNMODELED_CELL = 1u << 0,
    XR_LIR_OWNERSHIP_UNMODELED_REFERENCE = 1u << 1,
    XR_LIR_OWNERSHIP_UNMODELED_STATE = 1u << 2,
    XR_LIR_OWNERSHIP_UNMODELED_VIEW = 1u << 3,
    XR_LIR_OWNERSHIP_UNMODELED_CALLABLE = 1u << 4,
    XR_LIR_OWNERSHIP_UNMODELED_CONSTRUCTED = 1u << 5
};

typedef enum XrLirArraySetRegionStepKind {
    XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE = 1,
    XR_LIR_ARRAY_SET_REGION_EDGE_MOVE,
    XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE,
    XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE,
    XR_LIR_ARRAY_SET_REGION_EXIT_MOVE
} XrLirArraySetRegionStepKind;

/* Numeric descriptions of structurally closed local Array<i64> loops. They
 * grant no physical aliasing, borrow, bounds, uniqueness or mutation right.
 * Every future execution must preserve the complete original first iteration,
 * including its RHS, COPY failures, COW and error ordering. A numeric range
 * shape authorizes no runtime access; ordinary events/homes remain intact.
 * canonical_value alone does not merge ordinary physical homes. The separate
 * optional owner_home fact describes only its admitted unobserved window;
 * entry_value and exit_value retain the actual outside owners.
 * NEW/READ_CANDIDATE preserve a COPY plus its adjacent logical death; they are
 * not original MOVE events. source dies at drop, or at the selected MOVE edge.
 * Positions/edges are one-based, and first_step is a zero-based table offset. */
typedef struct XrLirArraySetRegionStep {
    XrLirArraySetRegionStepKind kind;
    uint32_t block, instruction, edge, source, target, drop;
} XrLirArraySetRegionStep;

enum {
    XR_LIR_ARRAY_SET_RANGE_NONE,
    XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN,
    XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT
};
/* An exact conditional arithmetic shape, independent of creator lifetimes.
 * NONE has every other field zero. The unit-forward shape still requires a
 * runtime nonnegative IV below an unchanged length representable by I64,
 * after the complete ordinary first iteration and existing runtime admission.
 * For a guarded limit, length instead names an invariant I64 bound. A consumer
 * must also prove at entry that the bound is at most the actual backing length.
 * Instruction positions are one-based; iv_argument is a zero-based ordinal. */
typedef struct XrLirArraySetRegionRange {
    uint32_t kind, iv, iv_argument, entry_iv, length, length_instruction;
    uint32_t condition, condition_instruction, increment, increment_block;
    uint32_t increment_instruction, one, latch_iv;
} XrLirArraySetRegionRange;

enum {
    XR_LIR_ARRAY_SET_OWNER_HOME_NONE,
    XR_LIR_ARRAY_SET_OWNER_HOME_CANONICAL_WINDOW,
    XR_LIR_ARRAY_SET_OWNER_HOME_READ_ONLY_WINDOW
};
/* An optional physical equivalence for an uninterrupted admitted window, not
 * a change to ordinary homes, COPY/death events or access permissions. Ordinals
 * are one-based within this region's step slice. NONE has both ordinals zero.
 * The producer discharges every step in [first_active_step,exit_step) as one
 * live token at the real canonical home. ENTRY remains ordinary; exit_step is
 * the exact original EXIT_MOVE. Full first-trip/runtime/range admission still
 * precedes activation. Before any exit or observation, transfer the real
 * canonical owner to its original empty exit home and clear canonical. A cache
 * only borrows that owner. The READ_ONLY kind has the same storage discipline
 * with one proven element GET and no mutation; it does not reinterpret a
 * CELL_READ/death pair as an element read. Consumers may ignore either kind. */
typedef struct XrLirArraySetRegionOwnerHome {
    uint32_t kind, first_active_step, exit_step;
} XrLirArraySetRegionOwnerHome;

enum {
    XR_LIR_ARRAY_REGION_PRIMARY_NONE,
    XR_LIR_ARRAY_REGION_PRIMARY_SET,
    XR_LIR_ARRAY_REGION_PRIMARY_GET,
    XR_LIR_ARRAY_REGION_PRIMARY_PUSH
};
enum { XR_LIR_ARRAY_PUSH_CAPACITY_NONE, XR_LIR_ARRAY_PUSH_CAPACITY_UNIT_CURRENT };
/* Conditional capacity-window identity, distinct from unchanged Array LEN.
 * The restart is before the original header. The sole LOCAL PUSH item is
 * CURRENT and the IV advances by one. All other scalar parameters are
 * forwarded unchanged, including a loop-invariant end carried in a header
 * parameter. Other operations are nonobserving scalar computations or exact
 * token transfers. Parameters have end_instruction zero, and function
 * parameters also have end_block zero.
 * This does not authorize access or prove any runtime value/capacity/RC.
 * In particular end-IV is not safe until dynamic signed/range checks pass.
 * A future consumer retains the ordinary first trip and must restore original
 * homes before growth, COW, failure or observation. Step anchors are one-based
 * in this window's slice; checkpoint is the original latch back to canonical.
 * NONE has all fields zero. */
typedef struct XrLirArrayPushCapacity {
    uint32_t kind, iv, iv_argument, entry_iv, end, end_block, end_instruction;
    uint32_t condition, condition_instruction, increment, increment_block, increment_instruction;
    uint32_t one, latch_iv, item, first_active_step, checkpoint_step;
} XrLirArrayPushCapacity;
/* The primary access is an original element operation, not CELL_READ/death.
 * SET retains its existing body contract. GET names the sole element read in
 * a closed unit/same-length read-only loop; no mutation permission follows. */
typedef struct XrLirArraySetRegion {
    uint32_t function, header, entry_block, entry_edge, latch;
    uint32_t exit_edge, exit_block, entry_value, canonical_value, exit_value;
    uint32_t body_block_count, primary_kind, primary_block, primary_instruction, first_step, step_count;
    XrLirArraySetRegionRange range;
    XrLirArraySetRegionOwnerHome owner_home;
    /* One-based GET in primary_block for a current-element I64 expression
     * ending at the primary SET. No changed scalar recurrence except the IV.
     * Consumers must still run the original final iteration and owner exit. */
    uint32_t element_read;
    XrLirArrayPushCapacity push_capacity;
} XrLirArraySetRegion;

typedef struct XrLirOwnershipFunction {
    uint32_t fact_offset, fact_count, unmodeled;
    uint32_t region_offset, region_count;
    uint32_t push_window_offset, push_window_count;
    bool external;
} XrLirOwnershipFunction;

/* The common receiving value-layout producer supplies these exact final-module
 * Record/Nullable/Cell facts from its sealed ValuePlan owned-leaf inventory. Core
 * ownership collection does not infer physical layouts or count child leaves.
 * Rows are strictly ordered by type; every used Record/Nullable requires a row,
 * including POD rows whose has_owned is false. A missing Cell row preserves
 * UNMODELED_CELL; a supplied Cell row must describe its actual owning leaf. */
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
XR_FUNC uint32_t xr_lir_ownership_array_region_count(const XrLirOwnershipPlan *);
XR_FUNC uint32_t xr_lir_ownership_array_region_step_count(const XrLirOwnershipPlan *);
/* Invalid row queries preserve the output; successful rows are owned copies. */
XR_FUNC XrLirStatus xr_lir_ownership_array_region_at(const XrLirOwnershipPlan *,
    uint32_t ordinal, XrLirArraySetRegion *);
XR_FUNC XrLirStatus xr_lir_ownership_array_region_step_at(const XrLirOwnershipPlan *,
    uint32_t ordinal, XrLirArraySetRegionStep *);
/* Separate, owned numeric tables; existing SET/GET consumers do not receive
 * PUSH windows through their executable region directory. */
XR_FUNC uint32_t xr_lir_ownership_array_push_window_count(const XrLirOwnershipPlan *);
XR_FUNC uint32_t xr_lir_ownership_array_push_step_count(const XrLirOwnershipPlan *);
XR_FUNC XrLirStatus xr_lir_ownership_array_push_window_at(const XrLirOwnershipPlan *,
    uint32_t ordinal, XrLirArraySetRegion *);
XR_FUNC XrLirStatus xr_lir_ownership_array_push_step_at(const XrLirOwnershipPlan *,
    uint32_t ordinal, XrLirArraySetRegionStep *);
/* Explicit independent current-IR verification for mutable/external inspection
 * and PARANOID tests. It validates the module once and rederives every function
 * once. Normal same-process Plan to Homes reception only copies
 * the owned producer tables; it does not invoke this full structural proof. */
XR_FUNC XrLirStatus xr_lir_ownership_array_regions_verify(const XrLirModule *,
    const XrLirOwnershipPlan *);

#endif // XLIR_OWNERSHIP_H
