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
#include "xray_native_value_image.h"

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
    XR_RBC_VALUE_STRING_EQUAL, XR_RBC_VALUE_STRING_NOT_EQUAL,
    XR_RBC_VALUE_STATE_READ, XR_RBC_VALUE_STATE_WRITE,
    XR_RBC_VALUE_STATE_CONSTRUCT, XR_RBC_VALUE_STATE_INITIALIZE, XR_RBC_VALUE_STATE_GROUP_INITIALIZE,
    XR_RBC_VALUE_CALL, XR_RBC_VALUE_RETURN, XR_RBC_VALUE_EDGE,
    XR_RBC_VALUE_ARRAY, XR_RBC_VALUE_BINDING_NEW, XR_RBC_VALUE_BINDING_READ,
    XR_RBC_VALUE_BINDING_WRITE, XR_RBC_VALUE_BINDING_DROP, XR_RBC_VALUE_MAP, XR_RBC_VALUE_DISPLAY,
    XR_RBC_VALUE_RECORD_NEW, XR_RBC_VALUE_RECORD_FIELD, XR_RBC_VALUE_CLASS_NEW, XR_RBC_VALUE_CLASS_FIELD,
    XR_RBC_VALUE_NULLABLE_NONE,
    XR_RBC_VALUE_NULLABLE_SOME, XR_RBC_VALUE_NULLABLE_IS_SOME, XR_RBC_VALUE_NULLABLE_UNWRAP,
    XR_RBC_VALUE_FUNCTION_NEW, XR_RBC_VALUE_FUNCTION_CALL, XR_RBC_VALUE_FUNCTION_ALIAS,
    XR_RBC_VALUE_FUNCTION_SCOPE_EXIT, XR_RBC_VALUE_SLICE, XR_RBC_VALUE_STRING_APPEND_OWNED,
    XR_RBC_VALUE_CELL, XR_RBC_VALUE_MAP_LITERAL, XR_RBC_VALUE_STRING_APPEND_BINDING,
    XR_RBC_VALUE_SET,
    XR_RBC_VALUE_ASSERT_CONDITION, XR_RBC_VALUE_I64_TRY_PARSE, XR_RBC_VALUE_F64_TRY_PARSE,
    XR_RBC_VALUE_BOOL_TRY_PARSE,
    XR_RBC_VALUE_CLASS_ARRAY_WRITE, XR_RBC_VALUE_CLASS_ARRAY_PUSH,
    /* Class-identity union observation: ADOPT retains a handle under another
     * nominal class row; IS tests the live handle's nominal identity. */
    XR_RBC_VALUE_CLASS_ADOPT, XR_RBC_VALUE_CLASS_IS,
    /* Tagged sum over one stored tag and one active owner-free variant payload.
     * SUM_NEW names its variant in index and its fields in the argument span;
     * SUM_TAG reads the tag; SUM_GET names one flattened payload field. */
    XR_RBC_VALUE_SUM_NEW, XR_RBC_VALUE_SUM_TAG, XR_RBC_VALUE_SUM_GET,
    XR_RBC_VALUE_CLASS_SCALAR_WRITE, XR_RBC_VALUE_HOST_PATH_BOOL,
    XR_RBC_VALUE_HOST_FILE_OPEN_WRITE, XR_RBC_VALUE_HOST_FILE_CLOSE, XR_RBC_VALUE_HOST_FILE_WRITE,
    XR_RBC_VALUE_STRING_BYTE_LENGTH,
    XR_RBC_VALUE_HOST_PATH_CLASS, XR_RBC_VALUE_HOST_FILE_READ, XR_RBC_VALUE_HOST_ENV_GET,
    /* A textual leaf: the callable in index selects creation or path query. */
    XR_RBC_VALUE_HOST_TEXT_RESULT,
    /* Directory uses a checked callable id and a scalar or managed home. */
    XR_RBC_VALUE_HOST_DIRECTORY,
    XR_RBC_VALUE_STRING_SLICE,
    /* STRING_REPLACE borrows source/other/index; first selects all matches. */
    XR_RBC_VALUE_STRING_REPLACE,
    XR_RBC_VALUE_STRING_INDEX_OF,
    /* Last occurrence over two String borrows; both operands stay borrowed. */
    XR_RBC_VALUE_STRING_LAST_INDEX_OF,
    /* Ordered comparison over two String borrows; index is the LIR predicate. */
    XR_RBC_VALUE_STRING_ORDER,
    /* One predicate over two String borrows: index is the runtime test ordinal. */
    XR_RBC_VALUE_STRING_TEST,
    XR_RBC_VALUE_INVOKE, XR_RBC_VALUE_INVOKE_RESULT, XR_RBC_VALUE_INVOKE_ERROR,
    XR_RBC_VALUE_RETURN_ERROR, XR_RBC_VALUE_PANIC_ERROR, XR_RBC_VALUE_UNCAUGHT_ERROR,
    XR_RBC_VALUE_CLASS_STRING_WRITE,
    XR_RBC_VALUE_HOST_DIRECTORY_CLEANUP_CLOSE, XR_RBC_VALUE_HOST_DIRECTORY_CLEANUP_POLL,
    XR_RBC_VALUE_HOST_DIRECTORY_FRAME, XR_RBC_VALUE_HOST_DIRECTORY_NEXT_CHECKED,
    XR_RBC_VALUE_CHECKED_RESULT, XR_RBC_VALUE_CHECKED_ERROR,
    XR_RBC_VALUE_HOST_DIRECTORY_CLOSE_CHECKED, XR_RBC_VALUE_HOST_DIRECTORY_RETURN_GUARD,
    XR_RBC_VALUE_HOST_DIRECTORY_FAIL,
    XR_RBC_VALUE_CHECKED_EMPTY_ARRAY, XR_RBC_VALUE_MANAGED_FAILURE, XR_RBC_VALUE_CHECKED_ARRAY_PUSH
} XrRbcValueOperation;

