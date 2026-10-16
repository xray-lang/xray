/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_record.inc.c - Source-ordered exact records and explicit spreads
 */
typedef struct SourceRecordEntry {
    XrXirRecordField field;
    SourceValue value;
    bool explicit_field;
} SourceRecordEntry;
static bool source_record_entry(SourceContext *ctx,AstNode *node,XrXirLiteral name,
    SourceValue value,bool explicit_field,SourceRecordEntry **entries,uint32_t *count,uint32_t *capacity) {
    for (uint32_t f=0;f<*count;++f) {
        if (!source_work(ctx,node)) return false;
        XrXirLiteral previous=(*entries)[f].field.name;
        if (name.length!=previous.length || !source_span_same(ctx,node,name.bytes,previous.bytes,name.length)) continue;
        if (explicit_field && (*entries)[f].explicit_field)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"duplicate explicit record field");
        if (explicit_field || !(*entries)[f].explicit_field) {
            (*entries)[f].field.type=value.type;(*entries)[f].value=value;
        }
        (*entries)[f].explicit_field|=explicit_field;return true;
    }
    if (*count==65536) return source_fail(ctx,node,XR_XIR_BUDGET,"record expanded arity exhausted");
    SourceRecordEntry *grown=source_query_append(ctx,*entries,count,capacity,sizeof(**entries));
    if (!grown) return false;
    *entries=grown;grown[*count-1]=(SourceRecordEntry){{name,value.type},value,explicit_field};return true;
}
static bool source_record_spread(SourceContext *ctx,AstNode *node,SourceRecordEntry **entries,
    uint32_t *count,uint32_t *capacity) {
    if (!node || node->type!=AST_SPREAD_EXPR || !node->as.spread_expr.expr)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"record spread requires one expression");
    SourceValue source;
    if (!source_plan_expression(ctx,node->as.spread_expr.expr,(SourceExpectedType){0},&source)) return false;
    const XrXirTypeNode *found=xr_xir_type_node(&ctx->types,source.type);
    bool record=found && found->kind==XR_XIR_TYPE_RECORD;
    if (!record && !xr_xir_type_is_struct(&ctx->types,source.type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"record spread requires an exact record or named struct");
    XrXirTypeDescription description={0};
    XrXirStatus status=xr_xir_compile_type_description(&ctx->compile,&ctx->types,source.type,&description);
    if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"record spread description failed");
    XrXirTypeNode descriptor=*description.node;description.node=&descriptor;
    if (!record) description.instance=&descriptor.nominal;
    uint32_t fields=description.field_count;
    for (uint32_t f=0;f<fields;++f) {
        XrXirFieldDescription field={0};
        status=xr_xir_compile_description_field(&ctx->compile,&description,f,&field);
        if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"record spread field description failed");
        uint32_t index;XrXirType type;SourceValue value;
        if (!source_struct_field(ctx,node,source.type,field.name.bytes,0,&index,&type) ||
            !source_recipe_record(ctx,(XrXirInstruction){record ? XR_XIR_RECORD_FIELD : XR_XIR_STRUCT_GET,
                type,{source.id,0},{0},index,{0}},&value) ||
            !source_record_entry(ctx,node,field.name,value,false,entries,count,capacity)) return false;
    }
    return true;
}
static bool source_record_literal(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *output) {
    ObjectLiteralNode *literal=&node->as.object_literal;
    if (literal->count<0 || (literal->count && (!literal->keys || !literal->values)))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"record literal entries are missing");
    XrXirType contextual=expected.type;
    while (expected.present && xr_xir_type_is_nullable(&ctx->types,contextual)) {
        if (!source_work(ctx,node)) return false;
        contextual=xr_xir_nullable_element(&ctx->types,contextual);
    }
    bool context=expected.present && xr_xir_type_is_record(&ctx->types,contextual);
    SourceRecordEntry *entries=NULL;uint32_t count=0,capacity=0;
    for (int i=0;i<literal->count;++i) {
        AstNode *key=literal->keys[i],*expression_node=literal->values[i];
        if (!source_work(ctx,expression_node)) return false;
        if (!key) {
            if (!source_record_spread(ctx,expression_node,&entries,&count,&capacity)) return false;
            continue;
        }
        if (key->type!=AST_RECORD_KEY || key->as.literal.string_length>UINT32_MAX)
            return source_fail(ctx,key,XR_XIR_BAD_TYPE,"record literal keys must be identifiers");
        XrXirLiteral name={key->as.literal.raw_value.string_val,(uint32_t)key->as.literal.string_length};
        SourceExpectedType hint={0};
        if (context) {
            uint32_t index;XrXirType type;
            if (!source_record_field(ctx,key,contextual,name.bytes,&index,&type)) return false;
            hint=(SourceExpectedType){type!=XR_XIR_UNIT,type,false,expected.inferred};
        }
        SourceValue value;
        if (!source_plan_expression(ctx,expression_node,hint,&value)) return false;
        if ((!context || expected.inferred) && !source_root_join_record(ctx,&value)) return false;
        if (!source_record_entry(ctx,key,name,value,true,&entries,&count,&capacity)) return false;
    }
    XrXirRecordField *fields=count ? source_alloc(ctx,count,sizeof(*fields)) : NULL;
    SourceValue *ordered=count ? source_recipe_storage(ctx,count,sizeof(*ordered)) : NULL;
    if (count && (!fields || !ordered)) return false;
    for (uint32_t f=0;f<count;++f) {
        if (!source_work(ctx,node)) return false;
        fields[f]=entries[f].field;
    }
    XrXirRecordShape *shape=NULL;XrXirType type;
    bool okay=source_record_shape_type(ctx,fields,count,&shape,&type);
    if (okay) {
        for (uint32_t f=0;f<count;++f) {
            if (!source_work(ctx,node)) {okay=false;break;}
            ordered[shape->source_to_field[f]]=entries[f].value;
        }
    }
    xr_xir_compile_record_shape_free(shape);
    if (!okay) return false;
    uint32_t payloads=0;
    for (uint32_t f=0;f<count;++f) {
        if (!source_work(ctx,node)) return false;
        if (ordered[f].type!=XR_XIR_UNIT) ordered[payloads++]=ordered[f];
    }
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_RECORD_NEW,type,{0},{0},0,{0}},ordered,payloads,output);
}
