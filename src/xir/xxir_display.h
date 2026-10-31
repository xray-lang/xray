/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_display.h - Concrete builtin display qualification
 *
 * KEY CONCEPT:
 *   Display follows closed value types, without invoking user members or
 *   granting access to nominal fields. Each pool node is visited at most once.
 */
#ifndef XXIR_DISPLAY_H
#define XXIR_DISPLAY_H
#include "xxir_types.h"
#include "xxir_map.h"
#include "xxir_receive_control.h"
typedef struct XirDisplayTypes {
    const XrXirTypes *types;
    uint8_t *seen;
    uint32_t *pending,count;
} XirDisplayTypes;
static inline XrXirStatus xir_display_enqueue(XirDisplayTypes *walk,XrXirType type) {
    if (type==XR_XIR_UNIT || type==XR_XIR_BOOL || type==XR_XIR_RUNE || type==XR_XIR_STRING ||
        xr_xir_type_is_number(type)) return XR_XIR_OK;
    if ((uint32_t)type<XR_XIR_CONSTRUCTED_TYPE_BASE ||
        (uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE>=walk->types->count) return XR_XIR_BAD_TYPE;
    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (!walk->seen[index]) {walk->seen[index]=1;walk->pending[walk->count++]=index;}
    return XR_XIR_OK;
}
static inline XrXirStatus xir_display_type(const XrXirCompileContext *compile,const XrXirTypes *types,
    XrXirType type,XrCompileRequestControl *control) {
    if (type==XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
    if (type==XR_XIR_BOOL || type==XR_XIR_RUNE || type==XR_XIR_STRING ||
        xr_xir_type_is_number(type)) return XR_XIR_OK;
    if (!compile || !compile->resources || !types || !types->nodes || !types->count) return XR_XIR_BAD_TYPE;
    XrXirStatus status=xir_receive_poll(control);if (status!=XR_XIR_OK) return status;
    if (sizeof(uint32_t)>SIZE_MAX/(size_t)types->count) return XR_XIR_OVERFLOW;
    uint64_t scratch=(uint64_t)types->count*(1u+sizeof(uint32_t))+sizeof(XirDisplayTypes);
    if (scratch>compile->limits.frame_bytes) return XR_XIR_BUDGET;
    XirDisplayTypes walk={.types=types};uint32_t rows=0;
    walk.seen=xir_type_copy_zero(compile,types->count,1,&status,control);
    if (status==XR_XIR_OK) status=xir_compile_resource_status(xr_compile_resources_alloc(compile->resources,
        (size_t)types->count*sizeof(*walk.pending),(void **)&walk.pending));
    if (status==XR_XIR_OK) status=xir_display_enqueue(&walk,type);
    while(status==XR_XIR_OK && walk.count) {
        status=xir_receive_step(control,&rows);if (status!=XR_XIR_OK) break;
        const XrXirTypeNode *node=&types->nodes[walk.pending[--walk.count]];
        if (node->parameter_span) {status=XR_XIR_BAD_TYPE;break;}
        if (node->kind==XR_XIR_TYPE_ARRAY || node->kind==XR_XIR_TYPE_NULLABLE)
            status=xir_display_enqueue(&walk,node->element);
        else if (node->kind==XR_XIR_TYPE_SET)
            status=xir_display_enqueue(&walk,xr_xir_set_element(types,(XrXirType)(node-types->nodes+XR_XIR_CONSTRUCTED_TYPE_BASE)));
        else if (node->kind==XR_XIR_TYPE_MAP) {
            status=xir_display_enqueue(&walk,xr_xir_map_key(types,(XrXirType)(node-types->nodes+XR_XIR_CONSTRUCTED_TYPE_BASE)));
            if (status==XR_XIR_OK) status=xir_display_enqueue(&walk,xr_xir_map_element(types,(XrXirType)(node-types->nodes+XR_XIR_CONSTRUCTED_TYPE_BASE)));
        } else if (node->kind==XR_XIR_TYPE_TUPLE && node->parameter_count && node->parameters) {
            for (uint32_t p=0;p<node->parameter_count && status==XR_XIR_OK;++p) {
                status=xir_receive_step(control,&rows);
                if (status==XR_XIR_OK) status=xir_display_enqueue(&walk,node->parameters[p].type);
            }
        } else if (node->kind==XR_XIR_TYPE_NOMINAL && types->nominals &&
            node->nominal.declaration<types->nominals->count) {
            const XrXirNominalTable *table=types->nominals;uint32_t d=node->nominal.declaration;
            if (!table->declarations && !table->identities) {status=XR_XIR_BAD_TYPE;break;}
            uint32_t flags=table->declarations?table->declarations[d].flags:table->identities[d].flags;
            uint32_t kind=table->declarations?table->declarations[d].kind:table->identities[d].kind;
            if (flags!=XR_XIR_NOMINAL_RECORD || kind!=XR_XIR_NOMINAL_STRUCT ||
                (node->nominal.field_count && !node->nominal.fields)) status=XR_XIR_BAD_TYPE;
            for (uint32_t f=0;f<node->nominal.field_count && status==XR_XIR_OK;++f) {
                status=xir_receive_step(control,&rows);
                if (status==XR_XIR_OK) status=xir_display_enqueue(&walk,node->nominal.fields[f]);
            }
        } else status=XR_XIR_BAD_TYPE;
    }
    xr_compile_resources_free(walk.pending);xr_compile_resources_free(walk.seen);return status;
}
#endif // XXIR_DISPLAY_H
