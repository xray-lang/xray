/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_boxed.h - Canonical value producers shared by native and bytecode
 */
#ifndef XNATIVE_BOXED_H
#define XNATIVE_BOXED_H
#include "xray_native_boxed.h"
#include "xir/xxir_value.h"
struct XrtValueRoot;
XR_FUNC bool xrt_boxed_type(const XrXirTypeArena *, XrXirType);
XR_FUNC XrtBoxed *xrt_boxed_create(const XrXirTypeArena *, XrXirType,
    const uint64_t *fields, uint32_t count, uint32_t source);
XR_FUNC void xrt_boxed_root_drain(struct XrtValueRoot *);
XR_FUNC XrXirValueStatus xrt_boxed_owned_copy(const XrtBoxed *, const XrXirTypeArena *, XrXirType, XrtBoxed **);
XR_FUNC void xrt_boxed_owned_drop(XrtBoxed **);
#endif // XNATIVE_BOXED_H
