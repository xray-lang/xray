/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_file.h - Real FILE snapshots with interactive birth state
 */
static void file_execute(XrSourceRepl *owner,const char *path) {
    XrSourceDiagnostic *diagnostic=NULL;XrSourceStatus status=xray_source_repl_begin_file(owner,path,&diagnostic);
    if(diagnostic){XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
        fprintf(stderr,"FILE begin status=%u: %s\n",(unsigned)status,info.message);}
    CHECK(status==XR_SOURCE_OK && !diagnostic);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);xray_source_result_free(take(owner,XR_SOURCE_RUN_RETURNED));
}
static void file_inputs(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    char path[]=XR_PUBLIC_REPL_FILE_FIXTURE "/main.xr";
    CHECK(xray_source_repl_begin_file(owner,path,NULL)==XR_SOURCE_OK);memset(path,'!',sizeof(path)-1);
    CHECK(xray_source_repl_begin_file(owner,XR_PUBLIC_REPL_FILE_FIXTURE "/main.xr",NULL)==XR_SOURCE_BUSY);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *survivor=take(owner,XR_SOURCE_RUN_RETURNED);integer(owner,"state",40);integer(owner,"it",11);
    execute(owner,"const saved=next\nconst first=next()\n");integer(owner,"first",41);
    file_execute(owner,XR_PUBLIC_REPL_FILE_FIXTURE "/main.xr");integer(owner,"state",40);
    execute(owner,"const prior=saved()\nconst fresh=next()\n");integer(owner,"prior",42);integer(owner,"fresh",41);
    execute(owner,"next()\n");integer(owner,"it",42);
    XrSourceDiagnostic *missing=NULL,*syntax=NULL,*bad=NULL;
    CHECK(xray_source_repl_begin_file(owner,XR_PUBLIC_REPL_FILE_FIXTURE "/absent.xr",&missing)==XR_SOURCE_NOT_FOUND && missing);
    CHECK(xray_source_repl_begin_file(owner,XR_PUBLIC_REPL_FILE_FIXTURE "/syntax.xr",&syntax)==XR_SOURCE_INVALID && syntax);
    XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(syntax,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(syntax,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK &&
        info.primary.line==1 && info.primary.path && strstr(info.primary.path,"syntax.xr") && *info.message);
    CHECK(xray_source_repl_begin_file(owner,XR_PUBLIC_REPL_FILE_FIXTURE "/invalid-project/entry.xr",&bad)==XR_SOURCE_INVALID && bad);
    xray_source_diagnostic_free(bad);bad=NULL;
    CHECK(xray_source_repl_begin_file(owner,XR_PUBLIC_REPL_FILE_FIXTURE,&bad)!=XR_SOURCE_OK && bad);
    xray_source_diagnostic_free(bad);bad=NULL;
    CHECK(xray_source_repl_begin_file(owner,"relative.xr",NULL)==XR_SOURCE_NOT_FOUND);
    CHECK(xray_source_repl_begin_file(owner,NULL,NULL)==XR_SOURCE_BAD_ARGUMENT);
    integer(owner,"it",42);XrSourceValue *bytes_value=read_value(owner,"bytes");
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);trace.owner=NULL;
    int64_t scalar=0;CHECK(xray_source_result_i64(survivor,&scalar)==XR_SOURCE_OK && scalar==11);
    const char *bytes=NULL;size_t length=0;static const char expected_bytes[]="owned\0中";
    CHECK(xray_source_value_bytes(bytes_value,&bytes,&length)==XR_SOURCE_OK && length==sizeof(expected_bytes)-1 && !memcmp(bytes,expected_bytes,length));
    CHECK((xray_source_diagnostic_report(syntax,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(syntax,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK && info.primary.line==1 && *info.message);
    xray_source_diagnostic_free(syntax);xray_source_diagnostic_free(missing);xray_source_value_free(bytes_value);xray_source_result_free(survivor);
    static const char expected[]="11\n11\n42\n";
    CHECK(trace.count==sizeof(expected)-1 && !memcmp(trace.bytes,expected,sizeof(expected)-1));
    puts("FILE REPL: real regular-file authority/imports, repeated path and old closure, no absent-file/MEMORY fallback, located syntax and owned survivors PASS");
}
static void file_snapshot(void) {
    char path[512];FILE *file=NULL;
    for(unsigned i=0;i<1024 && !file;++i) {
        int n=snprintf(path,sizeof(path),"%s/repl-owned-file-%u.xr",XR_PUBLIC_REPL_FILE_TEMP_ROOT,i);
        CHECK(n>0 && (size_t)n<sizeof(path));file=fopen(path,"wx");
    }
    CHECK(file);static const char original[]="const snapshot=72\nsnapshot\n";
    CHECK(fwrite(original,1,sizeof(original)-1,file)==sizeof(original)-1 && fclose(file)==0);
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    const char *relative=strrchr(path,'/');CHECK(relative && *++relative);
    CHECK(xray_source_repl_begin_file(owner,relative,NULL)==XR_SOURCE_OK);
    CHECK(remove(path)==0);CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *result=take(owner,XR_SOURCE_RUN_RETURNED);integer(owner,"snapshot",72);integer(owner,"it",72);
    CHECK(xray_source_repl_begin_file(owner,path,NULL)==XR_SOURCE_NOT_FOUND);integer(owner,"snapshot",72);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    trace.owner=NULL;int64_t value=0;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==72);xray_source_result_free(result);
    CHECK(trace.count==3 && !memcmp(trace.bytes,"72\n",3));
    puts("FILE snapshot: entry unlinked after begin, exact AST executes once, missing reload preserves old state, result survives owner death PASS");
}
