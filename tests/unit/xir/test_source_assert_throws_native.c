/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_assert_throws_native.c - Original owned catalog in generated C
 */
#include "xir/xxir_program.h"
#include "base/xresource_limits.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
extern const XrXirProgramSpec testing_demo_program;
extern const uint32_t testing_demo_functions[7];
extern const char testing_demo_source_sha256[65];
static XrXirCallStatus finish_test(XrXirInstance *instance) {
    XrXirInstanceResult result;
    unsigned slices=0;
    do {
        CHECK(++slices<=100000);
        result=xr_xir_instance_poll_bounded(instance,1024);
    } while(result.outcome.status==XR_XIR_CALL_READY);
    return result.outcome.status;
}
static void catalog_report(const char *path,const char *const names[7],const uint32_t functions[7]) {
    if(!path)return;
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fprintf(file,"{\"schema\":1,\"source\":\"demos/07-advanced/testing.xr\",\"sourceSha256\":\"%s\",\"entries\":[",testing_demo_source_sha256)>0);
    for(uint32_t i=0;i<7;++i)
        CHECK(fprintf(file,"%s{\"name\":\"%s\",\"function\":%u,\"role\":%u,\"timeout\":%u}",
            i?",":"",names[i],functions[i],i==5?2u:1u,i==6?5000u:0u)>0);
    CHECK(fputs("],\"passed\":6,\"skipped\":1,\"failed\":0}\n",file)>=0 && !fclose(file));
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&testing_demo_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    const char *names[]={"test_arithmetic","test_strings","test_arrays","test_nullable",
        "test_exceptions","test_todo","test_with_timeout"};
    for(uint32_t i=0;i<7;++i) {
        uint32_t f=testing_demo_functions[i];
        const XrXirFunctionIdentity *identity=&testing_demo_program.declarations->functions[f];
        CHECK(identity->test_role==(uint32_t)(i==5?XR_XIR_TEST_ROLE_SKIP:XR_XIR_TEST_ROLE_TEST));
        CHECK(identity->module==testing_demo_program.declarations->root_module);
        CHECK(identity->test_timeout_seconds==(i==6?5000u:0u));
        CHECK(xr_xir_instance_start(instance,f,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
        if(i==5) {CHECK(xr_xir_instance_start_test(instance,f)==XR_XIR_CALL_BAD_ARGUMENT);continue;}
        CHECK(xr_xir_instance_start_test(instance,f)==XR_XIR_CALL_READY);
        CHECK(finish_test(instance)==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_UNIT);xr_xir_value_drop(&value);
        printf("native %s PASS\n",names[i]);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    catalog_report(argc==2?argv[1]:NULL,names,testing_demo_functions);
    puts("original catalog: 6 PASS, 1 SKIP");return 0;
}
