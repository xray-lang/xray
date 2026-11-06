/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_instance_oom_cleanup.h - Observable terminal outcomes survive teardown
 *
 * The including fixture supplies CHECK and its actual runtime allocation observer.
 */
#ifndef XIR_INSTANCE_OOM_CLEANUP_H
#define XIR_INSTANCE_OOM_CLEANUP_H
#include "xir/xxir_program.h"
static void xir_fixture_close_instance(XrXirInstance *instance,bool failed,XrXirCallStatus completed) {
    CHECK(instance && (completed==XR_XIR_CALL_READY || completed==XR_XIR_CALL_BAD_ARGUMENT));
    size_t attempts=runtime_attempts;
    XrXirCallStatus prior=xr_xir_instance_poll_bounded(instance,1).outcome.status;
    CHECK(runtime_attempts==attempts);
    XrXirCallStatus expected=completed;
    if (prior==XR_XIR_CALL_OOM) {
        CHECK(failed && runtime_fail_at!=SIZE_MAX && runtime_attempts>runtime_fail_at);
        expected=prior;
    } else if (completed==XR_XIR_CALL_READY) {
        CHECK(prior==XR_XIR_CALL_BAD_STATE || prior==XR_XIR_CALL_CONSUMED);
    } else CHECK(prior==completed);
    XrXirCallStatus closed=xr_xir_instance_free(instance);
    if (closed!=expected) fprintf(stderr,"instance close actual%u expected%u fault%zu attempts%zu\n",closed,expected,runtime_fail_at,runtime_attempts);
    CHECK(closed==expected);
}
#endif
