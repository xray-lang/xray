/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_repl_array_consumer.inc.c - Mutation uses the exact received birth
 */
static void sdk_repl_array_execute(XrSourceRepl *owner,const char *source) {
    XrSourceDiagnostic *diagnostic=NULL;XrSourceStatus status=sdk_repl_begin(owner,source,&diagnostic);
    if(diagnostic){XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
        fprintf(stderr,"installed array status=%u: %s\n",(unsigned)status,info.message);}
    CHECK(status==XR_SOURCE_OK && !diagnostic);sdk_repl_finish(owner);
}
static XrSourceWriteStatus sdk_repl_array_collect(void *context,XrSourceOutputStream stream,const char *bytes,size_t count) {
    SdkReplTrace *trace=(SdkReplTrace *)context;
    CHECK(stream==XR_SOURCE_STDOUT && count<=sizeof(trace->bytes)-trace->count);
    memcpy(trace->bytes+trace->count,bytes,count);trace->count+=count;return XR_SOURCE_WRITE_OK;
}
static void sdk_repl_array_cases(const char *stdlib_path) {
    SdkReplTrace trace;memset(&trace,0,sizeof(trace));XrSourceRepl *owner=sdk_repl_open(stdlib_path,&trace);
    sdk_repl_array_execute(owner,"var items:Array<i64>=[]\nconst fixed=[4,5]\nvar calls=0\n");
    sdk_repl_array_execute(owner,"fn append(x:i64){items.push(x)}\nfn value()->i64{calls+=1;return calls}\n");
    sdk_repl_array_execute(owner,"append(value())\nappend(value())\nconst size=len(items)\nconst first=fixed[0]\n");
    sdk_repl_integer(owner,"calls",2);sdk_repl_integer(owner,"size",2);sdk_repl_integer(owner,"first",4);
    CHECK(sdk_repl_begin(owner,"append(3)\nassert(false)\n",NULL)==XR_SOURCE_OK);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_PANICKED);
    xray_source_result_free(sdk_repl_take(owner,XR_SOURCE_RUN_PANICKED));
    sdk_repl_array_execute(owner,"const retained=len(items)\n");sdk_repl_integer(owner,"retained",3);
    XrSourceValue *snapshot=NULL;CHECK(xray_source_repl_read(owner,"items",&snapshot)==XR_SOURCE_OK && snapshot);
    XrSourceDiagnostic *diagnostic=NULL;
    CHECK(sdk_repl_begin(owner,"fixed.push(9)\n",&diagnostic)==XR_SOURCE_REJECTED && diagnostic);
    XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && info.line==1);
    xray_source_diagnostic_free(diagnostic);sdk_repl_array_execute(owner,"const unchanged=fixed[0]\n");
    sdk_repl_integer(owner,"unchanged",4);
    sdk_repl_array_execute(owner,"var items:Array<i64>=[99]\nappend(4)\nconst fresh=len(items)\n");
    sdk_repl_integer(owner,"fresh",1);sdk_repl_array_execute(owner,"append(5)\n");
    CHECK(!trace.count);XrSourceEvent cleanup;
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner && cleanup.state==XR_SOURCE_RUN_READY);trace.owner=NULL;
    XrSourceValueKind kind;CHECK(xray_source_value_kind(snapshot,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_ARRAY);
    CHECK(xray_source_value_format(snapshot,sdk_repl_array_collect,&trace)==XR_SOURCE_OK);
    static const char expected[]="[1, 2, 3]";
    CHECK(trace.count==sizeof(expected)-1 && !memcmp(trace.bytes,expected,trace.count));xray_source_value_free(snapshot);
    puts("Installed Array history: exact birth push, argument once, const refusal, panic effect, shadow and owned snapshot PASS");
}
