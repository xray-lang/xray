/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_control_internal.h - Owned projections of numeric semantic summaries
 *
 * KEY CONCEPT:
 *   Numeric ROOT equations produce control facts and causes once.
 *   Retained projections own values independently of all body storage.
 */
#ifndef XXIR_EFFECT_CONTROL_INTERNAL_H
#define XXIR_EFFECT_CONTROL_INTERNAL_H

#include "xxir.h"

typedef struct EffectControlSummary EffectControlSummary;
struct XrXirEffects;
XR_FUNC void xir_effect_control_free(EffectControlSummary *summary);

typedef struct XirSuspendCauseStep {
    uint32_t function, instruction, callee, parameter;
    bool terminal, parameter_dependency, suspending;
} XirSuspendCauseStep;
typedef XrXirStatus (*XirSuspendCauseVisitor)(void *, const XirSuspendCauseStep *);
/* Presentation borrows the immutable numeric owner only during this call. */
XR_FUNC XrXirStatus xir_effects_suspend_trace(const struct XrXirEffects *effects,
    uint32_t function, XirSuspendCauseVisitor visit, void *context);

#endif // XXIR_EFFECT_CONTROL_INTERNAL_H
