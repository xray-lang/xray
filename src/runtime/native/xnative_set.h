/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_set.h - Canonical Set type and owner receipts
 *
 * KEY CONCEPT:
 *   Set values retain their canonical type owner across execution roots.
 */
#ifndef XNATIVE_SET_H
#define XNATIVE_SET_H
#include "xray_native_set.h"
#include "xnative_value.h"
#include "xir/xxir_type_arena.h"
XR_FUNC XrXirValueStatus xrt_set_types_new(uint64_t,XrXirTypeArena **,uint64_t *);
XR_FUNC const XrXirTypeArena *xrt_set_root_types(uint32_t);
XR_FUNC XrXirValueStatus xrt_set_root_types_ensure(const XrXirTypeArena **);
XR_FUNC XrXirValueStatus xrt_set_root_bind_types(const XrXirTypeArena *);
XR_FUNC XrXirValueStatus xrt_set_owned_copy(const XrtSet *,const XrXirTypeArena *,XrtSet **);
XR_FUNC XrtSet *xrt_set_root_take(XrtSet *);
XR_FUNC XrtSet *xrt_set_publish_pending(uint32_t);
XR_FUNC void xrt_set_root_drain(XrtValueRoot *);
#endif // XNATIVE_SET_H
