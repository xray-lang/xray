/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public_diagnostics.c - Owned parser failures at actual source entries
 */
#include "xray_source_ast.h"
#include "xray_source_repl.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);++failures;goto cleanup;}}while(0)
static XrSourceWriteStatus capture(void *context,XrSourceOutputStream stream,const char *bytes,size_t count) {
    size_t *written=context;
    if(stream!=XR_SOURCE_STDOUT || count!=3 || memcmp(bytes,"42\n",3))return XR_SOURCE_WRITE_IO;
    *written+=count;return XR_SOURCE_WRITE_OK;
}
static bool same_path(const char *actual,const char *expected) {
    if(!actual)return false;
    while(*actual && *expected) {
        char a=*actual++,b=*expected++;
        if(a=='\\')a='/';
        if(b=='\\')b='/';
        if(a!=b)return false;
    }
    return !*actual && !*expected;
}
static void run_case(unsigned mode) {
    XrSourceCompiler *compiler=NULL;XrSourceProgram *program=NULL;XrSourceAst *ast=NULL;
    XrSourceDiagnostic *diagnostic=NULL;XrSourceResult *result=NULL;
    XrSourceCompilerOptions options;XrSourceCallOptions runtime;XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};
    size_t written=0;char source[128]="var bad =\n";
    char root[]=XR_DIAGNOSTIC_FIXTURE "/entry.xr";
    const char *bad=XR_DIAGNOSTIC_FIXTURE "/坏 库.xr";
    XrSourceText input={NULL,"source-diagnostic-v1",source,strlen(source)};
    CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;
    CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);
    if(mode>=2) {
        strcpy(source,"import \"./坏 库\" as Bad\nprint(42)\n");
        input=(XrSourceText){root,NULL,source,strlen(source)};
    }
    XrSourceStatus status;
    if(mode==1)status=xray_source_compile_file(compiler,root,&program,&diagnostic);
    else if(mode==3) {
        CHECK(xray_source_parse_text(compiler,&input,&ast,NULL)==XR_SOURCE_OK);
        status=xray_source_compile_ast(compiler,ast,&program,&diagnostic);
    } else status=xray_source_compile_text(compiler,&input,&program,&diagnostic);
    CHECK(diagnostic && !program);
    CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
    printf("entry=%u status=%u location=%d:%d message=%s\n",mode,(unsigned)status,info.primary.line,info.primary.column,info.message);
    CHECK(status==XR_SOURCE_INVALID);
    CHECK(info.primary.line==2 && info.primary.column==1 && !strcmp(info.message,"expected expression"));
    CHECK(mode ? same_path(info.primary.path,bad) : info.primary.path==NULL);
    if(mode==1)CHECK(xray_source_compile_file(compiler,root,&program,NULL)==XR_SOURCE_INVALID && !program);
    else if(mode==3)CHECK(xray_source_compile_ast(compiler,ast,&program,NULL)==XR_SOURCE_INVALID && !program);
    else CHECK(xray_source_compile_text(compiler,&input,&program,NULL)==XR_SOURCE_INVALID && !program);
    XrSourceDiagnostic *held=diagnostic;
    CHECK(xray_source_compile_text(compiler,&input,&program,&held)==XR_SOURCE_BAD_ARGUMENT && held==diagnostic);
    memset(source,'x',sizeof(source));
    memset(root,'x',sizeof(root));
    xray_source_ast_free(ast);ast=NULL;
    const XrSourceText valid={NULL,"source-diagnostic-v1","print(42)\n",10};
    CHECK(xray_source_compile_text(compiler,&valid,&program,NULL)==XR_SOURCE_OK);
    xray_source_compiler_free(compiler);compiler=NULL;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.write=capture;runtime.context=&written;
    CHECK(xray_source_execute(program,&runtime,&result,NULL)==XR_SOURCE_OK && written==3);
    xray_source_result_free(result);result=NULL;xray_source_program_free(program);program=NULL;
    CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
    CHECK(info.primary.line==2 && info.primary.column==1);
    CHECK(!strcmp(info.message,"expected expression") && (mode ? same_path(info.primary.path,bad) : info.primary.path==NULL));
