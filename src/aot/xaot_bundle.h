/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_bundle.h - AOT sidecar bundle plan
 */

#ifndef XAOT_BUNDLE_H
#define XAOT_BUNDLE_H

#include "xaot_abi.h"
#include "xaot_container.h"
#include "../analysis/xglobal_summary.h"
#include "../ir/xi_module.h"
#include "../base/xdefs.h"
#include <stdint.h>

/* Pointer-keyed open-addressing index: maps an XiValue or XiFunc pointer to
 * its row in the corresponding plan array, turning the per-lookup linear
 * scan over the whole-bundle plan tables into O(1).  Stores array indices
 * (not row pointers) so plan-array realloc never invalidates it. */
typedef struct XaotPtrIndexSlot {
    const void *key;
    uint32_t idx;
} XaotPtrIndexSlot;

typedef struct XaotPtrIndex {
    XaotPtrIndexSlot *slots;
    uint32_t cap; /* power of two; 0 = unallocated */
    uint32_t count;
} XaotPtrIndex;

typedef struct XaotFuncPlan {
    XiFunc *func;
    uint32_t module_index;
    uint16_t depth;
    XaotFuncAbi abi;
} XaotFuncPlan;

typedef struct XaotValuePlan {
    const XiFunc *func;
    const XiValue *value;
    XaotValueRep rep;
} XaotValuePlan;

typedef struct XaotContainerTypePlan {
    XaotContainerPlan plan;
} XaotContainerTypePlan;

typedef enum XaotEnumScalarAction {
    XAOT_ENUM_SCALAR_RUNTIME_AGGREGATE = 0,
    XAOT_ENUM_SCALAR_COMPACT_AGGREGATE = 1,
} XaotEnumScalarAction;

enum {
    XAOT_ENUM_SCALAR_PAYLOAD_CAP = 16,
    XAOT_ENUM_SCALAR_EV_LAYOUT_ID = 1u << 0,
    XAOT_ENUM_SCALAR_EV_PAYLOAD_BOUND = 1u << 1,
    XAOT_ENUM_SCALAR_EV_TYPED_UNION = 1u << 2,
    XAOT_ENUM_SCALAR_EV_CONCRETE_TYPES = 1u << 3,
};

typedef struct XaotEnumPlan {
    const XiEnumData *enum_data;
    const XiEnumMemberData *members;
    const XrType *concrete_type;
    XrType **type_args;
    uint32_t module_index;
    uint32_t member_count;
    uint32_t layout_id;
    uint32_t scalar_evidence;
    uint16_t max_payload;
    uint16_t scalar_payload_cap;
    uint8_t type_arg_count;
    uint8_t scalar_action;
    bool owns_members;
    const char *c_type;
} XaotEnumPlan;

enum {
    XAOT_ARRAY_STORAGE_READ = 1u << 0,
    XAOT_ARRAY_STORAGE_MUTABLE = 1u << 1,
};

typedef struct XaotArrayStoragePlan {
    const XiFunc *func;
    const XiValue *value;
    const XiValue *origin;
    uint32_t flags;
    XaotContainerElemPlan elem;
} XaotArrayStoragePlan;

enum {
    XAOT_ARRAY_CACHE_READ = 1u << 0,
    XAOT_ARRAY_CACHE_MUTABLE = 1u << 1,
    XAOT_ARRAY_CACHE_DECLARE_LOCAL = 1u << 2,
    XAOT_ARRAY_CACHE_FRESH_RESULT = 1u << 3,
    XAOT_ARRAY_CACHE_VIEW = 1u << 4,
    XAOT_ARRAY_CACHE_FILL_LOOP = 1u << 5,
    XAOT_ARRAY_CACHE_NATIVE_LOCAL = 1u << 6,
    XAOT_ARRAY_CACHE_CLASS_FIELD = 1u << 7,
};

typedef struct XaotArrayCachePlan {
    const XiFunc *func;
    const XiValue *value;
    const XiValue *storage_value;
    uint32_t flags;
    XaotContainerElemPlan elem;
} XaotArrayCachePlan;

typedef struct XaotArrayClassFieldAllocPlan {
    const XiFunc *func;
    const XiValue *origin;
    const XiValue *store;
    const XiClassData *class_data;
    uint16_t field_idx;
    XaotContainerElemPlan elem;
} XaotArrayClassFieldAllocPlan;

/* Function attribute plan: prepare proves a function free of observable
 * effects and Cgen emits the matching C attribute so the host compiler can
 * CSE / LICM across call sites. CONST = touches no memory at all;
 * PURE = reads memory but never writes / throws / suspends. Mutually
 * exclusive; evidence is the body summary plus per-value effect flags
 * re-checked by the verifier. */
enum {
    XAOT_FN_ATTR_CONST = 1u << 0, /* __attribute__((const)) */
    XAOT_FN_ATTR_PURE = 1u << 1,  /* __attribute__((pure)) */
};

enum {
    XAOT_FN_ATTR_EV_BODY_SUMMARY = 1u << 0,
    XAOT_FN_ATTR_EV_XI_EFFECT_SCAN = 1u << 1,
    XAOT_FN_ATTR_EV_CALLEE_SUMMARY = 1u << 2,
};

enum {
    XAOT_PLAN_BODY_EV_BODY_SUMMARY = 1u << 0,
};

typedef struct XaotFuncAttrPlan {
    const XiFunc *func;
    XgFuncId body_func_id;
    uint32_t body_effect_bits;
    uint32_t body_escape_bits;
    uint32_t evidence;
    uint32_t flags;
} XaotFuncAttrPlan;

/* Bounds plan: prepare proves an XI_INDEX_GET / XI_INDEX_SET in-bounds so
 * Cgen can emit raw element access without runtime checks. The proof is
 * computed centrally here (not pattern-matched in the emitter) so the
 * verifier can re-derive it and the dump can audit which accesses went
 * unchecked and why. Evidence bits record which rule fired; accesses that
 * stay checked are recorded with evidence == 0 and an unproven reason so
 * the dump exposes the audit budget for bounds work. */
enum {
    /* Every predecessor is the true edge of `if (idx < arr.length)` and the
     * length load is not clobbered between the guard and the access. */
    XAOT_BOUNDS_EV_DOM_GUARD = 1u << 0,
    /* Single-block counted loop: `while (i < arr.length) { arr[i] = ...; i += 1 }`
     * with a zero start, +1 step, and a checked entry edge. */
    XAOT_BOUNDS_EV_COUNTED_LOOP = 1u << 1,
    /* Index proven >= 0 (range analysis or structural induction). */
    XAOT_BOUNDS_EV_NONNEG_INDEX = 1u << 2,
    /* No side-effect / write-memory op can clobber the length relation before access. */
    XAOT_BOUNDS_EV_NO_CLOBBER = 1u << 3,
};

/* Why an access stayed checked. Ordered by specificity: when both proof
 * paths fail, the most specific reason is recorded. */
enum {
    XAOT_BOUNDS_UNPROVEN_NONE = 0,         /* proven (evidence != 0) */
    XAOT_BOUNDS_UNPROVEN_NO_GUARD = 1,     /* no dominating idx < len test found */
    XAOT_BOUNDS_UNPROVEN_INDEX_RANGE = 2,  /* index not proven >= 0 */
    XAOT_BOUNDS_UNPROVEN_LEN_MISMATCH = 3, /* guard bound is not this array's length */
    XAOT_BOUNDS_UNPROVEN_CLOBBER = 4,      /* effectful op between guard / length and access */
};

typedef struct XaotBoundsPlan {
    const XiFunc *func;
    const XiValue *access; /* XI_INDEX_GET / XI_INDEX_SET */
    XgFuncId body_func_id;
    uint32_t body_effect_bits;
    uint32_t body_escape_bits;
    uint32_t body_evidence;
    uint32_t evidence;       /* XAOT_BOUNDS_EV_*; 0 = stays checked */
    uint8_t unproven_reason; /* XAOT_BOUNDS_UNPROVEN_*; 0 = proven */
} XaotBoundsPlan;

typedef enum XaotSpanAccessKind {
    XAOT_SPAN_ACCESS_INDEX_GET = 1,
    XAOT_SPAN_ACCESS_INDEX_SET,
    XAOT_SPAN_ACCESS_BYTE_LOAD,
    XAOT_SPAN_ACCESS_BYTE_STORE,
    XAOT_SPAN_ACCESS_BYTE_FILL,
    XAOT_SPAN_ACCESS_BYTE_COPY,
    XAOT_SPAN_ACCESS_BYTE_COMPARE,
    XAOT_SPAN_ACCESS_BYTE_COMMON_PREFIX,
    XAOT_SPAN_ACCESS_BYTE_REPEAT,
    XAOT_SPAN_ACCESS_SPAN_AS_BYTES,
    XAOT_SPAN_ACCESS_SPAN_FILL,
    XAOT_SPAN_ACCESS_SPAN_COPY,
    XAOT_SPAN_ACCESS_SPAN_COMPARE,
    XAOT_SPAN_ACCESS_REINTERPRET,
} XaotSpanAccessKind;

enum {
    XAOT_SPAN_EV_RECV_AGGREGATE = 1u << 0,
    XAOT_SPAN_EV_RECV_BYTE_SPAN = 1u << 1,
    XAOT_SPAN_EV_RECV_POD = 1u << 2,
    XAOT_SPAN_EV_ELEM_MATCH = 1u << 3,
    XAOT_SPAN_EV_WRITABLE = 1u << 4,
    XAOT_SPAN_EV_RANGE_PROVEN = 1u << 5,
    XAOT_SPAN_EV_LENGTH_REL_PROVEN = 1u << 6,
    XAOT_SPAN_EV_BYTE_LEN_NO_OVERFLOW = 1u << 7,
    XAOT_SPAN_EV_DATA_VALID = 1u << 8,
    XAOT_SPAN_EV_ENDIAN_CONST = 1u << 9,
    XAOT_SPAN_EV_NO_CLOBBER = 1u << 10,
};

enum {
    XAOT_SPAN_DROP_BOUNDS = 1u << 0,
    XAOT_SPAN_DROP_READONLY = 1u << 1,
    XAOT_SPAN_DROP_TYPE = 1u << 2,
    XAOT_SPAN_DROP_POD = 1u << 3,
    XAOT_SPAN_DROP_NULL_DATA = 1u << 4,
    XAOT_SPAN_DROP_OVERFLOW = 1u << 5,
    XAOT_SPAN_DROP_HELPER = 1u << 6,
};

