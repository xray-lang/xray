/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_runtime_generation.h - Verified module generation lifecycle authority
 *
 * KEY CONCEPT:
 *   A runtime authority owns generation numbers, immutable plan identities,
 *   lifecycle transitions, and bounded pins. A verified plan is never made
 *   executable unless the installed typed executor owns every required family.
 */

#ifndef XRAY_RUNTIME_GENERATION_H
#define XRAY_RUNTIME_GENERATION_H

#include "xray_export.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrTargetPlan XrTargetPlan;
typedef struct XrRuntimeGenerationAuthority XrRuntimeGenerationAuthority;
typedef struct XrLoadedModuleGeneration XrLoadedModuleGeneration;

#define XR_RUNTIME_GENERATION_SCHEMA_VERSION UINT32_C(1)
#define XR_RUNTIME_GENERATION_FINGERPRINT_SIZE 32u

typedef enum XrModuleGenerationState {
    XR_MODULE_GENERATION_LOADING = 0,
    XR_MODULE_GENERATION_VERIFIED,
    XR_MODULE_GENERATION_READY,
    XR_MODULE_GENERATION_ACTIVE,
    XR_MODULE_GENERATION_DRAINING,
    XR_MODULE_GENERATION_RETIRED,
    XR_MODULE_GENERATION_UNLOADED,
    XR_MODULE_GENERATION_STATE_COUNT,
} XrModuleGenerationState;

typedef enum XrModuleGenerationPinKind {
    XR_MODULE_GENERATION_PIN = 0,
    XR_MODULE_GENERATION_INFLIGHT_CALL,
    XR_MODULE_GENERATION_CALLBACK,
    XR_MODULE_GENERATION_DESTRUCTOR,
    XR_MODULE_GENERATION_STATIC_ROOT,
    XR_MODULE_GENERATION_PIN_KIND_COUNT,
} XrModuleGenerationPinKind;

typedef struct XrRuntimeGenerationBudget {
    uint32_t schema_version;
    uint32_t max_loaded_generations;
    uint32_t max_total_pins;
    uint32_t max_pins_per_generation;
    uint32_t max_pins_by_kind[XR_MODULE_GENERATION_PIN_KIND_COUNT];
} XrRuntimeGenerationBudget;

typedef struct XrModuleGenerationIdentity {
    uint32_t schema_version;
    uint32_t target_plan_schema_version;
    uint64_t generation_number;
    uint64_t completed_family_mask;
    uint64_t required_capability_mask;
    uint8_t semantic_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
    uint8_t target_profile_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
    uint8_t target_plan_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
    uint8_t runtime_abi_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
    uint8_t provider_set_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
    uint8_t object_header_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
    uint8_t generation_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
} XrModuleGenerationIdentity;

typedef struct XrModuleGenerationSnapshot {
    XrModuleGenerationIdentity identity;
    uint32_t state;
    uint32_t poisoned;
    uint32_t rollback_requested;
    uint32_t reserved;
    uint64_t revision;
    uint32_t total_pins;
    uint32_t pins_by_kind[XR_MODULE_GENERATION_PIN_KIND_COUNT];
    uint8_t poison_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
} XrModuleGenerationSnapshot;

XRAY_API bool xr_runtime_generation_authority_create(
    const XrRuntimeGenerationBudget *budget,
    XrRuntimeGenerationAuthority **authority, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_runtime_generation_authority_destroy(
    XrRuntimeGenerationAuthority **authority, char *diagnostic,
    size_t diagnostic_size);

XRAY_API bool xr_runtime_generation_activation_available(void);
XRAY_API bool xr_module_generation_load_verified_target_plan(
    XrRuntimeGenerationAuthority *authority,
    const XrTargetPlan *verified_target_plan,
    XrLoadedModuleGeneration **generation, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_module_generation_prepare(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_module_generation_activate(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_module_generation_begin_drain(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_module_generation_retire(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_module_generation_poison(
    XrLoadedModuleGeneration *generation,
    const uint8_t diagnostic_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE],
    char *diagnostic, size_t diagnostic_size);
XRAY_API bool xr_module_generation_rollback(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_module_generation_pin_acquire(
    XrLoadedModuleGeneration *generation, XrModuleGenerationPinKind kind,
    char *diagnostic, size_t diagnostic_size);
XRAY_API bool xr_module_generation_pin_release(
    XrLoadedModuleGeneration *generation, XrModuleGenerationPinKind kind,
    char *diagnostic, size_t diagnostic_size);
XRAY_API bool xr_module_generation_snapshot(
    const XrLoadedModuleGeneration *generation,
    XrModuleGenerationSnapshot *snapshot);
XRAY_API bool xr_module_generation_verify(
    const XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_module_generation_unload(
    XrLoadedModuleGeneration **generation, char *diagnostic,
    size_t diagnostic_size);

#endif  // XRAY_RUNTIME_GENERATION_H
