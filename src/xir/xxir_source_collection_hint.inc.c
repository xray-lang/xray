/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Repair snippets borrow original expressions only during report construction. */
static char *source_library_owned_name(SourceContext *,const char *,uint32_t);
static bool source_library_source_name(SourceContext *,const char *,uint32_t,bool *);
typedef struct SourceCollectionSnippet {const char *bytes;size_t length;} SourceCollectionSnippet;
typedef struct SourceCollectionHint {
    bool record,tuple;
    const char *name;
    SourceCollectionSnippet *values;
} SourceCollectionHint;
static bool source_collection_snippets(SourceContext *ctx,AstNode *node,AstNode **values,uint32_t count,SourceCollectionSnippet *out) {
    const XrModuleSpec *source=&ctx->graph->specs[ctx->module];
    if(!source->source_text)return false;
    size_t lines=1;
    for(size_t n=0;n<source->source_length;++n) {
        if(!(n&4095u) && !source_request_poll(ctx,node))return false;
        if(source->source_text[n]=='\n')++lines;
    }
    if(lines==SIZE_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"source line inventory exhausted");
    size_t *starts=source_alloc(ctx,lines+1,sizeof(*starts));if(!starts)return false;
    size_t line=1;starts[0]=0;
    for(size_t n=0;n<source->source_length;++n)if(source->source_text[n]=='\n')starts[line++]=n+1;
    starts[lines]=source->source_length;
    for(uint32_t n=0;n<count;++n) {
        if(!source_request_poll(ctx,node))return false;
        AstNode *value=values[n];if(!value)return false;
        XrNameSpan first={value->line,value->column};
        XrNameSpan end={value->end_line,value->end_column};
        if(node->type==AST_MAP_LITERAL && node->as.map_literal.entry_spans) {
            first=node->as.map_literal.entry_spans[n].value;end=node->as.map_literal.entry_spans[n].value_end;
        } else if(node->type==AST_ARRAY_LITERAL && node->as.array_literal.element_spans) {
            first=node->as.array_literal.element_spans[n].start;end=node->as.array_literal.element_spans[n].end;
        }
        if(first.line<=0 || first.column<=0 || end.line<=0 || end.column<=0 ||
            (size_t)first.line>lines || (size_t)end.line>lines)return false;
        size_t a=starts[first.line-1],b=starts[end.line-1];
        size_t x=(size_t)first.column-1,y=(size_t)end.column-1;
        if(x>starts[first.line]-a || y>starts[end.line]-b)return false;
        a+=x;b+=y;if(b<a || b>source->source_length)return false;
        out[n]=(SourceCollectionSnippet){source->source_text+a,b-a};
    }
    return true;
}
static bool source_collection_hint_prepare(SourceContext *ctx,AstNode *node,const XrHirCollectionInput *input,
    XrHirCollectionError error,SourceCollectionHint *out) {
    *out=(SourceCollectionHint){0};bool map=input->kind==XR_HIR_COLLECTION_MAP;
    if(map && error.kind==XR_HIR_COLLECTION_KEY_TYPE)return true;
    if(!map)for(uint32_t n=0;n<input->count;++n) {
        if(!source_request_poll(ctx,node))return false;
        if(input->entries[n].spread)return true;
    }
    if(map)for(uint32_t n=0;n<input->count;++n) {
        AstNode *key=node->as.map_literal.keys[n];
        if(!key || key->type!=AST_LITERAL_STRING || !key->as.literal.string_length ||
            memchr(key->as.literal.raw_value.string_val,0,key->as.literal.string_length))return true;
        if(key->as.literal.string_length>UINT32_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"field spelling exceeds format limit");
        uint32_t length=(uint32_t)key->as.literal.string_length;
        char *name=source_library_owned_name(ctx,key->as.literal.raw_value.string_val,length);bool valid=false;
        if(!name || !source_library_source_name(ctx,name,length,&valid))return false;
        if(!valid)return true;
    }
    if(map) {
        XrHirCollectionEntry *keys=source_alloc(ctx,input->count,sizeof(*keys));if(!keys)return false;
        for(uint32_t n=0;n<input->count;++n){keys[n]=input->entries[n];keys[n].value=XR_XIR_I64;}
        XrHirCollectionInput unique={.kind=XR_HIR_COLLECTION_MAP,.entries=keys,.count=input->count};
        XrHirCollection checked;XrHirCollectionError error_key;
        XrXirStatus status=xr_hir_collection_check(&ctx->compile,&ctx->types,&unique,ctx->request,&checked,&error_key);
        if(status==XR_XIR_BAD_TYPE)return true;
        if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"field spelling uniqueness could not be checked");
    }
    SourceCollectionSnippet *values=source_alloc(ctx,input->count,sizeof(*values));if(!values)return false;
    if(!source_collection_snippets(ctx,node,map?node->as.map_literal.values:node->as.array_literal.elements,input->count,values))
        return ctx->diagnostic.status==XR_XIR_OK;
    const char *name="Record";AstNode *binding=ctx->binding_site;
    AstNode *initializer=binding?binding->as.var_decl.initializer:NULL;
    while(initializer && initializer->type==AST_GROUPING)initializer=initializer->as.grouping;
    if(binding && initializer==node) {
        size_t length=source_text_size(ctx,binding->as.var_decl.name);
        char *owned=source_alloc(ctx,length+1,1);if(!owned || !source_copy_bytes(ctx,node,owned,binding->as.var_decl.name,length))return false;
        if(length && owned[0]>='a' && owned[0]<='z')owned[0]=(char)(owned[0]-'a'+'A');name=owned;
    }
    *out=(SourceCollectionHint){map,!map,name,values};return true;
}
static bool source_collection_hint_text(SourceContext *ctx,XrValueFormatSink sink,AstNode *node,
    const XrHirCollectionInput *input,SourceCollectionHint hint) {
    if(hint.tuple) {
        if(!xr_value_format_text(sink,"for a fixed number of elements, use a tuple: ("))return false;
        for(uint32_t n=0;n<input->count;++n)
            if((n && !xr_value_format_text(sink,", ")) || !xr_value_format_write(sink,hint.values[n].bytes,hint.values[n].length))return false;
        return xr_value_format_text(sink,")")!=0;
    }
    if(!xr_value_format_text(sink,"for a fixed set of fields, declare a struct: struct ") ||
        !xr_value_format_text(sink,hint.name) || !xr_value_format_text(sink," { "))return false;
    for(uint32_t n=0;n<input->count;++n) {
        AstNode *key=node->as.map_literal.keys[n];
        if((n && !xr_value_format_text(sink,"; ")) ||
            !xr_value_format_write(sink,key->as.literal.raw_value.string_val,key->as.literal.string_length) ||
            !xr_value_format_text(sink,": ") || !source_collection_type_text(ctx,sink,input->entries[n].value,0))return false;
    }
    if(!xr_value_format_text(sink," }, then write ") || !xr_value_format_text(sink,hint.name) || !xr_value_format_text(sink,"{"))return false;
    for(uint32_t n=0;n<input->count;++n) {
        AstNode *key=node->as.map_literal.keys[n];
        if((n && !xr_value_format_text(sink,", ")) ||
            !xr_value_format_write(sink,key->as.literal.raw_value.string_val,key->as.literal.string_length) ||
            !xr_value_format_text(sink,": ") || !xr_value_format_write(sink,hint.values[n].bytes,hint.values[n].length))return false;
    }
    return xr_value_format_text(sink,"}")!=0;
}
