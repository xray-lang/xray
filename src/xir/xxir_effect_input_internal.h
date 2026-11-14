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

/* Follows a place, Cell or view back through projections to the mutable module
 * binding whose storage it denotes. A class field step ends the walk: an object
 * is reached by identity, never by the binding's name. */
XR_FUNC bool xir_module_slot_root(const XrXirModule *module, const XrXirFunction *body, uint32_t value,
    uint32_t *slot);

#endif // XXIR_EFFECT_INPUT_INTERNAL_H