enum {
    XAOT_SPAN_UNPROVEN_NONE = 0,
    XAOT_SPAN_UNPROVEN_DYNAMIC_RECV,
    XAOT_SPAN_UNPROVEN_NOT_BYTE_SPAN,
    XAOT_SPAN_UNPROVEN_NOT_POD,
    XAOT_SPAN_UNPROVEN_READONLY_MAYBE,
    XAOT_SPAN_UNPROVEN_RANGE,
    XAOT_SPAN_UNPROVEN_LENGTH_REL,
    XAOT_SPAN_UNPROVEN_OVERFLOW,
    XAOT_SPAN_UNPROVEN_DATA_NULL,
    XAOT_SPAN_UNPROVEN_ENDIAN_DYNAMIC,
    XAOT_SPAN_UNPROVEN_CLOBBER,
    XAOT_SPAN_UNPROVEN_DYNAMIC_BOUNDARY,
    XAOT_SPAN_UNPROVEN_ELEM_MISMATCH,
};

typedef struct XaotSpanAccessPlan {
    const XiFunc *func;
    const XiValue *value;
    XgFuncId body_func_id;
    uint32_t body_effect_bits;
    uint32_t body_escape_bits;
    uint32_t body_evidence;
    uint8_t kind;               /* XaotSpanAccessKind */
    uint32_t evidence;          /* XAOT_SPAN_EV_* */
    uint32_t eliminated_checks; /* XAOT_SPAN_DROP_*; 0 = stays on checked/fallback path */
    uint8_t unproven_reason;    /* XAOT_SPAN_UNPROVEN_*; 0 = eliminated_checks != 0 */
} XaotSpanAccessPlan;

/* Alias plan: prepare proves a pointer unique over its storage so Cgen can
 * emit `restrict` and the C compiler gets Rust-noalias-grade information.
 * A wrong restrict is undefined behaviour, so each plan carries the full
 * evidence chain, the verifier re-derives it, and XRAY_AOT_NO_RESTRICT=1
 * suppresses plan creation for regression bisection. */
typedef enum XaotAliasKind {
    XAOT_ALIAS_UNIQUE_DATA = 1,  /* typed array data cache (_adN): the only
                                  * element-storage pointer in the function */
    XAOT_ALIAS_UNIQUE_RECV = 2,  /* native receiver unaliased in the method */
    XAOT_ALIAS_UNIQUE_PARAM = 3, /* pointer param provably unaliased */
} XaotAliasKind;

enum {
    /* Backing storage allocated by this function (array literal / Bytes /
     * with_capacity — a fresh malloc nothing else can point at yet). */
    XAOT_ALIAS_EV_FRESH_ALLOC = 1u << 0,
    /* Every XI_INDEX_GET / XI_INDEX_SET on the array has a proven bounds
     * plan, so every element access is emitted through the _adN cache and
     * never through a checked ->data slow path. */
    XAOT_ALIAS_EV_ALL_ACCESS_RAW = 1u << 1,
    /* Every other use is alias-free: unique fill push, length/size reads,
     * retain/release. No call, store, capture, or phi participation. */
    XAOT_ALIAS_EV_USE_WHITELIST = 1u << 2,
    /* No second array cache plan over the same backing in the function. */
    XAOT_ALIAS_EV_SOLE_CACHE = 1u << 3,
};

typedef struct XaotAliasPlan {
    const XiFunc *func;
    const XiValue *value; /* cache origin value (the _adN owner) */
    XgFuncId body_func_id;
    uint32_t body_effect_bits;
    uint32_t body_escape_bits;
    uint32_t body_evidence;
    uint8_t kind;      /* XaotAliasKind */
    uint32_t evidence; /* XAOT_ALIAS_EV_* */
} XaotAliasPlan;

typedef enum XaotAllocationAction {
    XAOT_ALLOC_ACTION_NONE = 0,
    XAOT_ALLOC_ACTION_STACK = 1,
    XAOT_ALLOC_ACTION_SROA = 2,
} XaotAllocationAction;

enum {
    XAOT_ALLOC_EV_STACK_ALLOC_OP = 1u << 0,
    XAOT_ALLOC_EV_NO_ESCAPE = 1u << 1,
    XAOT_ALLOC_EV_ORIGINAL_ALLOC_OP = 1u << 2,
    XAOT_ALLOC_EV_BODY_SUMMARY = 1u << 3,
};

typedef struct XaotAllocationPlan {
    const XiFunc *func;
    const XiValue *value;
    XgFuncId body_func_id;
    uint32_t body_effect_bits;
    uint32_t body_escape_bits;
    uint32_t body_evidence;
    uint16_t original_op;
    uint8_t escape;
    uint8_t action;
    uint32_t evidence;
} XaotAllocationPlan;

typedef enum XaotClosureRepresentation {
    XAOT_CLOSURE_RUNTIME = 1,
    XAOT_CLOSURE_STACK = 2,
    XAOT_CLOSURE_DIRECT_SYMBOL = 3,
} XaotClosureRepresentation;

enum {
    XAOT_CLOSURE_EV_XI_VALUE = 1u << 0,
    XAOT_CLOSURE_EV_TARGET_FUNC = 1u << 1,
    XAOT_CLOSURE_EV_CAPTURE_ARITY = 1u << 2,
    XAOT_CLOSURE_EV_NOESCAPE_STACK = 1u << 3,
    XAOT_CLOSURE_EV_DIRECT_SYMBOL = 1u << 4,
};

enum {
    XAOT_CLOSURE_UNPROVEN_NONE = 0,
    XAOT_CLOSURE_UNPROVEN_NO_TARGET = 1,
    XAOT_CLOSURE_UNPROVEN_CAPTURE_ARITY = 2,
};