typedef struct XrRbcValueAccess {
    XrRbcValueOperation operation;
    /* SSA ids in this function, including borrowed formals. Zero is absent.
     * A scalar operand/result resolves through locations; a managed one uses
     * the real common home, never a register address as an owning place. */
    uint32_t result, source, other;
    /* Literal/module-slot/callee/selected-home-edge ordinal, according to op.
     * CALL arguments are SSA ids in the immutable arguments span; window is
     * its ordinary physical call window. INVOKE uses source as a flag for
     * a selected-edge transfer following each receiver. A Slice result also uses other for
     * its one-based common HomeCall, including the proven actual origin and
     * caller landing. RETURN source names its SSA result and Slice index its
     * one-based common RETURN_TRANSFER event.
     * EDGE uses first/count source/target SSA pairs, count measuring pairs,
     * for type-zero transport absent from common typed homes. Its index is
     * UINT32_MAX when there is no common typed edge, never ambiguous edge zero.
     * A sealed Array capacity ENTRY or LATCH uses a one-based window. */
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
    /* STRING_SLICE borrows source, with the start and end SSA ids in other and index. */
    /* STRING_INDEX_OF borrows source and other and names the start SSA id in index. */
    /* STRING_TEST borrows source and other; index is its runtime test ordinal. */
    /* String BINDING accesses and STRING_APPEND_BINDING index their exact
     * common event. A writing access uses window=UINT32_MAX without a stage,
     * otherwise the zero-based independent VALUE workspace ordinal. Append
     * always has that candidate; a borrowed String formal copy uses its stage. */
    /* CELL index is its exact one-based common Cell event. Aliases use other
     * for a physical scope capability; operation hooks keep their normal SSA
     * operands. Scope exit and RELEASE never read retired natural bytes. */
    /* HOST_FILE_WRITE takes the byte offset from one ordinary argument row. */
    /* CHECKED_EMPTY_ARRAY carries the exact Array image row in index. The
     * common checked receivers use index to name their producing access. */
    uint32_t index, first, count, window;
} XrRbcValueAccess;
XR_STATIC_ASSERT(sizeof(XrRbcValueAccess)==32,"ordinary typed access rows occupy thirty-two bytes");

