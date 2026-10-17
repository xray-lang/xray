/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_input_diagnostics.c - Actual NUL validation failures at public entries
 */
#include "xray_source_ast.h"
#include "xray_source_repl.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static unsigned failures;
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);++failures;goto cleanup;}}while(0)
static const char invalid_source[]="// 中\n\0print(42)\n";
static const char imported_source[]="import \"./坏 库\" as Bad\nprint(42)\n";
static const char root_path[]=XR_INPUT_DIAGNOSTIC_FIXTURE "/entry.xr";
static const char bad_path[]=XR_INPUT_DIAGNOSTIC_FIXTURE "/坏 库.xr";
static bool same_path(const char *actual,const char *expected) {
    if(!actual)return false;
    while(*actual && *expected) {
        char a=*actual++,b=*expected++;if(a=='\\')a='/';if(b=='\\')b='/';if(a!=b)return false;
    }
    return !*actual && !*expected;
}
static bool exact_info(XrSourceDiagnostic *diagnostic,bool memory) {
    XrSourceDiagnosticInfo info={0};
    return xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && info.status==XR_SOURCE_INVALID &&
        info.line==2 && info.column==1 && !strcmp(info.message,"source text contains an embedded NUL") &&
        (memory?info.path==NULL:same_path(info.path,bad_path));
}
static XrSourceWriteStatus capture(void *context,XrSourceOutputStream stream,const char *bytes,size_t count) {
    size_t *written=context;
    if(stream!=XR_SOURCE_STDOUT || count!=3 || memcmp(bytes,"42\n",3))return XR_SOURCE_WRITE_IO;
    *written+=count;return XR_SOURCE_WRITE_OK;
}
static void compile_case(unsigned mode) {
    XrSourceCompiler *compiler=NULL;XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceAst *ast=NULL;XrSourceResult *result=NULL;
    XrSourceCompilerOptions options;XrSourceCallOptions runtime;
    size_t written=0;char bytes[128],path[sizeof(root_path)+sizeof(bad_path)];
    bool imported=mode>=3;bool memory=mode==0;
    const char *source=imported?imported_source:invalid_source;
    size_t length=imported?sizeof(imported_source)-1:sizeof(invalid_source)-1;
    memcpy(bytes,source,length);strcpy(path,imported?root_path:bad_path);
    XrSourceText input={memory?NULL:path,memory?"source-input-diagnostics":NULL,bytes,length};
    CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);
    XrSourceStatus status;
    if(mode==1 || mode==5)status=xray_source_compile_file(compiler,path,&program,&diagnostic);
    else if(mode==3) {
        CHECK(xray_source_parse_text(compiler,&input,&ast,NULL)==XR_SOURCE_OK);
        status=xray_source_compile_ast(compiler,ast,&program,&diagnostic);
    } else status=xray_source_compile_text(compiler,&input,&program,&diagnostic);
    CHECK(diagnostic && !program);
    XrSourceDiagnosticInfo info={0};CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
    printf("entry=%u status=%u location=%d:%d message=%s\n",mode,(unsigned)status,info.line,info.column,info.message);
    CHECK(status==XR_SOURCE_INVALID && exact_info(diagnostic,memory));
    XrSourceDiagnostic *held=diagnostic;
    CHECK(xray_source_compile_text(compiler,&input,&program,&held)==XR_SOURCE_BAD_ARGUMENT && held==diagnostic);
    if(mode==1 || mode==5)status=xray_source_compile_file(compiler,path,&program,NULL);
    else if(mode==3)status=xray_source_compile_ast(compiler,ast,&program,NULL);
    else status=xray_source_compile_text(compiler,&input,&program,NULL);
    CHECK(status==XR_SOURCE_INVALID && !program);
    memset(bytes,'x',sizeof(bytes));memset(path,'x',sizeof(path));xray_source_ast_free(ast);ast=NULL;
    const XrSourceText valid={NULL,"source-input-diagnostics","print(42)\n",10};
    CHECK(xray_source_compile_text(compiler,&valid,&program,NULL)==XR_SOURCE_OK);
    XrSourceProgram *held_program=program;
    CHECK(xray_source_compile_text(compiler,&valid,&program,NULL)==XR_SOURCE_BAD_ARGUMENT && program==held_program);
    xray_source_compiler_free(compiler);compiler=NULL;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);runtime.write=capture;runtime.context=&written;
    CHECK(xray_source_execute(program,&runtime,&result,NULL)==XR_SOURCE_OK && written==3);
    xray_source_program_free(program);program=NULL;xray_source_result_free(result);result=NULL;
    CHECK(exact_info(diagnostic,memory));
