/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_generator_internal.h - Sealed generator creation entry for both backends
 *
 * KEY CONCEPT:
 *   Only the owning executor holds code authority for a live producer.
 */
#ifndef XXIR_INSTANCE_GENERATOR_INTERNAL_H
#define XXIR_INSTANCE_GENERATOR_INTERNAL_H
#include "xxir_call.h"
struct XrXirCallRequest;
struct XirEffectInvocationSelection;
/* The executing frame and its sealed instruction select the complete factory. */
XR_FUNC XrXirCallStatus xr_xir_instance_generator_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *arguments,uint32_t count,XrXirValue *output);
/* Rechecks actual owned inputs after fallible producer-frame preparation. */
XR_FUNC XrXirCallStatus xr_xir_instance_generator_context(XrXirCallView *view,uint32_t instruction,
    const struct XrXirCallRequest *request,struct XirEffectInvocationSelection *output);
#endif
