/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_reference_internal.h - Real reference selection within one proof
 */
#ifndef XXIR_EFFECT_REFERENCE_INTERNAL_H
#define XXIR_EFFECT_REFERENCE_INTERNAL_H
#include "xxir_effect_contract_internal.h"

/* Only the current read-only operation lends this scope. Context selection
 * retains complete origin, lexical owner and real edge checks; open generics
 * keep their conservative bound path. No scope or result survives the callback.
 * Every new operation repeats full Source admission. Rejection is terminal. */
XR_FUNC XrXirStatus xir_effects_reference_root_scoped(XirEffectContextScope *scope,
    uint32_t function,uint32_t instruction,uint32_t *output);
#endif // XXIR_EFFECT_REFERENCE_INTERNAL_H
