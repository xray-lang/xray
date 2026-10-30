/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_nominal.inc.c - Declaration-bound ordinary constant recipes
 *
 * KEY CONCEPT:
 *   Both constructions share the immutable Source graph. Declaration order,
 *   complete owned identities and closed field types must agree across pools.
 */
#include "xxir_struct.h"
#include "xxir_enum.h"
static XrXirNominalIdentity source_comptime_nominal_identity(const XrXirNominalTable *table,uint32_t index) {
    if (!table->declarations) return table->identities[index];
    const XrXirNominalDeclaration *d=&table->declarations[index];
    return (XrXirNominalIdentity){d->module,d->name,d->exported,d->parameter_count,NULL,d->field_count,d->kind,d->variants,d->variant_count,d->flags,d->native};
}
static bool source_comptime_name_same(SourceContext *ctx,XrXirLiteral a,XrXirLiteral b) {
    return a.length==b.length&&source_span_same(ctx,NULL,a.bytes,b.bytes,a.length);
}
static bool source_comptime_nominal_identities(SourceContext *ctx,const XrXirTypes *source) {
    const XrXirNominalTable *a=source?source->nominals:NULL,*b=ctx->types.nominals;
    if (!a || !b || a->count!=b->count) return false;
    for (uint32_t i=0;i<a->count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        XrXirNominalIdentity x=source_comptime_nominal_identity(a,i),y=source_comptime_nominal_identity(b,i);
        if (x.kind!=y.kind || x.arity!=y.arity || x.exported!=y.exported || x.flags!=y.flags ||
            x.field_count!=y.field_count || x.variant_count!=y.variant_count || x.native.native_id!=y.native.native_id ||
            memcmp(x.native.source_fingerprint,y.native.source_fingerprint,sizeof(x.native.source_fingerprint)) ||
            !source_comptime_name_same(ctx,x.module,y.module) || !source_comptime_name_same(ctx,x.name,y.name)) return false;
        for (uint32_t f=0;f<x.field_count;++f) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            XrXirLiteral an=a->declarations?a->declarations[i].fields[f].name:a->identities[i].fields[f].name;
            XrXirLiteral bn=b->declarations?b->declarations[i].fields[f].name:b->identities[i].fields[f].name;
            uint32_t af=a->declarations?a->declarations[i].fields[f].flags:a->identities[i].fields[f].flags;
            uint32_t bf=b->declarations?b->declarations[i].fields[f].flags:b->identities[i].fields[f].flags;
            if (af!=bf || !source_comptime_name_same(ctx,an,bn)) return false;
        }
        for (uint32_t v=0;v<x.variant_count;++v) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if (x.variants[v].field_begin!=y.variants[v].field_begin || x.variants[v].field_count!=y.variants[v].field_count ||
                !source_comptime_name_same(ctx,x.variants[v].name,y.variants[v].name)) return false;
        }
    }
    return true;
}
static bool source_comptime_nominal_admit(SourceContext *ctx,AstNode *node,XrXirType target) {
    XrXirCtfeStatus status=xr_xir_compile_ctfe_type_admit(&ctx->compile,&ctx->types,target);
    return status==XR_XIR_CTFE_OK || source_comptime_failure(ctx,node,status);
}

