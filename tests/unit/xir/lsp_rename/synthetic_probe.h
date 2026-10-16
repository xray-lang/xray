/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * synthetic_probe.h - Preserve unnamed body and lexical caller boundaries
 *
 * KEY CONCEPT:
 *   Synthetic cleanup bodies retain no lexical name or source caller identity.
 */
#ifndef LSP_RENAME_SYNTHETIC_PROBE_H
#define LSP_RENAME_SYNTHETIC_PROBE_H
static int synthetic_once(const char *uri,unsigned mode) {
    static const char text[]="fn sink(v:i64){}\nfn run(value:i64)->i64 {\n"
        "    defer {\n        sink(value)\n    }\n    return value\n}\nconst answer=run(1)\n";
    static const char generic[]="fn sink<Item>(v:Item){}\nfn run<Item>(value:Item)->Item {\n"
        "    defer {\n        sink(value)\n    }\n    return value\n}\n";
    const char *input_text=mode?generic:text;
    XrCompileResources *r=sdk_ledger(&limits);
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    XlspSourceDocument document={uri,input_text,strlen(uri),strlen(input_text),7};
    XlspSourceBindings *owner=NULL;
    XrXirStatus status=xlsp_source_bindings_build(&context,&document,1,&owner);
    if(status!=XR_XIR_OK)fprintf(stderr,"Synthetic probe mode=%u Source status=%u\n",mode,(unsigned)status);
    if(status!=XR_XIR_OK){xlsp_source_bindings_free(owner);xr_compile_resources_release(r);
        CHECK(!runtime_live&&!runtime_bytes);CHECK(status==XR_XIR_OK);return 1;}
    CHECK(owner);
    const XrXirSourceView *view=xlsp_source_bindings_view(owner,0);
    const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(owner,0);
    const XrXirSourceCalls *calls=xlsp_source_bindings_calls(owner,0);
    CHECK(view&&syntax&&calls);uint32_t outer=0,sink=0,helper=0;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind==XR_XIR_SOURCE_FUNCTION&&!d->parent) {
            if(!strcmp(d->name,"run"))outer=d->id;
            if(!strcmp(d->name,"sink"))sink=d->id;
        }
    }
    CHECK(outer&&sink);
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind==XR_XIR_SOURCE_FUNCTION&&d->parent==outer){CHECK(!helper);helper=d->id;}
    }
    CHECK(helper);const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[helper-1];
    bool unnamed=fact->role==XR_XIR_SOURCE_SYNTAX_NONE&&!fact->name.line&&!fact->name.column&&
        !fact->name.end_line&&!fact->name.end_column;
    CHECK(!calls->functions[helper-1].known);
    unsigned observed=0;bool implicit=true;
    for(uint32_t i=0;i<calls->count;++i) {
        const XrXirSourceCall *call=&calls->items[i];
        if(call->reference==UINT32_MAX)continue;
        CHECK(call->reference<view->reference_count);
        if(view->references[call->reference].target!=sink)continue;
        ++observed;implicit=implicit&&!call->caller&&!call->module_initializer&&call->selection_known&&call->body==helper;
        CHECK(call->signature.known);
        if(mode){CHECK(call->signature.type_parameter_owner==outer&&call->signature.type_parameter_count==1);
            CHECK(!strcmp(call->signature.type_parameters[0],"Item"));}
    }
    fprintf(stderr,"Synthetic cleanup mode=%u declaration=%u role=%u nameSpan=%d:%d..%d:%d unnamed=%u calls=%u caller_zero=%u\n",
        mode,helper,(unsigned)fact->role,fact->name.line,fact->name.column,fact->name.end_line,fact->name.end_column,
        (unsigned)unnamed,observed,(unsigned)implicit);
    xlsp_source_bindings_free(owner);
    XlspSourceRenameInput input={0,{1,mode?14u:8u},"payload",7,false,NULL};XrJsonValue *result=NULL;
    status=xlsp_source_rename_json(&context,&document,1,&input,&result);
    fprintf(stderr,"Synthetic lexical rename status=%u result=%s\n",(unsigned)status,result?"JSON":"NULL");
    bool renamed=status==XR_XIR_OK&&result;
    if(renamed){XrJsonValue *changes=xjson_get_array(result,"documentChanges");CHECK(changes&&changes->as.array.count==1);
        XrJsonValue *edits=xjson_get_array(xjson_array_get(changes,0),"edits");CHECK(edits&&edits->as.array.count==3);}
    xjson_free(result);xr_compile_resources_release(r);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
    CHECK(unnamed&&observed==1&&implicit&&renamed);return 0;
}
static int synthetic_probe(const char *uri) {
    CHECK(!synthetic_once(uri,0));CHECK(!synthetic_once(uri,1));return 0;
}
#endif // LSP_RENAME_SYNTHETIC_PROBE_H
