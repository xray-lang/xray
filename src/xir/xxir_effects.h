/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effects.h - Owned control facts derived from verified XIR
 */
#ifndef XXIR_EFFECTS_H
#define XXIR_EFFECTS_H
#include "xxir.h"
typedef enum XrXirEffect { XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_MAY } XrXirEffect;
typedef struct XrXirFunctionEffects { XrXirEffect suspend, throws; } XrXirFunctionEffects;
typedef struct XrXirEffects XrXirEffects;
/* Reverification and inference consume one cumulative work allowance. */
XR_FUNC XrXirStatus xr_xir_effects_analyze(const XrXirArtifact *artifact,
    const XrXirBudget *budget, XrXirEffects **output);
/* The borrowed fact remains valid until its owning summary is freed. */
XR_FUNC const XrXirFunctionEffects *xr_xir_effects_function(const XrXirEffects *effects, uint32_t function);
XR_FUNC void xr_xir_effects_free(XrXirEffects *effects);
#endif // XXIR_EFFECTS_H
