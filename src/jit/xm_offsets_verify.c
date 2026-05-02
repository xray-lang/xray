/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_offsets_verify.c - Compile-time verification of JIT struct offsets
 *
 * KEY CONCEPT:
 *   This file is compiled to verify that the hardcoded offsets in
 *   xm_offsets.h match the actual struct layouts. If any offset
 *   changes, this file will fail to compile.
 */

#define XM_VERIFY_OFFSETS
#include "xm_offsets.h"
#include "../base/xchecks.h"
