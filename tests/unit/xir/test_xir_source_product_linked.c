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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
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
int main(int argc,char **argv) {
    CHECK(argc==8 && !strcmp(argv[4],"accept") && !strcmp(argv[6],"tiny"));
    XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,argv[1]};
    XrXirSourceProductRequest request={{session,argv[2],&authority,NULL,argv[3],NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;
    CHECK(xr_xir_source_product_build(&request,&product,NULL)==XR_XIR_OK);
    xr_compiler_session_delete(session);memset(&authority,0xcc,sizeof(authority));memset(&request,0xcc,sizeof(request));
    const XrXirSourceView *view=xr_xir_source_product_view(product);
    CHECK(view && view->complete && view->module_count && strlen(view->modules[0].path));
    CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_OK);
    XrXirSourceProductPacketView packet={0};
    CHECK(xr_xir_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK && packet.bytes && packet.length);
    XrXirCSource code={0};CHECK(xr_xir_source_product_emit(product,"linked_source",16777216,&code)==XR_XIR_OK);
    FILE *file=fopen(argv[5],"wb");CHECK(file && fwrite(code.text,1,code.length,file)==code.length && !fclose(file));
    xr_xir_c_source_free(&code);
    uint32_t entry=xr_xir_source_product_facts(product)->entry;XrXirProgram *program=NULL;
    CHECK(xr_xir_source_product_vm_take(product,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==XR_XIR_BAD_STAGE);
    CHECK(xr_xir_source_product_emit(product,"linked_source",16777216,&code)==XR_XIR_BAD_STAGE && !code.text && !code.length);
    XrXirProgram *second=NULL;
    CHECK(xr_xir_source_product_vm_take(product,(XrXirProgramBudget){33554432,64000000},&second)==XR_XIR_BAD_STAGE && !second);
    CHECK(xr_xir_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK && packet.bytes && packet.length);
    xr_xir_source_product_free(product);
    LinkedOutput output={0};output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,linked_bytes,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,linked_group,&output};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && !result.payload);xr_xir_value_drop(&result);
    CHECK(output.length==18 && !memcmp(output.bytes,"source-product-ok\n",18));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_program_drop(program);
    puts("actual linked source owner/verifier: dead producer, owned queries, typed stages and independent VM expectation PASS");
    return 0;
}
