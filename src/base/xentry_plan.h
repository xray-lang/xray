/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xentry_plan.h - Canonical reachable-runtime entry contract for VM and AOT.
 */

#ifndef XR_ENTRY_PLAN_H
#define XR_ENTRY_PLAN_H

#include <stdint.h>

typedef enum XrRuntimeCapability {
    XR_CAP_COROUTINE = 1u << 0,
    XR_CAP_CHANNEL = 1u << 1,
    XR_CAP_EXCEPTION = 1u << 2,
    XR_CAP_NATIVE = 1u << 3,
    XR_CAP_EXTERN = 1u << 4,
    XR_CAP_OBJECTS = 1u << 5,
    XR_CAP_DEEP_COPY = 1u << 6,
    XR_CAP_INSTANCEOF = 1u << 7,
    XR_CAP_SYS_THREAD = 1u << 8,
    XR_CAP_SCOPE = 1u << 9,
    XR_CAP_TIMER = 1u << 10,
    XR_CAP_NETPOLL = 1u << 11,
    XR_CAP_TASK = 1u << 12,
    XR_CAP_ATOMIC = 1u << 13,
    XR_CAP_WORK_QUEUE = 1u << 14,
    XR_CAP_RESULT_GROUP = 1u << 15,
    XR_CAP_COUNTDOWN_LATCH = 1u << 16,
    XR_CAP_SEMAPHORE = 1u << 17,
    XR_CAP_EVENT_COUNT = 1u << 18,
    XR_CAP_GENERATOR = 1u << 19,
    XR_CAP_STACKTRACE = 1u << 20,
    XR_CAP_PARALLEL = 1u << 21,
} XrRuntimeCapability;

typedef enum XrReachableEffect {
    XR_EFFECT_MAY_ERROR = 1u << 0,
    XR_EFFECT_MAY_SUSPEND = 1u << 1,
    XR_EFFECT_MAY_ALLOC = 1u << 2,
    XR_EFFECT_MAY_MUTATE = 1u << 3,
    XR_EFFECT_MAY_CALL_NATIVE = 1u << 4,
    XR_EFFECT_MAY_READ_MEM = 1u << 5,
    XR_EFFECT_MAY_CALL = 1u << 6,
    XR_EFFECT_MAY_SPAWN = 1u << 7,
    XR_EFFECT_ACCESSES_MUTABLE_MODULE = 1u << 8,
    XR_EFFECT_OBSERVES_TASK_ID = 1u << 9,
    XR_EFFECT_MAY_PANIC = 1u << 10,
} XrReachableEffect;

typedef enum XrRootRepresentation {
    XR_ROOT_ELIDED = 0,
    XR_ROOT_DESCRIPTOR,
    XR_ROOT_RESUMABLE_FRAME,
} XrRootRepresentation;

typedef enum XrSchedulerMode {
    XR_SCHED_NONE = 0,
    XR_SCHED_SINGLE,
    XR_SCHED_MULTI,
} XrSchedulerMode;

enum {
    XR_ENTRY_EV_GLOBAL_BODY = 1u << 0,
    XR_ENTRY_EV_CLOSED_WORLD_REACHABILITY = 1u << 1,
    XR_ENTRY_EV_ROOT_EFFECT = 1u << 2,
    XR_ENTRY_EV_TARGET_PROVIDER = 1u << 3,
    XR_ENTRY_EV_VERIFIED_BYTECODE = 1u << 4,
    XR_ENTRY_EV_ARTIFACT_ROOT_SET = 1u << 5,
};

typedef enum XrEntryUnprovenReason {
    XR_ENTRY_PROVEN = 0,
    XR_ENTRY_NO_ROOT_BODY,
    XR_ENTRY_PROVIDER_ABI,
    XR_ENTRY_MISSING_CAPABILITY,
    XR_ENTRY_MISSING_PROVIDER_HOOK,
    XR_ENTRY_OPEN_REACHABILITY,
    XR_ENTRY_MODULE_INIT_SUSPENDS,
} XrEntryUnprovenReason;

typedef struct XrEntryPlan {
    uint32_t entry_func_id;
    uint32_t reachable_body_count;
    uint32_t reachable_effect_bits;
    uint32_t required_capability_bits;
    uint32_t provided_capability_bits;
    uint32_t runtime_component_bits;
    uint32_t provider_hook_bits;
    uint32_t evidence;
    uint8_t root_representation;
    uint8_t scheduler_mode;
    uint8_t unproven_reason;
} XrEntryPlan;

#endif /* XR_ENTRY_PLAN_H */
