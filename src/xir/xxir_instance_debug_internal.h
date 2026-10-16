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
/* A display slot is admitted only for a real entered VM ancestor in the exact
 * current activation. Publication, module readiness and worker rules remain. */
XR_FUNC XrXirCallStatus xir_instance_observe_slot(XrXirCallView *,uint64_t,uint32_t,XrXirValue *);
/* Only a fresh genuine VM control callback may use this scope. It checks the
 * live invocation proof, publication and loan, without inventing an opcode.
 * Success owns one committed access; the caller must finish it on every exit. */
XR_FUNC XrXirCallStatus xir_instance_debug_class_access(XrXirCallView *,const XrXirValue *,XrXirCallClassAccess *);
/* Read-only capture at exact authenticated VM ingress. Class projections keep
 * their real descriptor scope and sealed parent proof; no loan is returned. */
XR_FUNC XrXirCallStatus xir_instance_observe_cell(XrXirCallView *,const XrXirValue *,XrXirValue *);
/* Control writes require the actual directed visit in addition to VM ingress. */
XR_FUNC XrXirCallStatus xir_instance_debug_cell_write(XrXirCallView *,const XrXirValue *,const XrXirValue *);
#endif // XXIR_INSTANCE_DEBUG_INTERNAL_H
