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
static bool source_comptime_name_same(SourceContext *ctx,AstNode *node,XrXirLiteral a,XrXirLiteral b) {
    return source_work_units(ctx,node,(uint64_t)a.length+b.length+1) && a.length==b.length &&
        (!a.length || !memcmp(a.bytes,b.bytes,a.length));
}
static bool source_comptime_nominal_identities(SourceContext *ctx,AstNode *node,const XrXirTypes *source) {
    const XrXirNominalTable *a=source?source->nominals:NULL,*b=ctx->types.nominals;
    if (!a || !b || a->count!=b->count) return false;
    for (uint32_t i=0;i<a->count;++i) {
        if (!source_work_units(ctx,node,2*sizeof(XrXirNominalIdentity))) return false;
        XrXirNominalIdentity x=source_comptime_nominal_identity(a,i),y=source_comptime_nominal_identity(b,i);
        if (x.kind!=y.kind || x.arity!=y.arity || x.exported!=y.exported || x.flags!=y.flags ||
            x.field_count!=y.field_count || x.variant_count!=y.variant_count || x.native.native_id!=y.native.native_id ||
            memcmp(x.native.source_fingerprint,y.native.source_fingerprint,sizeof(x.native.source_fingerprint)) ||
            !source_comptime_name_same(ctx,node,x.module,y.module) || !source_comptime_name_same(ctx,node,x.name,y.name)) return false;
        for (uint32_t f=0;f<x.field_count;++f) {
            if (!source_work_units(ctx,node,2*sizeof(XrXirNominalField))) return false;
            XrXirLiteral an=a->declarations?a->declarations[i].fields[f].name:a->identities[i].fields[f].name;
            XrXirLiteral bn=b->declarations?b->declarations[i].fields[f].name:b->identities[i].fields[f].name;
            uint32_t af=a->declarations?a->declarations[i].fields[f].flags:a->identities[i].fields[f].flags;
            uint32_t bf=b->declarations?b->declarations[i].fields[f].flags:b->identities[i].fields[f].flags;
            if (af!=bf || !source_comptime_name_same(ctx,node,an,bn)) return false;
        }
        for (uint32_t v=0;v<x.variant_count;++v) {
            if (!source_work_units(ctx,node,2*sizeof(XrXirNominalVariant))) return false;
            if (x.variants[v].field_begin!=y.variants[v].field_begin || x.variants[v].field_count!=y.variants[v].field_count ||
                !source_comptime_name_same(ctx,node,x.variants[v].name,y.variants[v].name)) return false;
        }
    }
    return true;
}
static bool source_comptime_nominal_work(SourceContext *ctx,AstNode *node,XrXirType target) {
    XrXirCtfeTypeShape shape={0};XrXirCtfeStatus status=xr_xir_compile_ctfe_type_shape(&ctx->compile,&ctx->types,target,&shape);
    if (status!=XR_XIR_CTFE_OK || shape.work>UINT64_MAX/32) return source_fail(ctx,node,
        status==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET,"comptime nominal receiving is not bounded");
    return source_work_units(ctx,node,shape.work*32);
}
typedef struct SourceComptimeNominalShape { uint32_t count,begin,variant; } SourceComptimeNominalShape;
static bool source_comptime_nominal_fields(SourceContext *ctx,AstNode *node,XrXirType type) {
    if (!source_work_units(ctx,node,sizeof(XrXirTypeNode)+sizeof(XrXirNominalDeclaration))) return false;
    const XrXirTypeNode *found=xr_xir_type_node(&ctx->types,type);
    if (!found || found->kind!=XR_XIR_TYPE_NOMINAL || found->parameter_span || found->nominal.declaration>=ctx->nominals.count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"comptime nominal type is not closed");
    XrXirNominalType nominal=found->nominal;
    const XrXirNominalDeclaration *declaration=&ctx->nominals.declarations[nominal.declaration];
    if (nominal.argument_count!=declaration->parameter_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"comptime nominal arguments changed");
    if (nominal.field_count==declaration->field_count) return true;
    if (nominal.field_count || nominal.fields)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime nominal field inventory is incomplete");
    uint32_t count=declaration->field_count;
    XrXirType *fields=source_alloc(ctx,count,sizeof(*fields));
    if (count && !fields) return false;
    SourceSubstitution substitution={nominal.arguments,nominal.argument_count};
    /* Only current declaration types and actual closed arguments derive the
     * cache. A value arena supplies neither Source fields nor access rights. */
    for (uint32_t f=0;f<count;++f) {
        if (!source_work_units(ctx,node,sizeof(XrXirNominalField)) ||
            !source_substitute(ctx,&substitution,declaration->fields[f].type,0,&fields[f])) return false;
        if (xr_xir_type_span(&ctx->types,fields[f]))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"comptime nominal field remains open");
    }
    /* Substitution may intern types and move the pool. Reacquire by identity. */
    XrXirTypeNode *owned=(XrXirTypeNode *)&ctx->types.nodes[(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE];
    owned->nominal.fields=fields;owned->nominal.field_count=count;return true;
}
static bool source_comptime_nominal_shape(SourceContext *ctx,AstNode *node,const XrXirValue *value,
    XrXirType target,SourceComptimeNominalShape *output) {
    if (!source_comptime_nominal_fields(ctx,node,target) || !source_comptime_nominal_work(ctx,node,target)) return false;
    const XrXirTypeArena *arena=xr_xir_value_arena(value);const XrXirTypes *source=xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *from=xr_xir_type_node(source,(XrXirType)value->type),*to=xr_xir_type_node(&ctx->types,target);
    if (!from || !to || from->kind!=XR_XIR_TYPE_NOMINAL || to->kind!=XR_XIR_TYPE_NOMINAL ||
        from->nominal.declaration!=to->nominal.declaration || from->nominal.field_count!=to->nominal.field_count ||
        !source_comptime_nominal_identities(ctx,node,source))
        return source_fail(ctx,node,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status,"comptime nominal declaration identity changed");
    for (uint32_t f=0;f<to->nominal.field_count;++f)
        if (!source_work_units(ctx,node,2*sizeof(XrXirType)) ||
            !source_comptime_type_matches(ctx,node,source,from->nominal.fields[f],to->nominal.fields[f])) return false;
    *output=(SourceComptimeNominalShape){to->nominal.field_count,0,0};
    if (xr_xir_type_is_enum(&ctx->types,target)) {
        XrXirEnumBorrow borrowed={0};
        if (xr_xir_enum_variant(value,&output->variant)!=XR_XIR_VALUE_OK || xr_xir_enum_borrow(value,&borrowed)!=XR_XIR_VALUE_OK)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime enum is invalid");
        const XrXirNominalTable *table=ctx->types.nominals;XrXirNominalIdentity identity=source_comptime_nominal_identity(table,to->nominal.declaration);
        if (output->variant>=identity.variant_count || identity.variants[output->variant].field_count!=borrowed.field_count)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime enum variant changed");
        output->count=borrowed.field_count;output->begin=identity.variants[output->variant].field_begin;
    }
    return true;
}
static bool source_comptime_nominal_get(SourceContext *ctx,AstNode *node,SourceComptimeTupleFrame *frame,XrXirValue *output) {
    if (!source_comptime_nominal_work(ctx,node,frame->target)) return false;
    XrXirValueStatus status;
    if (frame->enumeration) {
        XrXirEnumBorrow borrowed={0};status=xr_xir_enum_borrow(&frame->value,&borrowed);
        if (status==XR_XIR_VALUE_OK && frame->next<borrowed.field_count) status=xr_xir_value_copy(&borrowed.fields[frame->next],output);
        else status=XR_XIR_VALUE_BAD_ARGUMENT;
    } else {
        const XrXirTypeArena *arena=xr_xir_value_arena(&frame->value);XrXirDomain *domain=NULL;
        status=xr_xir_compile_domain_new(ctx->compile.resources,UINT64_MAX,&domain);
        if (status==XR_XIR_VALUE_OK) {
            XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=UINT64_MAX,.scratch_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES};
            status=xr_xir_struct_get(&frame->value,frame->next,&admission,output);
        }
        xr_xir_domain_drop(domain);
    }
    return status==XR_XIR_VALUE_OK || source_fail(ctx,node,status==XR_XIR_VALUE_OOM?XR_XIR_OUT_OF_MEMORY:
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE,"comptime nominal field receiving failed");
}
