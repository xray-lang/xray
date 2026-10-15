/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_display_type.h - Charged closed display capability over canonical types
 */
#ifndef XXIR_DISPLAY_TYPE_H
#define XXIR_DISPLAY_TYPE_H
#include "xxir_types.h"
#include "xxir_compile_memory.h"

static inline bool xir_display_scalar(XrXirType type) {
    return type==XR_XIR_BOOL || type==XR_XIR_RUNE || type==XR_XIR_STRING || xr_xir_type_is_number(type);
}
static inline XrXirStatus xir_display_enqueue(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type, unsigned char *seen, uint32_t *queue, uint32_t *count) {
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    if (xir_display_scalar(type)) return XR_XIR_OK;
    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (!types || index>=types->count) return XR_XIR_BAD_TYPE;
    if (!(seen[index/8] & (1u<<(index%8)))) {
        seen[index/8] |= (unsigned char)(1u<<(index%8)); queue[(*count)++]=index;
    }
    return XR_XIR_OK;
}
/* Each reachable descriptor is queued once, including recursive enum/Array
 * graphs. Display depth is a value rule, not an artificial type-depth limit. */
static inline XrXirStatus xir_compile_display_type(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type) {
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    if (xir_display_scalar(type)) return XR_XIR_OK;
    if (!types || !types->count || types->count>XR_XIR_CONSTRUCTED_TYPE_LIMIT-XR_XIR_CONSTRUCTED_TYPE_BASE)
        return XR_XIR_BAD_TYPE;
    size_t queue_bytes=(size_t)types->count*sizeof(uint32_t), seen_bytes=(types->count+7u)/8u;
    XrXirStatus status=XR_XIR_OK;
    unsigned char *scratch=xir_compile_calloc(context,queue_bytes+seen_bytes,1,&status);
    if (!scratch) return status;
    uint32_t *queue=(uint32_t *)scratch, count=0;
    unsigned char *seen=scratch+queue_bytes;
    status=xir_display_enqueue(context,types,type,seen,queue,&count);
    for (uint32_t next=0;next<count && status==XR_XIR_OK;++next) {
        if (!xir_compile_work(context,1)) { status=XR_XIR_BUDGET; break; }
        const XrXirTypeNode *node=&types->nodes[queue[next]];
        if (node->parameter_span) { status=XR_XIR_BAD_TYPE; break; }
        if (node->kind==XR_XIR_TYPE_ARRAY || node->kind==XR_XIR_TYPE_NULLABLE)
            status=xir_display_enqueue(context,types,node->element,seen,queue,&count);
        else if (xr_xir_type_is_enum(types,(XrXirType)(queue[next]+XR_XIR_CONSTRUCTED_TYPE_BASE))) {
            for (uint32_t field=0;field<node->nominal.field_count && status==XR_XIR_OK;++field)
                status=xir_display_enqueue(context,types,node->nominal.fields[field],seen,queue,&count);
        } else status=XR_XIR_BAD_TYPE;
    }
    xr_compile_resources_free(scratch); return status;
}
#endif // XXIR_DISPLAY_TYPE_H
