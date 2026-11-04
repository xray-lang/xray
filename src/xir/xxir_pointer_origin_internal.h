/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_pointer_origin_internal.h - Runtime-only external payload origins
 */
#ifndef XXIR_POINTER_ORIGIN_INTERNAL_H
#define XXIR_POINTER_ORIGIN_INTERNAL_H
#include "xxir_pointer.h"
typedef struct XirPointerOrigin XirPointerOrigin;
/* The owner authenticates its actual payload and keeps this stable reference
 * until retirement. Values retain only this origin, never the external payload.
 * An invalidated record is never reactivated, even if an address is reused. */
XR_FUNC XrXirValueStatus xir_pointer_origin_create(XrXirDomain *domain,XirPointerOrigin **output);
XR_FUNC void xir_pointer_origin_invalidate(XirPointerOrigin **owner);
/* Only an exact readonly Ptr<u8> receives an external address. The caller's
 * Root/declaration/loader checks remain mandatory; this grants no FFI call. */
XR_FUNC XrXirValueStatus xir_pointer_origin_readonly(XirPointerOrigin *origin,
    const XrXirTypeArena *arena,XrXirType type,uintptr_t address,XrXirValue *output);
#endif // XXIR_POINTER_ORIGIN_INTERNAL_H
