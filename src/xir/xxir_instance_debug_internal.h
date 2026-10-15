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
XR_FUNC XrXirCallStatus xir_instance_visit_vm(XrXirInstance *,const XrXirExecutorBinding *,uint64_t,bool *);
#endif // XXIR_INSTANCE_DEBUG_INTERNAL_H