static inline bool rbc_value_checked_primitive(XrRbcValueOperation operation) {
    return operation == XR_RBC_VALUE_HOST_DIRECTORY_NEXT_CHECKED || operation == XR_RBC_VALUE_CHECKED_EMPTY_ARRAY;
}

/* Numeric roles belong only to code with projected Array slots. Stable access
 * ordinals survive compaction; ordinary operation rows keep their own ABI. */
typedef struct XrRbcArrayRegionAccess {
    uint32_t region;
    XrLirArrayRegionOperation operation;
} XrRbcArrayRegionAccess;

typedef struct XrRbcArrayReadFold {
    uint32_t accumulator, next_accumulator, limit;
    uint64_t limit_bits;
    bool constant;
} XrRbcArrayReadFold;

typedef struct XrRbcArrayPushLimit {
    uint64_t bits;
    uint32_t value; /* Zero uses constant bits instead of a real scalar location. */
    bool available;
} XrRbcArrayPushLimit;

typedef struct XrRbcArrayRepeatLimit {
    uint64_t bits;
    uint32_t value; /* Zero denotes fixed bits. */
    bool available;
} XrRbcArrayRepeatLimit;

typedef struct XrRbcValueCode {
    const XrLirValueHomes *homes;
    const XrRbcValueLocation *locations; /* SSA indexed, including unused zero. */
    uint32_t value_count;
    const uint32_t *parameters; /* Logical LIR parameter SSA ids, in order. */
    uint32_t parameter_count;
    uint32_t error_type; /* Exact one-based value image row for a business error. */
    uint32_t directory_frame_source; /* Eager owned frame source before any guest cleanup registration. */
    bool error_unit, strict_utf8;
    /* Common String bindings need cold setup before any checked operand borrow.
     * The compiler projects this bit once; ordinary scalar functions never
     * acquire a typed activation or execute a new dispatch path. */
    bool string_binding_setup;
    bool map_formal_reads; /* Prepare borrowed Map addresses only for keyed formal reads. */
    const XrRbcValueAccess *accesses;
    size_t access_count;
    const uint32_t *arguments;
    size_t argument_count;
    const struct XrtPodDisplay *displays;
    size_t display_count;
    const uint32_t *scope_values; /* Physical scope ordinal -> original SSA capability. */
    uint32_t scope_count;
    XrLirArrayRegionBindings *array_regions; /* Owned numeric projection, no live addresses. */
    bool array_region_runtime; /* Compile-time SET-window consumption. */
    const XrRbcArrayRegionAccess *array_region_accesses; /* Same ordinal/count as accesses, or absent. */
    const XrRbcArrayReadFold *array_read_folds; /* Indexed like array_regions->regions, or absent. */
    XrLirArrayRegionBindings *array_push_windows;
    const XrRbcArrayPushLimit *array_push_limits; /* Indexed like array_push_windows->regions. */
    const XrRbcArrayRepeatLimit *array_repeat_limits; /* Indexed like array_regions->regions. */
} XrRbcValueCode;

/* A plain local owner of one Header: a String or container value in its own
 * Frame home, reached directly rather than through a binding, formal, borrow,
 * literal cache, view or callable transport. The generic path re-derives
 * exactly these facts on every execution; a specialized word relies on them. */
