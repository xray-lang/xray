/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_rooted.h - Real file authority within interactive history
 */
static XrSourceStatus rooted_begin(XrSourceRepl *owner,const char *path,const char *source,
    bool query,XrSourceDiagnostic **diagnostic) {
    XrSourceText input={path,NULL,source,strlen(source)};
    return query?xray_source_repl_begin_type(owner,&input,diagnostic):xray_source_repl_begin(owner,&input,diagnostic);
}
static void rooted_execute(XrSourceRepl *owner,const char *path,const char *source) {
    XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=rooted_begin(owner,path,source,false,&diagnostic);
    if(diagnostic){XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
        fprintf(stderr,"rooted %u at %s:%d: %s\n",(unsigned)status,info.path?info.path:path,info.line,info.message);}
    CHECK(status==XR_SOURCE_OK && !diagnostic);
    XrSourceEvent event=pause_or_finish(owner);
    if(event.state!=XR_SOURCE_RUN_RETURNED)fprintf(stderr,"rooted execution state=%u engine=%u source=%s\n",
        (unsigned)event.state,event.engine_status,source);
    CHECK(event.state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(take(owner,XR_SOURCE_RUN_RETURNED));
}
static void rooted_inputs(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    char path[]=XR_PUBLIC_REPL_FIXTURE "/script/unsaved.xr";
    char source[]="import \"./sibling\" as sibling\nvar state=0\n"
        "fn next()->i64{state+=1;return state+sibling.answer()}\nconst saved=next\nnext()\n";
    XrSourceDiagnostic *first_diagnostic=NULL;
    XrSourceStatus first_status=rooted_begin(owner,path,source,false,&first_diagnostic);
    if(first_diagnostic){XrSourceDiagnosticInfo detail;CHECK(xray_source_diagnostic_info(first_diagnostic,&detail)==XR_SOURCE_OK);
        fprintf(stderr,"first rooted %u stage%u at %s:%d: %s\n",(unsigned)first_status,detail.stage,detail.path?detail.path:path,detail.line,detail.message);}
    CHECK(first_status==XR_SOURCE_OK && !first_diagnostic);
    memset(path,'!',sizeof(path)-1);memset(source,'!',sizeof(source)-1);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *survivor=take(owner,XR_SOURCE_RUN_RETURNED);integer(owner,"it",43);
    const XrSourceReplBindingInfo *alias=binding_info(owner,"sibling",5);
    CHECK(alias && alias->kind==XR_SOURCE_REPL_MODULE && !alias->type_name);
    const char *same=XR_PUBLIC_REPL_FIXTURE "/script/unsaved.xr";
    rooted_execute(owner,same,"import \"./sibling\" as sibling\nvar state=100\n"
        "fn next()->i64{state+=1;return state+sibling.answer()}\nconst prior=saved()\nconst fresh=next()\nfresh\n");
    integer(owner,"prior",44);integer(owner,"fresh",143);integer(owner,"state",101);integer(owner,"it",143);
    execute(owner,"const again=saved()\n");integer(owner,"again",45);
    CHECK(rooted_begin(owner,same,"const discarded=saved()\nassert(false)\n",false,NULL)==XR_SOURCE_OK);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_PANICKED);xray_source_result_free(take(owner,XR_SOURCE_RUN_PANICKED));
    XrSourceValue *empty=NULL;CHECK(xray_source_repl_read(owner,"discarded",&empty)!=XR_SOURCE_OK && !empty);
    CHECK(rooted_begin(owner,same,"saved()\n",true,NULL)==XR_SOURCE_OK);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);xray_source_result_free(take(owner,XR_SOURCE_RUN_RETURNED));
    integer(owner,"it",143);execute(owner,"const after=saved()\n");integer(owner,"after",48);
    XrSourceDiagnostic *diagnostic=NULL;
    CHECK(rooted_begin(owner,same,"import \"./absent\" as absent\n",false,&diagnostic)!=XR_SOURCE_OK && diagnostic);
    XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && *info.message);
    xray_source_diagnostic_free(diagnostic);diagnostic=NULL;
    CHECK(begin(owner,"import \"./sibling\" as forbidden\n",&diagnostic)!=XR_SOURCE_OK && diagnostic);
    xray_source_diagnostic_free(diagnostic);diagnostic=NULL;
    rooted_execute(owner,XR_PUBLIC_REPL_FIXTURE "/project/unsaved.xr",
        "import \"./sibling\" as projectSibling\nconst projectAnswer=projectSibling.answer()\n");
    integer(owner,"projectAnswer",84);
    const char *invalid[]={XR_PUBLIC_REPL_FIXTURE "/project/nearer/unsaved.xr",
        XR_PUBLIC_REPL_FIXTURE "/invalid-lock/unsaved.xr"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        XrSourceStatus expected=i==0?XR_SOURCE_INVALID:XR_SOURCE_BAD_ARGUMENT;
        XrSourceStatus status=rooted_begin(owner,invalid[i],"const forbiddenFallback=1\n",false,&diagnostic);
        if(status!=expected)fprintf(stderr,"invalid rooted authority case=%zu status=%u expected=%u\n",i,(unsigned)status,(unsigned)expected);
        CHECK(status==expected && diagnostic);
        CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && *info.message);
        xray_source_diagnostic_free(diagnostic);diagnostic=NULL;
        CHECK(xray_source_repl_read(owner,"forbiddenFallback",&empty)!=XR_SOURCE_OK && !empty);
    }
    XrSourceText ambiguous={same,namespace_id,"1\n",2};
    CHECK(xray_source_repl_begin(owner,&ambiguous,NULL)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(rooted_begin(owner,"relative.xr","1\n",false,NULL)==XR_SOURCE_BAD_ARGUMENT);
    static const char expected[]="43\n143\ni64\n";
    CHECK(trace.count==sizeof(expected)-1 && !memcmp(trace.bytes,expected,sizeof(expected)-1));
    XrSourceValue *function=read_value(owner,"saved");XrSourceEvent cleanup;
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);trace.owner=NULL;
    int64_t value=0;CHECK(xray_source_result_i64(survivor,&value)==XR_SOURCE_OK && value==43);
    XrSourceValueKind kind;CHECK(xray_source_value_kind(function,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_CALLABLE);
    xray_source_result_free(survivor);xray_source_value_free(function);
    puts("rooted REPL: SCRIPT/project/import authority, repeated real path with distinct birth state, old closures, rollback, type query, producer death and no fallback PASS");
}

