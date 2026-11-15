/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_values.h - Execution-owned fixed-value context retirement
 */
#ifndef XNATIVE_VALUES_H
#define XNATIVE_VALUES_H
#include "xnative_value.h"

/* The caller owns this mutable root outside its setjmp function. Retirement
 * runs under the execution's cleanup boundary and never invokes guest code. */
XR_FUNC void xrt_native_values_drain(XrtValueRoot *);
#endif // XNATIVE_VALUES_H
