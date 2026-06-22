/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_string.h - VM isolate string helper API
 */

#ifndef XVM_STRING_H
#define XVM_STRING_H

#include "../runtime/xstrbuf.h"

XR_FUNC XrStrBuf *xr_strbuf_tmp(XrayIsolate *X);

#endif  // XVM_STRING_H
