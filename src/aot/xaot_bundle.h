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

typedef struct XaotEnumPlan {
    const XiEnumData *enum_data;
    const XiEnumMemberData *members;
    const XrType *concrete_type;
    XrType **type_args;
    uint32_t module_index;
    uint32_t member_count;
    uint32_t layout_id;
    uint16_t max_payload;
    uint8_t type_arg_count;
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

typedef enum XaotClosureRepresentation {
    XAOT_CLOSURE_RUNTIME = 1,
    XAOT_CLOSURE_STACK = 2,
} XaotClosureRepresentation;

enum {
    XAOT_CLOSURE_EV_XI_VALUE = 1u << 0,
    XAOT_CLOSURE_EV_TARGET_FUNC = 1u << 1,
    XAOT_CLOSURE_EV_CAPTURE_ARITY = 1u << 2,
    XAOT_CLOSURE_EV_NOESCAPE_STACK = 1u << 3,
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
XR_FUNC const XiFunc *xaot_bundle_find_dispatch_target_func(const XaotBundle *bundle,
                                                            const XaotDispatchTargetCase *target,
                                                            const char **out_module_prefix);
XR_FUNC const XaotInterfaceUsePlan *
xaot_bundle_find_interface_use_plan(const XaotBundle *bundle, XgInterfaceId interface_id,
                                    XgClassId implementor_class_id, XgCallsiteId use_site_id);
XR_FUNC const XaotInterfaceAbiPlan *xaot_bundle_find_interface_abi_plan(const XaotBundle *bundle,
                                                                        XgInterfaceId interface_id);
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
