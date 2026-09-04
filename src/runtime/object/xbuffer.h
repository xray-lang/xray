/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbuffer.h - Managed raw-byte storage runtime object
 *
 * KEY CONCEPT:
 *   Buffer storage and lifetime are runtime representation mechanics. The mem
 *   standard library owns allocation policy and public validation in Xray.
 */

#ifndef XBUFFER_H
#define XBUFFER_H

#include "../value/xvalue.h"

struct XrAggregateLayout;
struct XrNativeBodyDesc;
struct XrVMRuntime;

XR_FUNC struct XrNativeBodyDesc *xr_buffer_native_body_desc(void);
XR_FUNC XrValue xr_buffer_new(struct XrVMRuntime *isolate, int64_t length, bool zeroed,
                              size_t align);
XR_FUNC int64_t xr_buffer_length(XrValue value);
XR_FUNC void *xr_buffer_borrow_pointer(struct XrVMRuntime *isolate, XrValue value);
XR_FUNC XrValue xr_buffer_byte_view(struct XrVMRuntime *isolate, XrValue value, bool readonly);
XR_FUNC bool xr_buffer_resize(struct XrVMRuntime *isolate, XrValue value, int64_t new_length);
XR_FUNC bool xr_buffer_bytes(XrValue value, const uint8_t **data, size_t *length);
XR_FUNC XrValue xr_buffer_copy_from_bytes(struct XrVMRuntime *isolate, const uint8_t *data,
                                          size_t length);
XR_FUNC bool xr_buffer_materialize(XrValue value, void *dst, size_t size, size_t align,
                                   const struct XrAggregateLayout *layout);

#endif  // XBUFFER_H
