/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_repl_input_consumer.inc.c - Installed rooted and FILE authority
 */
static XrSourceRepl *sdk_repl_open(const char *stdlib_path,SdkReplTrace *trace) {
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    compiler.stdlib_path=stdlib_path;runtime.write=sdk_repl_output;runtime.context=trace;
    runtime.output_byte_limit=sizeof(trace->bytes);XrSourceRepl *owner=NULL;
    CHECK(xray_source_repl_new(&compiler,"installed-repl",&runtime,&owner)==XR_SOURCE_OK && owner);
    trace->owner=owner;return owner;
}
static void sdk_repl_path(char *path,size_t capacity,const char *root,const char *suffix) {
    int length=snprintf(path,capacity,"%s/%s",root,suffix);
    CHECK(length>0 && (size_t)length<capacity);
}
static void sdk_repl_finish(XrSourceRepl *owner) {
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED));
}
static void sdk_repl_rooted(XrSourceRepl *owner,const char *path,const char *source) {
    XrSourceText input={path,NULL,source,strlen(source)};
    XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_repl_begin(owner,&input,&diagnostic);
    if(diagnostic){XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
        fprintf(stderr,"installed rooted status=%u: %s\n",(unsigned)status,info.message);}
    CHECK(status==XR_SOURCE_OK && !diagnostic);sdk_repl_finish(owner);
}
static void sdk_repl_rooted_cases(const char *stdlib_path,const char *fixtures) {
    SdkReplTrace trace;memset(&trace,0,sizeof(trace));XrSourceRepl *owner=sdk_repl_open(stdlib_path,&trace);
    char path[2048];sdk_repl_path(path,sizeof(path),fixtures,"script/unsaved.xr");
    char source[]="import \"./sibling\" as sibling\nvar state=0\n"
        "fn next()->i64{state+=1;return state+sibling.answer()}\nconst saved=next\nnext()\n";
    XrSourceText input={path,NULL,source,sizeof(source)-1};
    CHECK(xray_source_repl_begin(owner,&input,NULL)==XR_SOURCE_OK);
    memset(source,'!',sizeof(source)-1);memset(path,'!',strlen(path));
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *survivor=sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED);sdk_repl_integer(owner,"it",43);
    sdk_repl_path(path,sizeof(path),fixtures,"script/unsaved.xr");
    sdk_repl_rooted(owner,path,"import \"./sibling\" as sibling\nvar state=100\n"
        "fn next()->i64{state+=1;return state+sibling.answer()}\nconst prior=saved()\nconst fresh=next()\nfresh\n");
    sdk_repl_integer(owner,"prior",44);sdk_repl_integer(owner,"fresh",143);
    sdk_repl_integer(owner,"state",101);sdk_repl_integer(owner,"it",143);
    static const char query[]="saved()\n";XrSourceText type={path,NULL,query,sizeof(query)-1};
    CHECK(xray_source_repl_begin_type(owner,&type,NULL)==XR_SOURCE_OK);sdk_repl_finish(owner);
    sdk_repl_integer(owner,"it",143);
    CHECK(sdk_repl_begin(owner,"const after=saved()\n",NULL)==XR_SOURCE_OK);sdk_repl_finish(owner);
    sdk_repl_integer(owner,"after",46);
    sdk_repl_path(path,sizeof(path),fixtures,"project/unsaved.xr");
    sdk_repl_rooted(owner,path,"import \"./sibling\" as siblingProject\nconst projectAnswer=siblingProject.answer()\n");
    sdk_repl_integer(owner,"projectAnswer",84);
    const char *invalid[]={"project/nearer/unsaved.xr","invalid-lock/unsaved.xr"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        sdk_repl_path(path,sizeof(path),fixtures,invalid[i]);
        XrSourceText denied={path,NULL,"const forbidden=1\n",18};XrSourceDiagnostic *diagnostic=NULL;
        XrSourceStatus expected=i==0?XR_SOURCE_INVALID:XR_SOURCE_BAD_ARGUMENT;
        CHECK(xray_source_repl_begin(owner,&denied,&diagnostic)==expected && diagnostic);
        xray_source_diagnostic_free(diagnostic);XrSourceValue *empty=NULL;
        CHECK(xray_source_repl_read(owner,"forbidden",&empty)!=XR_SOURCE_OK && !empty);
    }
    XrSourceText ambiguous={path,"installed-repl","1\n",2};
    CHECK(xray_source_repl_begin(owner,&ambiguous,NULL)==XR_SOURCE_BAD_ARGUMENT);
    XrSourceText relative={"relative.xr",NULL,"1\n",2};
    CHECK(xray_source_repl_begin(owner,&relative,NULL)==XR_SOURCE_BAD_ARGUMENT);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner);
    trace.owner=NULL;int64_t result=0;CHECK(xray_source_result_i64(survivor,&result)==XR_SOURCE_OK && result==43);
    xray_source_result_free(survivor);
    static const char expected[]="43\n143\ni64\n";
    CHECK(trace.count==sizeof(expected)-1 && !memcmp(trace.bytes,expected,sizeof(expected)-1));
    puts("Installed rooted REPL: real imports, repeated path births, old closure, type side effects and no fallback PASS");
}
static void sdk_repl_file_cases(const char *stdlib_path,const char *fixtures) {
    SdkReplTrace trace;memset(&trace,0,sizeof(trace));XrSourceRepl *owner=sdk_repl_open(stdlib_path,&trace);
    char path[2048];sdk_repl_path(path,sizeof(path),fixtures,"main.xr");
    CHECK(xray_source_repl_begin_file(owner,path,NULL)==XR_SOURCE_OK);
    memset(path,'!',strlen(path));CHECK(xray_source_repl_begin_file(owner,"missing.xr",NULL)==XR_SOURCE_BUSY);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *survivor=sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED);sdk_repl_integer(owner,"it",11);
    CHECK(sdk_repl_begin(owner,"const saved=next\nconst first=next()\n",NULL)==XR_SOURCE_OK);sdk_repl_finish(owner);
    sdk_repl_integer(owner,"first",41);sdk_repl_path(path,sizeof(path),fixtures,"main.xr");
    CHECK(xray_source_repl_begin_file(owner,path,NULL)==XR_SOURCE_OK);sdk_repl_finish(owner);
    CHECK(sdk_repl_begin(owner,"const prior=saved()\nconst fresh=next()\nnext()\n",NULL)==XR_SOURCE_OK);
    sdk_repl_finish(owner);sdk_repl_integer(owner,"prior",42);sdk_repl_integer(owner,"fresh",41);sdk_repl_integer(owner,"it",42);
    XrSourceValue *bytes_value=NULL;CHECK(xray_source_repl_read(owner,"bytes",&bytes_value)==XR_SOURCE_OK && bytes_value);
    XrSourceDiagnostic *syntax=NULL,*missing=NULL,*bad=NULL;
    sdk_repl_path(path,sizeof(path),fixtures,"syntax.xr");
    CHECK(xray_source_repl_begin_file(owner,path,&syntax)==XR_SOURCE_INVALID && syntax);
    sdk_repl_path(path,sizeof(path),fixtures,"absent.xr");
    CHECK(xray_source_repl_begin_file(owner,path,&missing)==XR_SOURCE_NOT_FOUND && missing);
    sdk_repl_path(path,sizeof(path),fixtures,"invalid-project/entry.xr");
    CHECK(xray_source_repl_begin_file(owner,path,&bad)==XR_SOURCE_INVALID && bad);xray_source_diagnostic_free(bad);bad=NULL;
    CHECK(xray_source_repl_begin_file(owner,fixtures,&bad)!=XR_SOURCE_OK && bad);xray_source_diagnostic_free(bad);
    CHECK(xray_source_repl_begin_file(owner,NULL,NULL)==XR_SOURCE_BAD_ARGUMENT);sdk_repl_integer(owner,"it",42);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner);trace.owner=NULL;
    int64_t result=0;CHECK(xray_source_result_i64(survivor,&result)==XR_SOURCE_OK && result==11);
    const char *bytes=NULL;size_t length=0;static const char expected_bytes[]="owned\0中";
    CHECK(xray_source_value_bytes(bytes_value,&bytes,&length)==XR_SOURCE_OK && length==sizeof(expected_bytes)-1 && !memcmp(bytes,expected_bytes,length));
    XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(syntax,&info)==XR_SOURCE_OK && info.line==1 && info.path && strstr(info.path,"syntax.xr") && *info.message);
    xray_source_diagnostic_free(syntax);xray_source_diagnostic_free(missing);xray_source_value_free(bytes_value);xray_source_result_free(survivor);
    CHECK(trace.count==9 && !memcmp(trace.bytes,"11\n11\n42\n",9));
    puts("Installed FILE REPL: real imports, repeated births, absent/directory/config refusals and owned survivors PASS");
}
static void sdk_repl_snapshot_input_cases(const char *stdlib_path) {
    SdkReplTrace trace;memset(&trace,0,sizeof(trace));XrSourceRepl *owner=sdk_repl_open(stdlib_path,&trace);
    static const struct {const char *text;XrSourceInputState expected;} cases[]={
        {"",XR_SOURCE_INPUT_COMPLETE},{"var =",XR_SOURCE_INPUT_COMPLETE},{"fn f(){",XR_SOURCE_INPUT_INCOMPLETE},
        {"([])",XR_SOURCE_INPUT_COMPLETE},{"/* {",XR_SOURCE_INPUT_INCOMPLETE},{"/* { */ 1",XR_SOURCE_INPUT_COMPLETE},
        {"// {",XR_SOURCE_INPUT_COMPLETE},{"\"中{\"",XR_SOURCE_INPUT_COMPLETE},{"\"unfinished",XR_SOURCE_INPUT_INCOMPLETE}};
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrSourceInputState state=XR_SOURCE_INPUT_INCOMPLETE;
        CHECK(xray_source_repl_check_input(owner,cases[i].text,strlen(cases[i].text),&state)==XR_SOURCE_OK && state==cases[i].expected);
    }
    char span[3]={'4','2','{'};XrSourceInputState state=XR_SOURCE_INPUT_INCOMPLETE;
    CHECK(xray_source_repl_check_input(owner,span,2,&state)==XR_SOURCE_OK && state==XR_SOURCE_INPUT_COMPLETE);
    char invalid[3]={'4',0,'2'};state=XR_SOURCE_INPUT_INCOMPLETE;
    CHECK(xray_source_repl_check_input(owner,invalid,sizeof(invalid),&state)==XR_SOURCE_INVALID && state==XR_SOURCE_INPUT_INCOMPLETE);
    CHECK(xray_source_repl_check_input(owner,NULL,1,&state)==XR_SOURCE_BAD_ARGUMENT && state==XR_SOURCE_INPUT_INCOMPLETE);
    CHECK(xray_source_repl_check_input(owner,NULL,0,&state)==XR_SOURCE_OK && state==XR_SOURCE_INPUT_COMPLETE);
    const char *path="installed-owned-snapshot.xr";
    CHECK(xray_source_repl_begin_file(owner,path,NULL)==XR_SOURCE_OK && remove(path)==0);
    state=XR_SOURCE_INPUT_INCOMPLETE;
    CHECK(xray_source_repl_check_input(owner,"42",2,&state)==XR_SOURCE_BUSY && state==XR_SOURCE_INPUT_INCOMPLETE);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *survivor=sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED);sdk_repl_integer(owner,"it",72);
    CHECK(xray_source_repl_begin_file(owner,path,NULL)==XR_SOURCE_NOT_FOUND);sdk_repl_integer(owner,"snapshot",72);
    CHECK(xray_source_repl_check_input(owner,"42",2,&state)==XR_SOURCE_OK && state==XR_SOURCE_INPUT_COMPLETE);
    sdk_repl_integer(owner,"it",72);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner);trace.owner=NULL;
    int64_t result=0;CHECK(xray_source_result_i64(survivor,&result)==XR_SOURCE_OK && result==72);xray_source_result_free(survivor);
    CHECK(trace.count==3 && !memcmp(trace.bytes,"72\n",3));
    puts("Installed input and snapshot: exact lexer span, pending exclusion, removed file and producer death PASS");
}
