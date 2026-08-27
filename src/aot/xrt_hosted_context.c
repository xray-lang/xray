/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Hosted-fragment runtime context storage.
 */

#include "../base/xdefs.h"

struct XrAotContext;

XR_THREAD_LOCAL const struct XrAotContext *xrt_hosted_aot_context = NULL;
