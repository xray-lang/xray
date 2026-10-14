/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_instance_retirement_probe.c - Same-source retirement state verification
 *
 * KEY CONCEPT:
 *   Rejected preparation has no accepted execution fault; exhaustion is a
 *   distinct lifetime fact checked before the real Instance is freed.
 */
#include "xir_instance_retirement_probe.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef CHECK
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#endif
#ifndef XIR_INSTANCE_IMPLEMENTATION_PRESENT
#include "xir/xxir_instance.c"
#endif
XR_FUNC XrXirCallStatus xr_test_instance_retirement(XrXirInstance *instance,XrXirCallStatus fault) {
    if(!instance) { CHECK(fault==XR_XIR_CALL_READY);return XR_XIR_CALL_READY; }
    XrXirCallStatus expected=fault;
    if(expected==XR_XIR_CALL_READY && instance->budget.exhausted)expected=XR_XIR_CALL_LIMIT;
    CHECK(xr_xir_task_executor_completion_status(instance->executor)==expected);
    return expected;
}
