/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public_values.c - Public owned data and actual typed invocation
 */
#include "xray_source_entry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef struct Output {char bytes[512];size_t length;} Output;
static XrSourceWriteStatus collect(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    Output *out=context;CHECK(stream==XR_SOURCE_STDOUT);
    if(length>sizeof(out->bytes)-out->length)return XR_SOURCE_WRITE_LIMIT;
    memcpy(out->bytes+out->length,bytes,length);out->length+=length;return XR_SOURCE_WRITE_OK;
}
static XrSourceCompiler *compiler_new(void) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=UINT64_C(256)<<20;
    options.live_bytes=UINT64_C(64)<<20;options.work=UINT64_C(512000000);
    XrSourceCompiler *compiler=NULL;CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);return compiler;
}
static XrSourceProgram *compile(XrSourceCompiler *compiler,const char *source,const char *path) {
    XrSourceText text={NULL,path,source,strlen(source)};XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_compile_text(compiler,&text,&program,&diagnostic);
    if(diagnostic) {XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
        fprintf(stderr,"compile status=%u: %s\n",(unsigned)status,info.message);}
    CHECK(status==XR_SOURCE_OK && program && !diagnostic);return program;
}
static XrSourceInvocation invocation(const char *name,const XrSourceValue *const *arguments,size_t count) {
    XrSourceInvocation request;CHECK(xray_source_invocation_init(&request,sizeof(request))==XR_SOURCE_OK);
    request.name=name;request.name_length=strlen(name);request.arguments=arguments;request.argument_count=count;return request;
}
static XrSourceResult *invoke(XrSourceProgram *program,const XrSourceInvocation *request,const XrSourceCallOptions *options) {
    XrSourceCallOptions defaults;
    if(!options){CHECK(xray_source_call_options_init(&defaults,sizeof(defaults))==XR_SOURCE_OK);options=&defaults;}
    XrSourceResult *result=NULL;XrSourceStatus status=xray_source_invoke(program,request,options,&result,NULL);
    if(status!=XR_SOURCE_OK)fprintf(stderr,"invoke %.*s status=%u\n",(int)request->name_length,request->name,(unsigned)status);
    CHECK(status==XR_SOURCE_OK && result);XrSourceEvent event;
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);return result;
}
static void scalar_equal(const XrSourceScalar *a,const XrSourceScalar *b) {
    CHECK(a->kind==b->kind);
    if(a->kind>=XR_SOURCE_VALUE_I8 && a->kind<=XR_SOURCE_VALUE_I64)CHECK(a->value.signed_value==b->value.signed_value);
    else if(a->kind>=XR_SOURCE_VALUE_U8 && a->kind<=XR_SOURCE_VALUE_U64)CHECK(a->value.unsigned_value==b->value.unsigned_value);
    else if(a->kind==XR_SOURCE_VALUE_BOOL)CHECK(a->value.boolean==b->value.boolean);
    else if(a->kind==XR_SOURCE_VALUE_RUNE)CHECK(a->value.rune==b->value.rune);
    else if(a->kind==XR_SOURCE_VALUE_F32)CHECK(!memcmp(&a->value.f32,&b->value.f32,sizeof(float)));
    else if(a->kind==XR_SOURCE_VALUE_F64)CHECK(!memcmp(&a->value.f64,&b->value.f64,sizeof(double)));
}
static void scalars(void) {
    static const XrSourceScalar cases[]={
        {.kind=XR_SOURCE_VALUE_UNIT},{.kind=XR_SOURCE_VALUE_BOOL,.value.boolean=1},
        {.kind=XR_SOURCE_VALUE_I8,.value.signed_value=INT8_MIN},{.kind=XR_SOURCE_VALUE_I16,.value.signed_value=INT16_MAX},
        {.kind=XR_SOURCE_VALUE_I32,.value.signed_value=INT32_MIN},{.kind=XR_SOURCE_VALUE_I64,.value.signed_value=INT64_MIN},
        {.kind=XR_SOURCE_VALUE_U8,.value.unsigned_value=UINT8_MAX},{.kind=XR_SOURCE_VALUE_U16,.value.unsigned_value=UINT16_MAX},
        {.kind=XR_SOURCE_VALUE_U32,.value.unsigned_value=UINT32_MAX},{.kind=XR_SOURCE_VALUE_U64,.value.unsigned_value=UINT64_MAX},
        {.kind=XR_SOURCE_VALUE_F32,.value.f32=-0.0f},{.kind=XR_SOURCE_VALUE_F64,.value.f64=-0.0},
        {.kind=XR_SOURCE_VALUE_RUNE,.value.rune=0x10ffff}
    };
    static const char *names[]={"unit","truth","i8Value","i16Value","i32Value","i64Value","u8Value","u16Value",
        "u32Value","u64Value","f32Value","f64Value","runeValue"};
    static const char source[]="export fn unit()->(){return ()}\n"
        "export fn truth(value:bool)->bool{return value}\n"
        "export fn i8Value(value:i8)->i8{return value}\nexport fn i16Value(value:i16)->i16{return value}\n"
        "export fn i32Value(value:i32)->i32{return value}\nexport fn i64Value(value:i64)->i64{return value}\n"
        "export fn u8Value(value:u8)->u8{return value}\nexport fn u16Value(value:u16)->u16{return value}\n"
        "export fn u32Value(value:u32)->u32{return value}\nexport fn u64Value(value:u64)->u64{return value}\n"
        "export fn f32Value(value:f32)->f32{return value}\nexport fn f64Value(value:f64)->f64{return value}\n"
        "export fn runeValue(value:rune)->rune{return value}\n";
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,source,"public-scalars");
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        XrSourceValue *value=NULL,*copy=NULL,*returned=NULL;
        CHECK(xray_source_value_new_scalar(compiler,&cases[i],&value)==XR_SOURCE_OK && value);
        CHECK(xray_source_value_copy(value,&copy)==XR_SOURCE_OK && copy);
        xray_source_value_free(value);const XrSourceValue *arguments[]={copy};XrSourceInvocation request=invocation(names[i],i?arguments:NULL,i?1:0);
        XrSourceResult *result=invoke(program,&request,NULL);
        CHECK(xray_source_result_value(result,&returned)==XR_SOURCE_OK && returned);xray_source_result_free(result);
        XrSourceScalar actual;CHECK(xray_source_value_scalar(returned,&actual)==XR_SOURCE_OK);scalar_equal(&cases[i],&actual);
        xray_source_value_free(copy);xray_source_value_free(returned);
    }
    const uint32_t f32bits=UINT32_C(0x7fc00000);const uint64_t f64bits=UINT64_C(0x7ff0000000000001);
    XrSourceScalar nan={.kind=XR_SOURCE_VALUE_F32};memcpy(&nan.value.f32,&f32bits,sizeof(f32bits));
    XrSourceValue *value=NULL;CHECK(xray_source_value_new_scalar(compiler,&nan,&value)==XR_SOURCE_OK);
    XrSourceScalar actual;CHECK(xray_source_value_scalar(value,&actual)==XR_SOURCE_OK);scalar_equal(&nan,&actual);xray_source_value_free(value);value=NULL;
    nan.kind=XR_SOURCE_VALUE_F64;memcpy(&nan.value.f64,&f64bits,sizeof(f64bits));
    CHECK(xray_source_value_new_scalar(compiler,&nan,&value)==XR_SOURCE_OK);
    const XrSourceValue *arguments[]={value};XrSourceInvocation request=invocation("f64Value",arguments,1);
    XrSourceResult *result=invoke(program,&request,NULL);XrSourceValue *returned=NULL;
    CHECK(xray_source_result_value(result,&returned)==XR_SOURCE_OK);xray_source_result_free(result);
    xray_source_value_free(value);xray_source_program_free(program);xray_source_compiler_free(compiler);
    CHECK(xray_source_value_scalar(returned,&actual)==XR_SOURCE_OK);scalar_equal(&nan,&actual);xray_source_value_free(returned);
    puts("public primitive scalar types=13; Unit result plus 12 typed arguments, exact integer/float bits passed");
}
static void refusals(void) {
    XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    XrSourceCompiler *compiler=compiler_new();
    static const XrSourceScalar invalid[]={
        {.kind=XR_SOURCE_VALUE_I8,.value.signed_value=128},{.kind=XR_SOURCE_VALUE_I16,.value.signed_value=-32769},
        {.kind=XR_SOURCE_VALUE_I32,.value.signed_value=INT64_MAX},{.kind=XR_SOURCE_VALUE_U8,.value.unsigned_value=256},
        {.kind=XR_SOURCE_VALUE_U16,.value.unsigned_value=65536},{.kind=XR_SOURCE_VALUE_U32,.value.unsigned_value=UINT64_MAX},
        {.kind=XR_SOURCE_VALUE_BOOL,.value.boolean=2},{.kind=XR_SOURCE_VALUE_RUNE,.value.rune=0xd800},
        {.kind=XR_SOURCE_VALUE_RUNE,.value.rune=0x110000}
    };
    XrSourceValue *value=NULL;
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
        CHECK(xray_source_value_new_scalar(compiler,&invalid[i],&value)==XR_SOURCE_INVALID && !value);
    XrSourceScalar nan={.kind=XR_SOURCE_VALUE_F32};uint32_t bits=UINT32_C(0x7f800001);memcpy(&nan.value.f32,&bits,sizeof(bits));
    CHECK(xray_source_value_new_scalar(compiler,&nan,&value)==XR_SOURCE_INVALID && !value);
    CHECK(xray_source_value_new_string(compiler,"\xc0\x80",2,&value)==XR_SOURCE_INVALID && !value);
    CHECK(xray_source_value_new_string(compiler,NULL,1,&value)==XR_SOURCE_BAD_ARGUMENT && !value);
    CHECK(xray_source_value_new_string(compiler,"",SIZE_MAX,&value)==XR_SOURCE_LIMIT && !value);
    CHECK(xray_source_value_new_string(compiler,NULL,0,&value)==XR_SOURCE_OK && value);
    XrSourceValue *occupied=value;CHECK(xray_source_value_copy(value,&occupied)==XR_SOURCE_BAD_ARGUMENT && occupied==value);
    XrSourceScalar saved={.kind=XR_SOURCE_VALUE_I64,.value.signed_value=37},actual;memcpy(&actual,&saved,sizeof(actual));
    CHECK(xray_source_value_scalar(value,&actual)==XR_SOURCE_INVALID && !memcmp(&actual,&saved,sizeof(actual)));
    XrSourceProgram *program=compile(compiler,"export fn echo(value:i64)->i64{return value}\n","public-refusals");
    const XrSourceValue *arguments[]={value};XrSourceInvocation request=invocation("echo",arguments,1);XrSourceCall *call=NULL;
    CHECK(xray_source_invoke_begin(program,&request,&options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    request.argument_count=0;CHECK(xray_source_invoke_begin(program,&request,&options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    request.argument_count=1;request.arguments=NULL;
    CHECK(xray_source_invoke_begin(program,&request,&options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    const XrSourceValue *missing[]={NULL};request.arguments=missing;
    CHECK(xray_source_invoke_begin(program,&request,&options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    request.api_version++;CHECK(xray_source_invoke_begin(program,&request,&options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    request.api_version--;request.struct_size--;
    CHECK(xray_source_invoke_begin(program,&request,&options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    request.struct_size++;request.argument_count=SIZE_MAX;
    CHECK(xray_source_invoke_begin(program,&request,&options,&call)==XR_SOURCE_LIMIT && !call);
    xray_source_value_free(value);xray_source_program_free(program);xray_source_compiler_free(compiler);
}
static const char aggregate_source[]="print(17)\nstruct Record{value:i64}\nenum Choice{Empty,Some{value:i64}}\nenum Failure{Bad}\n"
    "export fn array()->Array<i64>{return [41,42]}\nexport fn first(value:Array<i64>)->i64{return value[0]}\n"
    "export fn atomic()->Atomic<i64>{return Atomic<i64>(41)}\nexport fn load(value:Atomic<i64>)->i64{return value.load()}\n"
    "export fn record()->Record{return Record{value:41}}\nexport fn field(value:Record)->i64{return value.value}\n"
    "export fn choice()->Choice{return Choice.Some{value:41}}\n"
    "export fn tuple()->(i64,bool){return (41,true)}\nexport fn nullable()->i64?{return 41}\n"
    "export fn erased()->Error{return Failure.Bad}\nexport fn fail()->i64{throw erased()}\nexport fn error(value:Error)->Error{return value}\n"
    "export fn text(value:string)->string{Coro.yield();return value}\n"
    "export fn sum(a:i64,b:i64)->i64{return a+b}\n";
static void aggregates(void) {
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,aggregate_source,"public-aggregate");
    Output printed={0};XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.write=collect;options.context=&printed;
    static const struct {const char *name,*consumer;XrSourceValueKind kind;} cases[]={
        {"array","first",XR_SOURCE_VALUE_ARRAY},{"atomic","load",XR_SOURCE_VALUE_ATOMIC},
        {"record","field",XR_SOURCE_VALUE_STRUCT},{"choice",NULL,XR_SOURCE_VALUE_ENUM},
        {"tuple",NULL,XR_SOURCE_VALUE_TUPLE},{"nullable",NULL,XR_SOURCE_VALUE_NULLABLE}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        XrSourceInvocation request=invocation(cases[i].name,NULL,0);XrSourceResult *result=invoke(program,&request,&options);
        XrSourceValue *value=NULL,*copy=NULL;CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK);
        xray_source_result_free(result);CHECK(xray_source_value_copy(value,&copy)==XR_SOURCE_OK);xray_source_value_free(value);
        XrSourceValueKind kind=XR_SOURCE_VALUE_UNIT;CHECK(xray_source_value_kind(copy,&kind)==XR_SOURCE_OK && kind==cases[i].kind);
        if(cases[i].consumer) {
            const XrSourceValue *arguments[]={copy};request=invocation(cases[i].consumer,arguments,1);
            result=invoke(program,&request,&options);int64_t integer=0;
            CHECK(xray_source_result_i64(result,&integer)==XR_SOURCE_OK && integer==41);xray_source_result_free(result);
        }
        if(i==0) {Output out={0};CHECK(xray_source_value_format(copy,collect,&out)==XR_SOURCE_OK &&
            out.length==8 && !memcmp(out.bytes,"[41, 42]",8));}
        if(i==2) {
            XrSourceCompiler *other=compiler_new();XrSourceProgram *unrelated=compile(other,
                "struct Record{value:i64}\nexport fn field(value:Record)->i64{return value.value}\n","unrelated-aggregate");
            const XrSourceValue *arguments[]={copy};request=invocation("field",arguments,1);XrSourceCall *call=NULL;
            CHECK(xray_source_invoke_begin(unrelated,&request,&options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
            xray_source_program_free(unrelated);xray_source_compiler_free(other);
        }
        xray_source_value_free(copy);
    }
    XrSourceScalar scalar={.kind=XR_SOURCE_VALUE_I64,.value.signed_value=20};XrSourceValue *left=NULL,*right=NULL;
    CHECK(xray_source_value_new_scalar(compiler,&scalar,&left)==XR_SOURCE_OK);scalar.value.signed_value=22;
    CHECK(xray_source_value_new_scalar(compiler,&scalar,&right)==XR_SOURCE_OK);
    const XrSourceValue *pair[]={left,right};XrSourceInvocation sum=invocation("sum",pair,2);
    XrSourceResult *added=invoke(program,&sum,&options);int64_t total=0;
    CHECK(xray_source_result_i64(added,&total)==XR_SOURCE_OK && total==42);
    xray_source_result_free(added);xray_source_value_free(left);xray_source_value_free(right);
    XrSourceInvocation request=invocation("fail",NULL,0);XrSourceResult *result=NULL;
    CHECK(xray_source_invoke(program,&request,&options,&result,NULL)==XR_SOURCE_OK && result);
    XrSourceEvent event;CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_THROWN);
    XrSourceValue *error=NULL;CHECK(xray_source_result_value(result,&error)==XR_SOURCE_OK);xray_source_result_free(result);
    XrSourceValueKind kind;CHECK(xray_source_value_kind(error,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_ENUM);
    Output thrown={0};CHECK(xray_source_value_format(error,collect,&thrown)==XR_SOURCE_OK &&
        thrown.length==11 && !memcmp(thrown.bytes,"Failure.Bad",11));xray_source_value_free(error);error=NULL;
    request=invocation("erased",NULL,0);result=invoke(program,&request,&options);
    CHECK(xray_source_result_value(result,&error)==XR_SOURCE_OK);xray_source_result_free(result);
    CHECK(xray_source_value_kind(error,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_ERROR);
    const XrSourceValue *arguments[]={error};request=invocation("error",arguments,1);result=invoke(program,&request,&options);
    xray_source_result_free(result);xray_source_program_free(program);xray_source_compiler_free(compiler);
    Output formatted={0};CHECK(xray_source_value_format(error,collect,&formatted)==XR_SOURCE_OK &&
        formatted.length==11 && !memcmp(formatted.bytes,"Failure.Bad",11));xray_source_value_free(error);
}
static void argument_lifetime(void) {
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,aggregate_source,"public-lifetime");
    static const char expected[]="a\0中z";XrSourceValue *value=NULL;
    CHECK(xray_source_value_new_string(compiler,expected,sizeof(expected)-1,&value)==XR_SOURCE_OK);
    const XrSourceValue *arguments[]={value};XrSourceInvocation request=invocation("text",arguments,1);XrSourceCall *call=NULL;
    Output printed={0};XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.write=collect;options.context=&printed;CHECK(xray_source_invoke_begin(program,&request,&options,&call)==XR_SOURCE_OK && call);
    xray_source_value_free(value);value=NULL;xray_source_program_free(program);xray_source_compiler_free(compiler);
    unsigned yields=0;XrSourceResult *result=NULL;
    for(unsigned steps=0;steps<4096;++steps) {
        XrSourceEvent event;CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);
        if(event.state==XR_SOURCE_RUN_READY)continue;
        if(event.state==XR_SOURCE_RUN_SUSPENDED) {XrSourceWait wait;
            CHECK(xray_source_call_wait_request(call,event.epoch,event.wake,&wait,sizeof(wait))==XR_SOURCE_OK && wait.kind==XR_SOURCE_WAIT_YIELD);
            ++yields;CHECK(xray_source_call_resume(call,event.epoch,event.wake)==XR_SOURCE_OK);continue;}
        CHECK(event.state==XR_SOURCE_RUN_RETURNED);CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);break;
    }
    CHECK(result && yields==1 && printed.length==3 && !memcmp(printed.bytes,"17\n",3));
    CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK);CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK);
    xray_source_result_free(result);const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_value_bytes(value,&bytes,&length)==XR_SOURCE_OK && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    xray_source_value_free(value);
}
static void authority_values(void) {
    XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    static const char source[]="final class Box{value:i64 init(value:i64){this.value=value}}\n"
        "fn worker()->i64{return 41}\nexport fn box()->Box{return Box(41)}\n"
        "export fn callback()->fn()->i64{return worker}\n"
        "export fn task()->Task<i64>{const task=go worker();const observed=await task;assert(observed==41);return task}\n"
        "export fn readBox(value:Box)->i64{return value.value}\n"
        "export fn runCallback(value:fn()->i64)->i64{return value()}\n"
        "export fn awaitTask(value:Task<i64>)->i64{return await value}\n";
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,source,"public-authority");
    static const struct {const char *producer,*consumer;XrSourceValueKind kind;} cases[]={
        {"box","readBox",XR_SOURCE_VALUE_CLASS},{"callback","runCallback",XR_SOURCE_VALUE_CALLABLE},
        {"task","awaitTask",XR_SOURCE_VALUE_TASK}
    };
    XrSourceValue *copies[3]={0};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        XrSourceInvocation request=invocation(cases[i].producer,NULL,0);XrSourceResult *result=invoke(program,&request,NULL);
        XrSourceValue *value=NULL;CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK);
        xray_source_result_free(result);CHECK(xray_source_value_copy(value,&copies[i])==XR_SOURCE_OK);xray_source_value_free(value);
        XrSourceValueKind kind;CHECK(xray_source_value_kind(copies[i],&kind)==XR_SOURCE_OK && kind==cases[i].kind);
        const XrSourceValue *arguments[]={copies[i]};request=invocation(cases[i].consumer,arguments,1);XrSourceCall *call=NULL;
        if(i<2)CHECK(xray_source_invoke_begin(program,&request,&options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
        else {XrSourceResult *denied=NULL;XrSourceEvent failure={0};
            CHECK(xray_source_invoke(program,&request,&options,&denied,&failure)==XR_SOURCE_BAD_STATE && !denied &&
                failure.state==XR_SOURCE_RUN_API_ERROR);}
    }
    xray_source_program_free(program);xray_source_compiler_free(compiler);
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        XrSourceValueKind kind;CHECK(xray_source_value_kind(copies[i],&kind)==XR_SOURCE_OK && kind==cases[i].kind);
        xray_source_value_free(copies[i]);
    }
    puts("public Class/callable/Task data survives producers; foreign Class/callable denied; foreign Task await BAD_STATE");
}
int main(void) {scalars();refusals();aggregates();argument_lifetime();authority_values();return 0;}
