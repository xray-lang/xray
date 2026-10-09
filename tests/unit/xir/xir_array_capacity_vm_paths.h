/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_vm_paths.h - Explicit checked READ projection execution
 *
 * KEY CONCEPT:
 *   Complete Built graphs test the VM projection role independently of Source
 *   choosing to load a temporary before the read-only capacity operation.
 */
#ifndef XIR_ARRAY_CAPACITY_VM_PATHS_H
#define XIR_ARRAY_CAPACITY_VM_PATHS_H
#include "xir_construction_fixture.h"
#include "xir_array_metadata_fixture.h"
typedef struct CapacityVmPathFixture {
    XirArrayMetadataFixture metadata;
    XrXirInstruction entry[2];
    XrXirBlock entry_block;
    XrXirFunction functions[4];
    XrXirFunctionIdentity identities[4];
} CapacityVmPathFixture;
static void capacity_vm_path_fixture(CapacityVmPathFixture *path) {
    memset(path,0,sizeof(*path));
    XirArrayMetadataFixture *fixture=&path->metadata;
    xir_array_metadata_init(fixture);
    const XrXirType inner=(XrXirType)256,outer=(XrXirType)258;
    fixture->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=inner};
    fixture->ops[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},13,{0}};
    fixture->ops[1]=(XrXirInstruction){XR_XIR_ARRAY_WITH_CAPACITY,inner,{0},{0},0,{0}};
    fixture->ops[2]=(XrXirInstruction){XR_XIR_ARRAY_NEW,outer,{0,1},{0},0,{0}};
    fixture->ops[3]=(XrXirInstruction){XR_XIR_LOCAL_NEW,outer,{2},{0},0,{0}};
    fixture->ops[4]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}};
    fixture->ops[5]=(XrXirInstruction){XR_XIR_INDEX_PLACE,inner,{3,4},{0},0,{0}};
    fixture->ops[6]=(XrXirInstruction){XR_XIR_ARRAY_CAPACITY,XR_XIR_I64,{5},{0},0,{0}};
    fixture->ops[7]=(XrXirInstruction){XR_XIR_GE_INT,XR_XIR_BOOL,{6,0},{0},0,{0}};
    fixture->ops[8]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{7},{0},0,{0}};
    fixture->operands[0]=1;
    fixture->blocks[1].count=9;fixture->functions[1].instruction_count=9;
    fixture->functions[1].result=XR_XIR_BOOL;fixture->functions[1].operand_count=1;
    memcpy(&fixture->ops[9],fixture->ops,9*sizeof(*fixture->ops));
    fixture->ops[13].immediate=1;
    fixture->blocks[2]=(XrXirBlock){0,9,0,0};
    fixture->functions[2]=(XrXirFunction){"badIndex",8,NULL,0,XR_XIR_BOOL,
        &fixture->blocks[2],1,&fixture->ops[9],9,fixture->operands,1};
    path->entry[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}};
    path->entry[1]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    path->entry_block=(XrXirBlock){0,2,0,0};
    memcpy(path->functions,fixture->functions,sizeof(fixture->functions));
    memcpy(path->identities,fixture->identities,sizeof(fixture->identities));
    path->identities[1].exported=1;path->identities[2].exported=1;
    path->functions[3]=(XrXirFunction){"main",4,NULL,0,XR_XIR_I64,
        &path->entry_block,1,path->entry,2,NULL,0};
    fixture->declarations.functions=path->identities;fixture->declarations.entry_function=3;
    fixture->module.functions=path->functions;fixture->module.function_count=4;
}
static void capacity_vm_paths(CapacityCompile *run) {
    const uint64_t baseline=capacity_stats(&run->context).live_bytes;
    CapacityVmPathFixture fixture;capacity_vm_path_fixture(&fixture);
    XrXirArtifact *checked=NULL,*replay=NULL,*specialized=NULL,*lowered=NULL;
    CHECK(xir_fixture_check(&run->context, &fixture.metadata.module, &checked, NULL)==XR_XIR_OK);
    memset(&fixture,0xcc,sizeof(fixture));
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(&run->context,packet.bytes,packet.length,&replay,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(replay,&specialized,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(replay);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(specialized);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered&&program);
    XrXirInstance *instances[2]={0};
    for(unsigned i=0;i<2;++i){XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);}
    xr_xir_compile_program_drop(program);program=NULL;
    for(unsigned i=0;i<2;++i){
        XrXirValue result=capacity_run(instances[i],3);
        CHECK(result.type==XR_XIR_I64&&!result.reserved&&!result.payload);xr_xir_value_drop(&result);
        result=capacity_run(instances[i],1);
        CHECK(result.type==XR_XIR_BOOL&&!result.reserved&&result.payload==1);xr_xir_value_drop(&result);
        CHECK(xr_xir_instance_start(instances[i],2,NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus status=XR_XIR_CALL_READY;XrXirCallResult failure={0};
        while(status==XR_XIR_CALL_READY){
            failure=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX).outcome;status=failure.status;}
        CHECK(status==XR_XIR_CALL_BOUNDS&&failure.panic.detail.code==430&&
            failure.panic.detail.index==1&&failure.panic.detail.length==1);
        CHECK(!failure.value.type&&!failure.value.reserved&&!failure.value.payload);
        capacity_failed_output(instances[i]);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
    CHECK(!runtime_live&&!runtime_bytes&&capacity_stats(&run->context).live_bytes==baseline);
    puts("capacity VM complete Built/CheckedReplay/specialize/Lowered READ index-place cap>=13 dual BOUNDS430 index1/len1 physical0");
}
#endif // XIR_ARRAY_CAPACITY_VM_PATHS_H
