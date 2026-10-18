/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_receiver_bounds.h - Opaque return advertisements keep their upper bound
 */
#ifndef XIR_GENERATOR_RECEIVER_BOUNDS_H
#define XIR_GENERATOR_RECEIVER_BOUNDS_H
static void receiver_opaque_return_cases(void) {
    static const char prefix[]=
        "import time\n"
        "struct Value {value:i64\n"
        " iterator()->Iterator<i64> {defer {print(401)}\n"
        " time.sleep(5)\n"
        " yield this.value}}\n"
        "final class Object {const value:i64\n"
        " init(value:i64){this.value=value}\n"
        " iterator()->Iterator<i64> {defer {print(401)}\n"
        " time.sleep(5)\n"
        " yield this.value}}\n"
        "fn gather(it:Iterator<i64>)->i64 {var sum=0\n"
        " for(value in it){sum=sum+value}\n"
        " return sum}\n";
    const char *receiver[]={"Value{value:7}","Object(7)"};
    for(unsigned mode=0;mode<2;++mode) {
        char text[4096];int length=snprintf(text,sizeof(text),
            "%sfn escape()->fn()->Iterator<i64> {const receiver=%s\n return receiver.iterator}\n"
            "fn worker()->i64 {const make=escape()\n try{return gather(make())} catch(error){return -1}}\n"
            "export fn run()->i64{return await go worker()}\nfn main(){}\n",prefix,receiver[mode]);
        CHECK(length>0 && (size_t)length<sizeof(text));SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-receiver-opaque",NULL};
        XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,text,(size_t)length},&result,&diagnostic,NULL);
        if(status!=XR_XIR_BAD_TYPE || !strstr(diagnostic.message,"GO target lacks proof for worker execution"))
            fprintf(stderr,"Receiver negative case%u receiver=%s status%u diagnostic%u location=%u:%d:%d checked=%u snapshot=%u message=%s\n",
                mode,receiver[mode],(unsigned)status,(unsigned)diagnostic.status,diagnostic.module,
                diagnostic.line,diagnostic.column,(unsigned)(result.checked!=NULL),(unsigned)(result.snapshot!=NULL),diagnostic.message);
    CHECK(status==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot &&
            strstr(diagnostic.message,"GO target lacks proof for worker execution"));
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);source_fixture_owner_free(&owner);
    }
    puts("Generator receiver opaque returns:2 exact Source worker refusals PASS");
}
static void receiver_origin_cases(void) {
    static const char prefix[]=
        "struct Value {value:i64\n"
        " iterator()->Iterator<i64> {yield this.value}}\n"
        "final class Object {const value:i64\n init(value:i64){this.value=value}\n"
        " iterator()->Iterator<i64> {yield this.value}}\n";
    const char *worker[]={
        "fn worker()->i64 {const receiver=Value{value:7}\n"
        " var make=receiver.iterator\n make=receiver.iterator\n"
        " try{const unused=make()\n return 0} catch(error){return -1}}\n",
        "fn factory()->Iterator<i64> {const receiver=Value{value:7}\n return receiver.iterator()}\n"
        "fn worker()->i64 {var sum=0\n for(value in factory()){sum=sum+value}\n return sum}\n",
        "fn worker()->i64 {const receiver=Object(7)\n"
        " var make=receiver.iterator\n make=receiver.iterator\n"
        " try{const unused=make()\n return 0} catch(error){return -1}}\n",
        "fn factory()->Iterator<i64> {const receiver=Object(7)\n return receiver.iterator()}\n"
        "fn worker()->i64 {var sum=0\n for(value in factory()){sum=sum+value}\n return sum}\n"};
    for(unsigned mode=0;mode<4;++mode) {
        char text[2048];int length=snprintf(text,sizeof(text),
            "%s%s export fn run()->i64{return await go worker()}\nfn main(){}\n",prefix,worker[mode]);
        CHECK(length>0 && (size_t)length<sizeof(text));SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-value-origins",NULL};
        XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,text,(size_t)length},&result,&diagnostic,NULL);
        if(status!=XR_XIR_OK) fprintf(stderr,"Receiver origin case%u status%u message=%s\n",mode,(unsigned)status,diagnostic.message);
        CHECK(status==XR_XIR_OK && result.checked);
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);source_fixture_owner_free(&owner);
    }
    puts("Generator Value/Object receivers:4 local Callable Cell and returned Iterator Source cases PASS");
}
#endif
