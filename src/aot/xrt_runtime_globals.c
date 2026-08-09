/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_runtime_globals.c - XRT global-state owner for the compiler runtime
 *
 * Generated AOT programs define XRT_IMPL in their entry translation unit.
 * The compiler and stdlib bytecode generator instead link this object through
 * xray_core / xray_stdlib_bootstrap_core.  xray_aot_core intentionally omits
 * this file so a generated program never receives a second implementation.
 */

#define XRT_IMPL

#include "xrt_class.h"
#include "xrt_coll.h"
#include "xrt_exception.h"
