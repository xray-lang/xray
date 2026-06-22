/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xscope_transfer.h - Optional scope error transfer capability.
 */

#ifndef XSCOPE_TRANSFER_H
#define XSCOPE_TRANSFER_H

#include "../base/xconfig.h"

struct XrAotRuntime;
struct XrRuntimeCore;

XR_FUNC void xr_scope_transfer_enable_core(struct XrRuntimeCore *core);
XR_FUNC void xr_aot_runtime_enable_transfer(struct XrAotRuntime *runtime);

#endif  // XSCOPE_TRANSFER_H
