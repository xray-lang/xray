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
#define XR_XIR_ERROR_SYMBOLIC_VARIANT UINT32_MAX
/* Reverification and inference consume one cumulative work allowance. */
XR_FUNC XrXirStatus xr_xir_effects_analyze(const XrXirArtifact *artifact,
    const XrXirBudget *budget, XrXirEffects **output);
/* The borrowed fact remains valid until its owning summary is freed. */
XR_FUNC const XrXirFunctionEffects *xr_xir_effects_function(const XrXirEffects *effects, uint32_t function);
/* Numeric type identities refer to the analyzed artifact, without borrowing it. */
XR_FUNC bool xr_xir_effects_error(const XrXirEffects *effects, uint32_t function,
    XrXirType type, uint32_t variant);
XR_FUNC bool xr_xir_effects_error_unidentified(const XrXirEffects *effects, uint32_t function);
XR_FUNC bool xr_xir_effects_error_unknown(const XrXirEffects *effects, uint32_t function);
XR_FUNC void xr_xir_effects_free(XrXirEffects *effects);
#endif // XXIR_EFFECTS_H
