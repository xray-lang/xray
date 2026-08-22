/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbuiltin_receiver_registry.h - Shared receiver-specialized builtin registry.
 */

#ifndef XBUILTIN_RECEIVER_REGISTRY_H
#define XBUILTIN_RECEIVER_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../runtime/value/xtype.h"

typedef enum {
    XA_BUILTIN_RECEIVER_EXACT_INTEGER,
    XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER,
    XA_BUILTIN_RECEIVER_U8_ARRAY,
    XA_BUILTIN_RECEIVER_ARRAY,
    XA_BUILTIN_RECEIVER_MAP,
    XA_BUILTIN_RECEIVER_U8_SLICE,
    XA_BUILTIN_RECEIVER_POD_SLICE,
} XaBuiltinReceiverKind;

typedef enum {
    XA_BUILTIN_TYPE_NONE,
    XA_BUILTIN_TYPE_BOOL,
    XA_BUILTIN_TYPE_INT,
    XA_BUILTIN_TYPE_STRING,
    XA_BUILTIN_TYPE_U8,
    XA_BUILTIN_TYPE_U8_ARRAY,
    XA_BUILTIN_TYPE_U8_SLICE,
    XA_BUILTIN_TYPE_UNIT,
    XA_BUILTIN_TYPE_ENDIAN,
    XA_BUILTIN_TYPE_PARAM_0,
    XA_BUILTIN_TYPE_ARRAY_OF_PARAM_0,
    XA_BUILTIN_TYPE_RECEIVER_ELEM_TO_BOOL_FN,
    XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_BOOL_FN,
    XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_UNIT_FN,
    XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN,
    XA_BUILTIN_TYPE_PARAM_0_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN,
    XA_BUILTIN_TYPE_SLICE_OF_PARAM_0,
    XA_BUILTIN_TYPE_RECEIVER,
    XA_BUILTIN_TYPE_RECEIVER_ELEM,
    XA_BUILTIN_TYPE_RECEIVER_ELEM_NULLABLE,
    XA_BUILTIN_TYPE_RECEIVER_ELEM_COMPARE_FN,
    XA_BUILTIN_TYPE_ITERATOR_OF_RECEIVER_ELEM,
    XA_BUILTIN_TYPE_ITERATOR_OF_INDEX_RECEIVER_ELEM_TUPLE,
    XA_BUILTIN_TYPE_ITERATOR_OF_MAP_ENTRY_TUPLE,
    XA_BUILTIN_TYPE_ARRAY_OF_INDEX_RECEIVER_ELEM_TUPLE,
    XA_BUILTIN_TYPE_SLICE_OF_RECEIVER_ELEM,
    XA_BUILTIN_TYPE_PTR_OF_RECEIVER_ELEM,
    XA_BUILTIN_TYPE_MUT_PTR_OF_RECEIVER_ELEM,
} XaBuiltinMethodTypeKind;

typedef enum {
    XA_BUILTIN_TYPE_PARAMS_NONE,
    XA_BUILTIN_TYPE_PARAMS_T,
    XA_BUILTIN_TYPE_PARAMS_U,
} XaBuiltinMethodTypeParams;

typedef enum {
    XA_BUILTIN_EFFECT_READS_RECEIVER,
    XA_BUILTIN_EFFECT_MUTATES_RECEIVER,
} XaBuiltinMethodEffect;

typedef enum {
    XA_BUILTIN_ALLOCATION_NO_HEAP,
    XA_BUILTIN_ALLOCATION_MAY_HEAP,
} XaBuiltinMethodAllocation;

typedef enum {
    XA_BUILTIN_UNSAFE_NONE,
    XA_BUILTIN_UNSAFE_REQUIRED,
} XaBuiltinMethodUnsafeRequirement;

typedef enum XaBuiltinMethodMemoryEffect {
    XA_BUILTIN_MEMORY_STABLE_READ = 0,
    XA_BUILTIN_MEMORY_WRITE = 1u << 0,
    XA_BUILTIN_MEMORY_MAY_RELOCATE = 1u << 1,
    XA_BUILTIN_MEMORY_MAY_SHORTEN = 1u << 2,
    XA_BUILTIN_MEMORY_INVALIDATES_VIEWS = 1u << 3,
} XaBuiltinMethodMemoryEffect;

typedef uint32_t XaBuiltinMethodMemoryEffectSet;

