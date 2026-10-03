/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_product_linked.c - Consume the actual source-owner library
 *
 * KEY CONCEPT:
 *   No implementation inclusion can hide a stale verifier or missing dependency.
 */
#include "program/xr_xir_source_product.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include "module/xlockfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_native_projection_expectations.h"
typedef struct LinkedOutput {
    char bytes[64];
    size_t length;
    XrXirOutputSink sink;
} LinkedOutput;
static XrXirOutputStatus linked_bytes(void *context,XrXirOutputStream stream,
                                    const char *bytes,size_t length) {
    LinkedOutput *output=context;
    CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;
    return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus linked_group(void *context,const XrXirOutputGroup *group) {
    LinkedOutput *output=context;
    return xr_xir_output_render(&output->sink,group);
}
static void reject_foreign_lockfile(const XrXirSourceRequest *request) {
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrCompileResources *foreign=NULL;
    CHECK(xr_compile_resources_new(&limits,&foreign)==XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy=xr_compile_io_policy(foreign); XrLockfile *lock=NULL;
    CHECK(xr_lockfile_new_owned(&policy,&lock)==XR_OS_IO_OK);
    XrXirSourceRequest wrong=*request; wrong.lockfile=lock;
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(request->context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_source_check(&wrong,&result,&diagnostic,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot && diagnostic.status==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_compile_resources_stats(request->context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count==after.allocation_count && before.live_bytes==after.live_bytes);
    xr_lockfile_free_owned(lock); xr_compile_resources_release(foreign);
}
int main(int argc,char **argv) {
    CHECK(argc==8 && !strcmp(argv[4],"accept"));
    bool multi=!strcmp(argv[6],"multi"); CHECK(multi || !strcmp(argv[6],"tiny"));
    XrCompileResourceLimits limits={UINT64_C(256)*1024*1024,UINT64_C(128)*1024*1024,UINT64_C(1000000000)};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,argv[1]};
    XrXirSourceProductRequest request={{session,argv[2],&authority,&context,argv[3],NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    reject_foreign_lockfile(&request.source);
    XrOsIoPolicy policy=xr_compile_io_policy(resources); XrLockfile *lock=NULL;
    CHECK(xr_lockfile_new_owned(&policy,&lock)==XR_OS_IO_OK);
    request.source.lockfile=lock;
    XrXirSourceProduct *product=NULL;
    XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"source status=%u stage=%u line=%d column=%d: %s\n",
        status,diagnostic.stage,diagnostic.source.line,diagnostic.source.column,diagnostic.source.message);
    CHECK(status==XR_XIR_OK);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_lockfile_free_owned(lock);
    xr_compile_session_free(session);memset(&authority,0xcc,sizeof(authority));memset(&request,0xcc,sizeof(request));
    memset(&context,0xcc,sizeof(context));
    const XrXirSourceView *view=xr_xir_compile_source_product_view(product);
    CHECK(view && view->complete && view->module_count && strlen(view->modules[0].path));
    if (multi) CHECK(view->module_count>=3);
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_OK);
    XrXirSourceProductPacketView packet={0};
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK && packet.bytes && packet.length);
    CHECK(xr_xir_compile_source_product_context(product)->resources==resources);
    XrXirNativeProjectionRequest projection_request={product,"linked_source",16777216};
    XrXirNativeProjection *projection=NULL;
    CHECK(xr_compile_native_projection_prepare(&projection_request,&projection,NULL)==XR_XIR_OK);
    projection_expectations_check(product,projection,true);
    FILE *file=fopen(argv[5],"wb");CHECK(file && fwrite(xr_compile_native_projection_source(projection)->text,1,xr_compile_native_projection_source(projection)->length,file)==xr_compile_native_projection_source(projection)->length && !fclose(file));
    uint32_t entry=xr_xir_compile_source_product_facts(product)->entry;XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==XR_XIR_BAD_STAGE);
    XrXirNativeProjection *unavailable=NULL,*saved=unavailable;
    CHECK(xr_compile_native_projection_prepare(&projection_request,&unavailable,NULL)==XR_XIR_BAD_STAGE);
    CHECK(!memcmp(&unavailable,&saved,sizeof(saved)));
    XrXirProgram *second=NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&second)==XR_XIR_BAD_STAGE && !second);
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK && packet.bytes && packet.length);
    xr_xir_compile_source_product_free(product);
    CHECK(xr_compile_native_projection_source(projection)->length && xr_compile_native_projection_source(projection)->text[0]);
    xr_compile_native_projection_owner_free(projection);projection=NULL;
    xr_compile_native_projection_owner_free(projection);projection=NULL;
    LinkedOutput output={0};output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,linked_bytes,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,linked_group,&output};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && !result.payload);xr_xir_value_drop(&result);
    const char *expected=multi ? "owner\xe4\xb8\xad-ok 42 true\n" : "source-product-ok\n";
    CHECK(output.length==strlen(expected) && !memcmp(output.bytes,expected,output.length));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    XrCompileResourceStats final={0};
    CHECK(xr_compile_resources_stats(resources,&final)==XR_COMPILE_RESOURCE_OK);
    CHECK(final.work>baseline.work && final.allocation_count>baseline.allocation_count);
    CHECK(final.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(resources);
    puts("actual linked source owner/verifier: dead producer, owned queries, typed stages and independent VM expectation PASS");
    return 0;
}
