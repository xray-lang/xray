/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_boxed.h - Canonical structural values in synchronous execution
 */
#ifndef XRAY_NATIVE_BOXED_H
#define XRAY_NATIVE_BOXED_H
#include "xray_native_callable.h"
typedef struct XirNominalValue XrtBoxed;
typedef struct XrtBoxedImage {
    const XrtCallableTypeSpec *types;
    uint32_t count;
} XrtBoxedImage;
#ifdef __cplusplus
extern "C" {
#endif
/* The image is immutable for the execution. Each result owns one canonical
 * reference independently of its input handles and guest activation storage.
 * Fields admit String, I64, Bool, F64, Record and managed Nullable carriers. */
XRAY_API XrtBoxed *xrt_boxed_new(const XrtBoxedImage *, uint32_t type,
    const uint64_t *fields, uint32_t count, uint32_t source);
XRAY_API XrtBoxed *xrt_boxed_retain(XrtBoxed *, uint32_t source);
XRAY_API void xrt_boxed_drop(XrtBoxed *, uint32_t source);
XRAY_API uint64_t xrt_boxed_field(const XrtBoxed *, uint32_t field, uint32_t source);
XRAY_API bool xrt_boxed_present(const XrtBoxed *, uint32_t source);
struct XrtModuleState;
XRAY_API void xrt_boxed_state_prepare(struct XrtModuleState *, const XrtBoxedImage *, uint32_t source);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_BOXED_H
