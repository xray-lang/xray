/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_debug_internal.h - Exact live VM ingress for paused control
 */
#ifndef XXIR_INSTANCE_DEBUG_INTERNAL_H
#define XXIR_INSTANCE_DEBUG_INTERNAL_H
#include "xxir_program.h"
#include "xxir_call_observer_internal.h"
#include "xxir_call_internal.h"
XR_FUNC XrXirCallStatus xir_instance_visit_vm(XrXirInstance *,const XrXirExecutorBinding *,uint64_t,bool *);
/* Only a fresh genuine VM control callback may use this scope. It checks the
 * live invocation proof, publication and loan, without inventing an opcode.
 * Success owns one committed access; the caller must finish it on every exit. */
XR_FUNC XrXirCallStatus xir_instance_debug_class_access(XrXirCallView *,const XrXirValue *,XrXirCallClassAccess *);
#endif // XXIR_INSTANCE_DEBUG_INTERNAL_H
