/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_code.h - Cold typed operations over common stable SSA homes
 *
 * KEY CONCEPT:
 *   Code owns raw descriptions only. Managed operands name the common SSA
 *   homes, while scalar and POD locations retain their ordinary bytecode ABI.
 */
#ifndef XRBC_VALUE_CODE_H
#define XRBC_VALUE_CODE_H
#include "base/xchecks.h"
#include "xrbc_value_homes.h"
#include "xray_native_pod.h"
#include "lir/xlir_array_region_binding.h"
#include "lir/xlir_map.h"

/* Check a typed word after its module and home directories are available. */
XR_FUNC bool xr_rbc_value_instruction_valid(const XrRbcModule *, const XrRbcFunction *, size_t pc);

typedef enum XrRbcValueLocationKind {
    XR_RBC_VALUE_NOWHERE, XR_RBC_VALUE_REGISTER, XR_RBC_VALUE_SPILL, XR_RBC_VALUE_AGGREGATE
} XrRbcValueLocationKind;
typedef struct XrRbcValueLocation {
    XrRbcValueLocationKind kind;
    uint32_t index;
} XrRbcValueLocation;

typedef enum XrRbcValueOperation {
    XR_RBC_VALUE_STRING_LITERAL, XR_RBC_VALUE_STRING_I64, XR_RBC_VALUE_STRING_F64,
    XR_RBC_VALUE_STRING_CONCAT, XR_RBC_VALUE_COPY, XR_RBC_VALUE_MOVE, XR_RBC_VALUE_DROP,
    XR_RBC_VALUE_STRING_PRINT, XR_RBC_VALUE_STRING_LENGTH,
    XR_RBC_VALUE_STATE_READ, XR_RBC_VALUE_STATE_WRITE,
    XR_RBC_VALUE_STATE_CONSTRUCT, XR_RBC_VALUE_STATE_INITIALIZE, XR_RBC_VALUE_STATE_GROUP_INITIALIZE,
    XR_RBC_VALUE_CALL, XR_RBC_VALUE_RETURN, XR_RBC_VALUE_EDGE,
    XR_RBC_VALUE_ARRAY, XR_RBC_VALUE_BINDING_NEW, XR_RBC_VALUE_BINDING_READ,
    XR_RBC_VALUE_BINDING_WRITE, XR_RBC_VALUE_BINDING_DROP, XR_RBC_VALUE_MAP, XR_RBC_VALUE_DISPLAY,
    XR_RBC_VALUE_RECORD_NEW, XR_RBC_VALUE_RECORD_FIELD, XR_RBC_VALUE_NULLABLE_NONE,
    XR_RBC_VALUE_NULLABLE_SOME, XR_RBC_VALUE_NULLABLE_IS_SOME, XR_RBC_VALUE_NULLABLE_UNWRAP,
    XR_RBC_VALUE_FUNCTION_NEW, XR_RBC_VALUE_FUNCTION_CALL, XR_RBC_VALUE_FUNCTION_ALIAS,
    XR_RBC_VALUE_FUNCTION_SCOPE_EXIT, XR_RBC_VALUE_SLICE, XR_RBC_VALUE_STRING_APPEND_OWNED,
    XR_RBC_VALUE_CELL, XR_RBC_VALUE_MAP_LITERAL, XR_RBC_VALUE_STRING_APPEND_BINDING,
    XR_RBC_VALUE_SET,
    XR_RBC_VALUE_ASSERT_CONDITION, XR_RBC_VALUE_I64_TRY_PARSE
} XrRbcValueOperation;

