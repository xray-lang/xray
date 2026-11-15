/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * JSON is a compiler prelude namespace, never a Class or a value binding.
 */
#include "../hir/xhir_conversion.h"
static bool source_json_namespace_available(SourceContext *ctx) {
    if(source_native_type_shadowed(ctx,"JSON"))return false;
    /* Alias declarations occupy the ordinary type namespace as well. */
    AstNode *module=ctx->graph->specs[ctx->module].ast;
    for(int i=0;i<module->as.program.count;++i) {
        AstNode *item=module->as.program.statements[i];
        if(!source_request_poll(ctx,item))return false;
        if(item->type==AST_TYPE_ALIAS && source_text_same(ctx,item,item->as.type_alias.name,"JSON"))return false;
    }
    return ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_json_object_type(SourceContext *ctx,XrXirType *output) {
    /* The alias binds the governed constructor at its definition, independently
     * of a user's local declaration named Map at the use site. */
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_MAP,
        .element=XR_XIR_STRING,.result=XR_XIR_JSON_VALUE},output);
}
static bool source_json_path_type(SourceContext *ctx,XrXirType *output) {
    XrXirCallableParameter *members=source_alloc(ctx,2,sizeof(*members));
    if(!members)return false;
    members[0]=(XrXirCallableParameter){.type=XR_XIR_STRING};
    members[1]=(XrXirCallableParameter){.type=XR_XIR_I64};
    XrXirType element=XR_XIR_UNIT;
    bool valid=source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_DATA_UNION,
        .parameters=members,.parameter_count=2},&element);
    source_retire_array(ctx,members);
    return valid && source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=element},output);
}
static bool source_json_type_ref(SourceContext *ctx,const char *name,bool *matched,XrXirType *output) {
    *matched=false;
    bool value=source_text_same(ctx,NULL,name,"JSON.Value");
    bool object=source_text_same(ctx,NULL,name,"JSON.Object");
    bool path=source_text_same(ctx,NULL,name,"JSON.Path");
    bool bare=source_text_same(ctx,NULL,name,"JSON");
    if(!(value||object||path||bare) || !source_json_namespace_available(ctx))return ctx->diagnostic.status==XR_XIR_OK;
    *matched=true;
    if(bare)return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"JSON is a namespace, not a value type");
    if(value){*output=XR_XIR_JSON_VALUE;return true;}
    return object?source_json_object_type(ctx,output):source_json_path_type(ctx,output);
}
static bool source_json_null(SourceContext *ctx,AstNode *site,SourceValue *output) {
    XrXirSourceRange range=source_query_range(ctx,site,NULL);
    XrHirConversionOrigin origin={site->node_id,range.module,range.line,range.column,range.end_line,range.end_column};
    XrXirInstruction instruction={0};const char *reason=NULL;
    XrXirStatus status=xr_hir_json_null_check(origin,true,XR_XIR_JSON_VALUE,ctx->request,&instruction,&reason);
    return status==XR_XIR_OK?source_recipe_record(ctx,instruction,output):source_fail(ctx,site,status,reason);
}
