/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_inspection.h - Published observations and real type queries
 */
static const XrSourceReplBindingInfo *binding_info(XrSourceRepl *owner,const char *name,size_t expected) {
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    CHECK(xray_source_repl_bindings(owner,&rows,&count)==XR_SOURCE_OK && count==expected);
    for(size_t i=0;i<count;++i){CHECK(rows[i].name[0]!='$');if(!strcmp(rows[i].name,name))return &rows[i];}
    return NULL;
}
static void inspect_type(XrSourceRepl *owner,const char *source) {
    XrSourceText text={NULL,namespace_id,source,strlen(source)};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_repl_begin_type(owner,&text,&diagnostic);
    if(diagnostic){XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);fprintf(stderr,"type query %u: %s\n",(unsigned)status,info.message);}
    CHECK(status==XR_SOURCE_OK && !diagnostic);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(take(owner,XR_SOURCE_RUN_RETURNED));
}
static void metadata_inspection(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    const XrSourceReplBindingInfo *empty=NULL;size_t count=0;
    CHECK(xray_source_repl_bindings(owner,&empty,&count)==XR_SOURCE_OK && !empty && !count);
    inspect_type(owner,"1+2\n");CHECK(!binding_info(owner,"it",0));
    execute(owner,"var value=40\nconst fixed=2\nfn next()->i64{value+=1;return value}\n");
    const XrSourceReplBindingInfo *row=binding_info(owner,"value",3);
    CHECK(row && row->kind==XR_SOURCE_REPL_VALUE && !row->is_const && !row->implicit_result && !strcmp(row->type_name,"i64"));
    row=binding_info(owner,"fixed",3);CHECK(row && row->is_const);
    row=binding_info(owner,"next",3);CHECK(row && row->kind==XR_SOURCE_REPL_FUNCTION && !row->is_const && !strcmp(row->type_name,"function"));
    XrSourceValue *function=read_value(owner,"next");XrSourceValueKind kind;
    CHECK(xray_source_value_kind(function,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_CALLABLE);integer(owner,"value",40);
    execute(owner,"9\n");row=binding_info(owner,"it",4);CHECK(row && row->implicit_result && row->is_const);
    uint64_t result_birth=row->generation;
    inspect_type(owner,"next()\n");integer(owner,"value",41);integer(owner,"it",9);
    row=binding_info(owner,"it",4);CHECK(row && row->generation==result_birth);
    inspect_type(owner,"value=42\n");integer(owner,"value",42);integer(owner,"it",9);
    inspect_type(owner,"print(7)\n");integer(owner,"it",9);
    static const char failing[]="next()/(value-value)\n";
    XrSourceText failure={NULL,namespace_id,failing,sizeof(failing)-1};
    CHECK(xray_source_repl_begin_type(owner,&failure,NULL)==XR_SOURCE_OK);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_PANICKED);xray_source_result_free(take(owner,XR_SOURCE_RUN_PANICKED));
    integer(owner,"value",43);integer(owner,"it",9);
    XrSourceText rejected={NULL,namespace_id,"const notAnExpression=1\n",24};
    CHECK(xray_source_repl_begin_type(owner,&rejected,NULL)==XR_SOURCE_REJECTED);
    CHECK(!binding_info(owner,"notAnExpression",4));
    execute(owner,"var value=\"shadow\"\n");row=binding_info(owner,"value",4);CHECK(row && !strcmp(row->type_name,"string"));
    CHECK(begin(owner,"var abandoned=1\nassert(false)\n",NULL)==XR_SOURCE_OK);
    const XrSourceReplBindingInfo *pending=NULL;count=0;
    CHECK(xray_source_repl_bindings(owner,&pending,&count)==XR_SOURCE_BUSY && !pending && !count);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_PANICKED);xray_source_result_free(take(owner,XR_SOURCE_RUN_PANICKED));
    CHECK(!binding_info(owner,"abandoned",4));
    execute(owner,"fn optional()->i64?{return 4}\n");inspect_type(owner,"optional()\n");integer(owner,"it",9);
    execute(owner,"fn absent()->i64?{return null}\n");inspect_type(owner,"absent()\n");integer(owner,"it",9);
    execute(owner,"const pair=(1,true)\n");row=binding_info(owner,"pair",7);
    CHECK(row && row->is_const && !row->type_name);
    XrSourceValue *pair=read_value(owner,"pair");CHECK(xray_source_value_kind(pair,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_TUPLE);xray_source_value_free(pair);
    static const char expected[]="i64\n9\ni64\ni64\n7\n()\ni64\nnull\n";
    if(trace.count!=sizeof(expected)-1 || memcmp(trace.bytes,expected,sizeof(expected)-1))fprintf(stderr,"inspection output actual=%zu [%.*s]\n",trace.count,(int)trace.count,trace.bytes);
    CHECK(trace.count==sizeof(expected)-1 && !memcmp(trace.bytes,expected,sizeof(expected)-1));
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    trace.owner=NULL;CHECK(xray_source_value_kind(function,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_CALLABLE);xray_source_value_free(function);
}