typedef struct XrRbcValueAccess {
    XrRbcValueOperation operation;
    /* SSA ids in this function, including borrowed formals. Zero is absent.
     * A scalar operand/result resolves through locations; a managed one uses
     * the real common home, never a register address as an owning place. */
    uint32_t result, source, other;
    /* Literal/module-slot/callee/selected-home-edge ordinal, according to op.
     * CALL arguments are SSA ids in the immutable arguments span; window is
     * its ordinary physical call window. A Slice result also uses other for
     * its one-based common HomeCall, including the proven actual origin and
     * caller landing. A last-use Array CALL uses source and its original COPY
     * uses other for the same one-based common Array call move. Neither field
     * grants ownership on raw code. RETURN source names its SSA result and Slice index its
     * one-based common RETURN_TRANSFER event.
     * EDGE uses first/count source/target SSA pairs, count measuring pairs,
     * for type-zero transport absent from common typed homes. Its index is
     * UINT32_MAX when there is no common typed edge, never ambiguous edge zero. */
    /* ARRAY/MAP index is the verified catalog role, first/count its complete
     * SSA operands. A mutator's window selects its common pending workspace.
     * Keyed Map reads also project source/other to local Frame owner slots
     * (one-based, zero for a borrowed Map or String origin). For a Map
     * formal, window is its zero-based parameter, otherwise UINT32_MAX.
     * Local keyed edits project source to the writable binding slot and
     * other to the borrowed key slot, while keeping their pending workspace. */
    /* SET index is the dedicated LIR operation and other its one-based common
     * SET_OPERATION event. Operands and workspace follow the same real homes. */
    /* SLICE index is its closed role, first/count its full SSA operand span.
     * Only NEW over a real State reference uses source=XR_LIR_STATE_REF as the
     * received closed READ binding role, not another SSA operand or write cap.
     * window is a one-based common loan event for construction/END, zero for
     * readers and materialization. Natural bytes never carry a keeper owner. */
    /* STRING_APPEND_OWNED index is its one-based APPEND_TRANSFER event.
     * source moves, other borrows, and result has its own real staging slot. */
    /* String BINDING accesses and STRING_APPEND_BINDING index their exact
     * common event. A writing access uses window=UINT32_MAX without a stage,
     * otherwise the zero-based independent VALUE workspace ordinal. Append
     * always has that candidate; a borrowed String formal copy uses its stage. */
    /* CELL index is its exact one-based common Cell event. Aliases use other
     * for a physical scope capability; operation hooks keep their normal SSA
     * operands. Scope exit and RELEASE never read retired natural bytes. */
    uint32_t index, first, count, window;
} XrRbcValueAccess;
XR_STATIC_ASSERT(sizeof(XrRbcValueAccess)==32,"ordinary typed access rows occupy thirty-two bytes");

/* Numeric roles belong only to code with projected Array slots. Stable access
 * ordinals survive compaction; ordinary operation rows keep their own ABI. */
typedef struct XrRbcArrayRegionAccess {
    uint32_t region;
    uint8_t fixed_edge; /* Selected physical edge marker, independent of HomeEdge. */
    bool fixed;
    bool binding_identity; /* Admitted forwarding pair shares a single Frame slot. */
    bool sum_checkpoint; /* After the ordinary latch copies published the header state. */
    XrLirArrayRegionOperation operation;
} XrRbcArrayRegionAccess;

typedef struct XrRbcArrayCountedPush {
    uint64_t bits;
    uint32_t iv; /* Zero leaves this fixed row on ordinary scalar replay. */
    uint32_t limit; /* Zero uses constant bits instead of a scalar location. */
} XrRbcArrayCountedPush;

typedef struct XrRbcArrayCallAccess {
    uint32_t retain, call; /* One-based access ordinals; independent of compacted branches. */
} XrRbcArrayCallAccess;

typedef struct XrRbcValueCode {
    const XrLirValueHomes *homes;
    const XrRbcValueLocation *locations; /* SSA indexed, including unused zero. */
    uint32_t value_count;
    const uint32_t *parameters; /* Logical LIR parameter SSA ids, in order. */
    uint32_t parameter_count;
    /* Common String bindings need cold setup before any checked operand borrow.
     * The compiler projects this bit once; ordinary scalar functions never
     * acquire a typed activation or execute a new dispatch path. */
    bool string_binding_setup;
    bool map_formal_reads; /* Prepare borrowed Map addresses only for keyed formal reads. */
    const XrRbcValueAccess *accesses;
    size_t access_count;
    uint32_t array_entry_access; /* Exact entry COPY, zero if unavailable. */
    const XrRbcArrayCallAccess *array_calls; /* Indexed like homes->array_call_moves. */
    const uint32_t *arguments;
    size_t argument_count;
    const struct XrtPodDisplay *displays;
    size_t display_count;
    const uint32_t *scope_values; /* Physical scope ordinal -> original SSA capability. */
    uint32_t scope_count;
    XrLirArrayRegionBindings *array_regions; /* Owned numeric projection, no live addresses. */
    bool array_region_runtime; /* Compile-time SET-window consumption. */
    const XrRbcArrayRegionAccess *array_region_accesses; /* Same ordinal/count as accesses, or absent. */
    const XrRbcArrayCountedPush *array_counted_push; /* Optional lowering indexed like homes->fixed_array_updates. */
} XrRbcValueCode;

