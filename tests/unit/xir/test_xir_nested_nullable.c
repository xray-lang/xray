/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_nested_nullable.c - Whole Program optional layers and physical ownership
 *
 * KEY CONCEPT:
 *   Hand-built declarations and an independent packet agree before specialization.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);} } while(0)
#include "xir_nested_nullable_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_nested_nullable_fixture.h"
#include "xir_nested_nullable_golden.h"
#include "xir_nested_nullable64_golden.h"
#include "xir_nested_nullable65_golden.h"
#include "xir_nested_nullable72_golden.h"
#include "xir_nested_nullable_cases.h"
#include "xir_nested_nullable_value_cases.h"
static XrXirStatus nested_compile_pipeline(const XrXirCompileContext *context) {
    XrXirArtifact *checked=NULL,*read=NULL,*special=NULL,*lowered=NULL;
    XrXirCheckedPacket packet={0};XrXirCSource source={0};XrXirProgram *program=NULL;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirStatus status=xir_fixture_check(context, &nested_built, &checked, NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(checked,&packet,NULL);
    if(status==XR_XIR_OK){CHECK(packet.length==sizeof(nested_nullable72_golden) &&
        !memcmp(packet.bytes,nested_nullable72_golden,packet.length));
        status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);}
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(read,&special,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(special,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(special,&target,&lowered,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_emit_c(lowered,"nested_nullable",1048576,&source);
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,&program);
    xr_xir_compile_program_drop(program);xr_xir_compile_c_source_free(&source);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(special);
    xr_xir_compile_artifact_free(read);xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    return status;
}
static void nested_type_rejections(const XrXirCompileContext *context) {
    XrXirTypeNode nodes[2]={nested_nodes[0],nested_nodes[1]};XrXirTypes types={nodes,2,NULL,NULL};
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    nodes[0].element=XR_XIR_UNIT;CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[0]=nested_nodes[0];nodes[1].element=NESTED_OUTER;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[1]=nested_nodes[1];nodes[0].element=NESTED_OUTER;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[0]=nested_nodes[0];nodes[0].kind=XR_XIR_TYPE_CELL;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[0]=nested_nodes[0];nodes[1].parameter_span=1;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
}
static void nested_compile_faults(void) {
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass){
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        nested_nullable_compile_attempts=0;nested_nullable_compile_injected=false;
        nested_nullable_compile_fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=nested_compile_pipeline(&owner.context);
        const size_t attempts=nested_nullable_compile_attempts;nested_nullable_compile_fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=attempts;CHECK(sites && sites<10000);}
        else CHECK(nested_nullable_compile_injected && attempts>pass-1 && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(!runtime_live && !runtime_bytes);nested_nullable_compile_owner_free(&owner);
    }
    printf("nested whole pipeline metadata OOM sites=%zu; no partial owners\n",sites);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);nested_compile_faults();
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirArtifact *prior=NULL;
    size_t prior_attempts=nested_nullable_compile_attempts;
    size_t prior_live=nested_nullable_compile_live,prior_bytes=nested_nullable_compile_bytes;
    size_t prior_runtime_live=runtime_live,prior_runtime_bytes=runtime_bytes;
    CHECK(xr_xir_compile_checked_read(&owner.context,nested_nullable_golden,sizeof(nested_nullable_golden),&prior,NULL)==XR_XIR_BAD_STRUCTURE && !prior);
    prior=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&owner.context,nested_nullable_golden,sizeof(nested_nullable_golden),&prior,NULL)==XR_XIR_BAD_STRUCTURE && prior==(XrXirArtifact *)(uintptr_t)1);
    CHECK(nested_nullable_compile_attempts==prior_attempts && nested_nullable_compile_live==prior_live &&
        nested_nullable_compile_bytes==prior_bytes && runtime_live==prior_runtime_live && runtime_bytes==prior_runtime_bytes);
    prior=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,nested_nullable64_golden,sizeof(nested_nullable64_golden),&prior,NULL)==XR_XIR_BAD_STRUCTURE && !prior);
    prior=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&owner.context,nested_nullable64_golden,sizeof(nested_nullable64_golden),&prior,NULL)==XR_XIR_BAD_STRUCTURE && prior==(XrXirArtifact *)(uintptr_t)1);
    CHECK(nested_nullable_compile_attempts==prior_attempts && nested_nullable_compile_live==prior_live && nested_nullable_compile_bytes==prior_bytes && runtime_live==prior_runtime_live && runtime_bytes==prior_runtime_bytes);
    prior=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,nested_nullable65_golden,sizeof(nested_nullable65_golden),&prior,NULL)==XR_XIR_BAD_STRUCTURE && !prior);
    prior=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&owner.context,nested_nullable65_golden,sizeof(nested_nullable65_golden),&prior,NULL)==XR_XIR_BAD_STRUCTURE && prior==(XrXirArtifact *)(uintptr_t)1);
    CHECK(nested_nullable_compile_attempts==prior_attempts && nested_nullable_compile_live==prior_live && nested_nullable_compile_bytes==prior_bytes);
    nested_type_rejections(&owner.context);nested_storage_vectors(&owner.context);nested_deep_admission(&owner.context,32);nested_deep_admission(&owner.context,256);
    XrXirArtifact *checked=NULL,*read=NULL,*special=NULL,*lowered=NULL;
    CHECK(xir_fixture_check(&owner.context, &nested_built, &checked, NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==sizeof(nested_nullable72_golden) && !memcmp(packet.bytes,nested_nullable72_golden,packet.length));
    CHECK(xr_xir_compile_checked_read(&owner.context,nested_nullable72_golden,sizeof(nested_nullable72_golden),&read,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read,&special,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CHECK(module->declarations && module->function_count==8 && module->declarations->entry_function==1);
    XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,"nested_nullable",1048576,&source)==XR_XIR_OK);
    CHECK(source.length<1048576 && source.text);
    if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length && !fclose(file));}
    xr_xir_compile_c_source_free(&source);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirValue held[6]={{0}};nested_pair(program,held);
    nested_runtime_faults(program);nested_cancel_prefixes(program);
    xr_xir_compile_program_drop(program);nested_retained(held);
    CHECK(!runtime_live && !runtime_bytes);nested_nullable_compile_owner_free(&owner);
    puts("nested Nullable complete VM Program None/Some(None)/Some(Some(7)) PASS");return 0;
}
