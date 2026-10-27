/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_generated_debug.inc.c - Receive sealed literal observations
 *
 * KEY CONCEPT:
 *   Appended bodies retain their actual emitted points after the temporary
 *   writer retires. They do not acquire another function's source statements.
 */
static bool source_generated_debug_receive(SourceContext *owner,const SourceContext *writer,uint32_t first) {
    if (!owner) return true;
    if (owner->compile.resources!=writer->compile.resources ||
        (uint64_t)owner->function_count+owner->generated_debug_count!=first || first>writer->function_count)
        return source_fail(owner,NULL,XR_XIR_BAD_STRUCTURE,"generated debug inventory lost its append order");
    for (uint32_t f=first;f<writer->function_count;++f) {
        const SourceFunction *body=&writer->bodies[f];
        const XrXirFunction *function=&writer->functions[f];
        if (!body->region_sealed || body->count!=function->instruction_count || body->declaration ||
            (body->count && !body->debug_points) || body->debug_binding_count || body->throw_count ||
            body->handler_count || body->route_count)
            return source_fail(owner,NULL,XR_XIR_BAD_STRUCTURE,"generated debug body lost its sealed observations");
        for (uint32_t i=0;i<body->count;++i) {
            if (!((owner)->diagnostic.status == XR_XIR_OK)) return false;
            const XrXirSourceDebugPoint *point=&body->debug_points[i];
            if (point->statement || point->flags || point->environment)
                return source_fail(owner,NULL,XR_XIR_BAD_STRUCTURE,"generated debug body borrowed source identities");
        }
        SourceGeneratedDebug *row=source_alloc(owner,1,sizeof(*row));
        XrXirSourceDebugPoint *points=body->count?source_alloc(owner,body->count,sizeof(*points)):NULL;
        if (!row || (body->count && !points) ||
            !source_copy_bytes(owner,NULL,points,body->debug_points,(size_t)body->count*sizeof(*points))) return false;
        row->function=(XrXirSourceDebugFunction){.function=f,.parameter_count=function->parameter_count,
            .instruction_count=function->instruction_count,.points=points};
        row->next=owner->generated_debug;owner->generated_debug=row;++owner->generated_debug_count;
    }
    return true;
}