cleanup:
    xray_source_ast_free(ast);xray_source_compiler_free(compiler);xray_source_program_free(program);
    xray_source_result_free(result);xray_source_diagnostic_free(diagnostic);
}
static XrSourceStatus reject_repl(XrSourceRepl *owner,unsigned mode,const XrSourceText *input,
    const char *path,XrSourceDiagnostic **diagnostic) {
    if(mode==8 || mode==10)return xray_source_repl_begin_file(owner,path,diagnostic);
    if(mode>=11)return xray_source_repl_begin_type(owner,input,diagnostic);
    return xray_source_repl_begin(owner,input,diagnostic);
}
static void repl_case(unsigned mode) {
    XrSourceRepl *owner=NULL;XrSourceDiagnostic *diagnostic=NULL;XrSourceResult *result=NULL;
    XrSourceValue *value=NULL;XrSourceEvent event={0};XrSourceCompilerOptions options;XrSourceCallOptions runtime;
    bool memory=mode==6 || mode==11;bool imported=mode==9 || mode==10 || mode==13;
    size_t written=0;char bytes[128],path[sizeof(root_path)+sizeof(bad_path)];
    const char *source=imported?imported_source:invalid_source;
    size_t length=imported?sizeof(imported_source)-1:sizeof(invalid_source)-1;
    memcpy(bytes,source,length);strcpy(path,imported?root_path:bad_path);
    XrSourceText input={memory?NULL:path,memory?"source-input-diagnostics":NULL,bytes,length};
    CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;runtime.write=capture;runtime.context=&written;
    CHECK(xray_source_repl_new(&options,"source-input-diagnostics",&runtime,&owner)==XR_SOURCE_OK);
    XrSourceStatus status=reject_repl(owner,mode,&input,path,&diagnostic);
    CHECK(diagnostic && written==0);
    XrSourceDiagnosticInfo info={0};CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
    printf("entry=%u status=%u location=%d:%d message=%s\n",mode,(unsigned)status,info.line,info.column,info.message);
    CHECK(status==XR_SOURCE_INVALID && exact_info(diagnostic,memory));
    XrSourceDiagnostic *held=diagnostic;
    CHECK(reject_repl(owner,mode,&input,path,&held)==XR_SOURCE_BAD_ARGUMENT && held==diagnostic);
    CHECK(reject_repl(owner,mode,&input,path,NULL)==XR_SOURCE_INVALID && written==0);
    memset(bytes,'x',sizeof(bytes));memset(path,'x',sizeof(path));
    const char valid_source[]="const answer=42\nprint(answer)\n";
    const XrSourceText valid={NULL,"source-input-diagnostics",valid_source,sizeof(valid_source)-1};
    CHECK(xray_source_repl_begin(owner,&valid,NULL)==XR_SOURCE_OK);
    unsigned polls=0;
    do {CHECK(xray_source_repl_poll(owner,8,&event)==XR_SOURCE_OK && ++polls<100000);}
    while(event.state==XR_SOURCE_RUN_READY);
    CHECK(event.state==XR_SOURCE_RUN_RETURNED && written==3);
    CHECK(xray_source_repl_take(owner,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_repl_read(owner,"answer",&value)==XR_SOURCE_OK && value);
    CHECK(xray_source_repl_drop(&owner,&event)==XR_SOURCE_OK && !owner);
    XrSourceScalar scalar={0};
    CHECK(xray_source_value_scalar(value,&scalar)==XR_SOURCE_OK && scalar.kind==XR_SOURCE_VALUE_I64 && scalar.value.signed_value==42);
    CHECK(exact_info(diagnostic,memory));
cleanup:
    xray_source_repl_drop(&owner,&event);xray_source_diagnostic_free(diagnostic);
    xray_source_result_free(result);xray_source_value_free(value);
}
int main(void) {
    for(unsigned mode=0;mode<6;++mode)compile_case(mode);
    for(unsigned mode=6;mode<14;++mode)repl_case(mode);
    return failures?1:0;
}
