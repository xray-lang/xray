/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * One heterogeneous-literal error retains the first typed source occurrence. */
static bool source_collection_type_text(SourceContext *ctx,XrValueFormatSink sink,XrXirType type,uint32_t depth) {
    if(depth>=128 || !source_request_poll(ctx,NULL))return false;
    const char *scalar=source_scalar_type_name(type);
    if(scalar)return xr_value_format_text(sink,scalar)!=0;
    if(type==XR_XIR_ERROR)return xr_value_format_text(sink,"Error")!=0;
    if(type==XR_XIR_NEVER)return xr_value_format_text(sink,"Never")!=0;
    if(type==XR_XIR_BUFFER_STORAGE)return xr_value_format_text(sink,"__BufferStorage")!=0;
    if((uint32_t)type>=XR_XIR_TYPE_PARAMETER_BASE && (uint32_t)type<XR_XIR_TYPE_PARAMETER_LIMIT) {
        int count=0;XrGenericParam **parameters=source_type_parameters(ctx,&count);uint32_t index=(uint32_t)type-XR_XIR_TYPE_PARAMETER_BASE;
        if(index<(uint32_t)count)return xr_value_format_text(sink,parameters[index]->name)!=0;
        char text[32];int length=snprintf(text,sizeof(text),"T%u",index);
        return length>0 && (size_t)length<sizeof(text) && xr_value_format_write(sink,text,(size_t)length);
    }
    const XrXirTypeNode *borrow=xr_xir_type_node(&ctx->types,type);if(!borrow)return false;
    XrXirTypeNode node=*borrow;
    if(node.kind==XR_XIR_TYPE_NOMINAL) {
        if(node.nominal.declaration>=ctx->nominals.count)return false;
        XrXirLiteral name=ctx->nominals.declarations[node.nominal.declaration].name;
        if(!xr_value_format_write(sink,name.bytes,name.length))return false;
        if(!node.nominal.argument_count)return true;
        if(!xr_value_format_text(sink,"<"))return false;
        for(uint32_t n=0;n<node.nominal.argument_count;++n)
            if((n && !xr_value_format_text(sink,", ")) || !source_collection_type_text(ctx,sink,node.nominal.arguments[n],depth+1))return false;
        return xr_value_format_text(sink,">")!=0;
    }
    if(node.kind==XR_XIR_TYPE_FIXED_ARRAY) {
        char text[40];int length=snprintf(text,sizeof(text),"[u8; %u]",node.parameter_count);
        return length>0 && (size_t)length<sizeof(text) && xr_value_format_write(sink,text,(size_t)length);
    }
    if(node.kind==XR_XIR_TYPE_NULLABLE)return source_collection_type_text(ctx,sink,node.element,depth+1) && xr_value_format_text(sink,"?");
    if(node.kind==XR_XIR_TYPE_TUPLE || node.kind==XR_XIR_TYPE_UNION || node.kind==XR_XIR_TYPE_DATA_UNION || node.kind==XR_XIR_TYPE_CALLABLE) {
        bool union_type=node.kind==XR_XIR_TYPE_UNION || node.kind==XR_XIR_TYPE_DATA_UNION;
        if(!union_type && !xr_value_format_text(sink,"("))return false;
        for(uint32_t n=0;n<node.parameter_count;++n) {
            if(n && !xr_value_format_text(sink,union_type?" | ":", "))return false;
            if(!source_collection_type_text(ctx,sink,node.parameters[n].type,depth+1))return false;
        }
        if(!union_type && !xr_value_format_text(sink,")"))return false;
        return node.kind!=XR_XIR_TYPE_CALLABLE || (xr_value_format_text(sink," -> ") && source_collection_type_text(ctx,sink,node.result,depth+1));
    }
    const char *name=node.kind==XR_XIR_TYPE_ARRAY?"Array":node.kind==XR_XIR_TYPE_MAP?"Map":node.kind==XR_XIR_TYPE_SET?"Set":
        node.kind==XR_XIR_TYPE_SLICE?"Slice":node.kind==XR_XIR_TYPE_MUT_SLICE?"MutSlice":node.kind==XR_XIR_TYPE_ATOMIC?"Atomic":
        node.kind==XR_XIR_TYPE_TASK?"Task":node.kind==XR_XIR_TYPE_ITERATOR?"Iterator":node.kind==XR_XIR_TYPE_CHANNEL?"Channel":
        node.kind==XR_XIR_TYPE_PTR?"Ptr":node.kind==XR_XIR_TYPE_MUT_PTR?"MutPtr":node.kind==XR_XIR_TYPE_CELL?"Cell":NULL;
    if(!name || !xr_value_format_text(sink,name) || !xr_value_format_text(sink,"<") ||
        !source_collection_type_text(ctx,sink,node.element,depth+1))return false;
    if(node.kind==XR_XIR_TYPE_MAP || node.kind==XR_XIR_TYPE_TASK || node.kind==XR_XIR_TYPE_ITERATOR)
        if(!xr_value_format_text(sink,", ") || !source_collection_type_text(ctx,sink,node.result,depth+1))return false;
    return xr_value_format_text(sink,">")!=0;
}
static bool source_collection_types_text(SourceContext *ctx,XrValueFormatSink sink,bool map,bool key,bool note,XrHirCollectionError error) {
    const char *part=map?(key?"keys":"values"):"elements";
    if(!note)return xr_value_format_text(sink,"E0326: ") && xr_value_format_text(sink,map?"Map":"Array") &&
        xr_value_format_text(sink," literal ") && xr_value_format_text(sink,part) && xr_value_format_text(sink," have different types: ") &&
        source_collection_type_text(ctx,sink,error.expected,0) && xr_value_format_text(sink," and ") && source_collection_type_text(ctx,sink,error.actual,0);
    return xr_value_format_text(sink,key?"the first key is ":map?"the first value is ":"the first element is ") &&
        source_collection_type_text(ctx,sink,error.expected,0) && xr_value_format_text(sink,map?"; without a declared Map type, every ":"; without a declared Array type, every ") &&
        xr_value_format_text(sink,key?"key":map?"value":"element") && xr_value_format_text(sink," must be ") && source_collection_type_text(ctx,sink,error.expected,0);
}
#include "xxir_source_collection_hint.inc.c"
/* A contextual literal can retarget its own elements. A stored collection
 * cannot acquire an element conversion merely because it is spread. */
