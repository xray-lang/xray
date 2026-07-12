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

typedef enum {
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

#endif  // XBUILTIN_RECEIVER_REGISTRY_H
