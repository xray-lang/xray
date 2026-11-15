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
    size_t temporary_offset, temporary_first_owner;
    size_t first_bytes, byte_count;
} XrLirValueHome;
typedef struct XrLirHomeBytes { size_t offset, size; } XrLirHomeBytes;
typedef struct XrLirHomeTransfer { uint32_t source, target; } XrLirHomeTransfer;
typedef struct XrLirHomeEdge {
    uint32_t block, edge, target_block;
    size_t first, count;
} XrLirHomeEdge;
typedef struct XrLirHomeCall {
    uint32_t block, instruction, callee, result;
    size_t first, count;
} XrLirHomeCall;

/* These are copied producer positions, not another lifetime analysis. Kind and
 * operation preserve the common ownership fact ordinals; source/target are SSA
 * IDs in this function. Each edge describes only its selected branch. */
typedef struct XrLirHomeEvent {
    uint32_t kind, block, instruction, source, argument, edge, target_block, target;
    uint32_t operation;
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