typedef struct XaBuiltinPointerMethodSpec {
    const char *method_name;
    XaBuiltinMethodMemoryEffectSet effects;
    XaBuiltinMethodAllocation allocation;
} XaBuiltinPointerMethodSpec;

static const XaBuiltinPointerMethodSpec xa_builtin_pointer_method_specs[] = {
    {"deref", XA_BUILTIN_MEMORY_STABLE_READ, XA_BUILTIN_ALLOCATION_NO_HEAP},
    {"offset", XA_BUILTIN_MEMORY_STABLE_READ, XA_BUILTIN_ALLOCATION_NO_HEAP},
    {"isNull", XA_BUILTIN_MEMORY_STABLE_READ, XA_BUILTIN_ALLOCATION_NO_HEAP},
    {"copyFromNonOverlapping", XA_BUILTIN_MEMORY_WRITE, XA_BUILTIN_ALLOCATION_NO_HEAP},
};

static inline bool xa_builtin_pointer_memory_effect(const char *method_name,
                                                    XaBuiltinMethodMemoryEffectSet *out_effects) {
    if (!method_name)
        return false;
    for (size_t i = 0;
         i < sizeof(xa_builtin_pointer_method_specs) / sizeof(xa_builtin_pointer_method_specs[0]);
         i++) {
        const XaBuiltinPointerMethodSpec *spec = &xa_builtin_pointer_method_specs[i];
        if (strcmp(spec->method_name, method_name) == 0) {
            if (out_effects)
                *out_effects = spec->effects;
            return true;
        }
    }
    return false;
}

static inline bool xa_builtin_pointer_allocation(const char *method_name,
                                                 XaBuiltinMethodAllocation *out_allocation) {
    if (!method_name)
        return false;
    for (size_t i = 0;
         i < sizeof(xa_builtin_pointer_method_specs) / sizeof(xa_builtin_pointer_method_specs[0]);
         i++) {
        const XaBuiltinPointerMethodSpec *spec = &xa_builtin_pointer_method_specs[i];
        if (strcmp(spec->method_name, method_name) == 0) {
            if (out_allocation)
                *out_allocation = spec->allocation;
            return true;
        }
    }
    return false;
}

/* Native stdlib classes that are not language receiver intrinsics still need
 * sealed root-relative invalidation contracts.  Keep those contracts in one
 * registry instead of teaching the analyzer method-name exceptions. */
typedef struct XaBuiltinNamedReceiverMemorySpec {
    const char *type_name;
    const char *method_name;
    XaBuiltinMethodMemoryEffectSet effects;
} XaBuiltinNamedReceiverMemorySpec;

static const XaBuiltinNamedReceiverMemorySpec xa_builtin_named_receiver_memory_specs[] = {
    {"Buffer", "resize",
     XA_BUILTIN_MEMORY_WRITE | XA_BUILTIN_MEMORY_MAY_RELOCATE | XA_BUILTIN_MEMORY_MAY_SHORTEN |
         XA_BUILTIN_MEMORY_INVALIDATES_VIEWS},
};

static inline bool
xa_builtin_named_receiver_memory_effect(const char *type_name, const char *method_name,
                                        XaBuiltinMethodMemoryEffectSet *out_effects) {
    if (!type_name || !method_name)
        return false;
    for (size_t i = 0; i < sizeof(xa_builtin_named_receiver_memory_specs) /
                               sizeof(xa_builtin_named_receiver_memory_specs[0]);
         i++) {
        const XaBuiltinNamedReceiverMemorySpec *spec = &xa_builtin_named_receiver_memory_specs[i];
        if (strcmp(spec->type_name, type_name) == 0 &&
            strcmp(spec->method_name, method_name) == 0) {
            if (out_effects)
                *out_effects = spec->effects;
            return true;
        }
    }
    return false;
}

typedef enum {
    XA_BUILTIN_PROFILE_ALL,
    XA_BUILTIN_PROFILE_HEAP_CAPABLE,
} XaBuiltinMethodProfileAvailability;

typedef enum {
    XA_BUILTIN_DOC_GROUP_GENERAL,
    XA_BUILTIN_DOC_GROUP_EXACT_INTEGER,
    XA_BUILTIN_DOC_GROUP_ARRAY,
    XA_BUILTIN_DOC_GROUP_U8_ARRAY,
    XA_BUILTIN_DOC_GROUP_U8_SLICE,
    XA_BUILTIN_DOC_GROUP_POD_SLICE,
} XaBuiltinMethodDocumentationGroup;

