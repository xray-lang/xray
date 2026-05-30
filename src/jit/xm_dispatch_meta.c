/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_dispatch_meta.c - Runtime accessors for generated Xm dispatch metadata
 *
 * KEY CONCEPT:
 *   Provide one compiled instance of the generated dispatch metadata table.
 */

#define XM_DISPATCH_META_IMPL
#include "xm_dispatch_meta_gen.h"
#undef XM_DISPATCH_META_IMPL

#include "xm_dispatch_meta.h"

XR_FUNC const XmDispatchMeta *xm_dispatch_meta_find(XmOp op, XmDispatchBackend backend) {
    if ((uint32_t) op >= XM_OP_COUNT || (uint32_t) backend >= XM_DISPATCH_BACKEND__COUNT)
        return NULL;
    uint32_t idx = (uint32_t) op * XM_DISPATCH_BACKEND__COUNT + (uint32_t) backend;
    if (idx >= XM_DISPATCH_META_COUNT)
        return NULL;
    const XmDispatchMeta *meta = &xm_dispatch_meta[idx];
    if (meta->op != op || meta->backend != backend)
        return NULL;
    return meta;
}

XR_FUNC const char *xm_dispatch_meta_mcinsns(XmOp op, XmDispatchBackend backend) {
    const XmDispatchMeta *meta = xm_dispatch_meta_find(op, backend);
    return meta ? meta->mcinsns : "(missing)";
}