static inline bool rbc_value_header_owner(const XrtValueImage *image, const XrRbcValueCode *code, uint32_t value) {
    const XrLirValueHomes *homes = code->homes;
    if (!image || !image->types || !homes || !homes->values || !code->locations || !value ||
        value > code->value_count || value > homes->value_count) return false;
    const XrLirValueHome *home = &homes->values[value];
    if (!home->type || home->type > image->type_count || home->parameter || home->borrow_source ||
        home->owner_count != 1 || home->size != sizeof(XrtManagedHeader *) || home->byte_count ||
        home->transport != XR_LIR_CALLABLE_TRANSPORT_NONE || home->cell_transport || home->view_workspace ||
        home->view_storage != XR_LIR_SLICE_STORAGE_NONE || home->literal_cache_borrow || home->offset == SIZE_MAX ||
        home->first_owner >= homes->frame.slot_count || !homes->frame.owned_offsets ||
        homes->frame.owned_offsets[home->first_owner] != home->offset ||
        code->locations[value].kind != XR_RBC_VALUE_NOWHERE || (homes->bindings && homes->bindings[value].type))
        return false;
    XrtValueKind kind = image->types[home->type - 1].kind;
    return kind == XRT_VALUE_STRING || xrt_value_kind_is_array(kind) || kind == XRT_VALUE_MAP_STRING_I64 ||
        kind == XRT_VALUE_MAP_STRING_STRING || kind == XRT_VALUE_SET_I64;
}
/* An edge whose only remaining pair is one direct Header move. */
static inline bool rbc_value_owner_edge(const XrRbcValueCode *code, XrRbcValueAccess access) {
    const XrLirValueHomes *homes = code->homes;
    if (access.index == UINT32_MAX || !homes || !homes->edges || access.index >= homes->edge_count ||
        access.count != 1 || access.other || access.window || !code->arguments ||
        access.first > code->argument_count || code->argument_count - access.first < 2) return false;
    const XrLirHomeEdge *edge = &homes->edges[access.index];
    if (!edge->direct_header_move || edge->string_window || edge->count != 1 || !homes->transfers ||
        edge->first >= homes->transfer_count) return false;
    const XrLirHomeTransfer *transfer = &homes->transfers[edge->first];
    return code->arguments[access.first] == transfer->source && code->arguments[access.first + 1] == transfer->target;
}
/* Emission and admission select every word from the same admitted access
 * description and its image: a specialized word only where its static
 * premises hold and no Array region role claims the access. */
static inline XrRbcOp rbc_value_access_opcode(const XrtValueImage *image, const XrRbcValueCode *code, uint32_t index) {
    if (!code->accesses || index >= code->access_count) return XR_RBC_VALUE_ACCESS;
    XrRbcValueAccess access = code->accesses[index];
    if (access.operation == XR_RBC_VALUE_MAP) {
        if (xr_lir_map_reads_key(access.index)) return XR_RBC_MAP_READ;
        if (access.source && (access.index == XR_LIR_MAP_OP_SET || access.index == XR_LIR_MAP_OP_REMOVE))
            return XR_RBC_MAP_EDIT;
    }
    if (access.operation == XR_RBC_VALUE_STRING_LITERAL && code->homes && code->homes->values &&
        access.result && access.result <= code->homes->value_count &&
        code->homes->values[access.result].literal_cache_borrow) return XR_RBC_STRING_LITERAL_BORROW;
    if (code->array_region_accesses && code->array_region_accesses[index].region) return XR_RBC_VALUE_ACCESS;
    if (access.operation == XR_RBC_VALUE_DROP && !access.result && !access.other &&
        rbc_value_header_owner(image, code, access.source)) return XR_RBC_OWNER_DROP;
    if (access.operation == XR_RBC_VALUE_COPY && !access.other && access.result != access.source &&
        rbc_value_header_owner(image, code, access.result) && rbc_value_header_owner(image, code, access.source) &&
        code->homes->values[access.result].type == code->homes->values[access.source].type)
        return XR_RBC_OWNER_COPY;
    if (access.operation == XR_RBC_VALUE_EDGE && !access.result && !access.source && rbc_value_owner_edge(code, access))
        return XR_RBC_OWNER_EDGE;
    return XR_RBC_VALUE_ACCESS;
}

/* Pair existing bytecode words so both original source positions and
 * transfer facts remain available. The query must read this exact keeper. */
static inline bool rbc_literal_map_pair(const XrRbcValueCode *code, uint32_t first, uint32_t second) {
    if (((first >> 8) & 255u) || (second & 65535u) != XR_RBC_MAP_READ ||
        (first >> 16) >= code->access_count || (second >> 16) >= code->access_count) return false;
    const XrRbcValueAccess literal = code->accesses[first >> 16];
    const XrRbcValueAccess read = code->accesses[second >> 16];
    return rbc_value_access_opcode(NULL, code, first >> 16) == XR_RBC_STRING_LITERAL_BORROW &&
        rbc_value_access_opcode(NULL, code, second >> 16) == XR_RBC_MAP_READ &&
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
