/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_control_internal.h - Streaming control summaries for trusted producers
 *
 * KEY CONCEPT:
 *   Producers emit each final instruction during their existing traversal.
 *   Closure consumes numeric dependencies and declaration metadata only.
 */
#ifndef XXIR_EFFECT_CONTROL_INTERNAL_H
#define XXIR_EFFECT_CONTROL_INTERNAL_H

#include "xxir.h"

typedef struct EffectControlSummary EffectControlSummary;
typedef struct EffectSummaryBuild XirControlBuilder;

/* Input has already passed the producer's structural admission. The builder
 * copies its memory context and borrows request control until synchronous use ends. */
XR_FUNC XrXirStatus xir_effect_control_builder_new(const XrXirCompileContext *context,
    const XrXirModule *module, XrCompileRequestControl *control, XirControlBuilder **output);
XR_FUNC XrXirStatus xir_effect_control_begin(XirControlBuilder *builder,
    const XrXirModule *module, uint32_t function);
XR_FUNC XrXirStatus xir_effect_control_instruction(XirControlBuilder *builder,
    const XrXirModule *module, uint32_t function, uint32_t instruction);
XR_FUNC XrXirStatus xir_effect_control_end(XirControlBuilder *builder,
    const XrXirModule *module, uint32_t function);
/* Function identities may grow during production. Every final function must
 * have been emitted exactly once. No instruction array is read at closure. */
XR_FUNC XrXirStatus xir_effect_control_finish(XirControlBuilder *builder,
    const XrXirModule *module, EffectControlSummary **output);
XR_FUNC void xir_effect_control_builder_free(XirControlBuilder *builder);
XR_FUNC XrXirStatus xir_effect_control_retain(const XrXirCompileContext *context,
    EffectControlSummary *summary, EffectControlSummary **output);
XR_FUNC void xir_effect_control_free(EffectControlSummary *summary);

#endif // XXIR_EFFECT_CONTROL_INTERNAL_H
