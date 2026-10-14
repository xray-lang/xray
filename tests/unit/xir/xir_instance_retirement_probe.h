/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_instance_retirement_probe.h - Accepted execution and exhausted budget observations
 */
#ifndef XIR_INSTANCE_RETIREMENT_PROBE_H
#define XIR_INSTANCE_RETIREMENT_PROBE_H
#include "xir/xxir_program.h"
XR_FUNC XrXirCallStatus xr_test_instance_retirement(XrXirInstance *instance,XrXirCallStatus fault);
#endif // XIR_INSTANCE_RETIREMENT_PROBE_H
