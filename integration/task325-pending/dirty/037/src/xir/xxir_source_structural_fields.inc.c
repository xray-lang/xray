/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_structural_fields.inc.c - Declaration-bound readonly field selection
 */
typedef struct SourceStructuralField {
    uint32_t requirement,field;
    XrXirType type;
} SourceStructuralField;
static bool source_structural_field_select(SourceContext *ctx,AstNode *node,
    XrXirType receiver,const char *name,SourceStructuralField *output,bool *found) {
    *found=false;
    uint32_t id=(uint32_t)receiver;
    if (id<XR_XIR_TYPE_PARAMETER_BASE || id>=XR_XIR_TYPE_PARAMETER_LIMIT) return true;
    const XrXirGeneric *generic=&ctx->generics[ctx->function];
    uint32_t parameter=id-XR_XIR_TYPE_PARAMETER_BASE;
    if (parameter>=generic->parameter_count || !generic->constraints)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"structural receiver has no declaration binder");
    const XrXirConstraint *constraint=&generic->constraints[parameter];size_t length=0;
    if (!source_text_length(ctx,node,name,&length)) return false;
    for (uint32_t r=0;r<constraint->structural_count;++r) {
        if (!source_work(ctx,node)) return false;
        const XrXirStructuralRequirement *requirement=&constraint->structural_requirements[r];
        const XrXirTypeNode *shape=xr_xir_record_signature(&ctx->types,requirement->shape);
        if (!shape || requirement->policy!=XR_XIR_STRUCTURAL_READONLY_RECEIVER)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"structural field policy is not admitted");
        for (uint32_t f=0;f<shape->parameter_count;++f) {
            if (!source_work(ctx,node)) return false;
            XrXirLiteral field=shape->record.names[f];
            if (length!=field.length || !source_span_same(ctx,node,name,field.bytes,field.length)) continue;
            if (*found) {
                XrXirStatus status=xr_xir_compile_type_substitution_matches(&ctx->compile,&ctx->types,NULL,0,
                    output->type,shape->parameters[f].type);
                if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"conflicting structural field types");
            } else {*output=(SourceStructuralField){r,f,shape->parameters[f].type};*found=true;}
        }
    }
    return ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_structural_field_value(SourceContext *ctx,SourceValue receiver,
    SourceStructuralField field,SourceValue *output) {
    uint64_t selection=((uint64_t)field.requirement<<32)|field.field;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_FIELD_REQUIREMENT,field.type,
        {receiver.id,0},{0},(int64_t)selection,{0}},output);
}
