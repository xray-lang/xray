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
#include "xray_source_repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
static_assert(sizeof(XrSourceReplBindingInfo)==40,"Public REPL binding layout");
#else
_Static_assert(sizeof(XrSourceReplBindingInfo)==40,"Public REPL binding layout");
#endif
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
typedef struct SdkReplTrace {
    XrSourceRepl *owner;
    const XrSourceValue *write_value;
    char bytes[64];
    size_t count, write_callbacks;
} SdkReplTrace;
static XrSourceWriteStatus sdk_repl_output(void *context,XrSourceOutputStream stream,const char *bytes,size_t count) {
    SdkReplTrace *trace=(SdkReplTrace *)context;
    CHECK(stream==XR_SOURCE_STDOUT && count<=sizeof(trace->bytes)-trace->count);
    CHECK(xray_source_repl_cancel(trace->owner)==XR_SOURCE_BUSY);
    const XrSourceReplBindingInfo *rows=NULL;size_t row_count=0;
    CHECK(xray_source_repl_bindings(trace->owner,&rows,&row_count)==XR_SOURCE_BUSY && !rows && !row_count);
    XrSourceInputState state=XR_SOURCE_INPUT_INCOMPLETE;
    CHECK(xray_source_repl_check_input(trace->owner,"42",2,&state)==XR_SOURCE_BUSY && state==XR_SOURCE_INPUT_INCOMPLETE);
    CHECK(xray_source_repl_begin_file(trace->owner,"missing.xr",NULL)==XR_SOURCE_BUSY);
    if(trace->write_value) {
        CHECK(xray_source_repl_write(trace->owner,"value",trace->write_value)==XR_SOURCE_BUSY);
        ++trace->write_callbacks;
    }
    memcpy(trace->bytes+trace->count,bytes,count);trace->count+=count;return XR_SOURCE_WRITE_OK;
}
static XrSourceStatus sdk_repl_begin(XrSourceRepl *owner,const char *source,XrSourceDiagnostic **diagnostic) {
    XrSourceText text={NULL,"installed-repl",source,strlen(source)};
    return xray_source_repl_begin(owner,&text,diagnostic);
}
static XrSourceEvent sdk_repl_pump(XrSourceRepl *owner) {
    XrSourceEvent event;memset(&event,0,sizeof(event));unsigned polls=0;
    do {CHECK(xray_source_repl_poll(owner,8,&event)==XR_SOURCE_OK && ++polls<100000);}
    while(event.state==XR_SOURCE_RUN_READY);
    return event;
}
static XrSourceResult *sdk_repl_take(XrSourceRepl *owner,XrSourceRunState expected) {
    XrSourceResult *result=NULL;XrSourceEvent event;
    CHECK(xray_source_repl_take(owner,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==expected);
    return result;
}
static void sdk_repl_integer(XrSourceRepl *owner,const char *name,int64_t expected) {
    XrSourceValue *value=NULL;
    CHECK(xray_source_repl_read(owner,name,&value)==XR_SOURCE_OK && value);
    integer(value,expected);xray_source_value_free(value);
}
static void sdk_repl_cases(const char *stdlib_path) {
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    SdkReplTrace trace;memset(&trace,0,sizeof(trace));
    compiler.stdlib_path=stdlib_path;runtime.write=sdk_repl_output;runtime.context=&trace;runtime.output_byte_limit=sizeof(trace.bytes);
    XrSourceRepl *owner=NULL;
    CHECK(xray_source_repl_new(&compiler,"installed-repl",&runtime,&owner)==XR_SOURCE_OK && owner);trace.owner=owner;
    XrSourceText foreign={NULL,"foreign-repl","var no=0\n",9};
    CHECK(xray_source_repl_begin(owner,&foreign,NULL)==XR_SOURCE_BAD_ARGUMENT);
    char first[]="var state=40\n";
    CHECK(sdk_repl_begin(owner,first,NULL)==XR_SOURCE_OK);memset(first,0xa5,sizeof(first)-1);
    XrSourceValue *empty=NULL;
    CHECK(xray_source_repl_read(owner,"state",&empty)==XR_SOURCE_BUSY && !empty);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED));sdk_repl_integer(owner,"state",40);
    CHECK(sdk_repl_begin(owner,"state+=2\nstate\n",NULL)==XR_SOURCE_OK);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *survivor=sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED);
    int64_t answer=0;CHECK(xray_source_result_i64(survivor,&answer)==XR_SOURCE_OK && answer==42);
    sdk_repl_integer(owner,"it",42);
    CHECK(sdk_repl_begin(owner,"{state+=1\ndefer{state+=10}\nCoro.yield()\nstate+=100}\n",NULL)==XR_SOURCE_OK);
    XrSourceEvent suspended=sdk_repl_pump(owner);CHECK(suspended.state==XR_SOURCE_RUN_SUSPENDED);
    XrSourceWait wait;memset(&wait,0,sizeof(wait));
    CHECK(xray_source_repl_wait_request(owner,suspended.epoch,suspended.wake,&wait,sizeof(wait))==XR_SOURCE_OK && wait.kind==XR_SOURCE_WAIT_YIELD);
    CHECK(xray_source_repl_resume(owner,suspended.epoch,suspended.wake+1)!=XR_SOURCE_OK);
    CHECK(xray_source_repl_cancel(owner)==XR_SOURCE_OK);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_CANCELLED);
    xray_source_result_free(sdk_repl_take(owner,XR_SOURCE_RUN_CANCELLED));sdk_repl_integer(owner,"state",53);
    CHECK(xray_source_repl_resume(owner,suspended.epoch,suspended.wake)==XR_SOURCE_BAD_STATE);
    CHECK(sdk_repl_begin(owner,"{state+=1\nCoro.yield()\nstate+=2}\n",NULL)==XR_SOURCE_OK);
    suspended=sdk_repl_pump(owner);CHECK(suspended.state==XR_SOURCE_RUN_SUSPENDED);
    CHECK(xray_source_repl_resume(owner,suspended.epoch,suspended.wake)==XR_SOURCE_OK);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED));sdk_repl_integer(owner,"state",56);sdk_repl_integer(owner,"it",42);
    CHECK(sdk_repl_begin(owner,"\"kept\"\n",NULL)==XR_SOURCE_OK);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED));XrSourceValue *text=NULL;
    CHECK(xray_source_repl_read(owner,"it",&text)==XR_SOURCE_OK && text);
    XrSourceDiagnostic *diagnostic=NULL;
    CHECK(sdk_repl_begin(owner,"var failed=missingDefinition\n",&diagnostic)==XR_SOURCE_REJECTED && diagnostic);
    CHECK(trace.count==8 && !memcmp(trace.bytes,"42\nkept\n",8));
    XrSourceEvent cleanup;
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner && cleanup.state==XR_SOURCE_RUN_READY);trace.owner=NULL;
    CHECK(xray_source_result_i64(survivor,&answer)==XR_SOURCE_OK && answer==42);
    const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_value_bytes(text,&bytes,&length)==XR_SOURCE_OK && length==4 && !memcmp(bytes,"kept",4));
    XrSourceDiagnosticInfo info;
    CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && info.message && *info.message);
    xray_source_result_free(survivor);xray_source_value_free(text);xray_source_diagnostic_free(diagnostic);
    puts("Installed public REPL: typed history, busy callback, cancel/defer, resume and owned survivors PASS");
}

