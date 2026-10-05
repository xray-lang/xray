/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_native20_source.c - Source-derived whole Atomic Program qualification
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_atomic_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir_atomic_native20_cases.h"
static XrXirStatus atomic_build(AtomicCompileOwner *owner,XrXirSourceProduct **product) {
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus ss=xr_compile_session_new(owner->context.resources,&session);
    if(ss!=XR_COMPILER_SESSION_OK)return ss==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ATOMIC_FIXTURES};
    XrXirSourceProductRequest request={{session,XR_ATOMIC_FIXTURES "/root.xr",&authority,&owner->context,
        XR_ATOMIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,product,&diagnostic);
    if(status!=XR_XIR_OK && !atomic_program_compile_injected)fprintf(stderr,"Atomic source status=%u stage=%u line=%d %s\n",status,diagnostic.stage,diagnostic.source.line,diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);return status;
}

static void atomic_entries(AtomicCompileOwner *owner,XrXirSourceProduct *product,uint32_t *entries) {
    XrXirSourceProductPacketView packet={0};CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    XrXirArtifact *checked=NULL;CHECK(xr_xir_compile_checked_read(&owner->context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);CHECK(module && module->declarations);
    for(unsigned n=0;n<18;++n) {
        char name[16];int length=snprintf(name,sizeof(name),"case%u",n);CHECK(length>0 && (size_t)length<sizeof(name));entries[n]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *fn=module->functions+f;
            if(fn->name_length!=(size_t)length || memcmp(fn->name,name,(size_t)length))continue;
            if(module->declarations->functions[f].module!=module->declarations->root_module)continue;
            CHECK(entries[n]==UINT32_MAX && module->declarations->functions[f].exported && !fn->parameter_count);entries[n]=f;
        }
        CHECK(entries[n]!=UINT32_MAX);
    }
    xr_xir_compile_artifact_free(checked);
}
static XrXirStatus atomic_compiler_probe(XrCompileResourceLimits caps,XrCompileResourceStats *stats) {
    AtomicCompileOwner owner;XrCompileResourceStatus opened=atomic_compile_owner_new(caps,&owner);
    if(opened!=XR_COMPILE_RESOURCE_OK){atomic_compile_owner_free(&owner);return opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;}
    XrXirSourceProduct *product=NULL;XrXirProgram *program=NULL;XrXirCSource output={0};
    XrXirStatus status=atomic_build(&owner,&product);if(status!=XR_XIR_OK)CHECK(!product);
    if(status==XR_XIR_OK){status=xr_xir_compile_source_product_emit(product,"atomic_native20",1024*1024,&output);if(status!=XR_XIR_OK)CHECK(!output.text && !output.length);}
    if(status==XR_XIR_OK){status=xr_xir_compile_source_product_vm_take(product,&program);if(status!=XR_XIR_OK)CHECK(!program);}
    xr_xir_compile_program_drop(program);xr_xir_compile_c_source_free(&output);xr_xir_compile_source_product_free(product);
    if(stats)*stats=atomic_compile_stats(&owner);CHECK(!runtime_live && !runtime_bytes);atomic_compile_owner_free(&owner);return status;
}
static void atomic_compiler_faults(void) {
    atomic_program_compile_attempts=0;XrCompileResourceStats stats={0};CHECK(atomic_compiler_probe(atomic_compile_caps(),&stats)==XR_XIR_OK);
    size_t sites=atomic_program_compile_attempts;CHECK(sites && sites<20000);
    printf("Atomic20 compiler baseline sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    for(size_t ordinal=0;ordinal<sites;++ordinal){atomic_program_compile_attempts=0;atomic_program_compile_fail_at=ordinal;atomic_program_compile_injected=false;
        XrXirStatus status=atomic_compiler_probe(atomic_compile_caps(),NULL);atomic_program_compile_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && atomic_program_compile_injected && atomic_program_compile_attempts>ordinal);}
    for(unsigned axis=0;axis<3;++axis){XrCompileResourceLimits exact=atomic_compile_caps();if(!axis)exact.allocated_bytes=stats.allocated_bytes;else if(axis==1)exact.live_bytes=stats.peak_bytes;else exact.work=stats.work;
        CHECK(atomic_compiler_probe(exact,NULL)==XR_XIR_OK);if(!axis)--exact.allocated_bytes;else if(axis==1)--exact.live_bytes;else --exact.work;CHECK(atomic_compiler_probe(exact,NULL)==XR_XIR_BUDGET);}
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);if(argc==2 && !strcmp(argv[1],"--compiler")){atomic_compiler_faults();return 0;}
    AtomicCompileOwner owner;CHECK(atomic_compile_owner_new(atomic_compile_caps(),&owner)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceProduct *product=NULL;CHECK(atomic_build(&owner,&product)==XR_XIR_OK && product);uint32_t entries[18];atomic_entries(&owner,product,entries);
    if(argc==2){XrXirCSource output={0};CHECK(xr_xir_compile_source_product_emit(product,"atomic_native20",1024*1024,&output)==XR_XIR_OK);
        FILE *file=fopen(argv[1],"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
        CHECK(fputs("\nconst uint32_t atomic_native20_selection[18]={",file)>=0);
        for(unsigned n=0;n<18;++n)CHECK(fprintf(file,"%s%uu",n?",":"",entries[n])>0);CHECK(fputs("};\n",file)>=0 && !fclose(file));xr_xir_compile_c_source_free(&output);
    }else{XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);xr_xir_compile_source_product_free(product);product=NULL;
        for(unsigned n=0;n<18;++n){CHECK(xr_xir_compile_program_retain(program));CHECK(atomic_normal(program,entries[n],n)==atomic_expected[n]);}
        for(unsigned n=0;n<2;++n)atomic_runtime_faults(program,entries[n],n);
        printf("Atomic20 VM runtime actual faults i64=%zu bool=%zu; original 127 windows; physical0\n",atomic_runtime_sites[0],atomic_runtime_sites[1]);xr_xir_compile_program_drop(program);}
    xr_xir_compile_source_product_free(product);XrCompileResourceStats stats=atomic_compile_stats(&owner);
    printf("Atomic20 Source graph allocated=%llu peak=%llu work=%llu attempts=%zu physical0\n",(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,atomic_program_compile_attempts);
    atomic_compile_owner_free(&owner);return 0;
}