typedef enum {
#define XB_RECEIVER_METHOD(id, source_name, receiver, result, p0, p1, p2, param_count, min_params, \
                           type_params, effect, allocation, unsafe_requirement, lowering)          \
    XA_BUILTIN_RECEIVER_METHOD_##id,
#define XB_RECEIVER_VARIADIC_METHOD(id, source_name, receiver, result, p0, p1, p2, param_count,    \
                                    min_params, type_params, effect, allocation,                   \
                                    unsafe_requirement, lowering)                                  \
    XA_BUILTIN_RECEIVER_METHOD_##id,
#include "xbuiltin_receiver_method.def"
#undef XB_RECEIVER_VARIADIC_METHOD
#undef XB_RECEIVER_METHOD
    XA_BUILTIN_RECEIVER_METHOD_COUNT,
} XaBuiltinReceiverMethodId;

/* Widest fixed parameter list any entry in the .def table declares. */
#define XA_BUILTIN_RECEIVER_METHOD_MAX_PARAMS 3

typedef struct XaBuiltinReceiverMethodSpec {
    XaBuiltinReceiverMethodId method_id;
    const char *id;
    const char *source_name;
    XaBuiltinReceiverKind receiver;
    XaBuiltinMethodTypeKind result;
    XaBuiltinMethodTypeKind params[XA_BUILTIN_RECEIVER_METHOD_MAX_PARAMS];
    int param_count;
    int min_params;
    bool is_variadic;
    XaBuiltinMethodTypeParams type_params;
    XaBuiltinMethodEffect effect;
    XaBuiltinMethodAllocation allocation;
    XaBuiltinMethodUnsafeRequirement unsafe_requirement;
    const char *lowering;
} XaBuiltinReceiverMethodSpec;

static const XaBuiltinReceiverMethodSpec xa_builtin_receiver_methods[] = {
#define XB_RECEIVER_METHOD(id, source_name, receiver, result, p0, p1, p2, param_count, min_params, \
                           type_params, effect, allocation, unsafe_requirement, lowering)          \
    {XA_BUILTIN_RECEIVER_METHOD_##id,                                                              \
     #id,                                                                                          \
     source_name,                                                                                  \
     receiver,                                                                                     \
     result,                                                                                       \
     {p0, p1, p2},                                                                                 \
     param_count,                                                                                  \
     min_params,                                                                                   \
     false,                                                                                        \
     type_params,                                                                                  \
     effect,                                                                                       \
     allocation,                                                                                   \
     unsafe_requirement,                                                                           \
     lowering},
#define XB_RECEIVER_VARIADIC_METHOD(id, source_name, receiver, result, p0, p1, p2, param_count,    \
                                    min_params, type_params, effect, allocation,                   \
                                    unsafe_requirement, lowering)                                  \
    {XA_BUILTIN_RECEIVER_METHOD_##id,                                                              \
     #id,                                                                                          \
     source_name,                                                                                  \
     receiver,                                                                                     \
     result,                                                                                       \
     {p0, p1, p2},                                                                                 \
     param_count,                                                                                  \
     min_params,                                                                                   \
     true,                                                                                         \
     type_params,                                                                                  \
     effect,                                                                                       \
     allocation,                                                                                   \
     unsafe_requirement,                                                                           \
     lowering},
#include "xbuiltin_receiver_method.def"
#undef XB_RECEIVER_VARIADIC_METHOD
#undef XB_RECEIVER_METHOD
};

static inline size_t xa_builtin_receiver_method_count(void) {
    return sizeof(xa_builtin_receiver_methods) / sizeof(xa_builtin_receiver_methods[0]);
}

static inline const XaBuiltinReceiverMethodSpec *
xa_builtin_receiver_method_by_id(XaBuiltinReceiverMethodId method_id) {
    if (method_id < 0 || method_id >= XA_BUILTIN_RECEIVER_METHOD_COUNT)
        return NULL;
    const XaBuiltinReceiverMethodSpec *spec = &xa_builtin_receiver_methods[method_id];
    return spec->method_id == method_id ? spec : NULL;
}

/* Root-relative memory effects are keyed by the sealed receiver-method ID,
 * never by a parallel method-name whitelist. */
