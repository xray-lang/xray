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
     * non-View. The natural value's owner_count remains zero and first_owner
     * stays SIZE_MAX. offset/temporary_offset project the corresponding bundle
     * value field, never a second allocation of the natural bytes. */
    uint32_t view_workspace;
    /* A literal's private activation cache owns one immutable String.
     * Normal SSA COPY/MOVE/DROP never consumes the cache;
     * the Frame retires it. SIZE_MAX means no cache, including all formals.
     * Its actual owner offset is part of the same sealed Frame inventory. */
    size_t literal_cache_offset, literal_cache_owner;
    /* On this path all reads are owned appends' suffix operands, before release.
     * Its cache is the only physical owner; the logical SSA home stays empty. */
    bool literal_append_borrow;
    /* Verified suffix-only forwarding chain. Every member uses the physical
     * owner of this one-based producer. The selected edges do not move it. */
    uint32_t stationary_string;
    /* Cell's actual payload plan is in this same receiving layout image.
     * Borrowed Cell formals have no owner slot, offset or temporary. */
    uint32_t cell_payload_type;
    XrLirCellTransport cell_transport;
    XrLirSliceStorage view_storage;
    /* Exact zero-based physical formal ordinal, UINT32_MAX for local keepers. */
    uint32_t view_parameter;
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
    /* SIZE_MAX is absent. A borrowed Cell/Slice/Set formal's logical COPY first
     * retains into its real CELL/VIEW/HEADER_BORROW_STAGE owner. After all
     * staging succeeds the edge MOVEs that owner into its destination, using the
     * destination's distinct temporary. The formal never acquires a slot. */
    size_t borrowed_owner_workspace;
    XrLirSliceStorage source_storage;
} XrLirHomeTransfer;
typedef struct XrLirHomeEdge {
    uint32_t block, edge, target_block;
    /* One complete nonnullable owner MOVE between disjoint local slots.
     * Execution must still respect the live guard, pending and old result. */
    bool direct_header_move;
    uint32_t string_window; /* One-based capacity plan for a verified latch, or zero. */
    size_t first, count;
} XrLirHomeEdge;

typedef struct XrLirStringWindow {
    uint32_t header, index, next_index, latch_index, limit;
    uint32_t comparison; /* Zero-based position after any in-header constant. */
    bool constant_limit;
    uint64_t limit_bits;
    size_t header_owner, latch_owner, latch_target_owner, suffix_owner;
    /* Guard the same route at either boundary. The slot holding the live
     * String differs between the header and latch. */
    size_t header_empty_first, latch_empty_first, empty_count;
} XrLirStringWindow;
typedef struct XrLirHomeCall {
    uint32_t block, instruction, callee, result;
    size_t first, count;
    /* One-based caller landing workspace, zero for an ordinary result.
     * source_argument is the actual borrowed-origin SSA, not an argument
     * index. result_parameter is the exact callee physical formal ordinal. */
    uint32_t view_result_workspace, view_result_source_argument, view_result_parameter;
} XrLirHomeCall;

