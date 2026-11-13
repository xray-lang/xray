/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_map.h - Canonical Map owners retained across runtime boundaries
 *
 * KEY CONCEPT:
 *   The actual Map retains its arena and allocation domain. Root bookkeeping
 *   and module storage each account for their own real owning references.
 */
#ifndef XNATIVE_MAP_H
#define XNATIVE_MAP_H
#include "xray_native_map.h"
#include "xnative_value.h"
#include "xir/xxir_type_arena.h"
XR_FUNC XrXirValueStatus xrt_map_types_new(uint64_t limit, XrXirTypeArena **, uint64_t *bytes);
XR_FUNC XrXirValueStatus xrt_map_owned_copy(const XrtMap *, const XrXirTypeArena *, XrtMap **empty);
XR_FUNC XrtMap *xrt_map_root_take(XrtMap *);
XR_FUNC XrtMap *xrt_map_publish_pending(uint32_t source);
XR_FUNC void xrt_map_root_drain(XrtValueRoot *);
#endif // XNATIVE_MAP_H
