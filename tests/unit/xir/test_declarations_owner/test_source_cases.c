/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_cases.c - Actual Source validation and sticky initialization
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_enum.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static XrXirOutputStatus output(void *context,const XrXirOutputGroup *group) {
    unsigned *count=context;CHECK(group && group->stream==XR_XIR_STDOUT);++*count;return XR_XIR_OUTPUT_OK;
}
static void initialization(XrXirSourceProduct *product) {
    const XrXirSourceTests *tests=xr_xir_compile_source_product_tests(product);
    CHECK(tests->count==2);uint32_t before=tests->entries[0].function,test=tests->entries[1].function;
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    unsigned outputs=0;config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,output,&outputs};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start_test(instance,before)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll(instance);CHECK(result.outcome.status==XR_XIR_CALL_THROWN && outputs==1);
    for(unsigned i=0;i<3;++i) {
        CHECK(xr_xir_instance_start_test(instance,test)==XR_XIR_CALL_THROWN);
        CHECK(xr_xir_instance_start_test(instance,before)==XR_XIR_CALL_THROWN);
        CHECK(xr_xir_instance_poll(instance).epoch==result.epoch && outputs==1);
    }
    XrXirCallResult owned={0};CHECK(xr_xir_instance_copy_failure(instance,&owned)==XR_XIR_CALL_THROWN);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    XrXirEnumBorrow view={0};CHECK(xr_xir_enum_borrow(&owned.value,&view)==XR_XIR_VALUE_OK);
    CHECK(view.name.length==7 && !memcmp(view.name.bytes,"Failure",7));
    CHECK(view.member.length==6 && !memcmp(view.member.bytes,"Broken",6) && view.field_count==1);
    const char expected[]="owned initialization failure after Program and product destruction";
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&view.fields[0],&bytes,&length) && length==sizeof(expected)-1 &&
        !memcmp(bytes,expected,sizeof(expected)-1));
    xr_xir_call_result_drop(&owned);puts("initialization sticky; copied failure survives all producers");
}
int main(int argc,char **argv) {
    CHECK(argc==6);XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,argv[1]};
    const XrXirSourceProductRequest request={{session,argv[2],&authority,&context,argv[3],NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    fprintf(stderr,"status=%u stage=%u line=%d column=%d %s\n",status,diagnostic.stage,
        diagnostic.source.line,diagnostic.source.column,diagnostic.source.message);
    CHECK(status==(XrXirStatus)strtoul(argv[4],NULL,10));
    CHECK((status==XR_XIR_OK)==(product!=NULL));
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);xr_compile_resources_release(resources);
    if(!product)return 0;
    if(!strcmp(argv[5],"initialization")) {initialization(product);return 0;}
    const XrXirSourceTests *tests=xr_xir_compile_source_product_tests(product);
    printf("count=%u\n",tests->count);
    for(uint32_t i=0;i<tests->count;++i)printf("%s:%u:%u\n",tests->entries[i].name,
        tests->entries[i].role,tests->entries[i].timeout_seconds);
    if(!strcmp(argv[5],"export")) {
        CHECK(tests->count==1);uint32_t entry=tests->entries[0].function;
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        xr_xir_compile_program_drop(program);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_source_product_free(product);return 0;
}