static const XrSourceReplBindingInfo *sdk_repl_binding(XrSourceRepl *owner,const char *name,size_t expected) {
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    CHECK(xray_source_repl_bindings(owner,&rows,&count)==XR_SOURCE_OK && count==expected);
    const XrSourceReplBindingInfo *found=NULL;
    for(size_t i=0;i<count;++i) {
        CHECK(rows[i].name && rows[i].name[0]!='$');
        if(!strcmp(rows[i].name,name)){CHECK(!found);found=&rows[i];}
    }
    return found;
}
static void sdk_repl_inspection_cases(const char *stdlib_path) {
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    SdkReplTrace trace;memset(&trace,0,sizeof(trace));
    compiler.stdlib_path=stdlib_path;runtime.write=sdk_repl_output;runtime.context=&trace;runtime.output_byte_limit=sizeof(trace.bytes);
    XrSourceRepl *owner=NULL;
    CHECK(xray_source_repl_new(&compiler,"installed-repl",&runtime,&owner)==XR_SOURCE_OK && owner);trace.owner=owner;
    CHECK(!sdk_repl_binding(owner,"it",0));
    CHECK(sdk_repl_begin(owner,"var value=40\nconst fixed=2\nfn next()->i64{value+=1;return value}\n",NULL)==XR_SOURCE_OK);
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    CHECK(xray_source_repl_bindings(owner,&rows,&count)==XR_SOURCE_BUSY && !rows && !count);
    static const char query[]="next()\n";
    XrSourceText text={NULL,"installed-repl",query,sizeof(query)-1};
    CHECK(xray_source_repl_begin_type(owner,&text,NULL)==XR_SOURCE_BUSY);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED));
    const XrSourceReplBindingInfo *row=sdk_repl_binding(owner,"value",3);
    CHECK(row && row->kind==XR_SOURCE_REPL_VALUE && !row->is_const && !row->implicit_result && !strcmp(row->type_name,"i64"));
    row=sdk_repl_binding(owner,"fixed",3);CHECK(row && row->is_const && !row->implicit_result);
    row=sdk_repl_binding(owner,"next",3);CHECK(row && row->kind==XR_SOURCE_REPL_FUNCTION && !strcmp(row->type_name,"function"));
    CHECK(sdk_repl_begin(owner,"9\n",NULL)==XR_SOURCE_OK);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED));
    const XrSourceReplBindingInfo *published=sdk_repl_binding(owner,"it",4);
    CHECK(published && published->is_const && published->implicit_result);uint64_t birth=published->generation;
    CHECK(xray_source_repl_begin_type(owner,&text,NULL)==XR_SOURCE_OK);
    CHECK(sdk_repl_pump(owner).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *survivor=sdk_repl_take(owner,XR_SOURCE_RUN_RETURNED);
    sdk_repl_integer(owner,"value",41);sdk_repl_integer(owner,"it",9);
    CHECK(published->generation==birth && !strcmp(published->name,"it"));
    row=sdk_repl_binding(owner,"it",4);CHECK(row && row->generation==birth);
    static const char declaration[]="const leaked=4\n";
    XrSourceText rejected={NULL,"installed-repl",declaration,sizeof(declaration)-1};
    CHECK(xray_source_repl_begin_type(owner,&rejected,NULL)==XR_SOURCE_REJECTED);
    CHECK(!sdk_repl_binding(owner,"leaked",4));
    CHECK(trace.count==6 && !memcmp(trace.bytes,"9\ni64\n",6));
    XrSourceEvent cleanup;
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner && cleanup.state==XR_SOURCE_RUN_READY);trace.owner=NULL;
    XrSourceValue *type=NULL;const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_result_value(survivor,&type)==XR_SOURCE_OK && type);
    CHECK(xray_source_value_bytes(type,&bytes,&length)==XR_SOURCE_OK && length==3 && !memcmp(bytes,"i64",3));
    xray_source_value_free(type);xray_source_result_free(survivor);
    puts("Installed public REPL inspection: visible bindings, nonpublishing types and owned survivors PASS");
}

#include "source_sdk_repl_input_consumer.inc.c"
#include "source_sdk_repl_array_consumer.inc.c"
#include "source_sdk_repl_write_consumer.inc.c"

int main(int argc,char **argv) {
    CHECK(argc==4);
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
    sdk_repl_cases(argv[1]);
    sdk_repl_inspection_cases(argv[1]);
    sdk_repl_rooted_cases(argv[1],argv[2]);
    sdk_repl_file_cases(argv[1],argv[3]);
    sdk_repl_snapshot_input_cases(argv[1]);
    sdk_repl_array_cases(argv[1]);
    sdk_repl_write_cases(argv[1]);
    puts("Installed owning AST, typed values, persistent state, callable authority and producer death PASS");return 0;
}
