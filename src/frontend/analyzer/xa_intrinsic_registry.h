/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu
 * Licensed under the MIT License
 *
 * xa_intrinsic_registry.h - Canonical analyzer semantic intrinsic identities
 */

#ifndef XA_INTRINSIC_REGISTRY_H
#define XA_INTRINSIC_REGISTRY_H

#include "../../base/xdefs.h"
#include "../../shared/xr_native_type_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct XrType;

typedef enum XaIntrinsicId {
    XA_INTRINSIC_NONE = 0,
#define XA_INTRINSIC(id, numeric_id, key, family, lowering, effect, allocation, safety, min_arity, \
                     max_arity, flags, input_native, input_lanes, result_native, result_lanes)     \
    XA_INTRINSIC_##id = numeric_id,
#include "xa_intrinsic_registry.def"
#undef XA_INTRINSIC
} XaIntrinsicId;

typedef enum XaSemanticTypeId {
    XA_SEMANTIC_TYPE_NONE = 0,
    XA_SEMANTIC_TYPE_PARALLEL_PLAN = 4000,
    XA_SEMANTIC_TYPE_PARALLEL_OPTIONS = 4001,
} XaSemanticTypeId;

XR_FUNC XaSemanticTypeId xa_semantic_type_by_key(const char *key);
XR_FUNC const char *xa_semantic_type_source_name(XaSemanticTypeId id);

typedef enum XaIntrinsicFamily {
    XA_INTRINSIC_FAMILY_BITS = 1,
    XA_INTRINSIC_FAMILY_MEMORY,
    XA_INTRINSIC_FAMILY_SIMD,
    XA_INTRINSIC_FAMILY_ATOMIC,
    XA_INTRINSIC_FAMILY_PARALLEL,
    XA_INTRINSIC_FAMILY_TARGET,
    XA_INTRINSIC_FAMILY_CORE,
} XaIntrinsicFamily;

typedef enum XaIntrinsicLowering {
    XA_INTRINSIC_LOWERING_NONE = 0,
    XA_INTRINSIC_LOWERING_VEC_LOAD,
    XA_INTRINSIC_LOWERING_VEC_STORE,
    XA_INTRINSIC_LOWERING_VEC_SPLAT,
    XA_INTRINSIC_LOWERING_VEC_EXTRACT,
    XA_INTRINSIC_LOWERING_VEC_REPLACE,
    XA_INTRINSIC_LOWERING_VEC_ADD,
    XA_INTRINSIC_LOWERING_VEC_SUB,
    XA_INTRINSIC_LOWERING_VEC_MUL,
    XA_INTRINSIC_LOWERING_VEC_BIT_AND,
    XA_INTRINSIC_LOWERING_VEC_BIT_OR,
    XA_INTRINSIC_LOWERING_VEC_BIT_XOR,
    XA_INTRINSIC_LOWERING_VEC_BIT_NOT,
    XA_INTRINSIC_LOWERING_VEC_SHL,
    XA_INTRINSIC_LOWERING_VEC_SHR,
    XA_INTRINSIC_LOWERING_VEC_REINTERPRET,
    XA_INTRINSIC_LOWERING_VEC_SHUFFLE,
    XA_INTRINSIC_LOWERING_VEC_WIDEN_MUL,
    XA_INTRINSIC_LOWERING_VEC_REDUCE_ADD,
    XA_INTRINSIC_LOWERING_TARGET_SIMD_BYTES,
    XA_INTRINSIC_LOWERING_TARGET_SIMD_ACCELERATED,
    XA_INTRINSIC_LOWERING_TARGET_SIMD_RUNTIME_SELECTED,
    XA_INTRINSIC_LOWERING_BIT_ROTL,
    XA_INTRINSIC_LOWERING_BIT_ROTR,
    XA_INTRINSIC_LOWERING_BIT_MUL_HIGH,
    XA_INTRINSIC_LOWERING_BIT_BSWAP,
    XA_INTRINSIC_LOWERING_BIT_POPCOUNT,
    XA_INTRINSIC_LOWERING_BIT_CLZ,
    XA_INTRINSIC_LOWERING_BIT_CTZ,
    XA_INTRINSIC_LOWERING_BYTE_SLICE_TYPED_LOAD,
    XA_INTRINSIC_LOWERING_BYTE_SLICE_TYPED_STORE,
    XA_INTRINSIC_LOWERING_BYTE_SLICE_FILL,
    XA_INTRINSIC_LOWERING_BYTE_SLICE_COPY,
    XA_INTRINSIC_LOWERING_BYTE_SLICE_COMPARE,
    XA_INTRINSIC_LOWERING_BYTE_SLICE_COMMON_PREFIX,
    XA_INTRINSIC_LOWERING_BYTE_SLICE_REPEAT,
    XA_INTRINSIC_LOWERING_SLICE_REINTERPRET,
    XA_INTRINSIC_LOWERING_SLICE_DATA_PTR,
    XA_INTRINSIC_LOWERING_SLICE_WINDOW,
    XA_INTRINSIC_LOWERING_SLICE_AS_BYTES,
    XA_INTRINSIC_LOWERING_SLICE_FILL,
    XA_INTRINSIC_LOWERING_SLICE_COPY,
    XA_INTRINSIC_LOWERING_SLICE_COMPARE,
    XA_INTRINSIC_LOWERING_SLICE_GET,
    XA_INTRINSIC_LOWERING_ATOMIC_LOAD,
    XA_INTRINSIC_LOWERING_ATOMIC_STORE,
    XA_INTRINSIC_LOWERING_ATOMIC_RMW,
    XA_INTRINSIC_LOWERING_ATOMIC_TO_STRING,
    XA_INTRINSIC_LOWERING_PAR_FOR,
    XA_INTRINSIC_LOWERING_PAR_MAP,
    XA_INTRINSIC_LOWERING_PAR_MAP_INTO,
    XA_INTRINSIC_LOWERING_PAR_REDUCE,
} XaIntrinsicLowering;

