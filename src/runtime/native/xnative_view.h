/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_view.h - Root retirement of activation-owned view resources
 */
#ifndef XNATIVE_VIEW_H
#define XNATIVE_VIEW_H
#include "xray_native_view.h"
#include "xnative_value.h"
XR_FUNC void xrt_view_root_drain(XrtValueRoot *);
#endif // XNATIVE_VIEW_H
