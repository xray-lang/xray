/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_vm.h - VM-facing Map adapters
 */

#ifndef XMAP_VM_H
#define XMAP_VM_H

#include "xmap.h"

struct XrClosure;
struct XrIterator;
struct XrVMRuntime;

XR_FUNC struct XrIterator *xr_map_entries_iterator(struct XrVMRuntime *iso, XrMap *map);
XR_FUNC void xr_map_foreach(struct XrVMRuntime *iso, XrMap *map, struct XrClosure *callback);

#endif  // XMAP_VM_H
