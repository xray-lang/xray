/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_leaf.h - Admission for the numbered synchronous host ABI
 */
#ifndef XNATIVE_HOST_LEAF_H
#define XNATIVE_HOST_LEAF_H
#include "xnative_declaration.h"

static inline bool xr_native_blocking_path_bool(uint32_t callable) {
    switch (callable) {
#define XR_NATIVE_BLOCKING_PATH_BOOL(id, implementation) case id: return true;
#include "xnative_host_leaves.inc.h"
#undef XR_NATIVE_BLOCKING_PATH_BOOL
    default: return false;
    }
}
#endif // XNATIVE_HOST_LEAF_H
