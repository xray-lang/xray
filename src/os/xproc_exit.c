/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xproc_exit.c - Process termination with a defined signed status narrowing
 *
 * KEY CONCEPT:
 *   The low 32 bits form the signed C status; the host retains its ordinary
 *   exit reporting rules and C stream/atexit cleanup.
 */
#include "os_proc.h"
#include <limits.h>
#include <stdlib.h>

_Static_assert(INT_MAX==INT32_MAX && INT_MIN==INT32_MIN,"process status requires a 32-bit C int");
XR_FUNC_NORET void xr_proc_self_exit(int64_t code) {
    uint32_t low=(uint32_t)(uint64_t)code;
    int64_t status=low<=INT32_MAX?(int64_t)low:(int64_t)low-INT64_C(4294967296);
    exit((int)status);
}