typedef enum XrLirHomeBindingKind {
    XR_LIR_HOME_BINDING_NONE, XR_LIR_HOME_BINDING_LOCAL, XR_LIR_HOME_BINDING_PARAMETER,
    XR_LIR_HOME_BINDING_CELL_BORROW, XR_LIR_HOME_BINDING_STATE_BORROW,
    /* source is the actual constructed Cell SSA, not a snapshot Map owner. */
    XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW
} XrLirHomeBindingKind;
typedef struct XrLirHomeBinding {
    XrLirHomeBindingKind kind;
    uint32_t type, parameter, source;
    size_t offset, first_owner;
} XrLirHomeBinding;
typedef enum XrLirHomeWorkspaceKind {
    XR_LIR_HOME_WORKSPACE_VALUE,
    XR_LIR_HOME_WORKSPACE_VIEW_HOLD,
    XR_LIR_HOME_WORKSPACE_CELL_BORROW_STAGE,
    /* One real Header-only slot, no temporary. Logical formal COPY first
     * retains all sources here before one physical edge MOVE publication. */
    XR_LIR_HOME_WORKSPACE_VIEW_BORROW_STAGE,
    /* A READ Set formal has natural Header bytes and no owning Place. Its
     * selected-edge COPY pins one Header here before the complete edge MOVE. */
    XR_LIR_HOME_WORKSPACE_HEADER_BORROW_STAGE
} XrLirHomeWorkspaceKind;
typedef struct XrLirHomeWorkspace {
    uint32_t block, instruction, type;
    size_t offset, first_owner;
    /* VIEW_HOLD uses the shared Slice bundle geometry, not type's owner leaves.
     * offset/temporary_offset start complete bundles; each has one actual hold
     * slot. Other workspaces keep their existing VALUE interpretation. */
    XrLirHomeWorkspaceKind kind;
    /* A String append or borrowed String/Map/Set copy VALUE stage names the actual
     * nonconsumed binding here.
     * It has one pending/owner slot and no separate temporary. The matching
     * STRING_OPERATION supplies an append suffix; BINDING_NEW/WRITE supplies
     * a borrowed String/Map/Set formal copied into this true-empty stage. Its
     * registered owner is distinct from the actual destination; a single
     * replacement MOVEs it after retain/adopt without fabricating a formal
     * source Place. Selected-edge temporary storage keeps its separate role. */
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
    /* VALUE names its receiving workspace; END names the source workspace.
     * Selected edges carry both workspace ordinals in HomeTransfer instead. */
    uint32_t view_operation, view_workspace;
    XrLirSliceCleanupMode view_cleanup;
    /* CLEANUP may retire an empty/already-ended target; no natural value is
     * read. A Frame absent on this path implies no keeper was published. */
    XrLirOwnershipTransferMode hold_transfer_mode;
    XrLirCellOperation cell_operation;
    /* One-based receiving Cell and payload image ids, not compiler type ids. */
    uint32_t cell_type, cell_payload_type;
    XrLirCellTransport cell_transport;
    uint32_t cell_scope;
    XrLirSliceStorage view_storage;
    uint32_t view_parameter;
    /* Consumers use this bounded candidate; ordinary events and homes stay live.
     * The paired logical DROP remains present even when this field is set. */
    XrLirOwnershipTransferMode forwarding_mode;
    uint32_t forwarding_drop;
    uint32_t array_result, array_index, array_item;
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
    const XrLirStringWindow *string_windows; /* Sorted by header block. */
    size_t string_window_count;
    const size_t *string_window_slots;
    size_t string_window_slot_count;
    const XrLirHomeCall *calls;
    const uint32_t *arguments;
    size_t call_count, argument_count;
    XrtManagedFrameSpec frame;
    bool owned_frame_needed;
    /* SSA indexed, including zero. type names the exact payload image row,
     * not a heap Cell. Only LOCAL owns a real Header slot. Other rows borrow
     * a logical parameter (one-based) or an actual source SSA; their offset
     * and first_owner are SIZE_MAX. Builtin activation CELL/REF values remain
     * type zero; a constructed Cell has its own separate identity home.
     * String CELL/REF use the same scheme with the String image row. A String
     * or Map/Set STATE_REF row borrows its actual capability (source is its SSA id).
     * These rows confer no aliasing, mutation or escaping authority. */
    const XrLirHomeBinding *bindings;
    /* Real instruction-local pending owners for allocating mutators, including
     * Unit/scalar results. Positions are one-based and do not invent SSA ids.
     * Every slot starts at true NULL under the existing Frame initialization. */
    const XrLirHomeWorkspace *workspaces;
    size_t workspace_count;
    /* Unique same-typed input origin of a Slice result, UINT32_MAX absent. */
    uint32_t view_result_parameter;
    /* Owned structural preparations only. These do not merge any homes or
     * remove original events, and carry no physical execution permission. */
    const XrLirArraySetRegion *array_regions;
    const XrLirArraySetRegionStep *array_region_steps;
    uint32_t array_region_count, array_region_step_count;
    /* Separate owned facts for a future capacity consumer. They do not enter
     * the executable SET/GET directory or change ordinary value homes. */
    const XrLirArraySetRegion *array_push_windows;
    const XrLirArraySetRegionStep *array_push_steps;
    uint32_t array_push_window_count, array_push_step_count;
} XrLirValueHomes;

/* Only for consumers of common homes and the exact LIR edge. Raw execution
 * metadata never uses this fact to grant access to an owner. */
static inline bool xr_lir_home_stationary_edge(const XrLirValueHomes *homes,
    uint32_t source, uint32_t target) {
    if (!homes) return false;
    uint32_t producer = homes->values[source].stationary_string;
    return producer && producer == homes->values[target].stationary_string;
}

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
XR_FUNC const XrLirStringWindow *xr_lir_value_homes_string_window(const XrLirValueHomes *, uint32_t header);
XR_FUNC void xr_lir_value_homes_destroy(XrLirValueHomes *);
#endif // XLIR_VALUE_HOMES_H
