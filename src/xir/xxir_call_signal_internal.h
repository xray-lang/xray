/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_signal_internal.h - Single-use coordinates of a real direct call
 */
#ifndef XXIR_CALL_SIGNAL_INTERNAL_H
#define XXIR_CALL_SIGNAL_INTERNAL_H
#include "xxir_call.h"
typedef struct XirCallSignalOrigin { uint32_t caller,instruction,target; } XirCallSignalOrigin;
/* Coordinates alone grant no registration permission. Instance rechecks the
 * retained instruction and actual arguments both before staging and in the
 * committed native frame. Non-call actions and failed frame preparation clear
 * the pending coordinates. A native frame can consume its origin only once. */
XR_FUNC XrXirCallStatus xir_call_signal_stage(XrXirCallView *view,uint32_t instruction,uint32_t target);
XR_FUNC XrXirCallStatus xir_call_signal_origin(XrXirCallView *view,XirCallSignalOrigin *output);
#endif // XXIR_CALL_SIGNAL_INTERNAL_H
