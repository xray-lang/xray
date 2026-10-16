/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_generator_internal.h - Typed producer storage and executor membership
 *
 * KEY CONCEPT:
 *   Only the owning executor holds code authority for a live producer.
 */
#ifndef XXIR_GENERATOR_INTERNAL_H
#define XXIR_GENERATOR_INTERNAL_H
#include "xxir_task_internal.h"
typedef enum XirGeneratorState {
    XIR_GENERATOR_PREPARING, XIR_GENERATOR_PARKED, XIR_GENERATOR_RUNNING, XIR_GENERATOR_TERMINAL
} XirGeneratorState;
typedef struct XirGenerator {
    XirObject object;
    XrXirCallResult outcome;
    XrXirCallAccounting accounting;
    XrXirCall *call;
    XrXirCall *consumer;
    XrXirTaskExecutor *executor;
    struct XirGenerator *active_next, *active_previous;
    XirTaskActivation activation;
    uint64_t generation, identity;
    _Atomic(XirGeneratorState) state;
    _Atomic(bool) orphaned;
    bool child;
} XirGenerator;
XR_FUNC bool xr_xir_generator_storage_valid(const XirGenerator *generator);
/* Construction is private to an authenticated executing Instance view. */
XR_FUNC XrXirCallStatus xr_xir_task_executor_generator(XrXirTaskExecutor *executor,
    XrXirCallView *view, XrXirType type, const XrXirCallRequest *request, XrXirValue *output);
#endif
