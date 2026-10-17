/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_binding_projection.h - Current backend wiring after preparation
 */
#ifndef XIR_MIXED_BINDING_PROJECTION_H
#define XIR_MIXED_BINDING_PROJECTION_H
#include "xir/xxir_program_internal.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_value_place.h"
typedef struct XirMixedProjection {
    const XrXirArtifact *artifact;
    const XrXirVmBinding *bindings;
    uint32_t count;
    uint64_t native_mask;
    XrXirCallEntry entries[64];
    uint64_t vm_base[64];
} XirMixedProjection;

/* These local rows describe physical callback wiring and backend state size.
 * They grant no semantic permission; the common receiver independently checks
 * the actual current Input, Final, metadata and canonical layouts. */
static inline XrXirStatus xir_mixed_projection_prepare(const XrXirCompileContext *context,
    const XrXirArtifact *artifact,const XrXirVmBinding *bindings,const XrXirCallEntry *entries,
    uint32_t count,XirMixedProjection *projection) {
    if(!count || count>64 || !bindings || !entries) return XR_XIR_BAD_STRUCTURE;
    XrCompileResourceStatus physical=xr_compile_resources_allocation(context->resources,bindings,
        (uint64_t)count*sizeof(*bindings));
    if(physical!=XR_COMPILE_RESOURCE_OK)
        return physical==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(context,(uint64_t)count*(2*sizeof(*entries)+sizeof(uint64_t)))) return XR_XIR_BUDGET;
    projection->artifact=artifact;projection->bindings=bindings;projection->count=count;projection->native_mask=0;
    for(uint32_t f=0;f<count;++f){
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(artifact,f);
        if(!layout) return XR_XIR_BAD_STRUCTURE;
        uint64_t bytes=(uint64_t)layout->frame_bytes+(uint64_t)layout->outgoing_count*sizeof(XrXirValue)+
            (uint64_t)layout->path_count*sizeof(XrXirValuePathStep);
        if(bytes>entries[f].state_bytes) return XR_XIR_BAD_STRUCTURE;
        projection->entries[f]=entries[f];projection->vm_base[f]=entries[f].state_bytes-bytes;
    }
    return XR_XIR_OK;
}

static inline XrXirStatus xir_mixed_projection_native(const XrXirCompileContext *context,
    XirMixedProjection *projection,uint32_t function,const XrXirCallEntry *entry) {
    if(function>=projection->count) return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(context,sizeof(*entry)+sizeof(projection->native_mask))) return XR_XIR_BUDGET;
    projection->entries[function]=*entry;projection->native_mask|=UINT64_C(1)<<function;
    return XR_XIR_OK;
}

/* Call only after every allocating or releasing preparation operation. No
 * callback follows a successful match before the constructor publishes its
 * code lease. Binding identity never substitutes for the receiving proof. */
static inline XrXirStatus xir_mixed_projection_match(const XrXirCompileContext *context,
    const XrXirProgram *program,const XirMixedProjection *projection) {
    if(!program || program->entry_count!=projection->count || !program->permissions ||
        !program->permissions->input || program->permissions->input->artifact!=projection->artifact)
        return XR_XIR_BAD_STRUCTURE;
    XrCompileResourceStatus physical=xr_compile_resources_allocation(context->resources,
        projection->bindings,(uint64_t)projection->count*sizeof(*projection->bindings));
    if(physical!=XR_COMPILE_RESOURCE_OK)
        return physical==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<projection->count;++f){
        if(!xir_compile_work(context,2*sizeof(XrXirCallEntry)+sizeof(XrXirVmBinding)+sizeof(uint64_t)))
            return XR_XIR_BUDGET;
        const XrXirCallEntry *entry=&program->entries[f],*expected=&projection->entries[f];
        const XrXirVmBinding *binding=&projection->bindings[f];
        if(binding->artifact!=projection->artifact || binding->function!=f ||
            entry->abi_version!=expected->abi_version || entry->resume!=expected->resume ||
            entry->release!=expected->release || entry->environment!=expected->environment ||
            entry->state_bytes!=expected->state_bytes) return XR_XIR_BAD_STRUCTURE;
        if(projection->native_mask&(UINT64_C(1)<<f)) continue;
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(projection->artifact,f);
        if(!layout || entry->environment!=binding) return XR_XIR_BAD_STRUCTURE;
        uint64_t bytes=projection->vm_base[f]+layout->frame_bytes+
            (uint64_t)layout->outgoing_count*sizeof(XrXirValue)+
            (uint64_t)layout->path_count*sizeof(XrXirValuePathStep);
        if(bytes>UINT32_MAX || entry->state_bytes!=bytes) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
#endif // XIR_MIXED_BINDING_PROJECTION_H
