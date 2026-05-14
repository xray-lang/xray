/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_dict.c - Name-keyed top-level binding store
 */

#include "xglobal_dict.h"
#include "object/xmap.h"
#include "object/xstring.h"
#include "../base/xchecks.h"

void xr_global_dict_init(XrGlobalDict *gd, struct XrCoroutine *coro) {
    XR_DCHECK(gd != NULL, "xr_global_dict_init: NULL dict");
    XR_DCHECK(coro != NULL, "xr_global_dict_init: NULL coro (need a GC heap host)");
    gd->map = xr_map_with_capacity(coro, 16);
    XR_CHECK(gd->map != NULL, "xr_global_dict_init: map allocation failed");
}

void xr_global_dict_destroy(XrGlobalDict *gd) {
    if (!gd)
        return;
    /* The XrMap is GC-owned and will be reclaimed on isolate teardown.
     * Clearing the pointer makes any post-teardown access fault loudly
     * instead of silently reading freed memory. */
    gd->map = NULL;
}

XrValue xr_global_dict_get(XrGlobalDict *gd, XrString *name) {
    XR_DCHECK(gd != NULL, "xr_global_dict_get: NULL dict");
    XR_DCHECK(gd->map != NULL, "xr_global_dict_get: dict not initialized");
    XR_DCHECK(name != NULL, "xr_global_dict_get: NULL name");
    XrValue out;
    bool found;
    XR_MAP_GET_STRING_FAST(gd->map, name, out, found);
    return found ? out : xr_null();
}

void xr_global_dict_set(XrGlobalDict *gd, XrString *name, XrValue value) {
    XR_DCHECK(gd != NULL, "xr_global_dict_set: NULL dict");
    XR_DCHECK(gd->map != NULL, "xr_global_dict_set: dict not initialized");
    XR_DCHECK(name != NULL, "xr_global_dict_set: NULL name");
    XrValue key_val = xr_string_value(name);
    XR_MAP_SET_STRING_FAST(gd->map, name, key_val, value);
}

bool xr_global_dict_has(XrGlobalDict *gd, XrString *name) {
    XR_DCHECK(gd != NULL, "xr_global_dict_has: NULL dict");
    XR_DCHECK(gd->map != NULL, "xr_global_dict_has: dict not initialized");
    XR_DCHECK(name != NULL, "xr_global_dict_has: NULL name");
    return xr_map_find_string_fast(gd->map, name) != NULL;
}

uint32_t xr_global_dict_count(XrGlobalDict *gd) {
    if (!gd || !gd->map)
        return 0;
    return gd->map->count;
}

void xr_global_dict_iter(XrGlobalDict *gd, XrGlobalDictVisitor visit, void *ud) {
    XR_DCHECK(gd != NULL, "xr_global_dict_iter: NULL dict");
    XR_DCHECK(visit != NULL, "xr_global_dict_iter: NULL visitor");
    if (!gd->map || (gd->map->flags & XR_MAP_FLAG_DUMMY))
        return;
    uint32_t n = xr_map_sizenode(gd->map);
    for (uint32_t i = 0; i < n; i++) {
        XrMapNode *node = xr_map_node(gd->map, i);
        if (XR_MAP_NODE_EMPTY(node))
            continue;
        if (!XR_IS_STRING(node->key))
            continue; /* defensive: dict only ever holds string keys */
        visit(XR_TO_STRING(node->key), &node->value, ud);
    }
}