typedef enum XaIntrinsicEffectContract {
    XA_INTRINSIC_EFFECT_PURE = 0,
    XA_INTRINSIC_EFFECT_READ_ONLY,
    XA_INTRINSIC_EFFECT_WRITE_ONLY,
    XA_INTRINSIC_EFFECT_READ_WRITE,
    XA_INTRINSIC_EFFECT_MAY_THROW,
    XA_INTRINSIC_EFFECT_READ_MAY_THROW,
    XA_INTRINSIC_EFFECT_WRITE_MAY_THROW,
} XaIntrinsicEffectContract;

typedef enum XaIntrinsicAllocationContract {
    XA_INTRINSIC_ALLOCATION_NO_ALLOC = 0,
    XA_INTRINSIC_ALLOCATION_MAY_ALLOC,
} XaIntrinsicAllocationContract;

typedef enum XaIntrinsicSafetyContract {
    XA_INTRINSIC_SAFETY_SAFE = 0,
    XA_INTRINSIC_SAFETY_BOUNDS_CHECKED,
    XA_INTRINSIC_SAFETY_CONST_LANES,
} XaIntrinsicSafetyContract;

enum {
    XA_INTRINSIC_FLAG_NONE = 0,
    XA_INTRINSIC_FLAG_STATIC_RECEIVER = 1u << 0,
    XA_INTRINSIC_FLAG_ODD_LANES = 1u << 1,
    XA_INTRINSIC_FLAG_EXPLICIT_SHUFFLE = 1u << 2,
    XA_INTRINSIC_FLAG_SWAP_ADJACENT = 1u << 3,
    XA_INTRINSIC_FLAG_SWAP_LANES = 1u << 4,
    XA_INTRINSIC_FLAG_PLAN_RECEIVER = 1u << 5,
    XA_INTRINSIC_FLAG_UNZIP = 1u << 6,
    XA_INTRINSIC_FLAG_CONTIGUOUS_HALF = 1u << 7,
    XA_INTRINSIC_FLAG_UNZIP_ODD = XA_INTRINSIC_FLAG_UNZIP | XA_INTRINSIC_FLAG_ODD_LANES,
    XA_INTRINSIC_FLAG_CONTIGUOUS_HALF_ODD =
        XA_INTRINSIC_FLAG_CONTIGUOUS_HALF | XA_INTRINSIC_FLAG_ODD_LANES,
};

typedef struct XaIntrinsicShapeRule {
    uint8_t input_native_type;
    uint8_t input_lanes;
    uint8_t result_native_type;
    uint8_t result_lanes;
} XaIntrinsicShapeRule;

typedef struct XaIntrinsicDesc {
    XaIntrinsicId id;
    const char *key;
    XaIntrinsicFamily family;
    XaIntrinsicLowering lowering;
    XaIntrinsicShapeRule shape_rule;
    XaIntrinsicEffectContract effect;
    XaIntrinsicAllocationContract allocation;
    XaIntrinsicSafetyContract safety;
    uint16_t min_arity;
    uint16_t max_arity;
    uint32_t flags;
} XaIntrinsicDesc;

XR_FUNC const XaIntrinsicDesc *xa_intrinsic_by_id(XaIntrinsicId id);
XR_FUNC const XaIntrinsicDesc *xa_intrinsic_by_key(const char *key);
XR_FUNC const char *xa_intrinsic_source_member(const XaIntrinsicDesc *desc);
XR_FUNC XaIntrinsicId xa_intrinsic_compiler_receiver_method(const struct XrType *receiver,
                                                            const char *member_name);
XR_FUNC size_t xa_intrinsic_count(void);
XR_FUNC const XaIntrinsicDesc *xa_intrinsic_at(size_t index);
XR_FUNC bool xa_intrinsic_registry_validate(char *error, size_t error_size);

#endif  // XA_INTRINSIC_REGISTRY_H
