/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_projection_owner.c - Owned C before actual toolchain binding
 *
 * KEY CONCEPT:
 *   Actual Source C precedes SDK/Target binding. Captured file facts do not
 *   grant complete Target or execution authority.
 */
#include "aot/program/xr_xir_native_projection.h"
#include "toolchain/xcompiler_session.h"
#include "toolchain/xr_xir_runtime_sdk.h"
#include "app/toolchain/xtc_xir_target.h"
#include "base/xsha256.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "../../xir/xir_sdk_resource_test.h"
static char *manifest;
static size_t manifest_length;
static XrXirNativeProjection *prepare(const char *root,const char *source,const char *stdlib,
    XrCompileResources *resources) {
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceProductRequest request={{session,source,&authority,&context,stdlib,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;CHECK(xr_xir_compile_source_product_build(&request,&product,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);
    char prefix[]="bound_source";
    XrXirNativeProjectionRequest request_c={product,prefix,16777216};
    XrXirNativeProjection *owner=NULL;CHECK(xr_compile_native_projection_prepare(&request_c,&owner,NULL)==XR_XIR_OK);
    memset(prefix,'x',sizeof(prefix));memset(&context,0xCE,sizeof(context));
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    request_c.prefix="bound_source";
    XrXirNativeProjection *refused=NULL;CHECK(xr_compile_native_projection_prepare(&request_c,&refused,NULL)==XR_XIR_BAD_STAGE&&!refused);
    xr_xir_compile_source_product_free(product);xr_xir_compile_program_drop(program);
    CHECK(!strcmp(xr_compile_native_projection_facts(owner)->prefix,"bound_source"));
    return owner;
}
static XrXirStatus bounded(const char *root,const char *source,const char *stdlib,
    const XrCompileResourceLimits *limits,const XrToolchainBinding *binding,
    XrCompileResourceStats *stats,uint64_t *prepared_work) {
    XrCompileResources *resources=sdk_ledger(limits);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL;XrXirSourceProduct *product=NULL;XrXirNativeProjection *owner=NULL;
    XrCompilerSessionStatus session_status=xr_compile_session_new(resources,&session);
    XrXirStatus status=session_status==XR_COMPILER_SESSION_OK?XR_XIR_OK:
        session_status==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceProductRequest request={{session,source,&authority,&context,stdlib,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    if(status==XR_XIR_OK)status=xr_xir_compile_source_product_build(&request,&product,NULL);
    xr_compile_session_free(session);
    XrXirNativeProjectionRequest project={product,"bound_source",16777216};
    if(status==XR_XIR_OK)status=xr_compile_native_projection_prepare(&project,&owner,NULL);
    xr_xir_compile_source_product_free(product);
    if(status==XR_XIR_OK) {
        *prepared_work=sdk_stats(resources).work;size_t attempts_before=runtime_attempts;
        XrXirNativeInput output,saved;memset(&output,0xAE,sizeof(output));saved=output;
        status=xr_compile_native_projection_bind(owner,binding,&output);
        CHECK(runtime_attempts==attempts_before);
        if(status!=XR_XIR_OK)CHECK(!memcmp(&output,&saved,sizeof(output)));
    }
    *stats=sdk_stats(resources);
    xr_compile_resources_release(resources);xr_compile_native_projection_owner_free(owner);
    CHECK(!runtime_live&&!runtime_bytes);return status;
}
static void bind_limits(const char *root,const char *source,const char *stdlib,const XrToolchainBinding *binding) {
    XrCompileResourceStats baseline={0},actual={0};uint64_t prepared=0,ignored=0;
    CHECK(bounded(root,source,stdlib,&sdk_unlimited,binding,&baseline,&prepared)==XR_XIR_OK);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=sdk_unlimited;
        if(axis==0)limits.allocated_bytes=baseline.allocated_bytes-minus;
        if(axis==1)limits.live_bytes=baseline.peak_bytes-minus;
        if(axis==2)limits.work=baseline.work-minus;
        CHECK(bounded(root,source,stdlib,&limits,binding,&actual,&ignored)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
    }
    /* Walk every real submitted bind charge backwards. A rejected operation
     * leaves the prior committed work count, which selects the next boundary. */
    uint64_t boundary=baseline.work-prepared;size_t failures=0;
    while(boundary) {
        XrCompileResourceLimits limits=sdk_unlimited;limits.work=prepared+boundary-1;
        CHECK(bounded(root,source,stdlib,&limits,binding,&actual,&ignored)==XR_XIR_BUDGET);
        CHECK(ignored==prepared&&actual.work>=prepared&&actual.work<prepared+boundary);
        boundary=actual.work-prepared;++failures;
    }
    printf("prepare+bind three axes exact/minus1; %zu submitted bind work failures and physical zero PASS\n",failures);
}
static void dump_digest(const char *name,const uint8_t *digest) {
    printf("%s ",name);for(unsigned i=0;i<32;++i)printf("%02x",digest[i]);putchar('\n');
}
static XrToolchainBinding real_binding(XrCompileResources *resources,const char *sdk_root,
    const char *compiler,const char *version,const char *cpath,const XrXirNativeProjection *owner,bool alternate) {
    CHECK(xr_compile_native_projection_context(owner)->resources==resources);
    XrXirRuntimeSdkRequest sdk_request={sdk_root,manifest,manifest_length,resources};
    XrXirRuntimeSdk *sdk=NULL;CHECK(xr_xir_runtime_sdk_load(&sdk_request,&sdk)==XR_XIR_SDK_OK);
    CHECK(xr_xir_runtime_sdk_resources(sdk)==resources);
    const XrXirNativeProjectionSource *code=xr_compile_native_projection_source(owner);
    FILE *f=fopen(cpath,"wb");CHECK(f&&fwrite(code->text,1,code->length,f)==code->length&&!fclose(f));
    const char *argv[]={compiler,"/nologo","/std:c11",alternate ? "/O1" : "/O2","/c",cpath};
    XrXirTargetCommand command={sdk_root,argv,6,NULL,0};
    XrXirTargetDependency files[]={{compiler,XR_XIR_TARGET_COMPILER},{cpath,XR_XIR_TARGET_SOURCE}};
    XrXirTargetRequest target_request={resources,"x86_64-windows-msvc",3,2,11,files,2,&command,1,NULL};
    XrXirTargetSnapshot *target=NULL;CHECK(xtc_xir_target_capture(&target_request,&target)==XR_XIR_TARGET_OK);
    const XrXirTargetFacts *target_facts=xtc_xir_target_facts(target);
    bool found=false;for(uint32_t i=0;i<target_facts->file_count;++i) {
        const XrXirTargetFile *file=xtc_xir_target_file(target,i);
        if(file->kind==XR_XIR_TARGET_SOURCE) {
            CHECK(file->length==code->length&&!memcmp(file->digest,xr_compile_native_projection_facts(owner)->generated_digest.bytes,32));found=true;
        }
    }
    CHECK(found);
    XrToolchainInput input={XR_TOOLCHAIN_BINDING_SCHEMA_VERSION,XR_TOOLCHAIN_BINDING_PROVIDER_MSVC,
        {0},version,target_facts->triple,alternate ? "/nologo /std:c11 /O1 /c" : "/nologo /std:c11 /O2 /c",{{0}},{{0}},{{0}}};
    memcpy(input.sysroot_id.bytes,target_facts->sysroot_identity,32);
    memcpy(input.runtime_sdk_id.bytes,xr_xir_runtime_sdk_facts(sdk)->identity,32);
    memcpy(input.target_profile_id.bytes,target_facts->identity,32);
    XrToolchainBinding binding;CHECK(xr_compile_toolchain_binding_build(resources,&input,&binding)==XR_TOOLCHAIN_BINDING_OK);
    xtc_xir_target_free(target);xr_xir_runtime_sdk_free(sdk);
    return binding;
}
int main(int argc,char **argv) {
    CHECK(argc==8);(void)sdk_fixture_malloc;(void)sdk_fixture_free;
    char path[32768];CHECK(snprintf(path,sizeof(path),"%s/sdk_manifest.json",argv[4])>0);
    FILE *f=fopen(path,"rb");CHECK(f&&!fseek(f,0,SEEK_END));long bytes=ftell(f);CHECK(bytes>0&&!fseek(f,0,SEEK_SET));
    manifest_length=(size_t)bytes;manifest=malloc(manifest_length);CHECK(manifest);
    CHECK(fread(manifest,1,manifest_length,f)==manifest_length&&!fclose(f));
    DWORD initial_handles=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&initial_handles));
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrXirNativeProjection *owner=prepare(argv[1],argv[2],argv[3],resources);
    XrToolchainBinding binding=real_binding(resources,argv[4],argv[5],argv[6],argv[7],owner,false);
    XrToolchainBinding alternate=real_binding(resources,argv[4],argv[5],argv[6],argv[7],owner,true);
    const XrXirNativeProjectionFacts facts=*xr_compile_native_projection_facts(owner);
    const XrXirNativeProjectionSource code=*xr_compile_native_projection_source(owner);
    xr_compile_resources_release(resources);
    XrCompileResourceStats before=sdk_stats(resources);size_t allocations=runtime_attempts;
    XrXirNativeInput input;memset(&input,0xA6,sizeof(input));
    CHECK(xr_compile_native_projection_bind(owner,&binding,&input)==XR_XIR_OK);
    XrCompileResourceStats after=sdk_stats(resources);uint64_t bind_work=after.work-before.work;
    CHECK(runtime_attempts==allocations&&after.allocation_count==before.allocation_count&&after.live_bytes==before.live_bytes);
    CHECK(!memcmp(&facts,xr_compile_native_projection_facts(owner),sizeof(facts)));
    CHECK(!memcmp(input.generated_digest.bytes,facts.generated_digest.bytes,32));
    printf("WORDS %u %u %u %u %u %u %u %u %u %u\n",input.schema_version,input.checked_schema,input.checked_contract,
        input.value_abi,input.call_abi,input.program_abi,input.architecture,input.entry,input.function_count,input.module_count);
    dump_digest("SOURCE",input.source_checked_id.bytes);dump_digest("CLOSED",input.closed_checked_id.bytes);
    dump_digest("LAYOUT",input.lowered_layout_id.bytes);dump_digest("POLICY",input.codegen_policy_id.bytes);
    dump_digest("GENERATED",input.generated_digest.bytes);dump_digest("TOOLCHAIN",input.toolchain.id.bytes);dump_digest("INPUT",input.id.bytes);
    XrXirNativeInput second;
    CHECK(xr_compile_native_projection_bind(owner,&alternate,&second)==XR_XIR_OK);
    CHECK(memcmp(input.id.bytes,second.id.bytes,32)&&memcmp(input.toolchain.id.bytes,second.toolchain.id.bytes,32));
    CHECK(!memcmp(&input.source_checked_id,&second.source_checked_id,5*sizeof(XrFingerprint)));
    CHECK(!memcmp(&facts,xr_compile_native_projection_facts(owner),sizeof(facts)));
    XrToolchainBinding bad=binding;bad.schema_version=1;XrXirNativeInput untouched;memset(&untouched,0xB7,sizeof(untouched));input=untouched;
    CHECK(xr_compile_native_projection_bind(owner,&bad,&input)==XR_XIR_BAD_STRUCTURE&&!memcmp(&input,&untouched,sizeof(input)));
    CHECK(xr_compile_native_projection_bind(NULL,&binding,&input)==XR_XIR_BAD_STRUCTURE&&!memcmp(&input,&untouched,sizeof(input)));
    CHECK(xr_compile_native_projection_bind(owner,NULL,&input)==XR_XIR_BAD_STRUCTURE&&!memcmp(&input,&untouched,sizeof(input)));
    CHECK(xr_compile_native_projection_bind(owner,&binding,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!xr_compile_native_projection_context(NULL)&&!xr_compile_native_projection_source(NULL)&&!xr_compile_native_projection_facts(NULL));
    uint8_t digest[32];xr_sha256((const uint8_t *)code.text,code.length,digest);CHECK(!memcmp(digest,facts.generated_digest.bytes,32));
    /* Burn the remaining real work balance to the exact next bind, then reject
     * the following request without replacing the owner's ledger. */
    uint64_t used=sdk_stats(resources).work;
    CHECK(xr_compile_resources_work(resources,UINT64_MAX-used-bind_work)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_native_projection_bind(owner,&binding,&input)==XR_XIR_OK);
    input=untouched;CHECK(xr_compile_native_projection_bind(owner,&binding,&input)==XR_XIR_BUDGET&&!memcmp(&input,&untouched,sizeof(input)));
    xr_compile_native_projection_owner_free(owner);CHECK(!runtime_live&&!runtime_bytes);
    DWORD final_handles=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&final_handles)&&final_handles==initial_handles);
    bind_limits(argv[1],argv[2],argv[3],&binding);
    free(manifest);printf("actual C before real SDK/Target binding; bind work=%llu, zero allocations, dead producers and physical zero PASS\n",(unsigned long long)bind_work);
    return 0;
}
