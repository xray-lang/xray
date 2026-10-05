/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_tuple_sendable.inc.c - Bounded conjunction from authentic field facts
 */
#include "xxir_type_scratch_internal.h"
static XrXirStatus tuple_sendable_edge(const XrXirConstraintEnvironment *environment,
    XrXirType type, uint32_t earlier, unsigned char *pending, const XrXirCompileContext *work) {
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    if (type == XR_XIR_UNIT || type == XR_XIR_BOOL || type == XR_XIR_RUNE || xr_xir_type_is_number(type) ||
        type == XR_XIR_STRING) return XR_XIR_OK;
    uint32_t id=(uint32_t)type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
        const XrXirConstraint *fact=constraint_fact(environment,id-XR_XIR_TYPE_PARAMETER_BASE);
        return fact && (constraint_marker_closure(fact->markers) & XR_XIR_CONSTRAINT_SENDABLE) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    }
    const XrXirTypeNode *node=xr_xir_type_node(environment->types,type);
    if (node && node->kind==XR_XIR_TYPE_ATOMIC)
        return id-XR_XIR_CONSTRUCTED_TYPE_BASE < earlier ?
            constraint_atomic(environment,node->element,XR_XIR_CONSTRAINT_ATOMIC_VALUE,work) : XR_XIR_BAD_TYPE;
    if (!node || id-XR_XIR_CONSTRUCTED_TYPE_BASE >= earlier ||
        (node->kind != XR_XIR_TYPE_TUPLE && node->kind != XR_XIR_TYPE_ARRAY &&
         node->kind != XR_XIR_TYPE_NULLABLE)) return XR_XIR_BAD_TYPE;
    return xir_type_pending_mark(work,pending,id-XR_XIR_CONSTRUCTED_TYPE_BASE);
}
static XrXirStatus tuple_sendable(const XrXirConstraintEnvironment *environment,
    XrXirType type, const XrXirCompileContext *work) {
    XrXirStatus status=XR_XIR_OK;
    uint32_t count=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE+1;
    XirTypeScratch scratch={work->resources,NULL,0};
    unsigned char *pending=xir_type_scratch_pending(work,&scratch,count,&status);
    if (pending) status=tuple_sendable_edge(environment,type,count,pending,work);
    uint32_t at=count;
    while (at && status==XR_XIR_OK) {
        uint32_t index=0;bool found=false;
        status=xir_type_pending_next(work,pending,&at,&index,&found);
        if (status!=XR_XIR_OK || !found) break;
        const XrXirTypeNode *node=&environment->types->nodes[index];
        if (node->kind!=XR_XIR_TYPE_TUPLE)
            status=tuple_sendable_edge(environment,node->element,index,pending,work);
        else {
            if (!node->parameter_count || !node->parameters) {status=XR_XIR_BAD_STRUCTURE;break;}
            for (uint32_t p=0; p<node->parameter_count && status==XR_XIR_OK; ++p) {
                if (node->parameters[p].mode) {status=XR_XIR_BAD_TYPE;break;}
                status=tuple_sendable_edge(environment,node->parameters[p].type,index,pending,work);
            }
        }
    }
    xir_type_scratch_free(&scratch);return status;
}
