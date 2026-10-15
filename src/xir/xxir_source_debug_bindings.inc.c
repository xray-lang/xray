/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_debug_bindings.inc.c - Persistent lexical observations at emission
 */
static bool source_debug_environment(SourceContext *ctx,uint32_t *output) {
    uint32_t count=0,parent=0;
    for (SourceName *name=ctx->locals;name;name=name->next) {
        if (!source_work_units(ctx,NULL,sizeof(*name))) return false;
        if (!source_local_name(name) || !name->declaration || !source_name_ready(name)) continue;
        if (count==ctx->debug_name_capacity) {
            if (count>UINT32_MAX/2) return source_fail(ctx,NULL,XR_XIR_BUDGET,"source debug scope exhausted");
            uint32_t capacity=count ? count*2 : 16;
            SourceName **names=source_alloc(ctx,capacity,sizeof(*names));
            if (!names || !source_copy_bytes(ctx,NULL,names,ctx->debug_names,(size_t)count*sizeof(*names))) return false;
            ctx->debug_names=names;ctx->debug_name_capacity=capacity;
        }
        ctx->debug_names[count++]=name;
    }
    SourceFunction *body=&ctx->bodies[ctx->function];
    while (count) {
        if (!source_work_units(ctx,NULL,sizeof(XrXirSourceDebugBinding))) return false;
        SourceName *name=ctx->debug_names[--count];
        uint32_t flags=name->kind==SOURCE_UNIT_LOCAL ? XR_XIR_SOURCE_DEBUG_UNIT :
            name->mutable && !name->construction ? XR_XIR_SOURCE_DEBUG_CELL : 0;
        XrXirSourceDebugBinding binding={name->declaration,
            flags & XR_XIR_SOURCE_DEBUG_UNIT ? UINT32_MAX : name->index,parent,flags};
        uint32_t cached=name->debug_binding;
        if (name->debug_function==ctx->function && cached && cached<=body->debug_binding_count) {
            const XrXirSourceDebugBinding *previous=&body->debug_bindings[cached-1];
            if (previous->declaration==binding.declaration && previous->value==binding.value &&
                previous->parent==binding.parent && previous->flags==binding.flags) { parent=cached;continue; }
        }
        XrXirSourceDebugBinding *bindings=source_query_append(ctx,body->debug_bindings,
            &body->debug_binding_count,&body->debug_binding_capacity,sizeof(*bindings));
        if (!bindings) return false;
        body->debug_bindings=bindings;bindings[body->debug_binding_count-1]=binding;
        name->debug_function=ctx->function;name->debug_binding=body->debug_binding_count;parent=name->debug_binding;
    }
    *output=parent;return true;
}
