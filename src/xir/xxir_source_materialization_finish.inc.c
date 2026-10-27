/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_materialization_finish.inc.c - Declaration-born literal bodies
 *
 * KEY CONCEPT:
 *   Ordinary function IDs settle before any receiving array can grow.
 *   Nested requests are drained iteratively under the original finite owner.
 */
static bool source_materialization_storage(SourceContext *ctx,uint32_t needed) {
    if (needed<=ctx->function_capacity) return true;
    uint32_t capacity=ctx->function_capacity;
    if (capacity>ctx->compile.limits.functions/2) capacity=ctx->compile.limits.functions;
    else capacity=capacity?capacity*2:1;
    if (capacity<needed || capacity>ctx->compile.limits.functions)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"constant function storage exhausted");
    XrXirFunction *functions=source_alloc(ctx,capacity,sizeof(*functions));
    SourceFunction *bodies=source_alloc(ctx,capacity,sizeof(*bodies));
    XrXirFunctionIdentity *identities=source_alloc(ctx,capacity,sizeof(*identities));
    XrXirGeneric *generics=source_alloc(ctx,capacity,sizeof(*generics));
    if (!functions || !bodies || !identities || !generics) return false;
    if (!source_copy_bytes(ctx,NULL,functions,ctx->functions,(size_t)ctx->function_count*sizeof(*functions)) ||
        !source_copy_bytes(ctx,NULL,bodies,ctx->bodies,(size_t)ctx->function_count*sizeof(*bodies)) ||
        !source_copy_bytes(ctx,NULL,identities,ctx->identities,(size_t)ctx->function_count*sizeof(*identities)) ||
        !source_copy_bytes(ctx,NULL,generics,ctx->generics,(size_t)ctx->function_count*sizeof(*generics))) return false;
    ctx->functions=functions;ctx->bodies=bodies;ctx->identities=identities;ctx->generics=generics;
    ctx->function_capacity=capacity;return true;
}
static bool source_materialization_birth(SourceContext *ctx,const SourceMaterialization *request) {
    SourceName *owner=request->owner;uint32_t f=request->function;
    if (f!=ctx->function_count || !source_materialization_storage(ctx,f+1))
        return source_fail(ctx,request->site,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status,
            "constant function inventory changed");
    /* The current declaration supplies scope and ancestry. The closed literal
     * body needs no open generic binders, even for a concrete generic nominal. */
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    ctx->function=f;ctx->module=request->module;++ctx->function_count;
    char *name=source_alloc(ctx,40,1);
    if (!name || source_format(ctx,name,40,"$materialize%u",f)!=XR_DIAG_OK)
        return source_fail(ctx,request->site,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BUDGET:ctx->diagnostic.status,
            "constant materializer name failed");
    ctx->functions[f]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),NULL,0,request->type,NULL,0,NULL,0,NULL,0, XR_XIR_NEVER};
    ctx->identities[f]=(XrXirFunctionIdentity){.module=request->module,.nominal_owner=request->nominal+1,.method_kind=XR_XIR_MATERIALIZER};
    ctx->bodies[f].module=request->module;
    if (ctx->literal_definition) return true;
    XrXirSourceRange range=ctx->query.declarations[owner->declaration-1].range;
    ctx->bodies[f].node=owner->node;ctx->bodies[f].type_owner=owner->node;
    SourceName declaration={.name=name,.node=owner->node};
    if (!source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,owner->declaration,range)) return false;
    ctx->bodies[f].declaration=declaration.declaration;
    ((XrXirSourceDeclaration *)ctx->query.declarations)[declaration.declaration-1].type=source_query_type(ctx,request->type);
    return true;
}
static bool source_materialization_finish(SourceContext *ctx) {
    if (!ctx->comptime_input && !ctx->instance_constants && !ctx->literal_definition) return true;
    if (ctx->function_count!=ctx->materialization_begin)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"ordinary constant function inventory changed");
    SourceComptimeScope saved=source_comptime_scope(ctx);bool ok=true;
    for (SourceMaterialization *request=ctx->materializations;request;request=request->next) {
        if (!source_materialization_birth(ctx,request)) { ok=false;break; }
        ctx->locals=ctx->scope=NULL;ctx->facts=NULL;ctx->epochs=NULL;ctx->depth=0;
        ctx->active_expression=NULL;ctx->effect_argument=NULL;ctx->loop=NULL;
        ctx->type_scope=(SourceTypeScope){0};ctx->returned=false;ctx->flow_alternatives=0;
        SourceValue value={0};
        if (!source_comptime_constant(ctx,request->site,&request->value,request->type,&value) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{value.id},{0},0,{0}},NULL)) { ok=false;break; }
        ctx->returned=true;
        if (!finish_body(ctx)) { ok=false;break; }
    }
    if (ok && ctx->function_count-ctx->materialization_begin!=ctx->materialization_count)
        ok=source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"constant materializer coverage changed");
    source_comptime_restore(ctx,&saved);return ok;
}
static bool source_materialization_sort(SourceContext *ctx,XrXirMaterializationBinding **rows,uint32_t count) {
    if (count<2) return true;
    XrXirMaterializationBinding *scratch=source_alloc(ctx,count,sizeof(*scratch));
    if (!scratch) return false;
    XrXirMaterializationBinding *input=*rows,*output=scratch;
    for (uint32_t width=1;width<count;) {
        for (uint32_t begin=0;begin<count;) {
            uint32_t middle=begin+(count-begin<width?count-begin:width);
            uint32_t end=middle+(count-middle<width?count-middle:width);
            uint32_t left=begin,right=middle,next=begin;
            while (left<middle || right<end) {
                if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
                bool take=right==end || (left<middle && (input[left].caller<input[right].caller ||
                    (input[left].caller==input[right].caller && input[left].instruction<input[right].instruction)));
                output[next++]=input[take?left++:right++];
            }
            begin=end;
        }
        XrXirMaterializationBinding *swap=input;input=output;output=swap;
        width=width>count/2?count:width*2;
    }
    *rows=input;return true;
}
static bool source_materialization_bindings(SourceContext *ctx,XrXirMaterializationBinding **output,uint32_t *output_count) {
    uint32_t count=ctx->materialization_count,total=count;
    for (SourceLibraryUnit *unit=ctx->library_units;unit;unit=unit->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (total>ctx->compile.limits.instructions || unit->materialization_count>ctx->compile.limits.instructions-total)
            return source_fail(ctx,NULL,XR_XIR_BUDGET,"library literal inventory exhausted");
        total+=unit->materialization_count;
    }
    if (!total) return true;
    SourceMaterialization **requests=source_alloc(ctx,count,sizeof(*requests));
    XrXirMaterializationBinding *rows=source_alloc(ctx,total,sizeof(*rows));
    uint8_t *seen=source_alloc(ctx,count,1);
    if ((count && (!requests || !seen)) || !rows) return false;
    uint32_t next=0;
    for (SourceMaterialization *request=ctx->materializations;request;request=request->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (next==count || request->function!=ctx->materialization_begin+next)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"constant binding inventory changed");
        requests[next++]=request;
    }
    if (next!=count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"constant binding inventory is incomplete");
    next=0;
    for (uint32_t f=0;count && f<ctx->function_count;++f) {
        const XrXirFunction *body=&ctx->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            const XrXirInstruction *op=&body->instructions[i];
            if (op->op!=XR_XIR_CALL || op->immediate<0 || (uint64_t)op->immediate>=ctx->function_count ||
                ctx->identities[op->immediate].method_kind!=XR_XIR_MATERIALIZER) continue;
            if ((uint32_t)op->immediate<ctx->materialization_begin) continue;
            uint32_t index=(uint32_t)op->immediate-ctx->materialization_begin;
            if (index>=count || seen[index] || requests[index]->caller!=f || next==count)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"constant call binding changed");
            seen[index]=1;
            rows[next++]=(XrXirMaterializationBinding){f,i,(uint32_t)op->immediate,requests[index]->nominal};
        }
    }
    if (next!=count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"constant call coverage is incomplete");
    for (SourceLibraryUnit *unit=ctx->library_units;unit;unit=unit->next) {
        size_t bytes=(size_t)unit->materialization_count*sizeof(*rows);
        if (!source_copy_bytes(ctx,NULL,rows+next,unit->materializations,bytes)) return false;
        next+=unit->materialization_count;
    }
    if (next!=total || !source_materialization_sort(ctx,&rows,total)) return false;
    *output=rows;*output_count=total;return true;
}