typedef struct SourceComptimeNominalShape { uint32_t count,begin,variant; } SourceComptimeNominalShape;
static bool source_comptime_nominal_fields(SourceContext *ctx,AstNode *node,XrXirType type) {
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    const XrXirTypeNode *found=xr_xir_type_node(&ctx->types,type);
    if (!found || found->kind!=XR_XIR_TYPE_NOMINAL || found->parameter_span || found->nominal.declaration>=ctx->nominals.count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"comptime nominal type is not closed");
    XrXirNominalType nominal=found->nominal;XrHirNominalView shape={0};
    XrXirStatus status=xr_hir_nominal_view(&ctx->types,type,ctx->request,&shape,NULL);
    if(!source_hir_nominal_status(ctx,node,status,"comptime nominal arguments changed"))return false;
    if (nominal.field_count==shape.field_count) return true;
    if (nominal.field_count || nominal.fields)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime nominal field inventory is incomplete");
    uint32_t count=shape.field_count;
    XrXirType *fields=source_alloc(ctx,count,sizeof(*fields));
    if (count && !fields) return false;
    XrHirNominalPool pool=source_hir_nominal_pool(ctx);
    /* Only current declaration types and actual closed arguments derive the
     * cache. A value arena supplies neither Source fields nor access rights. */
    for (uint32_t f=0;f<count;++f) {
        status=xr_hir_nominal_field_type(&pool,type,f,&fields[f],NULL);
        if(!source_hir_nominal_status(ctx,node,status,"comptime nominal field receiving failed"))return false;
        if (xr_xir_type_span(&ctx->types,fields[f]))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"comptime nominal field remains open");
    }
    /* Substitution may intern types and move the pool. Reacquire by identity. */
    XrXirTypeNode *owned=(XrXirTypeNode *)&ctx->types.nodes[(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE];
    owned->nominal.fields=fields;owned->nominal.field_count=count;return true;
}
static bool source_comptime_nominal_shape(SourceContext *ctx,AstNode *node,const XrXirValue *value,
    XrXirType target,SourceComptimeNominalShape *output) {
    if (!source_comptime_nominal_fields(ctx,node,target) || !source_comptime_nominal_admit(ctx,node,target)) return false;
    const XrXirTypeArena *arena=xr_xir_value_arena(value);const XrXirTypes *source=xr_xir_compile_type_arena_types(arena);
    XrHirNominalView received={0},expected={0};
    XrXirStatus status=xr_hir_nominal_view(source,(XrXirType)value->type,ctx->request,&received,NULL);
    if(!source_hir_nominal_status(ctx,node,status,"comptime nominal source shape receiving failed"))return false;
    status=xr_hir_nominal_view(&ctx->types,target,ctx->request,&expected,NULL);
    if(!source_hir_nominal_status(ctx,node,status,"comptime nominal target shape receiving failed"))return false;
    const XrXirTypeNode *from=xr_xir_type_node(source,(XrXirType)value->type),*to=xr_xir_type_node(&ctx->types,target);
    if (!from || !to || from->kind!=XR_XIR_TYPE_NOMINAL || to->kind!=XR_XIR_TYPE_NOMINAL ||
        received.declaration!=expected.declaration || received.field_count!=expected.field_count ||
        from->nominal.field_count!=received.field_count || to->nominal.field_count!=expected.field_count ||
        !source_comptime_nominal_identities(ctx,source))
        return source_fail(ctx,node,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status,"comptime nominal declaration identity changed");
    for (uint32_t f=0;f<to->nominal.field_count;++f)
        if (!((ctx)->diagnostic.status == XR_XIR_OK) ||
            !source_comptime_type_matches(ctx,node,source,from->nominal.fields[f],to->nominal.fields[f])) return false;
    *output=(SourceComptimeNominalShape){to->nominal.field_count,0,0};
    if (expected.kind==XR_XIR_NOMINAL_ENUM) {
        XrXirEnumBorrow borrowed={0};
        if (xr_xir_enum_variant(value,&output->variant)!=XR_XIR_VALUE_OK || xr_xir_enum_borrow(value,&borrowed)!=XR_XIR_VALUE_OK)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime enum is invalid");
        if(output->variant>=expected.variant_count)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime enum variant changed");
        XrXirNominalVariant variant={0};status=xr_hir_nominal_variant(&ctx->types,target,output->variant,ctx->request,&variant,NULL);
        if(!source_hir_nominal_status(ctx,node,status,"comptime enum variant receiving failed"))return false;
        if (variant.field_count!=borrowed.field_count)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime enum variant changed");
        output->count=borrowed.field_count;output->begin=variant.field_begin;
    }
    return true;
}
static bool source_comptime_nominal_get(SourceContext *ctx,AstNode *node,SourceComptimeTupleFrame *frame,XrXirValue *output) {
    if (!source_comptime_nominal_admit(ctx,node,frame->target)) return false;
    XrXirValueStatus status;
    if (frame->enumeration) {
        XrXirEnumBorrow borrowed={0};status=xr_xir_enum_borrow(&frame->value,&borrowed);
        if (status==XR_XIR_VALUE_OK && frame->next<borrowed.field_count) status=xr_xir_value_copy(&borrowed.fields[frame->next],output);
        else status=XR_XIR_VALUE_BAD_ARGUMENT;
    } else {
        XrXirValueAdmission admission={0};
        if (!source_comptime_observation(ctx,node,&frame->value,&admission)) return false;
        status=xr_xir_struct_observe(&frame->value,frame->next,&admission,output);
    }
    return status==XR_XIR_VALUE_OK || source_fail(ctx,node,source_comptime_value_status(status),"comptime nominal field receiving failed");
}
