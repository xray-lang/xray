/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_projection_cases.h - Public-only data projections after producer death
 */
#ifndef SOURCE_PUBLIC_PROJECTION_CASES_H
#define SOURCE_PUBLIC_PROJECTION_CASES_H
static XrSourceValue *project_result(XrSourceResult *result) {
    XrSourceValue *value=NULL;CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK && value);
    xray_source_result_free(result);return value;
}
static void projection_bytes(const XrSourceValue *value,const char *expected,size_t size) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_value_bytes(value,&bytes,&length)==XR_SOURCE_OK && length==size && !memcmp(bytes,expected,size));
}
static void projection_cases(void) {
    static const char source[]="export fn tupleValue()->(string,(),i64?,(bool,string)){return (\"a\\0中z\",(),41,(true,\"nested\"))}\n"
        "export fn absent()->string?{return null}\n"
        "export fn assertion()->PanicInfo{try{assert(false,\"panic\\0中\")}catch panic(p){return p};return assertion()}\n"
        "export fn intrinsic(divisor:i64)->PanicInfo{try{print(10/divisor)}catch panic(p){return p};return intrinsic(divisor)}\n";
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,source,"public-projections");
    XrSourceInvocation request=invocation("tupleValue",NULL,0);XrSourceValue *tuple=project_result(invoke(program,&request,NULL));
    request=invocation("absent",NULL,0);XrSourceValue *absent=project_result(invoke(program,&request,NULL));
    request=invocation("assertion",NULL,0);XrSourceValue *assertion=project_result(invoke(program,&request,NULL));
    const XrSourceScalar scalar={.kind=XR_SOURCE_VALUE_I64,.value.signed_value=0};XrSourceValue *zero=NULL;
    CHECK(xray_source_value_new_scalar(compiler,&scalar,&zero)==XR_SOURCE_OK);
    const XrSourceValue *arguments[]={zero};request=invocation("intrinsic",arguments,1);
    XrSourceValue *intrinsic=project_result(invoke(program,&request,NULL));
    xray_source_value_free(zero);xray_source_program_free(program);xray_source_compiler_free(compiler);
    size_t count=37;CHECK(xray_source_value_tuple_count(tuple,&count)==XR_SOURCE_OK && count==4);
    XrSourceValue *text=NULL,*unit=NULL,*nullable=NULL,*nested=NULL,*present=NULL;
    CHECK(xray_source_value_tuple_get(tuple,0,&text)==XR_SOURCE_OK);
    CHECK(xray_source_value_tuple_get(tuple,1,&unit)==XR_SOURCE_OK);
    CHECK(xray_source_value_tuple_get(tuple,2,&nullable)==XR_SOURCE_OK);
    CHECK(xray_source_value_tuple_get(tuple,3,&nested)==XR_SOURCE_OK);
    XrSourceValue *output=NULL;CHECK(xray_source_value_tuple_get(tuple,4,&output)==XR_SOURCE_BAD_ARGUMENT && !output);
    CHECK(xray_source_value_tuple_get(tuple,SIZE_MAX,&output)==XR_SOURCE_LIMIT && !output);
    XrSourceValue *occupied=unit;CHECK(xray_source_value_tuple_get(tuple,0,&occupied)==XR_SOURCE_BAD_ARGUMENT && occupied==unit);
    xray_source_value_free(tuple);projection_bytes(text,"a\0中z",sizeof("a\0中z")-1);
    XrSourceValueKind kind;CHECK(xray_source_value_kind(unit,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_UNIT);
    CHECK(xray_source_value_tuple_count(nested,&count)==XR_SOURCE_OK && count==2);
    CHECK(xray_source_value_tuple_get(nested,1,&output)==XR_SOURCE_OK);xray_source_value_free(nested);
    projection_bytes(output,"nested",6);xray_source_value_free(output);output=NULL;
    int some=37;CHECK(xray_source_value_nullable(nullable,&some,&present)==XR_SOURCE_OK && some==1 && present);
    xray_source_value_free(nullable);XrSourceScalar actual;
    CHECK(xray_source_value_scalar(present,&actual)==XR_SOURCE_OK && actual.kind==XR_SOURCE_VALUE_I64 && actual.value.signed_value==41);
    some=37;CHECK(xray_source_value_nullable(absent,&some,&output)==XR_SOURCE_OK && some==0 && !output);
    occupied=present;some=37;
    CHECK(xray_source_value_nullable(absent,&some,&occupied)==XR_SOURCE_BAD_ARGUMENT && some==37 && occupied==present);
    CHECK(xray_source_value_nullable(text,&some,&output)==XR_SOURCE_INVALID && some==37 && !output);
    count=37;CHECK(xray_source_value_tuple_count(text,&count)==XR_SOURCE_INVALID && count==37);
    CHECK(xray_source_value_tuple_get(text,0,&output)==XR_SOURCE_INVALID && !output);
    int64_t code=37;CHECK(xray_source_value_panic_code(assertion,&code)==XR_SOURCE_OK && code==445);
    CHECK(xray_source_value_panic_message(assertion,&output)==XR_SOURCE_OK);xray_source_value_free(assertion);
    projection_bytes(output,"panic\0中",sizeof("panic\0中")-1);xray_source_value_free(output);output=NULL;
    CHECK(xray_source_value_panic_code(intrinsic,&code)==XR_SOURCE_OK && code==420);
    CHECK(xray_source_value_panic_message(intrinsic,&output)==XR_SOURCE_OK);xray_source_value_free(intrinsic);
    projection_bytes(output,"division by zero",16);xray_source_value_free(output);output=NULL;
    code=37;CHECK(xray_source_value_panic_code(text,&code)==XR_SOURCE_INVALID && code==37);
    CHECK(xray_source_value_panic_message(text,&output)==XR_SOURCE_INVALID && !output);
    xray_source_value_free(text);xray_source_value_free(unit);xray_source_value_free(absent);xray_source_value_free(present);
    puts("public Tuple/Nullable/PanicInfo projections own exact data after producer/parent death; refusal outputs preserved");
}
#endif // SOURCE_PUBLIC_PROJECTION_CASES_H
