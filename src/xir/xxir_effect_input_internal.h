/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_input_internal.h - Complete synchronous effect input
 *
 * KEY CONCEPT:
 *   Physical source and immutable construction facts use the same original
 *   coordinates. A request control is borrowed only for the receiving call.
 */
#ifndef XXIR_EFFECT_INPUT_INTERNAL_H
#define XXIR_EFFECT_INPUT_INTERNAL_H
#include "xxir_construction.h"

typedef struct XirEffectInput {
    const XrXirModule *module;
    const XrXirConstruction *construction;
    XrCompileRequestControl *control;
} XirEffectInput;

#endif // XXIR_EFFECT_INPUT_INTERNAL_H
