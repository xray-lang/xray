/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_codegen_opaque_core.h - Runtime-neutral optimizer barrier semantics.
 *
 * A codegen-opaque value preserves its exact representation while preventing
 * the selected native provider from deriving a compile-time value through the
 * boundary. VM execution only preserves the value because it has no native
 * optimizer; C generation selects a provider adapter for the frozen kind.
 *
 * A compiler fence preserves program state and has no runtime memory effect.
 * Its only observable obligation is to stop a native provider from reordering
 * memory operations across the source boundary. The VM therefore projects the
 * same plan to a void result while C generation emits the provider fence.
 */

#ifndef XR_CODEGEN_OPAQUE_CORE_H
#define XR_CODEGEN_OPAQUE_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum XrCodegenOpaqueKind {
    XR_CODEGEN_OPAQUE_VALUE = 0,
    XR_CODEGEN_OPAQUE_I64,
    XR_CODEGEN_OPAQUE_U64,
    XR_CODEGEN_OPAQUE_POINTER,
    XR_CODEGEN_OPAQUE_CONST_POINTER,
    XR_CODEGEN_OPAQUE_KIND_COUNT
} XrCodegenOpaqueKind;

typedef struct XrCodegenOpaquePlan {
    XrCodegenOpaqueKind kind;
    bool preserves_value_bits;
    bool blocks_native_constant_propagation;
} XrCodegenOpaquePlan;

static inline XrCodegenOpaquePlan xr_codegen_opaque_plan_core(XrCodegenOpaqueKind kind) {
    XrCodegenOpaquePlan plan = {kind, kind < XR_CODEGEN_OPAQUE_KIND_COUNT,
                                kind < XR_CODEGEN_OPAQUE_KIND_COUNT};
    return plan;
}

static inline bool xr_codegen_opaque_plan_is_exact_core(XrCodegenOpaquePlan plan) {
    return plan.kind < XR_CODEGEN_OPAQUE_KIND_COUNT && plan.preserves_value_bits &&
           plan.blocks_native_constant_propagation;
}

#define XR_CODEGEN_OPAQUE_OWNER_GUARD(owner_hi, owner_lo)                                       \
    ((void) sizeof(struct {                                                                       \
        unsigned int owner_id_must_be_shared_codegen_opaque                                     \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_HI &&            \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_LO)               \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_CODEGEN_OPAQUE_CONSUMER_GUARD(consumer_bit)                                          \
    ((void) sizeof(struct {                                                                       \
        unsigned int consumer_must_be_declared_for_shared_codegen_opaque                        \
            : (((uint32_t) (consumer_bit) != 0 &&                                                \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_CONSUMERS &                              \
                 (uint32_t) (consumer_bit)) != 0)                                                \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_CODEGEN_OPAQUE_OWNER_PLAN(owner_hi, owner_lo, consumer_bit, kind)                    \
    (XR_CODEGEN_OPAQUE_OWNER_GUARD((owner_hi), (owner_lo)),                                      \
     XR_CODEGEN_OPAQUE_CONSUMER_GUARD((consumer_bit)), xr_codegen_opaque_plan_core((kind)))

typedef struct XrCodegenFencePlan {
    bool preserves_program_state;
    bool has_runtime_memory_effect;
    bool blocks_native_memory_reordering;
} XrCodegenFencePlan;

static inline XrCodegenFencePlan xr_codegen_fence_plan_core(void) {
    XrCodegenFencePlan plan = {true, false, true};
    return plan;
}

static inline bool xr_codegen_fence_plan_is_exact_core(XrCodegenFencePlan plan) {
    return plan.preserves_program_state && !plan.has_runtime_memory_effect &&
           plan.blocks_native_memory_reordering;
}

#define XR_CODEGEN_FENCE_OWNER_GUARD(owner_hi, owner_lo)                                        \
    ((void) sizeof(struct {                                                                       \
        unsigned int owner_id_must_be_shared_codegen_compiler_fence                             \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_HI &&    \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_LO)       \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_CODEGEN_FENCE_CONSUMER_GUARD(consumer_bit)                                           \
    ((void) sizeof(struct {                                                                       \
        unsigned int consumer_must_be_declared_for_shared_codegen_compiler_fence                \
            : (((uint32_t) (consumer_bit) != 0 &&                                                \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_CONSUMERS &                      \
                 (uint32_t) (consumer_bit)) != 0)                                                \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_CODEGEN_FENCE_OWNER_PLAN(owner_hi, owner_lo, consumer_bit)                           \
    (XR_CODEGEN_FENCE_OWNER_GUARD((owner_hi), (owner_lo)),                                       \
     XR_CODEGEN_FENCE_CONSUMER_GUARD((consumer_bit)), xr_codegen_fence_plan_core())

#endif /* XR_CODEGEN_OPAQUE_CORE_H */
