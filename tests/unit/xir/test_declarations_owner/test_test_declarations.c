/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_test_declarations.c - Source-owned roles and sealed private test starts
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
typedef struct Output { char bytes[256]; size_t length; XrXirOutputSink sink; } Output;
static XrXirOutputStatus output_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    Output *output = context;
    CHECK(stream == XR_XIR_STDOUT && length <= sizeof(output->bytes) - output->length);
    memcpy(output->bytes + output->length, bytes, length); output->length += length;
    return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus output_group(void *context, const XrXirOutputGroup *group) {
    return xr_xir_output_render(&((Output *)context)->sink, group);
}
static XrXirCallStatus finish_test(XrXirInstance *instance) {
    XrXirInstanceResult result;
    unsigned slices = 0;
    do {
        CHECK(++slices <= 1024);
        result = xr_xir_instance_poll_bounded(instance, 1);
    } while (result.outcome.status == XR_XIR_CALL_READY);
    return result.outcome.status;
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4);
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    char input[1024]; CHECK(snprintf(input, sizeof(input), "%s/basic.xr", argv[1]) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, argv[1]};
    XrXirSourceProductRequest request = {{session,input,&authority,&context,argv[2],NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"status=%u stage=%u line=%d column=%d %s\n",status,
        diagnostic.stage,diagnostic.source.line,diagnostic.source.column,diagnostic.source.message);
    CHECK(status == XR_XIR_OK);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    const XrXirSourceTests *tests = xr_xir_compile_source_product_tests(product);
    static const char *const names[] = {"prepare","before_case","first","skipped","second","after_case","finish"};
    const uint32_t roles[] = {XR_XIR_TEST_ROLE_BEFORE_ALL,XR_XIR_TEST_ROLE_BEFORE_EACH,XR_XIR_TEST_ROLE_TEST,
        XR_XIR_TEST_ROLE_SKIP,XR_XIR_TEST_ROLE_TEST,XR_XIR_TEST_ROLE_AFTER_EACH,XR_XIR_TEST_ROLE_AFTER_ALL};
    CHECK(tests && tests->count == 7 && !xr_xir_compile_source_product_tests(NULL));
    uint32_t functions[7];
    for (uint32_t i = 0; i < tests->count; ++i) {
        CHECK(tests->entries[i].role == roles[i] && !strcmp(tests->entries[i].name,names[i]));
        CHECK(tests->entries[i].name_length == strlen(names[i]));
        CHECK(tests->entries[i].timeout_seconds == (i == 4 ? 7u : 0u));
        functions[i] = tests->entries[i].function;
    }
    if (argc == 4) {
        XrXirCSource code={0};CHECK(xr_xir_compile_source_product_emit(product,"test_roles",16777216,&code)==XR_XIR_OK);
        FILE *file=fopen(argv[3],"wb");CHECK(file && fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t test_role_functions[7]={%u,%u,%u,%u,%u,%u,%u};\n",
            functions[0],functions[1],functions[2],functions[3],functions[4],functions[5],functions[6])>0);
        CHECK(!fclose(file));xr_xir_compile_c_source_free(&code);
    }
    XrXirSourceProductPacketView source_packet={0},closed_packet={0};
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_SOURCE,&source_packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&closed_packet)==XR_XIR_OK);
    XrXirArtifact *source=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_read(xr_xir_compile_source_product_context(product),source_packet.bytes,source_packet.length,&source,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(xr_xir_compile_source_product_context(product),closed_packet.bytes,closed_packet.length,&closed,NULL)==XR_XIR_OK);
    const XrXirModule *original=xr_xir_compile_artifact_module(source),*specialized=xr_xir_compile_artifact_module(closed);
    uint32_t source_first=UINT32_MAX,imported=UINT32_MAX,plain=UINT32_MAX,cleanup=UINT32_MAX;
    for(uint32_t f=0;f<original->function_count;++f)
        if(original->declarations->functions[f].module==original->declarations->root_module &&
            original->declarations->functions[f].test_role==XR_XIR_TEST_ROLE_TEST) {source_first=f;break;}
    for(uint32_t f=0;f<specialized->function_count;++f) {
        const XrXirFunctionIdentity *identity=&specialized->declarations->functions[f];
        if(identity->test_role && identity->module!=specialized->declarations->root_module) imported=f;
        if(identity->cleanup_owner) {
            CHECK(identity->test_role==XR_XIR_TEST_ROLE_NONE && !identity->test_timeout_seconds);
            cleanup=f;
        }
        if(specialized->functions[f].name_length==13 && !memcmp(specialized->functions[f].name,"private_plain",13)) plain=f;
    }
    CHECK(source_first!=UINT32_MAX && source_first!=functions[2] && imported!=UINT32_MAX && plain!=UINT32_MAX && cleanup!=UINT32_MAX);
    uint32_t initializer=specialized->declarations->modules[specialized->declarations->root_module].initializer;
    xr_xir_compile_artifact_free(source);xr_xir_compile_artifact_free(closed);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&program) == XR_XIR_OK);
    CHECK(tests == xr_xir_compile_source_product_tests(product) && !strcmp(tests->entries[2].name,"first"));
    xr_xir_compile_source_product_free(product);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
    Output output = {0}; output.sink = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,output_bytes,&output,sizeof(output.bytes)};
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,output_group,&output};
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start_test(instance,imported)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_test(instance,plain)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_test(instance,initializer)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_test(instance,cleanup)==XR_XIR_CALL_BAD_ARGUMENT);
    const XrXirSourceTestEntry forged_entry={plain,XR_XIR_TEST_ROLE_TEST,0,"forged",6};
    const XrXirSourceTests forged={&forged_entry,1};
    CHECK(xr_xir_instance_start_test(instance,forged.entries[0].function)==XR_XIR_CALL_BAD_ARGUMENT);
    for (uint32_t i = 0; i < 7; ++i) {
        CHECK(xr_xir_instance_start(instance,functions[i],NULL,0) == XR_XIR_CALL_BAD_ARGUMENT);
        if (i == 3) { CHECK(xr_xir_instance_start_test(instance,functions[i]) == XR_XIR_CALL_BAD_ARGUMENT); continue; }
        CHECK(xr_xir_instance_start_test(instance,functions[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start_test(instance,functions[i]) == XR_XIR_CALL_BUSY);
        CHECK(finish_test(instance) == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance,&value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_UNIT); xr_xir_value_drop(&value);
    }
    size_t before_cancel = output.length;
    CHECK(xr_xir_instance_start_test(instance, functions[2]) == XR_XIR_CALL_READY);
    for (unsigned slices = 0; output.length == before_cancel; ++slices) {
        CHECK(slices < 1024);
        CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY);
    }
    CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_instance_start_test(instance, functions[5]) == XR_XIR_CALL_BUSY);
    CHECK(finish_test(instance) == XR_XIR_CALL_CANCELLED);
    XrXirValue cancelled = {0};
    CHECK(xr_xir_instance_take_result(instance, &cancelled) == XR_XIR_CALL_BAD_STATE);
    CHECK(!cancelled.type && !cancelled.reserved && !cancelled.payload);
    const uint32_t after_cancel[] = {functions[5], functions[4]};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start_test(instance, after_cancel[i]) == XR_XIR_CALL_READY);
        CHECK(finish_test(instance) == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_UNIT); xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    const char expected[] = "init\nbefore_all\nbefore_each\n1\n2\nafter_each\nafter_all\n3\nafter_each\n4\n";
    CHECK(output.length == sizeof(expected)-1 && !memcmp(output.bytes,expected,sizeof(expected)-1));
    puts("owned roles, producer destruction, private authority, one initialization: PASS");
    return 0;
}
