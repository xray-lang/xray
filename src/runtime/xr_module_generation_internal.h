/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_generation_internal.h - Generation authority storage and verifier
 */

#ifndef XR_MODULE_GENERATION_INTERNAL_H
#define XR_MODULE_GENERATION_INTERNAL_H

#include "../../include/xray_runtime_generation.h"
#include "../os/os_thread.h"

typedef enum XrModuleGenerationMutation {
    XR_MODULE_GENERATION_MUTATION_VERIFY = 0,
    XR_MODULE_GENERATION_MUTATION_PREPARE,
    XR_MODULE_GENERATION_MUTATION_ACTIVATE,
    XR_MODULE_GENERATION_MUTATION_BEGIN_DRAIN,
    XR_MODULE_GENERATION_MUTATION_RETIRE,
    XR_MODULE_GENERATION_MUTATION_POISON,
    XR_MODULE_GENERATION_MUTATION_ROLLBACK,
    XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
    XR_MODULE_GENERATION_MUTATION_PIN_RELEASE,
    XR_MODULE_GENERATION_MUTATION_UNLOAD,
} XrModuleGenerationMutation;

struct XrRuntimeGenerationAuthority {
    xr_mutex_t gate;
    XrRuntimeGenerationBudget budget;
    uint64_t next_generation;
    uint32_t live_generations;
    uint32_t total_pins;
};

struct XrLoadedModuleGeneration {
    XrRuntimeGenerationAuthority *authority;
    XrTargetPlan *plan;
    XrModuleGenerationIdentity identity;
    XrModuleGenerationState state;
    uint64_t revision;
    uint32_t pins_by_kind[XR_MODULE_GENERATION_PIN_KIND_COUNT];
    uint32_t total_pins;
    bool poisoned;
    bool rollback_requested;
    uint8_t poison_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
};

XR_FUNC bool xr_module_generation_verify_transition(
    const XrModuleGenerationSnapshot *before,
    const XrModuleGenerationSnapshot *after,
    XrModuleGenerationMutation mutation, XrModuleGenerationPinKind pin_kind,
    char *diagnostic, size_t diagnostic_size);

#endif  // XR_MODULE_GENERATION_INTERNAL_H
