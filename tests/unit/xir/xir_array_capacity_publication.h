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
XR_FUNC XrXirCallStatus xr_xir_instance_path_write_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValueReceiver *receiver,const XrXirValuePath *path,
    const XrXirValue *value,XrXirFaultDetail *fault) {
    XrXirCallStatus status=capacity_real_path_write_at(view,instruction,receiver,path,value,fault);
    if(status==XR_XIR_CALL_READY&&capacity_watched&&view_instance(view)==capacity_watched&&
        receiver->kind==XR_XIR_ROOT_CELL&&path&&!path->count&&
        xr_xir_type_is_array(capacity_watched->program->types,receiver->type)) {
        const XrXirDeclarations *decl=capacity_watched->program->declarations;
        for(uint32_t slot=0;slot<decl->slot_count;++slot) {
            const XrXirValue *stored=&capacity_watched->slots[slot];
            if(decl->slots[slot].module==decl->root_module&&stored->type==receiver->value.type&&
                stored->payload==receiver->value.payload) {
                CHECK(xr_xir_type_is_cell(capacity_watched->program->types,(XrXirType)stored->type));
                ++capacity_publications;capacity_publication_site=runtime_attempts;
            }
        }
    }
    return status;
}
static void capacity_watch(XrXirInstance *instance) {
    capacity_watched=instance;capacity_publications=0;capacity_publication_site=0;
}
#endif // XIR_ARRAY_CAPACITY_PUBLICATION_H
