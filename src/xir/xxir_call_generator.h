/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_generator.h - Typed pull production on the common Call trampoline
 *
 * KEY CONCEPT:
 *   A produced value parks its independent activation without a scheduler wake.
 */
#ifndef XXIR_CALL_GENERATOR_H
#define XXIR_CALL_GENERATOR_H
#include "xxir_call.h"
/* Only an Iterator-result entry with the generator flag can start this way.
 * Creation copies arguments and pins metadata before the first body dispatch. */
XR_FUNC XrXirCallStatus xr_xir_call_generator_new(const XrXirCallConfig *config, uint32_t entry,
    const XrXirValue *arguments, uint32_t count, XrXirCall **output);
/* Transfers exactly one yielded owner to an empty output and readies the same
 * frame. It never dispatches, completes the generator or consumes a wake token. */
XR_FUNC XrXirCallStatus xr_xir_call_generator_take(XrXirCall *call, XrXirValue *output);
#endif // XXIR_CALL_GENERATOR_H