typedef struct XaotClosurePlan {
    const XiFunc *func;
    const XiValue *value;
    const XiFunc *target_func;
    uint16_t capture_count;
    uint8_t representation;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotClosurePlan;

typedef enum XaotTransferSiteKind {
    XAOT_TRANSFER_GO_ARG = 1,
    XAOT_TRANSFER_THREAD_ARG = 2,
    XAOT_TRANSFER_CHAN_SEND = 3,
    XAOT_TRANSFER_CHAN_TRY_SEND = 4,
    XAOT_TRANSFER_CHAN_SEND_TIMEOUT = 5,
} XaotTransferSiteKind;

typedef enum XaotTransferAction {
    XAOT_TRANSFER_ACTION_SHARE = 1,
    XAOT_TRANSFER_ACTION_COPY = 2,
    XAOT_TRANSFER_ACTION_MOVE = 3,
    XAOT_TRANSFER_ACTION_DEEP_COPY = 4,
    XAOT_TRANSFER_ACTION_REJECT = 5,
} XaotTransferAction;

enum {
    XAOT_TRANSFER_EV_SITE = 1u << 0,
    XAOT_TRANSFER_EV_VALUE = 1u << 1,
    XAOT_TRANSFER_EV_MODE = 1u << 2,
    XAOT_TRANSFER_EV_TYPE = 1u << 3,
    XAOT_TRANSFER_EV_BOUNDARY_CLONE = 1u << 4,
};

enum {
    XAOT_TRANSFER_UNPROVEN_NONE = 0,
    XAOT_TRANSFER_UNPROVEN_NO_VALUE = 1,
    XAOT_TRANSFER_UNPROVEN_BAD_MODE = 2,
};

typedef struct XaotTransferPlan {
    const XiFunc *func;
    const XiValue *site;
    const XiValue *value;
    const XrType *value_type;
    XaotTypeKey value_type_key;
    uint16_t transfer_index;
    uint8_t site_kind;
    uint8_t mode;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotTransferPlan;

typedef struct XaotGlobalEvidencePlan {
    const XgGlobalEvidence *evidence;
    uint64_t evidence_hash;
    uint32_t profile;
} XaotGlobalEvidencePlan;

enum {
    XAOT_CLASS_HIER_EV_GLOBAL_SUMMARY = 1u << 0,
    XAOT_CLASS_HIER_EV_PARENT_RESOLVED = 1u << 1,
    XAOT_CLASS_HIER_EV_FINALITY_DERIVED = 1u << 2,
};

enum {
    XAOT_CLASS_UNPROVEN_NONE = 0,
    XAOT_CLASS_UNPROVEN_NO_GLOBAL_EVIDENCE = 1,
    XAOT_CLASS_UNPROVEN_INCONSISTENT_GRAPH = 2,
};

typedef struct XaotClassHierarchyPlan {
    XgClassId class_id;
    XgClassId parent_class_id;
    uint32_t flags;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotClassHierarchyPlan;

enum {
    XAOT_CLASS_LAYOUT_TYPED_PAYLOAD = 1u << 0,
    XAOT_CLASS_LAYOUT_PREFIX_PARENT = 1u << 1,
    XAOT_CLASS_LAYOUT_TYPE_ID = 1u << 2,
    XAOT_CLASS_LAYOUT_VTABLE = 1u << 3,
    XAOT_CLASS_LAYOUT_HEADER = 1u << 4,
};

typedef struct XaotClassLayoutPlan {
    XgClassId class_id;
    char *c_type_name;
    uint32_t instance_size;
    uint32_t instance_align;
    uint32_t field_start;
    uint32_t field_count;
    uint32_t flags;
} XaotClassLayoutPlan;

typedef enum XaotMethodDispatchKind {
    XAOT_DISPATCH_DIRECT = 1,
    XAOT_DISPATCH_VTABLE,
    XAOT_DISPATCH_ITABLE,
    XAOT_DISPATCH_TYPE_SWITCH,
    XAOT_DISPATCH_RUNTIME_FALLBACK,
} XaotMethodDispatchKind;

enum {
    XAOT_DISPATCH_EV_GLOBAL_CALLSITE = 1u << 0,
    XAOT_DISPATCH_EV_RECEIVER_CONCRETE = 1u << 1,
    XAOT_DISPATCH_EV_INFERRED_FINAL = 1u << 2,
    XAOT_DISPATCH_EV_METHOD_NOT_OVERRIDDEN = 1u << 3,
    XAOT_DISPATCH_EV_INTERFACE_OBJECT = 1u << 4,
    XAOT_DISPATCH_EV_SINGLE_IMPLEMENTOR = 1u << 5,
    XAOT_DISPATCH_EV_SMALL_IMPLEMENTOR_SET = 1u << 6,
    XAOT_DISPATCH_EV_OVERRIDE_GRAPH = 1u << 7,
};

enum {
    XAOT_DISPATCH_UNPROVEN_NONE = 0,
    XAOT_DISPATCH_UNPROVEN_NO_RECEIVER_TYPE = 1,
    XAOT_DISPATCH_UNPROVEN_NO_METHOD_ID = 2,
    XAOT_DISPATCH_UNPROVEN_POLYMORPHIC = 3,
    XAOT_DISPATCH_UNPROVEN_NO_INTERFACE_ID = 4,
    XAOT_DISPATCH_UNPROVEN_NO_TARGET_METHOD = 5,
    XAOT_DISPATCH_UNPROVEN_LARGE_IMPLEMENTOR_SET = 6,
};

typedef struct XaotMethodDispatchPlan {
    XgCallsiteId callsite_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    XgMethodId method_id;
    XgMethodId method_root_id;
    uint32_t method_name_id;
    uint32_t method_signature_key;
    uint32_t arg_type_key_start;
    uint16_t arg_count;
    XgClassId receiver_static_class_id;
    XgInterfaceId receiver_static_interface_id;
    uint8_t kind;
    uint32_t dispatch_slot;
    uint32_t target_start;
    uint16_t target_count;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotMethodDispatchPlan;

typedef struct XaotDispatchTargetCase {
    XgCallsiteId callsite_id;
    XgClassId receiver_class_id;
    XgMethodId method_id;
    XgClassId method_owner_class_id;
    XgFuncId method_body_func_id;
    uint32_t method_name_id;
    uint32_t method_signature_key;
    XgMethodId method_root_id;
    uint32_t method_override_depth;
    uint32_t evidence;
} XaotDispatchTargetCase;

enum {
    XAOT_INTERFACE_USE_REASON_IMPLEMENTS = 1u << 0,
    XAOT_INTERFACE_USE_REASON_VALUE = 1u << 1,
    XAOT_INTERFACE_USE_REASON_ARRAY = 1u << 2,
    XAOT_INTERFACE_USE_REASON_FIELD = 1u << 3,
    XAOT_INTERFACE_USE_REASON_RETURN = 1u << 4,
    XAOT_INTERFACE_USE_REASON_CAPTURE = 1u << 5,
    XAOT_INTERFACE_USE_REASON_PARAM = 1u << 6,
};

enum {
    XAOT_INTERFACE_USE_NEEDS_IFACE_OBJECT = 1u << 0,
    XAOT_INTERFACE_USE_NEEDS_ITABLE = 1u << 1,
    XAOT_INTERFACE_USE_TYPE_SWITCHABLE = 1u << 2,
};

typedef struct XaotInterfaceUsePlan {
    XgInterfaceId interface_id;
    XgClassId implementor_class_id;
    XgCallsiteId use_site_id;
    uint32_t reason;
    uint32_t flags;
} XaotInterfaceUsePlan;

typedef enum XaotInterfaceAbiSource {
    XAOT_INTERFACE_ABI_SOURCE_NONE = 0,
    XAOT_INTERFACE_ABI_SOURCE_BOXED_VALUE = 1,
    XAOT_INTERFACE_ABI_SOURCE_NATIVE_TYPE_ID = 2,
    XAOT_INTERFACE_ABI_SOURCE_DISPATCH_SLOT = 3,
} XaotInterfaceAbiSource;

enum {
    XAOT_INTERFACE_ABI_NEEDS_IFACE_OBJECT = 1u << 0,
    XAOT_INTERFACE_ABI_NEEDS_ITABLE = 1u << 1,
    XAOT_INTERFACE_ABI_NEEDS_TYPE_SWITCH_TAG = 1u << 2,
    XAOT_INTERFACE_ABI_BOXED_RECEIVER = 1u << 3,
};

enum {
    XAOT_INTERFACE_ABI_EV_GLOBAL_CALLSITE = 1u << 0,
    XAOT_INTERFACE_ABI_EV_INTERFACE_METHODS = 1u << 1,
    XAOT_INTERFACE_ABI_EV_IMPLEMENTOR_SET = 1u << 2,
    XAOT_INTERFACE_ABI_EV_DISPATCH_PLAN = 1u << 3,
    XAOT_INTERFACE_ABI_EV_OBJECT_USE = 1u << 4,
};

enum {
    XAOT_INTERFACE_ABI_UNPROVEN_NONE = 0,
    XAOT_INTERFACE_ABI_UNPROVEN_NO_CALLSITE = 1,
};

typedef struct XaotInterfaceAbiPlan {
    XgInterfaceId interface_id;
    uint32_t callsite_count;
    uint32_t implementor_count;
    uint32_t method_slot_count;
    uint32_t flags;
    uint8_t data_source;
    uint8_t type_source;
    uint8_t itable_source;
    uint8_t tag_source;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotInterfaceAbiPlan;

typedef enum XaotSpecializationAction {
    XAOT_SPECIALIZATION_DIRECT = 1,
    XAOT_SPECIALIZATION_TYPE_SWITCH,
    XAOT_SPECIALIZATION_FALLBACK,
} XaotSpecializationAction;

enum {
    XAOT_SPECIALIZATION_EV_GLOBAL_CALLSITE = 1u << 0,
    XAOT_SPECIALIZATION_EV_DISPATCH_PLAN = 1u << 1,
    XAOT_SPECIALIZATION_EV_IMPLEMENTOR_SET = 1u << 2,
    XAOT_SPECIALIZATION_EV_TARGET_CASES = 1u << 3,
};

enum {
    XAOT_SPECIALIZATION_UNPROVEN_NONE = 0,
    XAOT_SPECIALIZATION_UNPROVEN_NO_INTERFACE = 1,
    XAOT_SPECIALIZATION_UNPROVEN_NO_TARGET = 2,
    XAOT_SPECIALIZATION_UNPROVEN_LARGE_SET = 3,
    XAOT_SPECIALIZATION_UNPROVEN_DYNAMIC_BOUNDARY = 4,
};

typedef struct XaotGenericSpecializationPlan {
    XgCallsiteId callsite_id;
    XgFuncId owner_func_id;
    XgInterfaceId interface_id;
    uint32_t method_name_id;
    uint32_t method_signature_key;
    XgClassId single_implementor_class_id;
    uint32_t implementor_count;
    uint16_t target_count;
    uint8_t dispatch_kind;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotGenericSpecializationPlan;

typedef enum XaotGenericInstantiationAction {
    XAOT_GENERIC_INSTANTIATION_RECORD_ROOT = 1,
    XAOT_GENERIC_INSTANTIATION_SPECIALIZED_BODY,
    XAOT_GENERIC_INSTANTIATION_SPECIALIZED_ABI,
    XAOT_GENERIC_INSTANTIATION_SPECIALIZED_STORAGE,
} XaotGenericInstantiationAction;

enum {
    XAOT_GENERIC_INST_EV_GLOBAL_ROW = 1u << 0,
    XAOT_GENERIC_INST_EV_CONCRETE_TYPES = 1u << 1,
    XAOT_GENERIC_INST_EV_ORIGIN_ANCHOR = 1u << 2,
    XAOT_GENERIC_INST_EV_ROOT_CALLSITE = 1u << 3,
    XAOT_GENERIC_INST_EV_INTERFACE_CONSTRAINT = 1u << 4,
    XAOT_GENERIC_INST_EV_SPECIALIZED_BODY = 1u << 5,
    XAOT_GENERIC_INST_EV_SPECIALIZED_ABI = 1u << 6,
    XAOT_GENERIC_INST_EV_SPECIALIZED_STORAGE = 1u << 7,
};

enum {
    XAOT_GENERIC_INST_UNPROVEN_NONE = 0,
    XAOT_GENERIC_INST_UNPROVEN_NO_SPECIALIZED_BODY = 1,
    XAOT_GENERIC_INST_UNPROVEN_NO_SPECIALIZED_STORAGE = 2,
    XAOT_GENERIC_INST_UNPROVEN_NO_CONCRETE_TYPES = 3,
};

typedef struct XaotGenericInstantiationPlan {
    XgGenericInstId generic_inst_id;
    XgModuleId module_id;
    XgDeclId origin_decl_id;
    XgFuncId origin_func_id;
    XgMethodId origin_method_id;
    XgClassId origin_class_id;
    XgFuncId specialized_func_id;
    XgClassId specialized_class_id;
    XgCallsiteId root_callsite_id;
    XgInterfaceId constraint_interface_id;
    uint32_t name_id;
    uint32_t type_key;
    uint32_t type_arg_key_start;
    uint16_t type_arg_count;
    uint8_t inst_kind;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotGenericInstantiationPlan;

typedef enum XaotGenericBodyAction {
    XAOT_GENERIC_BODY_CLONE = 1,
    XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY,
    XAOT_GENERIC_BODY_DIRECT_CONSTRAINT_CALL,
    XAOT_GENERIC_BODY_REJECT,
} XaotGenericBodyAction;

typedef enum XaotGenericStorageAction {
    XAOT_GENERIC_STORAGE_TYPED_INLINE = 1,
    XAOT_GENERIC_STORAGE_REF_LANE,
    XAOT_GENERIC_STORAGE_BOXED,
    XAOT_GENERIC_STORAGE_SPECIALIZED_CLASS,
    XAOT_GENERIC_STORAGE_SPECIALIZED_STRUCT,
    XAOT_GENERIC_STORAGE_REJECT,
} XaotGenericStorageAction;

typedef enum XaotGenericCodeSizeAction {
    XAOT_GENERIC_CODESIZE_ALLOW_CLONE = 1,
    XAOT_GENERIC_CODESIZE_SHARE_CANONICAL_BODY,
    XAOT_GENERIC_CODESIZE_FORCE_CLONE,
    XAOT_GENERIC_CODESIZE_REJECT,
} XaotGenericCodeSizeAction;

enum {
    XAOT_GENERIC_BODY_EV_GLOBAL_ROW = 1u << 0,
    XAOT_GENERIC_BODY_EV_GENERIC_INST = 1u << 1,
    XAOT_GENERIC_BODY_EV_TYPE_ARGS = 1u << 2,
    XAOT_GENERIC_BODY_EV_ORIGIN_BODY = 1u << 3,
    XAOT_GENERIC_BODY_EV_SPECIALIZED_BODY = 1u << 4,
    XAOT_GENERIC_BODY_EV_ROOT_CALLSITE = 1u << 5,
};

enum {
    XAOT_GENERIC_STORAGE_EV_GLOBAL_ROW = 1u << 0,
    XAOT_GENERIC_STORAGE_EV_GENERIC_INST = 1u << 1,
    XAOT_GENERIC_STORAGE_EV_SPECIALIZED_TYPE = 1u << 2,
    XAOT_GENERIC_STORAGE_EV_CONTAINER_PLAN = 1u << 3,
};

enum {
    XAOT_GENERIC_CODESIZE_EV_GLOBAL_ROW = 1u << 0,
    XAOT_GENERIC_CODESIZE_EV_GENERIC_INST = 1u << 1,
    XAOT_GENERIC_CODESIZE_EV_BODY_USE = 1u << 2,
    XAOT_GENERIC_CODESIZE_EV_THRESHOLD = 1u << 3,
};

enum {
    XAOT_GENERIC_DEEPEN_UNPROVEN_NONE = 0,
    XAOT_GENERIC_DEEPEN_UNPROVEN_MISSING_CONCRETE_TYPES = 1,
    XAOT_GENERIC_DEEPEN_UNPROVEN_NO_SPECIALIZED_BODY = 2,
    XAOT_GENERIC_DEEPEN_UNPROVEN_UNSUPPORTED_STORAGE = 3,
    XAOT_GENERIC_DEEPEN_UNPROVEN_CODESIZE_THRESHOLD = 4,
    XAOT_GENERIC_DEEPEN_UNPROVEN_DYNAMIC_BOUNDARY = 5,
};

typedef struct XaotGenericBodyPlan {
    XgGenericBodyUseId use_id;
    XgGenericInstId generic_inst_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    XgFuncId origin_body_func_id;
    XgFuncId specialized_body_func_id;
    XgCallsiteId root_callsite_id;
    uint32_t type_key;
    uint32_t type_arg_key_start;
    uint16_t type_arg_count;
    uint32_t estimated_body_size;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotGenericBodyPlan;

typedef struct XaotGenericStoragePlan {
    XgGenericStorageId storage_id;
    XgGenericInstId generic_inst_id;
    XgModuleId module_id;
    uint8_t storage_kind;
    uint8_t action;
    uint32_t origin_type_key;
    uint32_t specialized_type_key;
    uint32_t elem_type_key;
    uint32_t key_type_key;
    uint32_t value_type_key;
    uint32_t container_plan_id;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotGenericStoragePlan;

typedef struct XaotGenericCodeSizePlan {
    XgGenericCodeSizeId code_size_id;
    XgGenericInstId generic_inst_id;
    XgModuleId module_id;
    XgGenericBodyUseId body_use_id;
    uint32_t origin_body_size_estimate;
    uint32_t specialized_body_size_estimate;
    uint32_t instantiation_count;
    uint32_t threshold;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotGenericCodeSizePlan;

typedef enum XaotDeriveAction {
    XAOT_DERIVE_FIELD_TABLE_SIDECAR = 1,
    XAOT_DERIVE_INLINE_GENERATED_BODY,
    XAOT_DERIVE_METADATA_ONLY,
    XAOT_DERIVE_DCE,
    XAOT_DERIVE_REJECT,
} XaotDeriveAction;

enum {
    XAOT_DERIVE_EV_GLOBAL_ROW = 1u << 0,
    XAOT_DERIVE_EV_OPT_IN = 1u << 1,
    XAOT_DERIVE_EV_FIELD_TABLE = 1u << 2,
    XAOT_DERIVE_EV_GENERATED_METHOD = 1u << 3,
};

enum {
    XAOT_DERIVE_UNPROVEN_NONE = 0,
    XAOT_DERIVE_UNPROVEN_NO_REACHABILITY = 1,
    XAOT_DERIVE_UNPROVEN_INVALID_KIND = 2,
};

typedef struct XaotDerivePlan {
    XgDeriveId derive_id;
    XgDeclId owner_decl_id;
    uint32_t type_key;
    uint8_t derive_kind;
    uint8_t action;
    uint32_t field_start;
    uint16_t field_count;
    uint32_t method_start;
    uint16_t method_count;
    uint32_t sidecar_index;
    XgFuncId generated_body_func_id;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotDerivePlan;

typedef enum XaotDerivedEqHashAction {
    XAOT_DERIVED_EQ_HASH_BUILTIN_FIELDS_INLINE = 1,
    XAOT_DERIVED_EQ_HASH_RECURSIVE_DERIVE_INLINE,
    XAOT_DERIVED_EQ_HASH_DIRECT_GENERATED_CALL,
    XAOT_DERIVED_EQ_HASH_REJECT_UNHASHABLE,
} XaotDerivedEqHashAction;

enum {
    XAOT_EQ_HASH_EV_EQ_ROW = 1u << 0,
    XAOT_EQ_HASH_EV_HASH_ROW = 1u << 1,
    XAOT_EQ_HASH_EV_SAME_TYPE = 1u << 2,
    XAOT_EQ_HASH_EV_SAME_FIELDS = 1u << 3,
    XAOT_EQ_HASH_EV_EQ_BODY = 1u << 4,
    XAOT_EQ_HASH_EV_HASH_BODY = 1u << 5,
};

enum {
    XAOT_EQ_HASH_UNPROVEN_NONE = 0,
    XAOT_EQ_HASH_UNPROVEN_MISSING_EQ = 1,
    XAOT_EQ_HASH_UNPROVEN_MISSING_HASH = 2,
    XAOT_EQ_HASH_UNPROVEN_TYPE_MISMATCH = 3,
    XAOT_EQ_HASH_UNPROVEN_FIELD_MISMATCH = 4,
};

typedef struct XaotDerivedEqHashPlan {
    XgDeclId owner_decl_id;
    uint32_t type_key;
    XgDeriveId eq_derive_id;
    XgDeriveId hash_derive_id;
    uint32_t field_start;
    uint16_t field_count;
    XgFuncId eq_body_func_id;
    XgFuncId hash_body_func_id;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotDerivedEqHashPlan;

typedef enum XaotDerivedCloneAction {
    XAOT_DERIVED_CLONE_BITWISE_COPY = 1,
    XAOT_DERIVED_CLONE_FIELDWISE_COPY,
    XAOT_DERIVED_CLONE_DEEP_COPY_PLAN,
    XAOT_DERIVED_CLONE_DIRECT_GENERATED_CALL,
    XAOT_DERIVED_CLONE_REJECT,
} XaotDerivedCloneAction;

enum {
    XAOT_CLONE_EV_CLONE_ROW = 1u << 0,
    XAOT_CLONE_EV_FIELD_TABLE = 1u << 1,
    XAOT_CLONE_EV_GENERATED_BODY = 1u << 2,
    XAOT_CLONE_EV_TRANSFER_PLAN = 1u << 3,
};

enum {
    XAOT_CLONE_UNPROVEN_NONE = 0,
    XAOT_CLONE_UNPROVEN_MISSING_CLONE = 1,
    XAOT_CLONE_UNPROVEN_UNSAFE_FIELD = 2,
    XAOT_CLONE_UNPROVEN_MISSING_TRANSFER_PLAN = 3,
};

typedef struct XaotDerivedClonePlan {
    XgDeclId owner_decl_id;
    uint32_t type_key;
    XgDeriveId clone_derive_id;
    uint32_t field_start;
    uint16_t field_count;
    XgFuncId clone_body_func_id;
    uint32_t transfer_plan_id;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotDerivedClonePlan;

typedef enum XaotJsonShapeAction {
    XAOT_JSON_SHAPE_OPEN_DYNAMIC = 1,
    XAOT_JSON_SHAPE_HIDDEN_CLASS,
    XAOT_JSON_SHAPE_RECORD_BRIDGE,
    XAOT_JSON_SHAPE_REJECT,
} XaotJsonShapeAction;

typedef enum XaotJsonAccessAction {
    XAOT_JSON_ACCESS_DIRECT_INDEX = 1,
    XAOT_JSON_ACCESS_SHAPE_GUARD_INDEX,
    XAOT_JSON_ACCESS_COMPUTED_KEY_GUARD,
    XAOT_JSON_ACCESS_DYNAMIC_LOOKUP,
    XAOT_JSON_ACCESS_REJECT,
} XaotJsonAccessAction;

typedef enum XaotJsonCodecAction {
    XAOT_JSON_CODEC_PARSE_DOM_BRIDGE = 1,
    XAOT_JSON_CODEC_PARSE_RUNTIME_DIRECT,
    XAOT_JSON_CODEC_DECODE_VALIDATE_COPY,
    XAOT_JSON_CODEC_ENCODE_FIELD_TABLE,
    XAOT_JSON_CODEC_ENCODE_DERIVE_SIDECAR,
    XAOT_JSON_CODEC_STRINGIFY_DYNAMIC_WALK,
    XAOT_JSON_CODEC_REJECT,
} XaotJsonCodecAction;

enum {
    XAOT_JSON_EV_GLOBAL_ROW = 1u << 0,
    XAOT_JSON_EV_STATIC_KEY = 1u << 1,
    XAOT_JSON_EV_RECEIVER_SHAPE = 1u << 2,
    XAOT_JSON_EV_FIELD_INDEX = 1u << 3,
    XAOT_JSON_EV_RECORD_BRIDGE = 1u << 4,
    XAOT_JSON_EV_INPUT_SHAPE = 1u << 5,
    XAOT_JSON_EV_OUTPUT_SHAPE = 1u << 6,
    XAOT_JSON_EV_TARGET_TYPE = 1u << 7,
    XAOT_JSON_EV_DERIVE = 1u << 8,
};

enum {
    XAOT_JSON_UNPROVEN_NONE = 0,
    XAOT_JSON_UNPROVEN_COMPUTED_KEY = 1,
    XAOT_JSON_UNPROVEN_RECEIVER_SHAPE_UNKNOWN = 2,
    XAOT_JSON_UNPROVEN_STALE_SHAPE = 3,
    XAOT_JSON_UNPROVEN_INVALID_KIND = 4,
    XAOT_JSON_UNPROVEN_OPEN_SHAPE = 5,
    XAOT_JSON_UNPROVEN_MISSING_TARGET_TYPE = 6,
    XAOT_JSON_UNPROVEN_UNSUPPORTED_CODEC = 7,
};

typedef struct XaotJsonShapePlan {
    XgJsonShapeId json_shape_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    uint32_t type_key;
    uint32_t field_name_start;
    uint16_t field_count;
    uint8_t shape_kind;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
    uint64_t shape_hash;
} XaotJsonShapePlan;

typedef struct XaotJsonAccessPlan {
    XgJsonAccessId json_access_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    XgJsonShapeId receiver_shape_id;
    uint32_t key_name_id;
    uint32_t result_type_key;
    uint16_t field_ordinal;
    uint8_t access_kind;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotJsonAccessPlan;

typedef struct XaotJsonCodecPlan {
    XgJsonCodecId codec_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint8_t codec_kind;
    uint8_t action;
    uint32_t input_type_key;
    uint32_t target_type_key;
    XgJsonShapeId input_shape_id;
    XgJsonShapeId output_shape_id;
    uint16_t field_count;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotJsonCodecPlan;

typedef enum XaotRecordShapeAction {
    XAOT_RECORD_SHAPE_SEALED_RECORD = 1,
    XAOT_RECORD_SHAPE_OPTIONS_BAG,
    XAOT_RECORD_SHAPE_SPREAD_RESULT,
    XAOT_RECORD_SHAPE_STATIC_RECORD,
    XAOT_RECORD_SHAPE_REJECT,
} XaotRecordShapeAction;

typedef enum XaotRecordAccessAction {
    XAOT_RECORD_ACCESS_DIRECT_FIELD = 1,
    XAOT_RECORD_ACCESS_COPY_DESTRUCTURE,
    XAOT_RECORD_ACCESS_CHECKED_FIELD,
    XAOT_RECORD_ACCESS_REJECT,
} XaotRecordAccessAction;

enum {
    XAOT_RECORD_EV_GLOBAL_ROW = 1u << 0,
    XAOT_RECORD_EV_SEALED = 1u << 1,
    XAOT_RECORD_EV_STATIC_FIELD = 1u << 2,
    XAOT_RECORD_EV_RECEIVER_SHAPE = 1u << 3,
    XAOT_RECORD_EV_FIELD_INDEX = 1u << 4,
    XAOT_RECORD_EV_JSON_BRIDGE = 1u << 5,
};

enum {
    XAOT_RECORD_UNPROVEN_NONE = 0,
    XAOT_RECORD_UNPROVEN_INVALID_KIND = 1,
    XAOT_RECORD_UNPROVEN_RECEIVER_SHAPE_UNKNOWN = 2,
    XAOT_RECORD_UNPROVEN_STALE_SHAPE = 3,
    XAOT_RECORD_UNPROVEN_DYNAMIC_FIELD = 4,
};

typedef struct XaotRecordShapePlan {
    XgRecordShapeId record_shape_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    uint32_t type_key;
    uint32_t field_name_start;
    uint16_t field_count;
    uint8_t shape_kind;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
    uint64_t shape_hash;
} XaotRecordShapePlan;

typedef struct XaotRecordAccessPlan {
    XgRecordAccessId record_access_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    XgRecordShapeId receiver_shape_id;
    uint32_t field_name_id;
    uint32_t result_type_key;
    uint16_t field_ordinal;
    uint8_t access_kind;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotRecordAccessPlan;

typedef enum XaotOptionsAction {
    XAOT_OPTIONS_DEFAULT_ELIDED = 1,
    XAOT_OPTIONS_DEFAULT_FILL_TABLE,
    XAOT_OPTIONS_REQUIRED_CHECK,
    XAOT_OPTIONS_CALLSITE_SPECIALIZED,
    XAOT_OPTIONS_REJECT,
} XaotOptionsAction;

enum {
    XAOT_OPTIONS_EV_GLOBAL_ROW = 1u << 0,
    XAOT_OPTIONS_EV_CALLSITE = 1u << 1,
    XAOT_OPTIONS_EV_PARAM_SHAPE = 1u << 2,
    XAOT_OPTIONS_EV_SUPPLIED_SHAPE = 1u << 3,
    XAOT_OPTIONS_EV_DEFAULT_MASK = 1u << 4,
    XAOT_OPTIONS_EV_REQUIRED_MASK = 1u << 5,
};

enum {
    XAOT_OPTIONS_UNPROVEN_NONE = 0,
    XAOT_OPTIONS_UNPROVEN_INVALID_ACTION = 1,
    XAOT_OPTIONS_UNPROVEN_MISSING_CALLSITE = 2,
    XAOT_OPTIONS_UNPROVEN_STALE_SHAPE = 3,
    XAOT_OPTIONS_UNPROVEN_OWNER_MISMATCH = 4,
    XAOT_OPTIONS_UNPROVEN_COUNT_MISMATCH = 5,
};

typedef struct XaotOptionsPlan {
    XgOptionsId options_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    XgCallsiteId callsite_id;
    XgRecordShapeId param_shape_id;
    XgRecordShapeId supplied_shape_id;
    uint32_t supplied_field_mask_id;
    uint32_t default_field_mask_id;
    uint32_t required_field_mask_id;
    uint16_t supplied_count;
    uint16_t default_count;
    uint16_t required_count;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotOptionsPlan;

typedef enum XaotMapShapeAction {
    XAOT_MAP_SHAPE_RUNTIME_HASH = 1,
    XAOT_MAP_SHAPE_PREALLOC_HASH,
    XAOT_MAP_SHAPE_SMALL_INLINE,
    XAOT_MAP_SHAPE_DENSE_ENUM_TABLE,
    XAOT_MAP_SHAPE_DENSE_INT_TABLE,
    XAOT_MAP_SHAPE_BOOL_DIRECT,
    XAOT_MAP_SHAPE_READONLY_STATIC_TABLE,
    XAOT_MAP_SHAPE_REJECT,
} XaotMapShapeAction;

typedef enum XaotKeyAccessAction {
    XAOT_KEY_ACCESS_DIRECT_DENSE_INDEX = 1,
    XAOT_KEY_ACCESS_BOOL_DIRECT_LOOKUP,
    XAOT_KEY_ACCESS_PREHASHED_LOOKUP,
    XAOT_KEY_ACCESS_INLINE_SMALL_SCAN,
    XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP,
    XAOT_KEY_ACCESS_GENERIC_HASH_LOOKUP,
    XAOT_KEY_ACCESS_REJECT,
} XaotKeyAccessAction;

typedef enum XaotHashEqAction {
    XAOT_HASH_EQ_BUILTIN_INLINE = 1,
    XAOT_HASH_EQ_DERIVE_INLINE,
    XAOT_HASH_EQ_DIRECT_CALL,
    XAOT_HASH_EQ_DYNAMIC_REJECT,
} XaotHashEqAction;

enum {
    XAOT_MAP_EV_GLOBAL_ROW = 1u << 0,
    XAOT_MAP_EV_LITERAL = 1u << 1,
    XAOT_MAP_EV_CONST_KEY = 1u << 2,
    XAOT_MAP_EV_PREHASH = 1u << 3,
    XAOT_MAP_EV_HASH_EQ = 1u << 4,
    XAOT_MAP_EV_DENSE_DOMAIN = 1u << 5,
    XAOT_MAP_EV_SMALL = 1u << 6,
    XAOT_MAP_EV_BOOL_DOMAIN = 1u << 7,
};

enum {
    XAOT_MAP_UNPROVEN_NONE = 0,
    XAOT_MAP_UNPROVEN_INVALID_KIND = 1,
    XAOT_MAP_UNPROVEN_COMPUTED_KEY = 2,
    XAOT_MAP_UNPROVEN_MISSING_SHAPE = 3,
    XAOT_MAP_UNPROVEN_MISSING_HASH_EQ = 4,
    XAOT_MAP_UNPROVEN_UNHASHABLE = 5,
};

typedef struct XaotMapShapePlan {
    XgMapShapeId shape_id;
    XgModuleId module_id;
    XgFuncId owner_func_id;
    uint8_t container_kind;
    uint8_t source;
    uint32_t key_type_key;
    uint32_t value_type_key;
    uint32_t entry_start;
    uint16_t entry_count;
    uint32_t literal_count;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
    uint64_t shape_hash;
} XaotMapShapePlan;

typedef struct XaotKeyAccessPlan {
    XgKeyAccessId access_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t container_kind;
    uint8_t op;
    XgMapShapeId receiver_shape_id;
    uint32_t receiver_type_key;
    uint32_t key_type_key;
    uint32_t value_type_key;
    uint32_t key_const_id;
    uint64_t key_prehash;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotKeyAccessPlan;

typedef struct XaotHashEqPlan {
    XgHashEqId hash_eq_id;
    uint32_t type_key;
    uint8_t kind;
    XgDeriveId eq_derive_id;
    XgDeriveId hash_derive_id;
    XgFuncId eq_func_id;
    XgFuncId hash_func_id;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotHashEqPlan;

typedef enum XaotSequenceAccessAction {
    XAOT_SEQUENCE_ACCESS_CHECKED_INDEX = 1,
    XAOT_SEQUENCE_ACCESS_DIRECT_LENGTH,
    XAOT_SEQUENCE_ACCESS_CHECKED_SLICE,
    XAOT_SEQUENCE_ACCESS_ITER_HELPER,
    XAOT_SEQUENCE_ACCESS_REJECT,
} XaotSequenceAccessAction;

typedef enum XaotCapacityAction {
    XAOT_CAPACITY_CHECKED_GROW = 1,
    XAOT_CAPACITY_RESERVE_ONCE,
    XAOT_CAPACITY_CLEAR_DIRECT,
    XAOT_CAPACITY_BUILDER_FINISH,
    XAOT_CAPACITY_RUNTIME_HELPER,
    XAOT_CAPACITY_REJECT,
} XaotCapacityAction;

typedef enum XaotBulkAction {
    XAOT_BULK_INLINE_MEMCPY = 1,
    XAOT_BULK_INLINE_MEMMOVE,
    XAOT_BULK_INLINE_MEMSET,
    XAOT_BULK_INLINE_MEMCMP,
    XAOT_BULK_TYPED_LOOP,
    XAOT_BULK_RUNTIME_HELPER,
    XAOT_BULK_REJECT,
} XaotBulkAction;

typedef enum XaotEncodingAction {
    XAOT_ENCODING_VALIDATE_ELIDED = 1,
    XAOT_ENCODING_VALIDATE_ONCE,
    XAOT_ENCODING_RUNTIME_VALIDATE,
    XAOT_ENCODING_TRANSCODE,
    XAOT_ENCODING_REJECT,
} XaotEncodingAction;

enum {
    XAOT_SEQUENCE_EV_GLOBAL_ROW = 1u << 0,
    XAOT_SEQUENCE_EV_RECEIVER_TYPE = 1u << 1,
    XAOT_SEQUENCE_EV_ELEM_TYPE = 1u << 2,
    XAOT_SEQUENCE_EV_CONST_INDEX = 1u << 3,
    XAOT_SEQUENCE_EV_LENGTH_EXPR = 1u << 4,
    XAOT_SEQUENCE_EV_MUTATING = 1u << 5,
};

enum {
    XAOT_SEQUENCE_UNPROVEN_NONE = 0,
    XAOT_SEQUENCE_UNPROVEN_INVALID_KIND = 1,
    XAOT_SEQUENCE_UNPROVEN_MISSING_RECEIVER_TYPE = 2,
    XAOT_SEQUENCE_UNPROVEN_COMPUTED_INDEX = 3,
    XAOT_SEQUENCE_UNPROVEN_NEGATIVE_INDEX = 4,
    XAOT_SEQUENCE_UNPROVEN_DYNAMIC_LENGTH = 5,
};

enum {
    XAOT_CAPACITY_EV_GLOBAL_ROW = 1u << 0,
    XAOT_CAPACITY_EV_RECEIVER_TYPE = 1u << 1,
    XAOT_CAPACITY_EV_ELEM_TYPE = 1u << 2,
    XAOT_CAPACITY_EV_EXACT_COUNT = 1u << 3,
    XAOT_CAPACITY_EV_LOOP_APPEND = 1u << 4,
    XAOT_CAPACITY_EV_MAY_GROW = 1u << 5,
};

enum {
    XAOT_CAPACITY_UNPROVEN_NONE = 0,
    XAOT_CAPACITY_UNPROVEN_INVALID_KIND = 1,
    XAOT_CAPACITY_UNPROVEN_MISSING_RECEIVER_TYPE = 2,
    XAOT_CAPACITY_UNPROVEN_COUNT_UNKNOWN = 3,
};

enum {
    XAOT_BULK_EV_GLOBAL_ROW = 1u << 0,
    XAOT_BULK_EV_POD = 1u << 1,
    XAOT_BULK_EV_OVERLAP_POSSIBLE = 1u << 2,
    XAOT_BULK_EV_READONLY_SRC = 1u << 3,
    XAOT_BULK_EV_WRITE_BARRIER = 1u << 4,
    XAOT_BULK_EV_LENGTH_EXPR = 1u << 5,
};

enum {
    XAOT_BULK_UNPROVEN_NONE = 0,
    XAOT_BULK_UNPROVEN_INVALID_KIND = 1,
    XAOT_BULK_UNPROVEN_NON_POD = 2,
    XAOT_BULK_UNPROVEN_WRITE_BARRIER = 3,
    XAOT_BULK_UNPROVEN_LENGTH_UNKNOWN = 4,
};

enum {
    XAOT_ENCODING_EV_GLOBAL_ROW = 1u << 0,
    XAOT_ENCODING_EV_KNOWN_UTF8 = 1u << 1,
    XAOT_ENCODING_EV_VALIDATED_ONCE = 1u << 2,
    XAOT_ENCODING_EV_SCALAR_BOUNDARY = 1u << 3,
    XAOT_ENCODING_EV_STATIC_LITERAL = 1u << 4,
    XAOT_ENCODING_EV_INPUT_TYPE = 1u << 5,
    XAOT_ENCODING_EV_OUTPUT_TYPE = 1u << 6,
};

enum {
    XAOT_ENCODING_UNPROVEN_NONE = 0,
    XAOT_ENCODING_UNPROVEN_INVALID_KIND = 1,
    XAOT_ENCODING_UNPROVEN_RAW_BYTES_UNKNOWN = 2,
};

typedef struct XaotSequenceAccessPlan {
    XgSequenceAccessId access_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t sequence_kind;
    uint8_t access_kind;
    uint32_t receiver_type_key;
    uint32_t elem_type_key;
    uint32_t index_expr_id;
    uint32_t length_expr_id;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotSequenceAccessPlan;

typedef struct XaotCapacityPlan {
    XgCapacityOpId op_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t sequence_kind;
    uint8_t op_kind;
    uint32_t receiver_type_key;
    uint32_t elem_type_key;
    uint32_t count_expr_id;
    uint32_t loop_id;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotCapacityPlan;

typedef struct XaotBulkPlan {
    XgBulkOpId op_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t op_kind;
    uint32_t elem_type_key;
    uint32_t src_type_key;
    uint32_t dst_type_key;
    uint32_t length_expr_id;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotBulkPlan;

typedef struct XaotEncodingPlan {
    XgEncodingOpId op_id;
    XgFuncId owner_func_id;
    uint32_t source_span_id;
    uint32_t body_ordinal;
    uint8_t op_kind;
    uint32_t input_type_key;
    uint32_t output_type_key;
    uint8_t action;
    uint32_t evidence;
    uint8_t unproven_reason;
} XaotEncodingPlan;

enum {
    XAOT_METADATA_EV_GLOBAL_BODY = 1u << 0,
    XAOT_METADATA_EV_DECL_ATTRIBUTE = 1u << 1,
};

enum {
    XAOT_METADATA_UNPROVEN_NONE = 0,
    XAOT_METADATA_UNPROVEN_NO_REACHABILITY = 1,
};

typedef struct XaotMetadataReachabilityPlan {
    uint32_t metadata;
    uint32_t body_count;
    uint32_t decl_count;
    uint32_t evidence;
    uint32_t profile_action;
    uint8_t unproven_reason;
} XaotMetadataReachabilityPlan;

enum {
    XAOT_CAPABILITY_EV_GLOBAL_BODY = 1u << 0,
    XAOT_CAPABILITY_EV_TRANSFER_PLAN = 1u << 1,
};

typedef enum XaotCapabilityProfileAction {
    XAOT_CAPABILITY_ACTION_ALLOW = 1,
    XAOT_CAPABILITY_ACTION_LINK,
    XAOT_CAPABILITY_ACTION_REJECT,
    XAOT_CAPABILITY_ACTION_DEBUG_ONLY,
} XaotCapabilityProfileAction;

enum {
    XAOT_CAPABILITY_UNPROVEN_NONE = 0,
    XAOT_CAPABILITY_UNPROVEN_NO_BODY = 1,
};

typedef struct XaotCapabilityPlan {
    uint32_t capability;
    uint32_t body_count;
    uint32_t transfer_count;
    uint32_t evidence;
    uint32_t profile_action;
    uint8_t unproven_reason;
} XaotCapabilityPlan;

enum {
    XAOT_STATIC_DATA_EV_GLOBAL_BODY = 1u << 0,
};

typedef enum XaotStaticDataAction {
    XAOT_STATIC_DATA_ACTION_PROVE = 1,
    XAOT_STATIC_DATA_ACTION_MATERIALIZE,
    XAOT_STATIC_DATA_ACTION_RUNTIME_INIT,
    XAOT_STATIC_DATA_ACTION_REJECT,
} XaotStaticDataAction;

typedef enum XaotStaticDataSection {
    XAOT_STATIC_DATA_SECTION_NONE = 0,
    XAOT_STATIC_DATA_SECTION_EVIDENCE,
    XAOT_STATIC_DATA_SECTION_RODATA,
    XAOT_STATIC_DATA_SECTION_RUNTIME_INIT,
} XaotStaticDataSection;

enum {
    XAOT_STATIC_DATA_UNPROVEN_NONE = 0,
    XAOT_STATIC_DATA_UNPROVEN_NO_BODY = 1,
};

typedef struct XaotStaticDataPlan {
    uint32_t static_data;
    uint32_t body_count;
    uint32_t evidence;
    uint32_t action;
    uint32_t section;
    uint32_t align;
    uint64_t type_hash;
    uint64_t data_hash;
    uint8_t unproven_reason;
} XaotStaticDataPlan;

enum {
    XAOT_LINK_DEP_EV_GLOBAL_SUMMARY = 1u << 0,
};

enum {
    XAOT_LINK_DEP_UNPROVEN_NONE = 0,
    XAOT_LINK_DEP_UNPROVEN_NO_SUMMARY = 1,
};

typedef struct XaotLinkDependencyPlan {
    XgLinkId link_id;
    uint8_t kind;
    uint32_t name_id;
    uint32_t evidence;
    uint8_t unproven_reason;
    char name[XG_LINK_DEP_NAME_MAX];
} XaotLinkDependencyPlan;

typedef struct XaotPrepareStats {
    uint32_t functions_total;
    uint32_t functions_native_abi;
    uint32_t functions_tagged_abi;
    uint32_t functions_coro_abi;
    uint32_t values_total;
    uint32_t values_scalar;
    uint32_t values_tagged;
    uint32_t values_ptr;
    uint32_t values_aggregate;
    uint32_t values_view;
    uint32_t values_void;
    uint32_t boundary_count;
    uint32_t containers_total;
    uint32_t containers_array;
    uint32_t containers_map;
    uint32_t containers_set;
    uint32_t containers_direct;
    uint32_t array_storage_total;
    uint32_t array_storage_read;
    uint32_t array_storage_mutable;
    uint32_t array_cache_total;
    uint32_t array_cache_read;
    uint32_t array_cache_mutable;
} XaotPrepareStats;

typedef struct XaotBundle {
    XiModule **modules;
    uint32_t nmodules;
    uint32_t entry_module;
    XaotFuncPlan *func_plans;
    uint32_t nfunc_plans;
    uint32_t func_plan_cap;
    XaotValuePlan *value_plans;
    uint32_t nvalue_plans;
    uint32_t value_plan_cap;
    XaotContainerTypePlan *container_plans;
    uint32_t ncontainer_plans;
    uint32_t container_plan_cap;
    XaotEnumPlan *enum_plans;
    uint32_t nenum_plans;
    uint32_t enum_plan_cap;
    XaotArrayStoragePlan *array_storage_plans;
    uint32_t narray_storage_plans;
    uint32_t array_storage_plan_cap;
    XaotArrayCachePlan *array_cache_plans;
    uint32_t narray_cache_plans;
    uint32_t array_cache_plan_cap;
    XaotArrayClassFieldAllocPlan *array_class_field_alloc_plans;
    uint32_t narray_class_field_alloc_plans;
    uint32_t array_class_field_alloc_plan_cap;
    XaotFuncAttrPlan *func_attr_plans;
    uint32_t nfunc_attr_plans;
    uint32_t func_attr_plan_cap;
    XaotBoundsPlan *bounds_plans;
    uint32_t nbounds_plans;
    uint32_t bounds_plan_cap;
    XaotSpanAccessPlan *span_access_plans;
    uint32_t nspan_access_plans;
    uint32_t span_access_plan_cap;
    XaotAliasPlan *alias_plans;
    uint32_t nalias_plans;
    uint32_t alias_plan_cap;
    XaotAllocationPlan *allocation_plans;
    uint32_t nallocation_plans;
    uint32_t allocation_plan_cap;
    XaotClosurePlan *closure_plans;
    uint32_t nclosure_plans;
    uint32_t closure_plan_cap;
    XaotTransferPlan *transfer_plans;
    uint32_t ntransfer_plans;
    uint32_t transfer_plan_cap;
    XaotGlobalEvidencePlan global_evidence_plan;
    XaotClassHierarchyPlan *class_hierarchy_plans;
    uint32_t nclass_hierarchy_plans;
    uint32_t class_hierarchy_plan_cap;
    XaotClassLayoutPlan *class_layout_plans;
    uint32_t nclass_layout_plans;
    uint32_t class_layout_plan_cap;
    XaotMethodDispatchPlan *method_dispatch_plans;
    uint32_t nmethod_dispatch_plans;
    uint32_t method_dispatch_plan_cap;
    XaotDispatchTargetCase *dispatch_target_cases;
    uint32_t ndispatch_target_cases;
    uint32_t dispatch_target_case_cap;
    XaotInterfaceUsePlan *interface_use_plans;
    uint32_t ninterface_use_plans;
    uint32_t interface_use_plan_cap;
    XaotInterfaceAbiPlan *interface_abi_plans;
    uint32_t ninterface_abi_plans;
    uint32_t interface_abi_plan_cap;
    XaotGenericSpecializationPlan *generic_specialization_plans;
    uint32_t ngeneric_specialization_plans;
    uint32_t generic_specialization_plan_cap;
    XaotGenericInstantiationPlan *generic_instantiation_plans;
    uint32_t ngeneric_instantiation_plans;
    uint32_t generic_instantiation_plan_cap;
    XaotGenericBodyPlan *generic_body_plans;
    uint32_t ngeneric_body_plans;
    uint32_t generic_body_plan_cap;
    XaotGenericStoragePlan *generic_storage_plans;
    uint32_t ngeneric_storage_plans;
    uint32_t generic_storage_plan_cap;
    XaotGenericCodeSizePlan *generic_code_size_plans;
    uint32_t ngeneric_code_size_plans;
    uint32_t generic_code_size_plan_cap;
    XaotDerivePlan *derive_plans;
    uint32_t nderive_plans;
    uint32_t derive_plan_cap;
    XaotDerivedEqHashPlan *derived_eq_hash_plans;
    uint32_t nderived_eq_hash_plans;
    uint32_t derived_eq_hash_plan_cap;
    XaotDerivedClonePlan *derived_clone_plans;
    uint32_t nderived_clone_plans;
    uint32_t derived_clone_plan_cap;
    XaotJsonShapePlan *json_shape_plans;
    uint32_t njson_shape_plans;
    uint32_t json_shape_plan_cap;
    XaotJsonAccessPlan *json_access_plans;
    uint32_t njson_access_plans;
    uint32_t json_access_plan_cap;
    XaotJsonCodecPlan *json_codec_plans;
    uint32_t njson_codec_plans;
    uint32_t json_codec_plan_cap;
    XaotRecordShapePlan *record_shape_plans;
    uint32_t nrecord_shape_plans;
    uint32_t record_shape_plan_cap;
    XaotRecordAccessPlan *record_access_plans;
    uint32_t nrecord_access_plans;
    uint32_t record_access_plan_cap;
    XaotOptionsPlan *options_plans;
    uint32_t noptions_plans;
    uint32_t options_plan_cap;
    XaotMapShapePlan *map_shape_plans;
    uint32_t nmap_shape_plans;
    uint32_t map_shape_plan_cap;
    XaotKeyAccessPlan *key_access_plans;
    uint32_t nkey_access_plans;
    uint32_t key_access_plan_cap;
    XaotHashEqPlan *hash_eq_plans;
    uint32_t nhash_eq_plans;
    uint32_t hash_eq_plan_cap;
    XaotSequenceAccessPlan *sequence_access_plans;
    uint32_t nsequence_access_plans;
    uint32_t sequence_access_plan_cap;
    XaotCapacityPlan *capacity_plans;
    uint32_t ncapacity_plans;
    uint32_t capacity_plan_cap;
    XaotBulkPlan *bulk_plans;
    uint32_t nbulk_plans;
    uint32_t bulk_plan_cap;
    XaotEncodingPlan *encoding_plans;
    uint32_t nencoding_plans;
    uint32_t encoding_plan_cap;
    XaotMetadataReachabilityPlan *metadata_plans;
    uint32_t nmetadata_plans;
    uint32_t metadata_plan_cap;
    XaotCapabilityPlan *capability_plans;
    uint32_t ncapability_plans;
    uint32_t capability_plan_cap;
    XaotStaticDataPlan *static_data_plans;
    uint32_t nstatic_data_plans;
    uint32_t static_data_plan_cap;
    XaotLinkDependencyPlan *link_dependency_plans;
    uint32_t nlink_dependency_plans;
    uint32_t link_dependency_plan_cap;
    XaotBoundaryStep *boundary_steps;
    uint32_t nboundary_steps;
    uint32_t boundary_step_cap;
    XaotPtrIndex value_index;             /* XiValue* -> value_plans row */
    XaotPtrIndex func_index;              /* XiFunc*  -> func_plans row */
    XaotPtrIndex array_storage_index;     /* XiValue* (value) -> array_storage_plans row */
    XaotPtrIndex array_cache_index;       /* XiValue* (value) -> array_cache_plans row */
    XaotPtrIndex array_class_field_index; /* XiValue* (origin) -> array_class_field_alloc row */
    XaotPtrIndex func_attr_index;         /* XiFunc*  -> func_attr_plans row */
    XaotPtrIndex bounds_index;            /* XiValue* (access) -> bounds_plans row */
    XaotPtrIndex span_access_index;       /* XiValue* (access op) -> span_access_plans row */
    XaotPtrIndex alias_index;             /* XiValue* (value) -> alias_plans row */
    XaotPtrIndex allocation_index;        /* XiValue* (alloc) -> allocation_plans row */
    XaotPtrIndex closure_index;           /* XiValue* (closure alloc) -> closure_plans row */
    XaotPrepareStats stats;
    const char *error_msg;
} XaotBundle;

XR_FUNC bool xaot_bundle_init(XaotBundle *bundle, XiModule **modules, uint32_t nmodules,
                              uint32_t entry_module);
XR_FUNC void xaot_bundle_free(XaotBundle *bundle);
XR_FUNC bool xaot_bundle_set_global_evidence(XaotBundle *bundle, const XgGlobalEvidence *evidence,
                                             uint32_t profile);
XR_FUNC const XaotClassHierarchyPlan *
xaot_bundle_find_class_hierarchy_plan(const XaotBundle *bundle, XgClassId class_id);
XR_FUNC const XaotClassLayoutPlan *xaot_bundle_find_class_layout_plan(const XaotBundle *bundle,
                                                                      XgClassId class_id);
XR_FUNC const XaotMethodDispatchPlan *
xaot_bundle_find_method_dispatch_plan(const XaotBundle *bundle, XgCallsiteId callsite_id);
XR_FUNC const XaotMethodDispatchPlan *
xaot_bundle_find_method_dispatch_plan_for_xi_call(const XaotBundle *bundle, const XiValue *call);
XR_FUNC const XiFunc *xaot_bundle_find_method_func(const XaotBundle *bundle, XgMethodId method_id,
                                                   const char **out_module_prefix);
XR_FUNC const XiFunc *xaot_bundle_find_body_func(const XaotBundle *bundle, XgFuncId body_func_id,
                                                 const char **out_module_prefix);
XR_FUNC const XiFunc *xaot_bundle_find_dispatch_target_func(const XaotBundle *bundle,
                                                            const XaotDispatchTargetCase *target,
                                                            const char **out_module_prefix);
XR_FUNC const XaotInterfaceUsePlan *
xaot_bundle_find_interface_use_plan(const XaotBundle *bundle, XgInterfaceId interface_id,
                                    XgClassId implementor_class_id, XgCallsiteId use_site_id);
XR_FUNC const XaotInterfaceAbiPlan *xaot_bundle_find_interface_abi_plan(const XaotBundle *bundle,
                                                                        XgInterfaceId interface_id);
XR_FUNC const XaotGenericSpecializationPlan *
xaot_bundle_find_generic_specialization_plan(const XaotBundle *bundle, XgCallsiteId callsite_id);
XR_FUNC const XaotGenericInstantiationPlan *
xaot_bundle_find_generic_instantiation_plan(const XaotBundle *bundle,
                                            XgGenericInstId generic_inst_id);
XR_FUNC const XaotGenericBodyPlan *xaot_bundle_find_generic_body_plan(const XaotBundle *bundle,
                                                                      XgGenericBodyUseId use_id);
XR_FUNC const XaotGenericStoragePlan *
xaot_bundle_find_generic_storage_plan(const XaotBundle *bundle, XgGenericStorageId storage_id);
XR_FUNC const XaotGenericCodeSizePlan *
xaot_bundle_find_generic_code_size_plan(const XaotBundle *bundle, XgGenericCodeSizeId code_size_id);
XR_FUNC const XaotDerivePlan *xaot_bundle_find_derive_plan(const XaotBundle *bundle,
                                                           XgDeriveId derive_id);
XR_FUNC const XaotDerivedEqHashPlan *xaot_bundle_find_derived_eq_hash_plan(const XaotBundle *bundle,
                                                                           uint32_t type_key);
XR_FUNC const XaotDerivedClonePlan *xaot_bundle_find_derived_clone_plan(const XaotBundle *bundle,
                                                                        uint32_t type_key);
XR_FUNC const XaotJsonShapePlan *xaot_bundle_find_json_shape_plan(const XaotBundle *bundle,
                                                                  XgJsonShapeId json_shape_id);
XR_FUNC const XaotJsonAccessPlan *xaot_bundle_find_json_access_plan(const XaotBundle *bundle,
                                                                    XgJsonAccessId json_access_id);
XR_FUNC const XaotJsonCodecPlan *xaot_bundle_find_json_codec_plan(const XaotBundle *bundle,
                                                                  XgJsonCodecId codec_id);
XR_FUNC const XaotRecordShapePlan *
xaot_bundle_find_record_shape_plan(const XaotBundle *bundle, XgRecordShapeId record_shape_id);
XR_FUNC const XaotRecordAccessPlan *
xaot_bundle_find_record_access_plan(const XaotBundle *bundle, XgRecordAccessId record_access_id);
XR_FUNC const XaotOptionsPlan *xaot_bundle_find_options_plan(const XaotBundle *bundle,
                                                             XgOptionsId options_id);
XR_FUNC const XaotMapShapePlan *xaot_bundle_find_map_shape_plan(const XaotBundle *bundle,
                                                                XgMapShapeId shape_id);
XR_FUNC const XaotKeyAccessPlan *xaot_bundle_find_key_access_plan(const XaotBundle *bundle,
                                                                  XgKeyAccessId access_id);
XR_FUNC const XaotHashEqPlan *xaot_bundle_find_hash_eq_plan(const XaotBundle *bundle,
                                                            uint32_t type_key);
XR_FUNC const XaotSequenceAccessPlan *
xaot_bundle_find_sequence_access_plan(const XaotBundle *bundle, XgSequenceAccessId access_id);
XR_FUNC const XaotCapacityPlan *xaot_bundle_find_capacity_plan(const XaotBundle *bundle,
                                                               XgCapacityOpId op_id);
XR_FUNC const XaotBulkPlan *xaot_bundle_find_bulk_plan(const XaotBundle *bundle, XgBulkOpId op_id);
XR_FUNC const XaotEncodingPlan *xaot_bundle_find_encoding_plan(const XaotBundle *bundle,
                                                               XgEncodingOpId op_id);
XR_FUNC const XaotMetadataReachabilityPlan *xaot_bundle_find_metadata_plan(const XaotBundle *bundle,
                                                                           uint32_t metadata);
XR_FUNC const XaotCapabilityPlan *xaot_bundle_find_capability_plan(const XaotBundle *bundle,
                                                                   uint32_t capability);
XR_FUNC bool xaot_bundle_sync_transfer_capability_plans(XaotBundle *bundle);
XR_FUNC uint32_t xaot_static_data_action_for(uint32_t profile, uint32_t static_data);
XR_FUNC uint32_t xaot_static_data_section_for(uint32_t static_data, uint32_t action);
XR_FUNC uint32_t xaot_static_data_align_for(uint32_t static_data, uint32_t action);
XR_FUNC uint64_t xaot_static_data_type_hash_for(uint32_t static_data, uint32_t action);
XR_FUNC uint64_t xaot_static_data_data_hash_for(const XgGlobalEvidence *evidence,
                                                uint32_t static_data, uint32_t action);
XR_FUNC const XaotStaticDataPlan *xaot_bundle_find_static_data_plan(const XaotBundle *bundle,
                                                                    uint32_t static_data);
XR_FUNC const XaotLinkDependencyPlan *
xaot_bundle_find_link_dependency_plan(const XaotBundle *bundle, XgLinkId link_id);
XR_FUNC XaotFuncPlan *xaot_bundle_add_func_plan(XaotBundle *bundle, XiFunc *func,
                                                uint32_t module_index, uint16_t depth);
XR_FUNC const XaotFuncPlan *xaot_bundle_find_func_plan(const XaotBundle *bundle,
                                                       const XiFunc *func);
XR_FUNC XaotValuePlan *xaot_bundle_add_value_plan(XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value);
XR_FUNC const XaotValuePlan *xaot_bundle_find_value_plan(const XaotBundle *bundle,
                                                         const XiValue *value);
XR_FUNC XaotValuePlan *xaot_bundle_find_value_plan_mut(XaotBundle *bundle, const XiValue *value);
XR_FUNC XaotContainerTypePlan *xaot_bundle_add_container_plan(XaotBundle *bundle,
                                                              const XrType *type);
XR_FUNC const XaotContainerTypePlan *xaot_bundle_find_container_plan(const XaotBundle *bundle,
                                                                     const XrType *type);
XR_FUNC XaotEnumPlan *xaot_bundle_add_enum_plan(XaotBundle *bundle, const XiEnumData *enum_data,
                                                uint32_t module_index);
XR_FUNC const XaotEnumPlan *xaot_bundle_find_enum_plan(const XaotBundle *bundle,
                                                       const XiEnumData *enum_data);
XR_FUNC const XaotEnumPlan *xaot_bundle_find_enum_plan_for_type(const XaotBundle *bundle,
                                                                const XrType *type);
XR_FUNC bool xaot_bundle_prepare_enum_plan_for_type(XaotBundle *bundle, const XrType *type);
XR_FUNC XaotArrayStoragePlan *
xaot_bundle_add_array_storage_plan(XaotBundle *bundle, const XiFunc *func, const XiValue *value,
                                   const XiValue *origin, uint32_t flags,
                                   const XaotContainerElemPlan *elem);
XR_FUNC const XaotArrayStoragePlan *xaot_bundle_find_array_storage_plan(const XaotBundle *bundle,
                                                                        const XiValue *value);
XR_FUNC XaotArrayCachePlan *xaot_bundle_add_array_cache_plan(XaotBundle *bundle, const XiFunc *func,
                                                             const XiValue *value,
                                                             const XiValue *storage_value,
                                                             uint32_t flags,
                                                             const XaotContainerElemPlan *elem);
XR_FUNC const XaotArrayCachePlan *xaot_bundle_find_array_cache_plan(const XaotBundle *bundle,
                                                                    const XiValue *value);
XR_FUNC XaotArrayClassFieldAllocPlan *xaot_bundle_add_array_class_field_alloc_plan(
    XaotBundle *bundle, const XiFunc *func, const XiValue *origin, const XiValue *store,
    const XiClassData *class_data, uint16_t field_idx, const XaotContainerElemPlan *elem);
XR_FUNC const XaotArrayClassFieldAllocPlan *
xaot_bundle_find_array_class_field_alloc_plan(const XaotBundle *bundle, const XiValue *origin);
XR_FUNC const XaotArrayClassFieldAllocPlan *
xaot_bundle_find_array_class_field_alloc_plan_for_store(const XaotBundle *bundle,
                                                        const XiValue *store);
XR_FUNC const XaotArrayClassFieldAllocPlan *xaot_bundle_find_array_class_field_alloc_plan_for_field(
    const XaotBundle *bundle, const XiFunc *func, const XiClassData *class_data,
    uint16_t field_idx);
XR_FUNC XaotFuncAttrPlan *xaot_bundle_add_func_attr_plan(XaotBundle *bundle, const XiFunc *func,
                                                         uint32_t flags, const XgBodySummary *body);
XR_FUNC const XaotFuncAttrPlan *xaot_bundle_find_func_attr_plan(const XaotBundle *bundle,
                                                                const XiFunc *func);
XR_FUNC XaotBoundsPlan *xaot_bundle_add_bounds_plan(XaotBundle *bundle, const XiFunc *func,
                                                    const XiValue *access,
                                                    const XgBodySummary *body, uint32_t evidence,
                                                    uint8_t unproven_reason);
XR_FUNC const XaotBoundsPlan *xaot_bundle_find_bounds_plan(const XaotBundle *bundle,
                                                           const XiValue *access);
XR_FUNC XaotSpanAccessPlan *
xaot_bundle_add_span_access_plan(XaotBundle *bundle, const XiFunc *func, const XiValue *value,
                                 const XgBodySummary *body, uint8_t kind, uint32_t evidence,
                                 uint32_t eliminated_checks, uint8_t unproven_reason);
XR_FUNC const XaotSpanAccessPlan *xaot_bundle_find_span_access_plan(const XaotBundle *bundle,
                                                                    const XiValue *value);
XR_FUNC XaotAliasPlan *xaot_bundle_add_alias_plan(XaotBundle *bundle, const XiFunc *func,
                                                  const XiValue *value, const XgBodySummary *body,
                                                  uint8_t kind, uint32_t evidence);
XR_FUNC const XaotAliasPlan *xaot_bundle_find_alias_plan(const XaotBundle *bundle,
                                                         const XiValue *value);
XR_FUNC XaotAllocationPlan *xaot_bundle_add_allocation_plan(XaotBundle *bundle, const XiFunc *func,
                                                            const XiValue *value,
                                                            const XgBodySummary *body,
                                                            uint8_t action, uint16_t original_op,
                                                            uint8_t escape, uint32_t evidence);
XR_FUNC const XaotAllocationPlan *xaot_bundle_find_allocation_plan(const XaotBundle *bundle,
                                                                   const XiValue *value);
XR_FUNC XaotClosurePlan *
xaot_bundle_add_closure_plan(XaotBundle *bundle, const XiFunc *func, const XiValue *value,
                             const XiFunc *target_func, uint16_t capture_count,
                             uint8_t representation, uint32_t evidence, uint8_t unproven_reason);
XR_FUNC const XaotClosurePlan *xaot_bundle_find_closure_plan(const XaotBundle *bundle,
                                                             const XiValue *value);
XR_FUNC XaotTransferPlan *xaot_bundle_add_transfer_plan(
    XaotBundle *bundle, const XiFunc *func, const XiValue *site, uint16_t transfer_index,
    const XiValue *value, const XrType *value_type, const XaotTypeKey *value_type_key,
    uint8_t site_kind, uint8_t mode, uint8_t action, uint32_t evidence, uint8_t unproven_reason);
XR_FUNC const XaotTransferPlan *xaot_bundle_find_transfer_plan(const XaotBundle *bundle,
                                                               const XiValue *site,
                                                               uint16_t transfer_index);
XR_FUNC XaotBoundaryStep *xaot_bundle_add_boundary_step(XaotBundle *bundle,
                                                        XaotBoundaryStepKind kind,
                                                        const XiFunc *func, const XiValue *value,
                                                        const XiValue *input,
                                                        XaotBoundaryReason reason);
XR_FUNC const XaotBoundaryStep *
xaot_bundle_find_boundary_step(const XaotBundle *bundle, XaotBoundaryStepKind kind,
                               const XiFunc *func, const XiValue *value, const XiValue *input);
XR_FUNC const XaotBoundaryStep *
xaot_bundle_find_boundary_step_ex(const XaotBundle *bundle, XaotBoundaryStepKind kind,
                                  const XiFunc *func, const XiValue *value, const XiValue *input,
                                  const XiFunc *target_func, uint16_t arg_index);
XR_FUNC char *xaot_bundle_dump_plan(const XaotBundle *bundle);

#endif  // XAOT_BUNDLE_H
