/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_fill_publication.h - Observe successful real whole-root publication
 *
 * KEY CONCEPT:
 *   The observer forwards the actual value-path implementation unchanged and
 *   records only successful publication through the watched module Array root.
 */
#ifndef XIR_ARRAY_FILL_PUBLICATION_H
#define XIR_ARRAY_FILL_PUBLICATION_H
static XrXirInstance *fill_watched;
static unsigned fill_publications;
static size_t fill_publication_allocation;
XR_FUNC XrXirCallStatus xr_xir_instance_path_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver,const XrXirValuePath *path,
    const XrXirValue *value,XrXirFaultDetail *fault) {
    XrXirCallStatus status=fill_real_path_write(view,receiver,path,value,fault);
    if(status==XR_XIR_CALL_READY&&fill_watched&&view_instance(view)==fill_watched&&
        receiver->kind==XR_XIR_ROOT_SLOT&&path&&!path->count&&
        xr_xir_type_is_array(xr_xir_compile_type_arena_types(fill_watched->program->arena),receiver->type)) {
        ++fill_publications;fill_publication_allocation=runtime_attempts;
    }
    return status;
}
static void fill_watch(XrXirInstance *instance) {
    fill_watched=instance;fill_publications=0;fill_publication_allocation=0;
}
#endif