static void rooted_nominals(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    const char *same=XR_PUBLIC_REPL_FIXTURE "/script/nominal.xr";
    rooted_execute(owner,same,"struct Item{value:i64}\nconst original=Item{value:7}\n"
        "fn accept(item:Item)->i64{return item.value}\nconst acceptOld=accept\n"
        "fn oldValue()->i64{return original.value}\n");
    XrSourceValue *survivor=read_value(owner,"original");
    rooted_execute(owner,same,"struct Item{value:i64}\nconst current=Item{value:9}\nconst priorValue=oldValue()\n");
    integer(owner,"priorValue",7);
    XrSourceDiagnostic *diagnostic=NULL;
    CHECK(rooted_begin(owner,same,"const wrong=acceptOld(current)\n",false,&diagnostic)==XR_SOURCE_REJECTED && diagnostic);
    XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && *info.message);
    xray_source_diagnostic_free(diagnostic);
    execute(owner,"const sameBirth=acceptOld(original)\n");integer(owner,"sameBirth",7);
    rooted_execute(owner,same,"struct Item{value:string}\nconst changed=Item{value:\"new\"}\nconst stillOld=oldValue()\n");
    integer(owner,"stillOld",7);CHECK(!trace.count);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    trace.owner=NULL;XrSourceValueKind kind;
    CHECK(xray_source_value_kind(survivor,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_STRUCT);xray_source_value_free(survivor);
    puts("rooted REPL nominal births: same path and same spelling/shape remain distinct, wrong birth refused, old state and owned value survive PASS");
}
