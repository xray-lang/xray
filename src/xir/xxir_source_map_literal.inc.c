/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Written operands and typed provenance feed one strict constructor. */
static bool source_map_bare_key_failure(SourceContext *ctx,AstNode *key) {
    if(!key || key->type!=AST_VARIABLE || !key->as.variable.name || ctx->report ||
        (ctx->diagnostic.status!=XR_XIR_BAD_VALUE && ctx->diagnostic.status!=XR_XIR_BAD_TYPE))return false;
    XrXirStatus allocation=XR_XIR_OK;
    char *hint=source_map_bare_key_hint(ctx,key,&allocation);
    XrFrontendReport *report=NULL;uint32_t id=0;
    XrCompileResourceStatus status=XR_COMPILE_RESOURCE_OK;
    if(allocation==XR_XIR_OK)status=xr_frontend_report_new(ctx->compile.resources,&report);
    const XrModuleSpec *source=&ctx->graph->specs[ctx->module];
    if(allocation==XR_XIR_OK && status==XR_COMPILE_RESOURCE_OK)
        status=xr_frontend_report_source(report,source->source_path,source->source_text,source->source_length,NULL,&id);
    if(allocation==XR_XIR_OK && status==XR_COMPILE_RESOURCE_OK) {
        XrFrontendLabel label={hint,xr_frontend_report_location(report,id,key->line,key->column,NULL)};
        status=xr_frontend_report_add(report,0,ctx->diagnostic.message,
            xr_frontend_report_location(report,id,ctx->diagnostic.line,ctx->diagnostic.column,NULL),&label,1);
    }
    xr_compile_resources_free(hint);
    if(allocation!=XR_XIR_OK || status!=XR_COMPILE_RESOURCE_OK) {
        xr_frontend_report_free(report);
        ctx->diagnostic.status=allocation!=XR_XIR_OK?allocation:xir_compile_resource_status(status);
        snprintf(ctx->diagnostic.message,sizeof(ctx->diagnostic.message),"Map key diagnostic allocation failed");
        return false;
    }
    xr_frontend_report_seal(report,false);ctx->report=report;return false;
}
static bool source_map_literal(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *value) {
    MapLiteralNode *map=&node->as.map_literal;
    if(map->count<0 || map->count>65536 || (map->count && (!map->keys || !map->values)))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Map literal has no finite complete entry list");
    if(expected.present && !xr_xir_type_is_map(&ctx->types,expected.type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Map literal requires a Map context");
    bool known=expected.present;XrXirType key=XR_XIR_UNIT,element=XR_XIR_UNIT;
    if(known) {
        const XrXirTypeNode *type=xr_xir_type_node(&ctx->types,expected.type);key=type->element;element=type->result;
    }
    uint32_t count=(uint32_t)map->count,used=0;
    SourceValue *operands=count?source_alloc(ctx,(size_t)count*2,sizeof(*operands)):NULL;
    XrHirCollectionEntry *entries=count?source_alloc(ctx,count,sizeof(*entries)):NULL;
    if(count && (!operands || !entries))return false;
    for(uint32_t n=0;n<count;++n) {
        if(!source_request_poll(ctx,node))return false;
        bool spread=map->keys[n]==NULL;uint32_t width=spread?1u:2u;
        if(width>65536-used)return source_fail(ctx,node,XR_XIR_BUDGET,"Map literal operands exceed format limit");
        if(spread) {
            AstNode *expression=map->values[n];
            if(!expression || expression->type!=AST_SPREAD_EXPR)
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Map spread has no source expression");
            XrXirType context=XR_XIR_UNIT;
            if(known && !source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_MAP,.element=key,.result=element},&context))return false;
            if(!source_collection_expression(ctx,expression->as.spread_expr.expr,
                (SourceExpectedType){known,context,false,expected.inferred},!expected.present || expected.inferred,&operands[used]))return false;
            entries[n]=(XrHirCollectionEntry){.value=operands[used].type,.spread=true};
            const XrXirTypeNode *source=xr_xir_type_node(&ctx->types,operands[used].type);
            if(source && source->kind==XR_XIR_TYPE_MAP && (!known ||
                (!n && expected.inferred && (key!=source->element || element!=source->result)))) {
                key=source->element;element=source->result;known=true;
                if(expected.inferred)expected.present=false;
            }
        } else {
            if(!map->values[n])return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Map literal entry has no value expression");
            if(!source_collection_expression(ctx,map->keys[n],
                (SourceExpectedType){known,key,false,expected.inferred},!expected.present || expected.inferred,&operands[used]))
                return source_map_bare_key_failure(ctx,map->keys[n]);
            XrXirConstantKey constant={0};
            if(!source_collection_key(ctx,map->keys[n],operands[used],&constant))return false;
            if(!known)key=operands[used].type;
            if(!source_collection_expression(ctx,map->values[n],(SourceExpectedType){known,element,false,expected.inferred},
                !expected.present || expected.inferred,&operands[used+1]))return false;
            if(!known){element=operands[used+1].type;known=true;}
            if(!n && expected.inferred && (key!=operands[used].type || element!=operands[used+1].type)) {
                key=operands[used].type;element=operands[used+1].type;expected.present=false;
            }
            entries[n]=(XrHirCollectionEntry){.key=operands[used].type,.value=operands[used+1].type,.constant=constant};
            if((!expected.present || expected.inferred) &&
                (!source_root_join_record(ctx,&operands[used]) || !source_root_join_record(ctx,&operands[used+1])))return false;
        }
        used+=width;
    }
    XrHirCollection checked={0};XrHirCollectionError error={0};
    XrHirCollectionInput input={.kind=XR_HIR_COLLECTION_MAP,.expected=expected.type,.has_expected=expected.present,.entries=entries,.count=count};
    XrXirStatus status=xr_hir_collection_check(&ctx->compile,&ctx->types,&input,ctx->request,&checked,&error);
    if(status==XR_XIR_BAD_TYPE && error.kind==XR_HIR_COLLECTION_DUPLICATE)return source_collection_duplicate(ctx,node,entries,error);
    if(status==XR_XIR_BAD_TYPE && (!expected.present || expected.inferred) &&
        (error.kind==XR_HIR_COLLECTION_KEY_TYPE || error.kind==XR_HIR_COLLECTION_VALUE_TYPE))
        return source_collection_type_failure(ctx,node,&input,error);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"Map literal requires one exact key and value type and Map spread sources");
    if(checked.operands!=used)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Map literal checked operands changed");
    XrXirType type=XR_XIR_UNIT;
    if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_MAP,.element=checked.key,.result=checked.value},&type))return false;
    uint32_t declaration=0,ordinal=0;
    const XrNativeTypeDeclaration *native=source_hash_native(ctx,type);
    return native && source_native_hash_declaration(ctx,native,&declaration) && source_map_literal_metadata(ctx,node,&ordinal) &&
        source_collection_construct(ctx,node,&input,type,operands,used,ordinal,value) &&
        source_query_target_token_reference(ctx,node,declaration,XR_XIR_SOURCE_TYPE_USE);
}