cleanup:
    xray_source_ast_free(ast);xray_source_compiler_free(compiler);xray_source_program_free(program);
    xray_source_diagnostic_free(diagnostic);xray_source_result_free(result);
}
static void run_repl(unsigned mode) {
    XrSourceRepl *owner=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceResult *result=NULL;XrSourceValue *value=NULL;
    XrSourceCompilerOptions options;XrSourceCallOptions runtime;
    XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};XrSourceEvent event={0};size_t written=0;
    char root[]=XR_DIAGNOSTIC_FIXTURE "/entry.xr";
    const char *bad=XR_DIAGNOSTIC_FIXTURE "/坏 库.xr";
    char source[128]="var bad =\n";
    bool rooted=mode>=5 && mode<=7;
    CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;runtime.write=capture;runtime.context=&written;
    CHECK(xray_source_repl_new(&options,"source-diagnostic-v1",&runtime,&owner)==XR_SOURCE_OK);
    if(rooted)strcpy(source,"import \"./坏 库\" as Bad\nprint(42)\n");
    XrSourceText input={rooted?root:NULL,rooted?NULL:"source-diagnostic-v1",source,strlen(source)};
    XrSourceStatus status=mode==6 ? xray_source_repl_begin_file(owner,root,&diagnostic) :
        mode>=7 ? xray_source_repl_begin_type(owner,&input,&diagnostic) :
        xray_source_repl_begin(owner,&input,&diagnostic);
    CHECK(diagnostic && written==0);
    CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
    printf("entry=%u status=%u location=%d:%d message=%s\n",mode,(unsigned)status,info.primary.line,info.primary.column,info.message);
    CHECK(status==XR_SOURCE_INVALID && info.primary.line==2 && info.primary.column==1);
    CHECK(!strcmp(info.message,"expected expression") && (rooted?same_path(info.primary.path,bad):info.primary.path==NULL));
    XrSourceDiagnostic *held=diagnostic;
    CHECK(xray_source_repl_begin(owner,&input,&held)==XR_SOURCE_BAD_ARGUMENT && held==diagnostic);
    status=mode==6 ? xray_source_repl_begin_file(owner,root,NULL) :
        mode>=7 ? xray_source_repl_begin_type(owner,&input,NULL) : xray_source_repl_begin(owner,&input,NULL);
    CHECK(status==XR_SOURCE_INVALID && written==0);
    memset(source,'x',sizeof(source));memset(root,'x',sizeof(root));
    const char valid_source[]="const answer=42\nprint(answer)\n";
    const XrSourceText valid={NULL,"source-diagnostic-v1",valid_source,sizeof(valid_source)-1};
    CHECK(xray_source_repl_begin(owner,&valid,NULL)==XR_SOURCE_OK);
    unsigned polls=0;
    do {CHECK(xray_source_repl_poll(owner,8,&event)==XR_SOURCE_OK && ++polls<100000);}
    while(event.state==XR_SOURCE_RUN_READY);
    CHECK(event.state==XR_SOURCE_RUN_RETURNED && written==3);
    CHECK(xray_source_repl_take(owner,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_repl_read(owner,"answer",&value)==XR_SOURCE_OK && value);
    CHECK(xray_source_repl_drop(&owner,&event)==XR_SOURCE_OK && !owner);
    XrSourceScalar scalar={0};
    CHECK(xray_source_value_scalar(value,&scalar)==XR_SOURCE_OK && scalar.kind==XR_SOURCE_VALUE_I64 &&
        scalar.value.signed_value==42);
    CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
    CHECK(info.primary.line==2 && info.primary.column==1 && !strcmp(info.message,"expected expression"));
    CHECK(rooted?same_path(info.primary.path,bad):info.primary.path==NULL);
cleanup:
    xray_source_repl_drop(&owner,&event);xray_source_diagnostic_free(diagnostic);
    xray_source_result_free(result);xray_source_value_free(value);
}
int main(void) {
    for(unsigned mode=0;mode<4;++mode)run_case(mode);
    for(unsigned mode=4;mode<9;++mode)run_repl(mode);
    return failures?1:0;
}