static inline XaBuiltinMethodMemoryEffectSet
xa_builtin_receiver_method_memory_effect(const XaBuiltinReceiverMethodSpec *spec) {
    if (!spec)
        return XA_BUILTIN_MEMORY_STABLE_READ;
    XaBuiltinMethodMemoryEffectSet result = spec->effect == XA_BUILTIN_EFFECT_MUTATES_RECEIVER
                                                ? XA_BUILTIN_MEMORY_WRITE
                                                : XA_BUILTIN_MEMORY_STABLE_READ;
    switch (spec->method_id) {
        case XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM:
        case XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM:
        case XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH:
        case XA_BUILTIN_RECEIVER_METHOD_ARRAY_UNSHIFT:
        case XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESERVE:
            result |= XA_BUILTIN_MEMORY_MAY_RELOCATE;
            break;
        case XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESIZE:
            result |= XA_BUILTIN_MEMORY_MAY_RELOCATE | XA_BUILTIN_MEMORY_MAY_SHORTEN;
            break;
        case XA_BUILTIN_RECEIVER_METHOD_ARRAY_POP:
        case XA_BUILTIN_RECEIVER_METHOD_ARRAY_SHIFT:
        case XA_BUILTIN_RECEIVER_METHOD_ARRAY_CLEAR:
            result |= XA_BUILTIN_MEMORY_MAY_SHORTEN;
            break;
        default:
            break;
    }
    return result;
}

/* Element types a Slice<T> may span POD-wise. */
static inline bool xa_builtin_type_is_pod_span_elem(XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

/* Does `receiver` satisfy a spec's declared receiver kind? Single definition
 * shared by the call/expr visitors and the error-set analyzer: a divergent copy
 * would let one pass believe a call is an intrinsic while another does not. */
static inline bool xa_builtin_receiver_matches_type(XrType *receiver, XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return receiver && receiver->kind == XR_KIND_INT && !receiver->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return xr_type_is_exact_unsigned_integer(receiver);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver && XR_TYPE_IS_ARRAY(receiver);
        case XA_BUILTIN_RECEIVER_MAP:
            return receiver && receiver->kind == XR_KIND_MAP && receiver->map.key_type &&
                   receiver->map.value_type;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver && XR_TYPE_IS_SLICE(receiver) && receiver->container.element_type &&
                   xa_builtin_type_is_pod_span_elem(receiver->container.element_type);
    }
    return false;
}

/* Instantiate the exact result declared by the generated Map entry-iterator
 * row.  Both semantic analysis and IR lowering consume this constructor, so
 * an imported/stale method signature cannot replace (K, V) with unknown at
 * the typed-IR boundary. Type constructors allocate from the current analyzer
 * pool; executable runtime identity is deliberately absent from this compiler
 * authority. */
static inline XrType *
xa_builtin_map_entries_iterator_result_type(XrType *receiver,
                                            XaBuiltinReceiverMethodId method_id) {
    const XaBuiltinReceiverMethodSpec *spec = xa_builtin_receiver_method_by_id(method_id);
    if (!spec || spec->method_id != XA_BUILTIN_RECEIVER_METHOD_MAP_ENTRIES_ITERATOR ||
        spec->receiver != XA_BUILTIN_RECEIVER_MAP ||
        spec->result != XA_BUILTIN_TYPE_ITERATOR_OF_MAP_ENTRY_TUPLE ||
        !xa_builtin_receiver_matches_type(receiver, spec->receiver))
        return NULL;
    XrType *entry_elements[2] = {receiver->map.key_type, receiver->map.value_type};
    XrType *entry = xr_type_new_tuple(NULL, entry_elements, 2);
    if (!entry)
        return NULL;
    XrType *iterator_args[1] = {entry};
    return xr_type_new_generic_instance(NULL, "Iterator", NULL, iterator_args, 1);
}

/* Does this parameter slot take a callback? Higher-order intrinsics propagate
 * whatever their callback throws, so unlike the rest of the table they are not
 * unconditionally no-throw. */
static inline bool xa_builtin_method_type_is_callback(XaBuiltinMethodTypeKind kind) {
    switch (kind) {
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_TO_BOOL_FN:
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_BOOL_FN:
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_UNIT_FN:
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN:
        case XA_BUILTIN_TYPE_PARAM_0_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN:
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_COMPARE_FN:
            return true;
        default:
            return false;
    }
}