static bool source_collection_literal_retargetable(SourceContext *ctx,AstNode *node,bool map,uint32_t depth) {
    if(!source_request_poll(ctx,node))return false;
    if(depth>=128)return source_fail(ctx,node,XR_XIR_BUDGET,"collection repair syntax depth exhausted");
    if(node && node->type==AST_GROUPING)
        return source_collection_literal_retargetable(ctx,node->as.grouping,map,depth+1);
    if(!node || node->type!=(map?AST_MAP_LITERAL:AST_ARRAY_LITERAL))return false;
    if(!map && node->as.array_literal.is_repeat)return false;
    uint32_t count=(uint32_t)(map?node->as.map_literal.count:node->as.array_literal.count);
    AstNode **items=map?node->as.map_literal.values:node->as.array_literal.elements;
    for(uint32_t n=0;n<count;++n) {
        if(!source_request_poll(ctx,node))return false;
        AstNode *item=items[n];
        if(item && item->type==AST_SPREAD_EXPR &&
            !source_collection_literal_retargetable(ctx,item->as.spread_expr.expr,map,depth+1))return false;
    }
    return true;
}
static bool source_collection_spread_retargetable(SourceContext *ctx,AstNode *node,bool map,uint32_t ordinal) {
    AstNode *item=map?node->as.map_literal.values[ordinal]:node->as.array_literal.elements[ordinal];
    return item && item->type==AST_SPREAD_EXPR &&
        source_collection_literal_retargetable(ctx,item->as.spread_expr.expr,map,0);
}
#include "xxir_source_collection_union_hint.inc.c"
#include "xxir_source_collection_json_hint.inc.c"
static bool source_collection_type_failure(SourceContext *ctx,AstNode *node,const XrHirCollectionInput *input,XrHirCollectionError error) {
    bool map=input->kind==XR_HIR_COLLECTION_MAP,key=error.kind==XR_HIR_COLLECTION_KEY_TYPE;
    if(error.first>=input->count || error.second>=input->count)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"collection type error has no source ordinal");
    SourceCollectionHint hint={0};if(!source_collection_hint_prepare(ctx,node,input,error,&hint))return false;
    SourceCollectionUnionHint union_hint={0};
    if(!key && !source_collection_union_hint(ctx,node,input,&union_hint))return false;
    bool json_hint=false;
    if(!key && !source_collection_json_hint(ctx,node,input,&json_hint))return false;
    bool shape_hint=hint.record || hint.tuple || key;
    uint32_t json_index=shape_hint?3u:2u,union_index=json_index+(json_hint?1u:0u);
    uint32_t text_count=union_index+(union_hint.present?1u:0u);
    SourceCollectionText texts[5]={{0},{0},{0},{0},{0}};
    for(uint32_t n=0;n<text_count;++n)for(unsigned pass=0;pass<2;++pass) {
        XrValueFormatSink sink={&texts[n],source_collection_write};
        bool rendered=false;
        if(n<2)rendered=source_collection_types_text(ctx,sink,map,key,n!=0,error);
        else if(shape_hint && n==2)rendered=key?
            xr_value_format_text(sink,"declare Map<K, V> with one key type for all entries")!=0:
            source_collection_hint_text(ctx,sink,node,input,hint);
        else if(json_hint && n==json_index)rendered=source_collection_json_text(ctx,sink,node);
        else if(union_hint.present && n==union_index)rendered=source_collection_union_text(ctx,sink,map,union_hint);
        if(!rendered)return source_fail(ctx,node,XR_XIR_BUDGET,"collection type diagnostic exceeds format limit");
        if(!pass) {
            if(texts[n].used==SIZE_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"collection type diagnostic exceeds format limit");
            texts[n].capacity=texts[n].used;texts[n].bytes=source_alloc(ctx,texts[n].used+1,1);
            if(!texts[n].bytes)return false;texts[n].used=0;
        }
    }
    AstNode **items=map?(key?node->as.map_literal.keys:node->as.map_literal.values):node->as.array_literal.elements;
    AstNode *a=items[error.first],*b=items[error.second];
    if(map){if(!a)a=node->as.map_literal.values[error.first];if(!b)b=node->as.map_literal.values[error.second];}
    if(!a || !b)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"collection type error lost its source occurrence");
    XrNameSpan first={a->line,a->column},second={b->line,b->column};
    if(map && node->as.map_literal.entry_spans) {
        XrMapLiteralEntrySpan x=node->as.map_literal.entry_spans[error.first],y=node->as.map_literal.entry_spans[error.second];
        first=key?x.key:x.value;second=key?y.key:y.value;
    } else if(!map && node->as.array_literal.element_spans) {
        first=node->as.array_literal.element_spans[error.first].start;
        second=node->as.array_literal.element_spans[error.second].start;
    }
    XrXirStatus allocation=XR_XIR_OK;
    char *quote=map && key && b->type==AST_VARIABLE && b->as.variable.name?
        source_map_bare_key_hint(ctx,b,&allocation):NULL;
    if(allocation!=XR_XIR_OK)return source_fail(ctx,b,allocation,"Map key diagnostic allocation failed");
    const XrModuleSpec *source=&ctx->graph->specs[ctx->module];XrFrontendReport *report=NULL;uint32_t id=0;
    XrCompileResourceStatus status=xr_frontend_report_new(ctx->compile.resources,&report);
    if(status==XR_COMPILE_RESOURCE_OK)status=xr_frontend_report_source(report,source->source_path,source->source_text,source->source_length,NULL,&id);
    if(status==XR_COMPILE_RESOURCE_OK) {
        XrFrontendLabel labels[4]={{texts[1].bytes,xr_frontend_report_location(report,id,first.line,first.column,NULL)}};
        uint32_t label_count=1;
        if(shape_hint)labels[label_count++]=(XrFrontendLabel){texts[2].bytes,
            xr_frontend_report_location(report,id,node->line,node->column,NULL)};
        AstNode *site=source_collection_binding_site(ctx,node);
        if(json_hint)labels[label_count++]=(XrFrontendLabel){texts[json_index].bytes,
            xr_frontend_report_location(report,id,site->line,site->column,NULL)};
        if(union_hint.present)labels[label_count++]=(XrFrontendLabel){texts[union_index].bytes,
            xr_frontend_report_location(report,id,site->line,site->column,NULL)};
        if(quote)labels[label_count++]=(XrFrontendLabel){quote,
            xr_frontend_report_location(report,id,second.line,second.column,NULL)};
        status=xr_frontend_report_add(report,XR_ERR_COLLECTION_LITERAL_TYPES,texts[0].bytes,
            xr_frontend_report_location(report,id,second.line,second.column,NULL),labels,label_count);
    }
    xr_compile_resources_free(quote);
    if(status!=XR_COMPILE_RESOURCE_OK) {
        xr_frontend_report_free(report);return source_fail(ctx,node,xir_compile_resource_status(status),"collection type diagnostic allocation failed");
    }
    xr_frontend_report_seal(report,false);xr_frontend_report_free(ctx->report);ctx->report=report;
    source_fail(ctx,b,XR_XIR_BAD_TYPE,texts[0].bytes);ctx->diagnostic.line=second.line;ctx->diagnostic.column=second.column;return false;
}
