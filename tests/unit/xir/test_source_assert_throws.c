/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_assert_throws.c - Execute the original demo through its owned test catalog
 */
#include "program/xr_xir_source_product.h"
#include "base/xresource_limits.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static XrXirCallStatus finish(XrXirInstance *instance) {
    XrXirInstanceResult result;
    unsigned slices = 0;
    do {
        CHECK(++slices < 100000);
        result = xr_xir_instance_poll_bounded(instance,1024);
    } while (result.outcome.status == XR_XIR_CALL_READY);
    if (result.outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr,"terminal status=%u panic=%u\n",result.outcome.status,result.outcome.panic.detail.code);
    return result.outcome.status;
}
static void source_sha256(const char *path,char output[65]) {
    FILE *file=fopen(path,"rb");CHECK(file);XrSHA256Context context;xr_sha256_init(&context);
    uint8_t bytes[4096],digest[32];size_t length;
    while((length=fread(bytes,1,sizeof(bytes),file)))xr_sha256_update(&context,bytes,length);
    CHECK(!ferror(file)&&!fclose(file));xr_sha256_final(&context,digest);
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){output[i*2]=hex[digest[i]>>4];output[i*2+1]=hex[digest[i]&15];}
    output[64]=0;
}
static void catalog_report(const char *path,const char *const names[7],const uint32_t functions[7],const char digest[65]) {
    if(!path)return;
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fprintf(file,"{\"schema\":1,\"source\":\"demos/07-advanced/testing.xr\",\"sourceSha256\":\"%s\",\"entries\":[",digest)>0);
    for(uint32_t i=0;i<7;++i)
        CHECK(fprintf(file,"%s{\"name\":\"%s\",\"function\":%u,\"role\":%u,\"timeout\":%u}",
            i?",":"",names[i],functions[i],i==5?2u:1u,i==6?5000u:0u)>0);
    CHECK(fputs("],\"passed\":6,\"skipped\":1,\"failed\":0}\n",file)>=0 && !fclose(file));
}
int main(int argc, char **argv) {
    CHECK(argc == 4 || argc == 5);
    XrCompileResourceLimits limits = XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(resources,&session) == XR_COMPILE_RESOURCE_OK);
    char source[2048];
    CHECK(snprintf(source,sizeof(source),"%s/demos/07-advanced/testing.xr",argv[1]) > 0);
    char digest[65];source_sha256(source,digest);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,argv[1]};
    XrXirSourceProductRequest request = {
        {session,source,&authority,&context,argv[2],NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if (status != XR_XIR_OK) {
        XrFrontendRoot error = {0};
        if (diagnostic.failure.report) xr_frontend_report_root(diagnostic.failure.report,0,&error);
        fprintf(stderr,"source status=%u stage=%u %d:%d %s\n",status,diagnostic.stage,
            error.primary.line,error.primary.column,error.message ? error.message : "");
    }
    CHECK(status == XR_XIR_OK);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);
    const XrXirSourceTests *tests = xr_xir_compile_source_product_tests(product);
    static const char *const names[] = {"test_arithmetic","test_strings","test_arrays","test_nullable",
        "test_exceptions","test_todo","test_with_timeout"};
    uint32_t functions[7];
    CHECK(tests && tests->count == 7);
    for (uint32_t i=0;i<7;++i) {
        const XrXirSourceTestEntry *test = &tests->entries[i];
        CHECK(!strcmp(test->name,names[i]) && test->name_length == strlen(names[i]));
        CHECK(test->role == (uint32_t)(i==5 ? XR_XIR_TEST_ROLE_SKIP : XR_XIR_TEST_ROLE_TEST));
        CHECK(test->timeout_seconds == (i==6 ? 5000u : 0u));
        functions[i] = test->function;
    }
    XrXirCSource code = {0};
    CHECK(xr_xir_compile_source_product_emit(product,"testing_demo",16777216,&code) == XR_XIR_OK);
    FILE *file = fopen(argv[3],"wb");
    CHECK(file && fwrite(code.text,1,code.length,file) == code.length);
    CHECK(fprintf(file,"\nconst uint32_t testing_demo_functions[7]={%u,%u,%u,%u,%u,%u,%u};\n",
        functions[0],functions[1],functions[2],functions[3],functions[4],functions[5],functions[6]) > 0);
    CHECK(fprintf(file,"const char testing_demo_source_sha256[65]=\"%s\";\n",digest)>0);
    CHECK(!fclose(file));xr_xir_compile_c_source_free(&code);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&program) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (uint32_t i=0;i<7;++i) {
        CHECK(xr_xir_instance_start(instance,functions[i],NULL,0) == XR_XIR_CALL_BAD_ARGUMENT);
        if (i==5) {
            CHECK(xr_xir_instance_start_test(instance,functions[i]) == XR_XIR_CALL_BAD_ARGUMENT);
            continue;
        }
        CHECK(xr_xir_instance_start_test(instance,functions[i]) == XR_XIR_CALL_READY);
        CHECK(finish(instance) == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance,&value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_UNIT);xr_xir_value_drop(&value);
        printf("VM %s PASS\n",names[i]);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_compile_resources_release(resources);
    catalog_report(argc==5?argv[4]:NULL,names,functions,digest);
    puts("original catalog: 6 PASS, 1 SKIP");
    return 0;
}
