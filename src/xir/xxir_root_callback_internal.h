/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_root_callback_internal.h - Private owned Root callback activation
 */
#ifndef XXIR_ROOT_CALLBACK_INTERNAL_H
#define XXIR_ROOT_CALLBACK_INTERNAL_H
#include "xxir_program.h"

/* Prepares and publishes one sibling activation only from an actual same-Root
 * owned FunctionBinding: zero explicit parameters, Unit result, Never error.
 * Captures and the code lease stay owned until that independent Call retires.
 * The live Parent Call, binding ticket, epoch and wait records are untouched.
 * A busy, initializing, borrowed, unknown or NoSuspend frontier defers with
 * BAD_STATE/BUSY and performs no callback. This grants no Source registration
 * or native call permission; those require their own sealed site admission. */
XR_FUNC XrXirCallStatus xir_instance_callback_begin(XrXirInstance *instance,
    const XrXirValue *function);
/* An observation of the actual executor-owned sibling, not an execution or
 * handler-registration capability. Invalid/foreign owners return false. */
XR_FUNC bool xir_instance_callback_active(const XrXirInstance *instance);
#endif // XXIR_ROOT_CALLBACK_INTERNAL_H
