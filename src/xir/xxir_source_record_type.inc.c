/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_record_type.inc.c - Canonical exact record annotations and identities
 */
static bool source_record_shape_type(SourceContext *ctx,const XrXirRecordField *input,
    uint32_t count,XrXirRecordShape **shape,XrXirType *type) {
    int scope=0;(void)source_type_parameters(ctx,&scope);
    XrXirRecordShapeInput description={&ctx->types,input,count,(uint32_t)scope};
    XrXirStatus status=xr_xir_compile_record_shape_new(&ctx->compile,&description,shape);
    if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,"record fields require unique identifier names and ordinary types");
    XrXirCallableParameter *fields=count ? source_alloc(ctx,count,sizeof(*fields)) : NULL;
    XrXirLiteral *names=count ? source_alloc(ctx,count,sizeof(*names)) : NULL;
    if (count && (!fields || !names)) return false;
    for (uint32_t f=0;f<count;++f) {
        if (!source_work(ctx,NULL)) return false;
        fields[f].type=(*shape)->fields[f].type;names[f]=(*shape)->fields[f].name;
    }
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_RECORD,
        .parameters=fields,.parameter_count=count,.record={names}},type);
}
static bool source_record_type_ref(SourceContext *ctx,XrTypeRef *ref,XrXirType *type) {
    uint32_t count=ref->nchildren;
    if ((count && (!ref->children || !ref->field_names)) || ctx->depth>=128)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"record annotation requires bounded named fields");
    XrXirRecordField *fields=count ? source_alloc(ctx,count,sizeof(*fields)) : NULL;
    if (count && !fields) return false;
    ++ctx->depth;bool okay=true;
    for (uint32_t f=0;f<count && okay;++f) {
        if (ref->field_readonly && ref->field_readonly[f]) {
            okay=source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"record field qualifiers require a checked readonly field contract");break;
        }
        size_t length=0;
        okay=source_text_length(ctx,NULL,ref->field_names[f],&length) && length<=UINT32_MAX &&
            source_type(ctx,ref->children[f],&fields[f].type);
        if (okay) fields[f].name=(XrXirLiteral){ref->field_names[f],(uint32_t)length};
    }
    --ctx->depth;XrXirRecordShape *shape=NULL;
    if (okay) okay=source_record_shape_type(ctx,fields,count,&shape,type);
    xr_xir_compile_record_shape_free(shape);return okay;
}
static bool source_record_field(SourceContext *ctx,AstNode *node,XrXirType type,
    const char *name,uint32_t *index,XrXirType *field_type) {
    const XrXirTypeNode *record=xr_xir_record_signature(&ctx->types,type);size_t length=0;
    if (!record || !source_text_length(ctx,node,name,&length))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"member receiver requires an exact record");
    for (uint32_t f=0;f<record->parameter_count;++f) {
        if (!source_work(ctx,node)) return false;
        XrXirLiteral field=record->record.names[f];
        if (length==field.length && source_span_same(ctx,node,name,field.bytes,field.length)) {
            *index=f;*field_type=record->parameters[f].type;return true;
        }
    }
    return source_fail(ctx,node,XR_XIR_BAD_TYPE,"unknown record field");
}
