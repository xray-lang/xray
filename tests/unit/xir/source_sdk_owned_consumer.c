/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_owned_consumer.c - Owning public data after all producer deaths
 */
#include "xray_source_ast.h"
#include "xray_source_instance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
static XrSourceInvocation request(const char *name,const XrSourceValue *const *arguments,size_t count) {
    XrSourceInvocation result;
    CHECK(xray_source_invocation_init(&result,sizeof(result))==XR_SOURCE_OK);
    result.name=name;result.name_length=strlen(name);result.arguments=arguments;result.argument_count=count;return result;
}
static XrSourceValue *execute(XrSourceInstance *owner,const XrSourceInvocation *invocation) {
    XrSourceResult *result=NULL;XrSourceValue *value=NULL;XrSourceEvent event;
    CHECK(xray_source_instance_execute(owner,invocation,&result,NULL)==XR_SOURCE_OK && result);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK && value);
    xray_source_result_free(result);return value;
}
static void integer(const XrSourceValue *value,int64_t expected) {
    XrSourceScalar actual;
    CHECK(xray_source_value_scalar(value,&actual)==XR_SOURCE_OK && actual.kind==XR_SOURCE_VALUE_I64 && actual.value.signed_value==expected);
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    char source[]="fn initial()->i64{return 40}\nvar state=initial()\n"
        "export fn next(delta:i64)->i64{state+=delta;return state}\n"
        "export fn read()->i64{return state}\n"
        "export fn reader()->fn(i64)->i64{return fn(extra:i64)->i64{return state+extra}}\n"
        "export fn echo(value:string)->string{return value}\n"
        "export fn parts()->(i64,string){return (state,\"stable\")}\n";
    XrSourceCompilerOptions options;
    CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);options.stdlib_path=argv[1];
    XrSourceCompiler *compiler=NULL;XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK && compiler);
    XrSourceText text={NULL,"installed-owned-api",source,sizeof(source)-1};XrSourceAst *ast=NULL;
    XrSourceStatus parsed=xray_source_parse_text(compiler,&text,&ast,&diagnostic);
    if(diagnostic){XrSourceDiagnosticInfo detail;CHECK(xray_source_diagnostic_info(diagnostic,&detail)==XR_SOURCE_OK);fprintf(stderr,"parse=%u line=%d col=%d %s\n",(unsigned)parsed,detail.line,detail.column,detail.message);}
    else if(parsed!=XR_SOURCE_OK)fprintf(stderr,"parse=%u without diagnostic\n",(unsigned)parsed);
    CHECK(parsed==XR_SOURCE_OK && ast && !diagnostic);
    memset(source,0xa5,sizeof(source));XrSourceProgram *program=NULL;
    XrSourceStatus compiled=xray_source_compile_ast(compiler,ast,&program,&diagnostic);
    if(diagnostic){XrSourceDiagnosticInfo detail;CHECK(xray_source_diagnostic_info(diagnostic,&detail)==XR_SOURCE_OK);fprintf(stderr,"compile=%u %s\n",(unsigned)compiled,detail.message);}
    CHECK(compiled==XR_SOURCE_OK && program && !diagnostic);xray_source_ast_free(ast);
    XrSourceScalar scalar;memset(&scalar,0,sizeof(scalar));scalar.kind=XR_SOURCE_VALUE_I64;scalar.value.signed_value=2;
    XrSourceValue *two=NULL,*string=NULL;
    CHECK(xray_source_value_new_scalar(compiler,&scalar,&two)==XR_SOURCE_OK && two);
    const unsigned char binary[]={ 'A',0,0xe4,0xb8,0xad };
    CHECK(xray_source_value_new_string(compiler,(const char *)binary,sizeof(binary),&string)==XR_SOURCE_OK && string);
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    XrSourceInstance *first=NULL,*other=NULL;
    CHECK(xray_source_instance_new(program,&runtime,&first)==XR_SOURCE_OK && first);
    CHECK(xray_source_instance_new(program,&runtime,&other)==XR_SOURCE_OK && other);
    xray_source_program_free(program);xray_source_compiler_free(compiler);
    const XrSourceValue *arguments[]={two},*strings[]={string};
    XrSourceInvocation next=request("next",arguments,1),read=request("read",NULL,0),echo=request("echo",strings,1);
    XrSourceValue *answer=execute(first,&next),*independent=execute(other,&read),*echoed=execute(first,&echo);
    integer(answer,42);integer(independent,40);xray_source_value_free(string);
    XrSourceInvocation parts=request("parts",NULL,0),reader=request("reader",NULL,0);
    XrSourceValue *tuple=execute(first,&parts),*function=execute(first,&reader),*function_copy=NULL;
    CHECK(xray_source_value_copy(function,&function_copy)==XR_SOURCE_OK && function_copy);xray_source_value_free(function);
    XrSourceCall *call=NULL;
    CHECK(xray_source_instance_begin_function(other,function_copy,arguments,1,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    CHECK(xray_source_instance_begin_function(first,two,arguments,1,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    CHECK(xray_source_instance_begin_function(first,function_copy,arguments,1,&call)==XR_SOURCE_OK && call);
    XrSourceInstance *held=first;CHECK(xray_source_instance_drop(&first,NULL)==XR_SOURCE_BUSY && first==held);
    xray_source_value_free(two);xray_source_value_free(function_copy);
    XrSourceEvent event;unsigned steps=0;
    do {CHECK(xray_source_call_poll(call,64,&event)==XR_SOURCE_OK && ++steps<10000);}while(event.state==XR_SOURCE_RUN_READY);
    CHECK(event.state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *result=NULL;XrSourceValue *forty_four=NULL;
    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_result_value(result,&forty_four)==XR_SOURCE_OK && forty_four);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);xray_source_result_free(result);
    CHECK(xray_source_instance_drop(&first,NULL)==XR_SOURCE_OK && !first);
    CHECK(xray_source_instance_drop(&other,NULL)==XR_SOURCE_OK && !other);
    integer(answer,42);integer(independent,40);integer(forty_four,44);
    const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_value_bytes(echoed,&bytes,&length)==XR_SOURCE_OK && length==sizeof(binary) && !memcmp(bytes,binary,length));
    size_t fields=0;XrSourceValue *field=NULL;
    CHECK(xray_source_value_tuple_count(tuple,&fields)==XR_SOURCE_OK && fields==2);
    CHECK(xray_source_value_tuple_get(tuple,1,&field)==XR_SOURCE_OK && field);xray_source_value_free(tuple);
    CHECK(xray_source_value_bytes(field,&bytes,&length)==XR_SOURCE_OK && length==6 && !memcmp(bytes,"stable",6));
    xray_source_value_free(field);xray_source_value_free(echoed);xray_source_value_free(answer);
    xray_source_value_free(independent);xray_source_value_free(forty_four);
    puts("Installed owning AST, typed values, persistent state, callable authority and producer death PASS");return 0;
}
