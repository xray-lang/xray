/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_unit_values.c - Logical Unit arguments with no physical frame payload
 *
 * KEY CONCEPT:
 *   Complete independent wire fields and actual leaf execution distinguish a
 *   canonical Unit value from a statement that merely has a Unit result type.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "../xir_construction_fixture.h"
#include "../xir_source_fixture_owner.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_tuple.h"
#include "unit84_wire_golden.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA==32u && XR_XIR_CHECKED_CONTRACT==84u &&
    XR_XIR_TUPLE_NEW==143 && XR_XIR_RETURN==33 && XR_XIR_I64==2 && XR_XIR_UNIT==0,
    "Independent Unit wire fields require an explicit protocol update");

static XrXirArtifact *unit_checked(const XrXirCompileContext *context,bool declarations) {
    const XrXirType parameter=XR_XIR_UNIT;
    const XrXirInstruction unit_ops[]={
        {XR_XIR_TUPLE_NEW,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2,0},{0},0,{0}}};
    const XrXirInstruction init_ops[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction entry_ops[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirBlock blocks[]={{0,1,0,0},{0,3,0,0},{0,2,0,0}};
    const XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init_ops,1,NULL,0},
        {"unit",4,&parameter,1,XR_XIR_I64,&blocks[1],1,unit_ops,3,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&blocks[2],1,entry_ops,2,NULL,0}};
    const XrXirSourceModule module={"root",4,NULL,0,0,0};
    const XrXirFunctionIdentity identities[]={{0},{0,1,0,0,0,0,0,0,0,0},{0}};
    const XrXirDeclarations owner={&module,1,identities,NULL,0,NULL,0,0,2,NULL};
    const XrXirModule built={XR_XIR_BUILT,declarations?functions:&functions[1],declarations?3:1,
        declarations?&owner:NULL,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL;
    CHECK(xir_fixture_check(context,&built,&checked,NULL)==XR_XIR_OK);
    return checked;
}

static XrXirArtifact *unit_lowered(const XrXirCompileContext *context,bool declarations) {
    XrXirArtifact *checked=unit_checked(context,declarations),*lowered=NULL;
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    return lowered;
}

static void unit_wire(const XrXirCompileContext *context) {
    XrXirArtifact *checked=unit_checked(context,false),*received=NULL;
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==sizeof(unit84_packet) && !memcmp(packet.bytes,unit84_packet,packet.length));
    xr_xir_compile_artifact_free(checked);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_checked_read(context,unit84_packet,sizeof(unit84_packet),&received,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(received);
    CHECK(module->functions[0].parameter_count==1 && module->functions[0].parameters[0]==XR_XIR_UNIT);
    xr_xir_compile_artifact_free(received);received=NULL;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_checked_read(context,previous75_packet,sizeof(previous75_packet),&received,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!received && xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
        after.live_bytes==before.live_bytes);
    CHECK(xr_xir_compile_checked_read(context,previous76_packet,sizeof(previous76_packet),&received,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!received && xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
        after.live_bytes==before.live_bytes);
}

static void unit_value_uses(const XrXirCompileContext *context) {
    const XrXirType parameter=XR_XIR_UNIT;const uint32_t argument=1;
    const XrXirInstruction callee[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1,0},{0},0,{0}}};
    const XrXirInstruction caller[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
        {XR_XIR_TUPLE_NEW,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_CALL,XR_XIR_I64,{0,1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2,0},{0},0,{0}}};
    const XrXirBlock blocks[]={{0,2,0,0},{0,4,0,0}};
    const XrXirFunction functions[]={
        {"consume",7,&parameter,1,XR_XIR_I64,&blocks[0],1,callee,2,NULL,0},
        {"caller",6,NULL,0,XR_XIR_I64,&blocks[1],1,caller,4,&argument,1}};
    const XrXirModule built={XR_XIR_BUILT,functions,2,NULL,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL;
    CHECK(xir_fixture_check(context,&built,&checked,NULL)==XR_XIR_OK);
    XrXirInstruction *operations=(XrXirInstruction *)checked->module.functions[1].instructions;
    operations[1].op=XR_XIR_PRINT;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_BAD_VALUE);
    XrXirCheckedPacket output={0};
    CHECK(xr_xir_compile_checked_write(checked,&output,NULL)==XR_XIR_BAD_VALUE && !output.bytes && !output.length);
    operations[1].op=XR_XIR_TUPLE_NEW;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    operations[1].args[1]=1;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_BAD_STRUCTURE);
    operations[1].args[1]=0;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    operations[1].args[0]=1;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_BAD_STRUCTURE);
    operations[1].args[0]=0;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
}

