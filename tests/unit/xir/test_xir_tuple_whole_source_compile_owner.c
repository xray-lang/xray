/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_whole_source_compile_owner.c - Complete source compiler fault ownership
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir/xxir.h"
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "tuple_runtime_cases.h"
typedef struct WholeSourceResult { XrXirProgram *program; uint32_t entry; bool keep; } WholeSourceResult;
static XrXirStatus whole_source(const XrXirCompileContext *context,void *opaque) {
    WholeSourceResult *result=opaque;
    CHECK(result && !result->program);
    size_t runtime_blocks=runtime_live,runtime_size=runtime_bytes;
    XrCompilerSession *session=NULL;XrXirSourceProduct *product=NULL;
    XrXirSourceProductDiagnostic diagnostic={0};XrXirArtifact *checked=NULL;
    XrXirCSource source={0};XrXirProgram *program=NULL;
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    XrXirStatus status=created==XR_COMPILER_SESSION_OK?XR_XIR_OK:
        created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    CHECK(created==XR_COMPILER_SESSION_OK || created==XR_COMPILER_SESSION_BUDGET || created==XR_COMPILER_SESSION_OUT_OF_MEMORY);
    if(status!=XR_XIR_OK){CHECK(!session && runtime_live==runtime_blocks && runtime_bytes==runtime_size);return status;}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TUPLE_FIXTURES};
    XrXirSourceProductRequest request={{session,XR_TUPLE_FIXTURES "/root.xr",&authority,context,
        XR_TUPLE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if(status!=XR_XIR_OK)CHECK(!product);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
    memset(&request,0xcc,sizeof(request));memset(&authority,0xcc,sizeof(authority));
    uint32_t entry=UINT32_MAX;
    if(status==XR_XIR_OK){
        CHECK(product && xr_xir_compile_source_product_view(product));
        const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(product);
        CHECK(facts && facts->module_count>=2);
        CHECK(xr_xir_compile_source_product_context(product)->resources==context->resources);
        XrXirSourceProductPacketView packet={0};
        CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
        status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL);
        if(status!=XR_XIR_OK)CHECK(!checked);
    }
    if(status==XR_XIR_OK){
        const XrXirModule *module=xr_xir_compile_artifact_module(checked);
        for(uint32_t f=0;f<module->function_count;++f){
            const XrXirFunction *function=&module->functions[f];
            if(function->name_length==4 && !memcmp(function->name,"make",4)){CHECK(entry==UINT32_MAX);entry=f;}
        }
        CHECK(entry!=UINT32_MAX);
    }
    xr_xir_compile_artifact_free(checked);
    if(status==XR_XIR_OK){
        status=xr_xir_compile_source_product_emit(product,"tuple_source",UINT64_C(8388608),&source);
        if(status!=XR_XIR_OK)CHECK(!source.text && !source.length);
    }
    xr_xir_compile_c_source_free(&source);
    if(status==XR_XIR_OK){
        status=xr_xir_compile_source_product_vm_take(product,&program);
        if(status!=XR_XIR_OK)CHECK(!program);
    }
    xr_xir_compile_source_product_free(product);
    if(status==XR_XIR_OK && result->keep){CHECK(program);result->program=program;result->entry=entry;}
    else xr_xir_compile_program_drop(program);
    if(status!=XR_XIR_OK)CHECK(!result->program);
    if(status!=XR_XIR_OK || !result->keep)CHECK(runtime_live==runtime_blocks && runtime_bytes==runtime_size);
    return status;
}
static void string_is(const XrXirValue *value,const char *expected) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length==strlen(expected) && !memcmp(bytes,expected,length));
}
static void vm_oracle(XrXirProgram *program,uint32_t entry) {
    tuple_runtime_faults(program,entry);
    XrXirValue escaped[2]={{0},{0}};
    for (uint32_t i=0;i<2;++i) {
        XrXirInstanceConfig config;XrXirInstance *instance=NULL;unsigned groups=0;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,tuple_output,&groups};
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&escaped[i])==XR_XIR_CALL_RETURNED);
        CHECK(groups==1);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (uint32_t i=0;i<2;++i) {
        XrXirValue inner={0},generic={0},number={0},text={0},unit={0};
        CHECK(xr_xir_tuple_get(&escaped[i],2,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==1323);
        CHECK(xr_xir_tuple_get(&escaped[i],0,&inner)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_tuple_get(&inner,0,&number)==XR_XIR_VALUE_BAD_ARGUMENT && number.payload==1323);
        xr_xir_value_drop(&number);
        CHECK(xr_xir_tuple_get(&inner,0,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==11);
        CHECK(xr_xir_tuple_get(&inner,1,&unit)==XR_XIR_VALUE_OK && !unit.payload);
        CHECK(xr_xir_tuple_get(&inner,3,&unit)==XR_XIR_VALUE_OK && !unit.payload);
        CHECK(xr_xir_tuple_get(&inner,2,&text)==XR_XIR_VALUE_OK);string_is(&text,"tuple-owner");
        CHECK(xr_xir_tuple_get(&escaped[i],1,&generic)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&escaped[i]);xr_xir_value_drop(&inner);string_is(&text,"tuple-owner");xr_xir_value_drop(&text);
        xr_xir_value_drop(&number);
        CHECK(xr_xir_tuple_get(&generic,0,&number)==XR_XIR_VALUE_OK && number.payload==7);
        CHECK(xr_xir_tuple_get(&generic,1,&text)==XR_XIR_VALUE_OK);string_is(&text,"generic-owner");
        xr_xir_value_drop(&generic);string_is(&text,"generic-owner");xr_xir_value_drop(&text);xr_xir_value_drop(&number);
    }
    CHECK(!runtime_live && !runtime_bytes);
    puts("VM independent Tuple oracle order1323/i64=11/generic7/strings/Unit and Program/Instance first-drop physical0 PASS");
}
int main(void) {
    XrCompileResources *unpublished=NULL;
    source_program_compile_attempts=0;source_program_compile_fail_at=0;source_program_compile_injected=false;
    CHECK(xr_compile_resources_new(&library_compile_limits,&unpublished)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    source_program_compile_fail_at=SIZE_MAX;
    CHECK(!unpublished && source_program_compile_injected && source_program_compile_attempts==1);
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    puts("Tuple receiver ledger constructor compilerOOM=1 no publication physical0 PASS");
    WholeSourceResult result={0};
    library_compile_operation_cases("Tuple whole multi-module Source/write/read/specialize/Lowered/emit/VMseal",whole_source,&result);
    CHECK(!runtime_live && !runtime_bytes);
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    result.keep=true;CHECK(whole_source(&owner.context,&result)==XR_XIR_OK && result.program);
    vm_oracle(result.program,result.entry);result.program=NULL;
    library_compile_owner_drop(&owner);library_compile_observer_free();
    CHECK(!source_program_compile_live && !source_program_compile_bytes && !runtime_live && !runtime_bytes);
    return 0;
}
