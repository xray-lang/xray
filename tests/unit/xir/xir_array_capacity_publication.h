/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_publication.h - Observe real whole-root reserve publication
 *
 * KEY CONCEPT:
 *   The observer forwards the actual path writer and counts successful writes
 *   only through the watched module Array root.
 */
#ifndef XIR_ARRAY_CAPACITY_PUBLICATION_H
#define XIR_ARRAY_CAPACITY_PUBLICATION_H
static XrXirInstance *capacity_watched;
static unsigned capacity_publications;
static size_t capacity_publication_site;
XR_FUNC XrXirCallStatus xr_xir_instance_path_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver,const XrXirValuePath *path,
    const XrXirValue *value,XrXirFaultDetail *fault) {
    XrXirCallStatus status=capacity_real_path_write(view,receiver,path,value,fault);
    if(status==XR_XIR_CALL_READY&&capacity_watched&&view_instance(view)==capacity_watched&&
        receiver->kind==XR_XIR_ROOT_SLOT&&path&&!path->count&&
        xr_xir_type_is_array(xr_xir_compile_type_arena_types(capacity_watched->program->arena),receiver->type)) {
        ++capacity_publications;capacity_publication_site=runtime_attempts;
    }
    return status;
}
static void capacity_watch(XrXirInstance *instance) {
    capacity_watched=instance;capacity_publications=0;capacity_publication_site=0;
}
#endif // XIR_ARRAY_CAPACITY_PUBLICATION_H
