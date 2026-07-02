/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * url.h - URL module loader and C-level percent encoding helpers
 */

#ifndef XR_STDLIB_URL_H
#define XR_STDLIB_URL_H

#include "../../src/base/xdefs.h"
#include <stddef.h>

XR_FUNC int xr_url_encode(const char *str, size_t len, char *buf, size_t buf_size);
XR_FUNC int xr_url_decode(const char *str, size_t len, char *buf, size_t buf_size);
XR_FUNC int xr_url_encode_form(const char *str, size_t len, char *buf, size_t buf_size);
XR_FUNC int xr_url_decode_form(const char *str, size_t len, char *buf, size_t buf_size);

struct XrVMRuntime;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_url(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_URL_H
