/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_function_internal.h - Actual sealed function producer entry
 *
 * KEY CONCEPT:
 *   The current frame and real owned instruction select metadata; no caller flags authorize a body.
 */
#ifndef XXIR_INSTANCE_FUNCTION_INTERNAL_H
#define XXIR_INSTANCE_FUNCTION_INTERNAL_H
#include "xxir_program.h"
#include "xxir_effect_contract_internal.h"
XR_FUNC XrXirCallStatus xr_xir_instance_function_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *captures,uint32_t count,XrXirValue *output);
/* This proves a real producer identity; it does not close its capture environment. */
XR_FUNC XrXirCallStatus xr_xir_instance_function_producer(XrXirCallView *view,
    const XrXirValue *function,XirEffectProducerView *output);
/* Only authentic current-frame inputs may close a conditional carrier. */
XR_FUNC XrXirCallStatus xr_xir_instance_resolve_function_at(XrXirCallView *view,
    uint32_t instruction,const XrXirValue *function,const XrXirValue *arguments,
    uint32_t count,uint32_t *entry);
/* A true direct call carries its selected finite context into the child frame. */
XR_FUNC XrXirCallStatus xr_xir_instance_call_at(XrXirCallView *view,uint32_t instruction,
    uint32_t entry,const XrXirValue *arguments,uint32_t count);
#endif // XXIR_INSTANCE_FUNCTION_INTERNAL_H
