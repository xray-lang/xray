/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_homes.h - One cold projection of physical homes and owner events
 *
 * KEY CONCEPT:
 *   C and bytecode consume the same sealed physical facts and selected-edge
 *   staging. Borrowed formals never acquire references to enter a function.
 */
#ifndef XLIR_VALUE_HOMES_H
#define XLIR_VALUE_HOMES_H
#include "xlir_value_layout.h"
#include "runtime/native/xnative_managed_root.h"

typedef struct XrLirValueHome {
    /* Type references are one-based, zero absent. Subtract one to index the
     * common value-layout enumeration and its receiving raw image. */
    uint32_t type, parameter;
    size_t offset, size, first_owner, owner_count;
    /* Every verified owned result/block owner has disjoint true-empty staging
     * for whole-value replacement, including a reused call/return landing.
     * Borrowed formals never reserve it. POD block parameters only stage bytes. */
    size_t temporary_offset, temporary_first_owner;
    size_t first_bytes, byte_count;
    /* Callable rows keep the same type id as their signature plan, while the
     * actual slot inventory follows this verified transport. Frame/formal
     * borrows have no registered owner leaves, including a borrowed non-NULL
     * Env pointer. environment_type names a constructor's same-image Env. */
    XrLirCallableTransport transport;
    uint32_t borrow_source, environment_type;
    /* One-based existing workspace ordinal, zero for an unowned formal or a
     * non-View. The natural value's owner_count remains zero. */
    uint32_t view_workspace;
} XrLirValueHome;
typedef struct XrLirHomeBytes { size_t offset, size; } XrLirHomeBytes;
typedef struct XrLirHomeTransfer {
    uint32_t source, target;
    /* Copied from the selected ownership event. NONE is a POD byte transfer;
     * BORROW preserves the full representation without an owning transaction. */
    XrLirOwnershipTransferMode transfer_mode;
    /* Separate actual keepers for a borrowed Slice edge. The natural bytes
     * remain BORROW; these one-based workspace ordinals describe physical RC. */
    XrLirOwnershipTransferMode hold_transfer_mode;
    uint32_t source_workspace, target_workspace;
} XrLirHomeTransfer;
typedef struct XrLirHomeEdge {
    uint32_t block, edge, target_block;
    size_t first, count;
} XrLirHomeEdge;
typedef struct XrLirHomeCall {
    uint32_t block, instruction, callee, result;
    size_t first, count;
} XrLirHomeCall;

typedef enum XrLirHomeBindingKind {
    XR_LIR_HOME_BINDING_NONE, XR_LIR_HOME_BINDING_LOCAL, XR_LIR_HOME_BINDING_PARAMETER,
    XR_LIR_HOME_BINDING_CELL_BORROW, XR_LIR_HOME_BINDING_STATE_BORROW
} XrLirHomeBindingKind;
typedef struct XrLirHomeBinding {
    XrLirHomeBindingKind kind;
    uint32_t type, parameter, source;
    size_t offset, first_owner;
} XrLirHomeBinding;
typedef enum XrLirHomeWorkspaceKind {
    XR_LIR_HOME_WORKSPACE_VALUE,
    XR_LIR_HOME_WORKSPACE_VIEW_HOLD
} XrLirHomeWorkspaceKind;
typedef struct XrLirHomeWorkspace {
    uint32_t block, instruction, type;
    size_t offset, first_owner;
    /* VIEW_HOLD uses the shared Slice bundle geometry, not type's owner leaves.
     * offset/temporary_offset start complete bundles; each has one actual hold
     * slot. Other workspaces keep their existing VALUE interpretation. */
    XrLirHomeWorkspaceKind kind;
    uint32_t value;
    size_t temporary_offset, temporary_first_owner;
} XrLirHomeWorkspace;

/* These are copied producer positions, not another lifetime analysis. Kind and
 * operation preserve the common ownership fact ordinals; source/target are SSA
 * IDs in this function. Each edge describes only its selected branch. */
typedef struct XrLirHomeEvent {
    uint32_t kind, block, instruction, source, argument, edge, target_block, target;
    uint32_t operation;
    uint32_t callable_function, callable_scope;
    XrLirCallableTransport callable_transport;
    bool callable_materialize;
    XrLirOwnershipTransferMode transfer_mode;
    uint32_t view_operation, view_workspace;
    XrLirOwnershipTransferMode hold_transfer_mode;
} XrLirHomeEvent;

typedef struct XrLirValueHomes {
    uint32_t source_function, value_count, result_type, unmodeled;
    const XrLirValueHome *values; /* Indexed by SSA ID, including unused zero. */
    const XrLirHomeEvent *events;
    size_t event_count;
    const XrLirHomeBytes *bytes; /* Relative non-owner spans; hooks never write owner fields. */
    size_t byte_count;
    const XrLirHomeTransfer *transfers;
    const XrLirHomeEdge *edges;
    size_t transfer_count, edge_count;
    const XrLirHomeCall *calls;
    const uint32_t *arguments;
    size_t call_count, argument_count;
    XrtManagedFrameSpec frame;
    bool owned_frame_needed;
    /* SSA indexed, including zero. type names the exact payload image row,
     * not a heap Cell. Only LOCAL owns a real Header slot. Other rows borrow
     * a logical parameter (one-based) or an actual source SSA; their offset
     * and first_owner are SIZE_MAX. Ordinary values[CELL/REF] remain type zero.
     * These rows confer no aliasing, mutation or escaping authority. */
    const XrLirHomeBinding *bindings;
    /* Real instruction-local pending owners for allocating mutators, including
     * Unit/scalar results. Positions are one-based and do not invent SSA ids.
     * Every slot starts at true NULL under the existing Frame initialization. */
    const XrLirHomeWorkspace *workspaces;
    size_t workspace_count;
} XrLirValueHomes;

typedef struct XrLirValueHomesRequest {
    const XrLirModule *module;
    XrLirFunction function;
    const XrLirValueLayouts *layouts;
    const XrLirOwnershipPlan *ownership;
} XrLirValueHomesRequest;

/* Inputs describe the exact same finished final module. This producer neither
 * redoes language admission nor invents lifetime facts. All returned bytes are
 * owned, with no retained module, Plan, Ops, Layout or runtime payload owner.
 * The type ordinal belongs to layouts' common enumeration. Call callee IDs and
 * event positions retain source-module identities for each backend to bind.
 *
 * Inline POD homes have offsets for bytecode but do not require C to allocate a
 * Root or imitate interpreter activations. owned_frame_needed is true only for
 * real owner/pending slots. Scalar-only functions return a NULL descriptor.
 * Output must start NULL; a rejection preserves output and all input owners. */
XR_FUNC XrLirValueLayoutStatus xr_lir_value_homes_create(const XrLirValueHomesRequest *, XrLirValueHomes **empty);
XR_FUNC void xr_lir_value_homes_destroy(XrLirValueHomes *);
#endif // XLIR_VALUE_HOMES_H