/* Whether an intrinsic can put an error on the value-return channel.
 *
 * Every method in this table is an Array / Slice / exact-integer primitive
 * implemented in C. Those raise panics (xrt_throw_error → the panic channel,
 * E04xx) and never write pending_error, so on the value-return error channel
 * they are no-throw. The exception is the higher-order ones, which re-raise
 * whatever their callback throws — proving those needs the callback's effect,
 * which this table does not carry, so they stay unproven (fail-closed). */
static inline bool xa_builtin_receiver_method_is_nothrow(const XaBuiltinReceiverMethodSpec *spec) {
    if (!spec)
        return false;
    for (int i = 0; i < XA_BUILTIN_RECEIVER_METHOD_MAX_PARAMS; i++) {
        if (xa_builtin_method_type_is_callback(spec->params[i]))
            return false;
    }
    return true;
}

static inline XaBuiltinMethodProfileAvailability
xa_builtin_receiver_method_profile_availability(const XaBuiltinReceiverMethodSpec *spec) {
    if (!spec)
        return XA_BUILTIN_PROFILE_ALL;
    return spec->allocation == XA_BUILTIN_ALLOCATION_MAY_HEAP ? XA_BUILTIN_PROFILE_HEAP_CAPABLE
                                                              : XA_BUILTIN_PROFILE_ALL;
}

static inline XaBuiltinMethodDocumentationGroup
xa_builtin_receiver_method_documentation_group(const XaBuiltinReceiverMethodSpec *spec) {
    if (!spec)
        return XA_BUILTIN_DOC_GROUP_GENERAL;
    switch (spec->receiver) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return XA_BUILTIN_DOC_GROUP_EXACT_INTEGER;
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return XA_BUILTIN_DOC_GROUP_U8_ARRAY;
        case XA_BUILTIN_RECEIVER_ARRAY:
            return XA_BUILTIN_DOC_GROUP_ARRAY;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return XA_BUILTIN_DOC_GROUP_U8_SLICE;
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return XA_BUILTIN_DOC_GROUP_POD_SLICE;
    }
    return XA_BUILTIN_DOC_GROUP_GENERAL;
}

static inline const char *
xa_builtin_receiver_profile_availability_label(XaBuiltinMethodProfileAvailability profile) {
    switch (profile) {
        case XA_BUILTIN_PROFILE_ALL:
            return "all build profiles";
        case XA_BUILTIN_PROFILE_HEAP_CAPABLE:
            return "heap-capable profiles";
    }
    return "unknown profile";
}

static inline const char *
xa_builtin_receiver_documentation_group_label(XaBuiltinMethodDocumentationGroup group) {
    switch (group) {
        case XA_BUILTIN_DOC_GROUP_EXACT_INTEGER:
            return "exact-width integer bit methods";
        case XA_BUILTIN_DOC_GROUP_ARRAY:
            return "Array<T> collection methods";
        case XA_BUILTIN_DOC_GROUP_U8_ARRAY:
            return "Array<u8> byte bulk methods";
        case XA_BUILTIN_DOC_GROUP_U8_SLICE:
            return "Slice<u8> byte range methods";
        case XA_BUILTIN_DOC_GROUP_POD_SLICE:
            return "Slice<T> POD range methods";
        case XA_BUILTIN_DOC_GROUP_GENERAL:
            return "receiver-specialized builtin methods";
    }
    return "receiver-specialized builtin methods";
}

static inline const char *xa_builtin_receiver_effect_label(XaBuiltinMethodEffect effect) {
    switch (effect) {
        case XA_BUILTIN_EFFECT_READS_RECEIVER:
            return "reads receiver";
        case XA_BUILTIN_EFFECT_MUTATES_RECEIVER:
            return "mutates receiver";
    }
    return "unknown effect";
}

static inline const char *
xa_builtin_receiver_allocation_label(XaBuiltinMethodAllocation allocation) {
    switch (allocation) {
        case XA_BUILTIN_ALLOCATION_NO_HEAP:
            return "no heap allocation";
        case XA_BUILTIN_ALLOCATION_MAY_HEAP:
            return "may allocate";
    }
    return "unknown allocation";
}

static inline const char *
xa_builtin_receiver_unsafe_requirement_label(XaBuiltinMethodUnsafeRequirement requirement) {
    switch (requirement) {
        case XA_BUILTIN_UNSAFE_NONE:
            return "safe call";
        case XA_BUILTIN_UNSAFE_REQUIRED:
            return "requires unsafe";
    }
    return "unknown unsafe requirement";
}

#endif  // XBUILTIN_RECEIVER_REGISTRY_H