static void unit_callable_types(const XrXirCompileContext *context) {
    XrXirCallableParameter parameter={XR_XIR_UNIT,XR_PARAM_READ};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_NONE};
    const XrXirTypes types={.nodes=&node,.count=1};
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    parameter.mode=XR_PARAM_REF;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    parameter.mode=XR_PARAM_READ;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    node.kind=XR_XIR_TYPE_ARRAY;node.parameters=NULL;node.parameter_count=0;node.result=XR_XIR_UNIT;node.flags=0;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
}
static void unit_primitives(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirDomainStats before=xr_xir_domain_stats(domain);
    XrXirValue unit={0},output={0},occupied={XR_XIR_BOOL,0,1};
    XrXirValueAdmission admission={.domain=domain,.work=16};
    CHECK(xr_xir_value_argument(&unit,NULL,XR_XIR_UNIT));
    CHECK(xr_xir_tuple_new(XR_XIR_UNIT,NULL,0,&admission,&output)==XR_XIR_VALUE_OK);
    CHECK(!output.type && !output.reserved && !output.payload && admission.work==15);
    CHECK(xr_xir_value_admit(&unit,XR_XIR_UNIT,&admission)==XR_XIR_VALUE_OK);
    const XrXirValue malformed[]={{XR_XIR_UNIT,1,0},{XR_XIR_UNIT,0,1},{XR_XIR_BOOL,0,0}};
    for (uint32_t i=0;i<3;++i) {
        CHECK(!xr_xir_value_argument(&malformed[i],NULL,XR_XIR_UNIT));
        CHECK(xr_xir_value_admit(&malformed[i],XR_XIR_UNIT,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    }
    CHECK(xr_xir_tuple_new(XR_XIR_UNIT,&unit,0,&admission,&output)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_tuple_new(XR_XIR_UNIT,NULL,1,&admission,&output)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_tuple_new(XR_XIR_UNIT,NULL,0,&admission,&occupied)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(occupied.type==XR_XIR_BOOL && !occupied.reserved && occupied.payload==1);
    admission.work=0;
    CHECK(xr_xir_tuple_new(XR_XIR_UNIT,NULL,0,&admission,&output)==XR_XIR_VALUE_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload);
    XrXirDomainStats after=xr_xir_domain_stats(domain);
    CHECK(after.live_bytes==before.live_bytes && after.allocations==before.allocations && after.frees==before.frees);
    xr_xir_domain_drop(domain);
}

static void unit_leaf(const XrXirCompileContext *context) {
    XrXirArtifact *lowered=unit_lowered(context,true);XrXirValue argument={0},output={0};
    const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(lowered,1);
    CHECK(layout->frame_bytes==8 && layout->offsets[0]==UINT32_MAX && layout->offsets[1]==UINT32_MAX);
    CHECK(layout->parameters[0].size==16 && layout->parameters[0].alignment==8);
    XrXirCtfeRequest request={.artifact=lowered,.function=1,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.arguments=&argument,.argument_count=1,
        .limits={3,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,lowered->checked_identity,32);
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    xr_xir_value_drop(&output);
    XrXirCtfeCallRequest calls={.evaluation=request,.call_depth=128};
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    xr_xir_value_drop(&output);
    argument.reserved=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_BAD_ARGUMENT && !output.type && !output.payload);
    argument.reserved=0;argument.payload=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_BAD_ARGUMENT);
    argument.payload=0;request.argument_count=0;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_BAD_ARGUMENT);
    request.argument_count=1;request.limits.steps=2;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    request.limits.steps=3;request.limits.frame_bytes=0;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;output=(XrXirValue){XR_XIR_I64,0,9};
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_BAD_ARGUMENT && output.type==XR_XIR_I64 && output.payload==9);
    output=(XrXirValue){0};XrXirRunContext run={3,8,0,0,0,0};
    CHECK(xr_xir_compile_vm_run(lowered,1,&run,&argument,1,&output)==XR_XIR_RUN_OK && output.type==XR_XIR_I64 && output.payload==42);
    CHECK(!run.live_bytes && run.allocations==run.frees);
    xr_xir_value_drop(&output);xr_xir_compile_artifact_free(lowered);
}

static void unit_emit(const XrXirCompileContext *context,const char *path) {
    XrXirArtifact *lowered=unit_lowered(context,false);XrXirCSource source={0};
    CHECK(xr_xir_compile_emit_leaf_c(lowered,"unit_boundary",1048576,&source)==XR_XIR_OK);
    CHECK(source.text && source.length && !strstr(source.text,"({"));
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
    xr_xir_compile_c_source_free(&source);xr_xir_compile_artifact_free(lowered);
}

int main(int argc,char **argv) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    if (argc==3 && !strcmp(argv[1],"--emit-leaf")) unit_emit(&owner.context,argv[2]);
    else {
        CHECK(argc==1);unit_wire(&owner.context);unit_value_uses(&owner.context);unit_callable_types(&owner.context);
        unit_primitives();unit_leaf(&owner.context);
        puts("canonical Unit values, exact wire rejection, non-value statements and public leaf boundaries passed");
    }
    source_fixture_owner_free(&owner);return 0;
}