static inline size_t rbc_value_legacy_array_region_count(const XrRbcValueCode *code) {
    return code->array_regions ? code->array_regions->region_count : 0;
}

static inline bool rbc_value_binding_forwarding_places(const XrRbcValueCode *code,
    XrRbcValueAccess access, XrtManagedFrameForward *places) {
    bool create = access.operation == XR_RBC_VALUE_BINDING_NEW;
    if ((!create && access.operation != XR_RBC_VALUE_BINDING_READ) || !access.index) return false;
    const XrLirValueHomes *homes = code->homes;
    XR_DCHECK(homes && access.index <= homes->event_count, "binding events have their common homes");
    if (homes->events[access.index - 1].forwarding_mode != XR_LIR_OWNERSHIP_TRANSFER_MOVE) return false;
    const XrLirHomeBinding *binding = &homes->bindings[create ? access.result : access.source];
    const XrLirValueHome *value = &homes->values[create ? access.source : access.result];
    *places = (XrtManagedFrameForward){create ? value->first_owner : binding->first_owner,
        create ? value->offset : binding->offset, create ? binding->first_owner : value->first_owner,
        create ? binding->offset : value->offset};
    return true;
}

static inline bool rbc_value_binding_identity(const XrRbcValueCode *code, XrRbcValueAccess access) {
    XrtManagedFrameForward places;
    return rbc_value_binding_forwarding_places(code, access, &places) &&
        places.source_slot == places.destination_slot && places.source_offset == places.destination_offset;
}

/* A leaf needs no call, return or owning destination. Both emission and
 * admission select the word from the same admitted access description. */
static inline XrRbcOp rbc_value_access_opcode(const XrRbcValueCode *code, XrRbcValueAccess access) {
    if (access.operation == XR_RBC_VALUE_MAP) {
        if (xr_lir_map_reads_key(access.index)) return XR_RBC_MAP_READ;
        if (access.source && (access.index == XR_LIR_MAP_OP_SET || access.index == XR_LIR_MAP_OP_REMOVE))
            return XR_RBC_MAP_EDIT;
    }
    if (access.operation == XR_RBC_VALUE_STRING_LITERAL && code->homes && code->homes->values &&
        access.result && access.result <= code->homes->value_count &&
        code->homes->values[access.result].literal_cache_borrow) return XR_RBC_STRING_LITERAL_BORROW;
    return XR_RBC_VALUE_ACCESS;
}

/* Pair existing bytecode words so both original source positions and
 * transfer facts remain available. The query must read this exact keeper. */
static inline bool rbc_literal_map_pair(const XrRbcValueCode *code, uint32_t first, uint32_t second) {
    if (((first >> 8) & 255u) || (second & 65535u) != XR_RBC_MAP_READ ||
        (first >> 16) >= code->access_count || (second >> 16) >= code->access_count) return false;
    const XrRbcValueAccess literal = code->accesses[first >> 16];
    const XrRbcValueAccess read = code->accesses[second >> 16];
    return rbc_value_access_opcode(code, literal) == XR_RBC_STRING_LITERAL_BORROW &&
        rbc_value_access_opcode(code, read) == XR_RBC_MAP_READ &&
        read.other && read.other == rbc_local_borrow_slot(code->homes, literal.result) &&
        read.count == 2 && code->arguments && read.first <= code->argument_count &&
        code->argument_count - read.first >= 2 && code->arguments[(size_t)read.first + 1] == literal.result;
}

static inline uint32_t rbc_map_formal(const XrRbcValueCode *code, uint32_t value) {
    if (!code->homes || !value || value > code->homes->value_count) return UINT32_MAX;
    const XrLirValueHome *home = &code->homes->values[value];
    uint32_t parameter = home->parameter - 1;
    return parameter < code->parameter_count && code->parameters && code->parameters[parameter] == value ?
        parameter : UINT32_MAX;
}

/* Descriptions borrow the actual code lease, as the existing common homes do.
 * Raw bytecode cannot acquire typed owning permissions from forged pointers.
 * Scalar-only code without any typed operation has no descriptor. */
XR_FUNC const XrRbcValueCode *xr_rbc_module_value_code(const XrRbcModule *, uint32_t function);
typedef struct XrRbcValueRuntime XrRbcValueRuntime;
#endif // XRBC_VALUE_CODE_H
