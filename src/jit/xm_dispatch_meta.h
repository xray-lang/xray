/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_dispatch_meta.h - Runtime accessors for generated Xm dispatch metadata
 *
 * KEY CONCEPT:
 *   Keep backend dispatch diagnostics tied to the generated isel metadata.
 */

#ifndef XM_DISPATCH_META_H
#define XM_DISPATCH_META_H

#include "xm_dispatch_meta_gen.h"
#include "../base/xdefs.h"

XR_FUNC const XmDispatchMeta *xm_dispatch_meta_find(XmOp op, XmDispatchBackend backend);

XR_FUNC const char *xm_dispatch_meta_mcinsns(XmOp op, XmDispatchBackend backend);

#endif  // XM_DISPATCH_META_H
