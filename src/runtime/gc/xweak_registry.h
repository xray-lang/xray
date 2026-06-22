/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xweak_registry.h - Isolate-local weak container registry.
 */

#ifndef XWEAK_REGISTRY_H
#define XWEAK_REGISTRY_H

#include "../../base/xdefs.h"
#include "xgc_header.h"

struct XrayIsolate;
struct XrCoroHeap;
struct XrMap;
struct XrSet;

XR_FUNC void xr_weak_registry_register_map(struct XrayIsolate *isolate, struct XrMap *map);
XR_FUNC void xr_weak_registry_unregister_map(struct XrayIsolate *isolate, struct XrMap *map);
XR_FUNC void xr_weak_registry_register_set(struct XrayIsolate *isolate, struct XrSet *set);
XR_FUNC void xr_weak_registry_unregister_set(struct XrayIsolate *isolate, struct XrSet *set);
XR_FUNC void xr_weak_registry_target_dying(struct XrayIsolate *isolate, XrObjHeader *target,
                                           struct XrCoroHeap *owner_heap);
XR_FUNC void xr_weak_registry_destroy(struct XrayIsolate *isolate);

#endif  // XWEAK_REGISTRY_H
