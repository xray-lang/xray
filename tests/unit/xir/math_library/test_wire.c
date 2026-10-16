/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_wire.c - Independent math packets and authenticated shape attacks
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xsha256.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_math_ir.h"
#include "xir/xxir_math_runtime.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#include "../xir_library_compile_owner.h"
#include "wire_golden.h"
#include "invalid_entry_wire.h"
#include "../rest_declarations/retired78_wire.h"
#include "../export_constants/retired78_wire.h"
_Static_assert(XR_XIR_MATH_UNARY_F64==154 && XR_XIR_MATH_BINARY_F64==155 &&
    XR_XIR_MATH_UNARY_I64==156 && XR_XIR_RANDOM_F64==157 && XR_XIR_RANDOM_I64==158 &&
    XR_XIR_CHECKED_SCHEMA==32 && XR_XIR_CHECKED_CONTRACT==84,"Independent math operation framing");
static XrXirStatus math_wire_operation(const XrXirCompileContext *context,void *unused) {
    (void)unused;XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_read(context,math_wire,sizeof(math_wire),&artifact,&diagnostic);
    if (status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"Math packet read status%u function%u block%u instruction%u reason%u\n",
            status,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
    if (status==XR_XIR_OK) {
        status=xr_xir_compile_checked_write(artifact,&packet,NULL);
        if (status==XR_XIR_OK) CHECK(packet.length==sizeof(math_wire) && !memcmp(packet.bytes,math_wire,packet.length));
    }
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(artifact);return status;
}
static void math_old_packets(const XrXirCompileContext *context) {
    const void *inputs[]={rest_wire,constant_wire};const size_t lengths[]={sizeof(rest_wire),sizeof(constant_wire)};
    for (unsigned i=0;i<2;++i) {
        XrCompileResourceStats before=library_compile_stats(context);size_t attempts=source_program_compile_attempts;
        XrXirArtifact *artifact=NULL;
        CHECK(xr_xir_compile_checked_read(context,inputs[i],lengths[i],&artifact,NULL)==XR_XIR_BAD_STRUCTURE && !artifact);
        XrCompileResourceStats after=library_compile_stats(context);
        CHECK(after.live_bytes==before.live_bytes && after.allocated_bytes==before.allocated_bytes &&
            after.allocation_count==before.allocation_count && attempts==source_program_compile_attempts);
    }
}
static void math_attacks(const XrXirCompileContext *context) {
    const uint32_t fields[][2]={{MATH_POW+24,0},{MATH_POW+24,18},{MATH_POW+24,UINT32_MAX},
        {MATH_POW+28,1},{MATH_POW+4,12},{MATH_POW+8,2},{MATH_POW+20,1},{MATH_POW+32,1},
        {MATH_SQRT+12,1},{MATH_FLOOR+4,13},{MATH_RANDOM+8,1},{MATH_RANDOM+24,19},
        {MATH_RANDOMINT+24,6},{MATH_RANDOMINT+4,13}};
    for (unsigned a=0;a<sizeof(fields)/sizeof(*fields);++a) {
        uint8_t bytes[sizeof(math_wire)];memcpy(bytes,math_wire,sizeof(bytes));
        for (unsigned b=0;b<4;++b) bytes[fields[a][0]+b]=(uint8_t)(fields[a][1]>>(8*b));
        XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,bytes,32);
        xr_sha256_update(&sha,bytes+64,sizeof(bytes)-64);xr_sha256_final(&sha,bytes+32);
        XrXirArtifact *artifact=NULL;
        XrXirStatus status=xr_xir_compile_checked_read(context,bytes,sizeof(bytes),&artifact,NULL);
        if (status==XR_XIR_OK) fprintf(stderr,"Unexpected math shape attack%u accepted\n",a);
        CHECK(status!=XR_XIR_OK && !artifact);
    }
}
static XrXirResumeEntry math_random_resume;
static unsigned math_authority_probes;
static XrXirAction math_probe_authority(XrXirCallView *view) {
    XrXirCallView copied=*view;int64_t output=55;
    CHECK(xr_xir_call_math_random(NULL,XR_XIR_MATH_RANDOM,0,0,&output)==XR_XIR_RUN_BAD_ARGUMENT && output==55);
    CHECK(xr_xir_call_math_random(&copied,XR_XIR_MATH_RANDOM,0,0,&output)==XR_XIR_RUN_BAD_ARGUMENT && output==55);
    CHECK(xr_xir_call_math_random(view,XR_XIR_MATH_SQRT,0,0,&output)==XR_XIR_RUN_BAD_ARGUMENT && output==55);
    ++math_authority_probes;return math_random_resume(view);
}
static XrXirAction math_exhaust_work(XrXirCallView *view) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);CHECK(admission);
    admission->work=0;int64_t output=55;
    CHECK(xr_xir_call_math_random(view,XR_XIR_MATH_RANDOM,0,0,&output)==XR_XIR_RUN_STEP_LIMIT && output==55);
    return xr_xir_call_fault(XR_XIR_RUN_STEP_LIMIT);
}
static void math_execution(const XrXirCompileContext *context) {
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,math_wire,sizeof(math_wire),&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&entries)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,entries,module->function_count,
        module->declarations,{0},module->types,xr_xir_compile_program_proof(lowered)};
    math_random_resume=entries[4].resume;entries[4].resume=math_probe_authority;
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    const XrXirValue arguments[][2]={{{XR_XIR_F64,0,INT64_C(0x4008000000000000)},{XR_XIR_F64,0,INT64_C(0x4000000000000000)}},
        {{XR_XIR_F64,0,(-INT64_C(0x3ffc000000000000))},{0}},
        {{XR_XIR_F64,0,INT64_C(0x4054400000000000)},{0}},{{0},{0}},{{XR_XIR_I64,0,4},{XR_XIR_I64,0,-2}}};
    const uint32_t counts[]={2,1,1,0,2};
    for (unsigned i=0;i<5;++i) {
        CHECK(xr_xir_instance_start(instance,i+1,counts[i]?arguments[i]:NULL,counts[i])==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(!result.reserved);
        if (i==0 || i==2) CHECK(result.type==XR_XIR_F64 && result.payload==INT64_C(0x4022000000000000));
        else if (i==1) CHECK(result.type==XR_XIR_I64 && result.payload==-3);
        else if (i==4) CHECK(result.type==XR_XIR_I64 && result.payload>=-2 && result.payload<=4);
        else {double value=0;memcpy(&value,&result.payload,sizeof(value));CHECK(result.type==XR_XIR_F64 && value>=0.0 && value<1.0);}
        xr_xir_value_drop(&result);
    }
    CHECK(math_authority_probes);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    entries[4].resume=math_exhaust_work;program=NULL;instance=NULL;
    CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,4,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_LIMIT);
    /* Disposal preserves exhaustion of the same cumulative execution bank. */
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_LIMIT);xr_xir_compile_program_drop(program);
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);xr_xir_compile_artifact_free(lowered);
}
int main(void) {
    library_compile_operation_cases("Independent complete math read/write",math_wire_operation,NULL);
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *invalid=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,math_invalid_entry_wire,
        sizeof(math_invalid_entry_wire),&invalid,NULL)==XR_XIR_BAD_TYPE && !invalid);
    math_old_packets(&owner.context);math_attacks(&owner.context);math_execution(&owner.context);
    library_compile_owner_drop(&owner);library_compile_observer_free();
    puts("Independent five math operations, fourteen rehashed attacks, exact budgets/physical FI and complete old78 early refusal PASS");
    return 0;
}
