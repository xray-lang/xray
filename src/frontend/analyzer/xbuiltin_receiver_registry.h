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

typedef enum {
    XA_BUILTIN_RECEIVER_EXACT_INTEGER,
    XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER,
    XA_BUILTIN_RECEIVER_U8_ARRAY,
    XA_BUILTIN_RECEIVER_ARRAY,
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

typedef struct XaBuiltinReceiverMethodSpec {
    XaBuiltinReceiverMethodId method_id;
    const char *id;
    const char *source_name;
    XaBuiltinReceiverKind receiver;
    XaBuiltinMethodTypeKind result;
    XaBuiltinMethodTypeKind params[3];
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
            return "Array<byte> byte bulk methods";
        case XA_BUILTIN_DOC_GROUP_U8_SLICE:
            return "Slice<byte> byte range methods";
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
