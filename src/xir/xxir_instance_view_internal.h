/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_view_internal.h - Current Instance permission for module views
 *
 * KEY CONCEPT:
 *   Published module storage grants a shared view only to its actual module.
 */
#ifndef XXIR_INSTANCE_VIEW_INTERNAL_H
#define XXIR_INSTANCE_VIEW_INTERNAL_H
#include "xxir_call_internal.h"
#include "xxir_program.h"

XR_FUNC XrXirCallStatus xir_instance_view_module_root(XrXirCallView *view,const XrXirValue *root);
/* Borrowed only by the synchronous query; none of these coordinates grants
 * permission or outlives the actual frame and its admission owner. */
typedef struct XirBufferViewOriginQuery {
    const XrXirCallMetadata *metadata;
    uint32_t target;
    const XrXirValue *arguments;
    uint32_t count;
    const XrXirCellAuthority *authority;
    XrXirValueAdmission *admission;
} XirBufferViewOriginQuery;
/* A missing genuine native/owned body proof returns no root. A present proof
 * independently resolves the current actual receiver and returns one temporary
 * owned Storage observation; callers drop it before leaving this operation. */
XR_FUNC XrXirCallStatus xir_instance_buffer_view_origin(XrXirCallView *view,
    const XirBufferViewOriginQuery *query,XrXirProgram **owner,
    uint32_t *parameter,uint32_t *field,XrXirValue *output);
/* The frame owns the Program returned by origin capture. Terminal observation
 * uses that exact owner and metadata identity, without inventing a callback. */
XR_FUNC XrXirCallStatus xir_instance_buffer_view_recheck(const XrXirProgram *owner,
    const void *context,const XirBufferViewOriginQuery *query,
    uint32_t *parameter,uint32_t *field,XrXirValue *output);
#endif /* XXIR_INSTANCE_VIEW_INTERNAL_H */
