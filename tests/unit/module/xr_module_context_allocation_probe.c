/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Compile the actual coordinator with a test-only allocation boundary.
 */

#include "xr_module_source_allocation_probe.h"
#include "../../../src/api/xisolate_scripting.c"
