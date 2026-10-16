/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_origins.c - Declared candidates and every actual returned leaf
 *
 * KEY CONCEPT:
 *   Hidden captures and local owners never replace the signature candidate.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_types_internal.h"
#include "tuple_owner_observer.h"

typedef struct OriginFixture {
    XrXirTypeNode nodes[8];XrXirCallableParameter fields[2],call_parameters[2];XrXirTypes types;
    XrXirType parameters[4][4];XrXirInstruction operations[4][16];
    XrXirBlock blocks[4][4];uint32_t operands[4][48];XrXirFunction functions[4];XrXirModule module;
    XrXirSourceModule source;XrXirFunctionIdentity identities[4];XrXirDeclarations declarations;
} OriginFixture;
static XrXirType origin_type(uint32_t index) {return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+index);}
static XrXirCompileContext origin_owner(void) {
    return owner_new((XrCompileResourceLimits){1048576,1048576,4000000});
}
static void origin_fixture(OriginFixture *f) {
    *f=(OriginFixture){0};
    f->fields[0]=(XrXirCallableParameter){origin_type(0),0};f->fields[1]=f->fields[0];
    f->call_parameters[0]=f->fields[0];
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=origin_type(0)};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields,.parameter_count=2};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=f->call_parameters,
        .parameter_count=1,.result=origin_type(0),.flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->types=(XrXirTypes){f->nodes,5,NULL,NULL};
    for (uint32_t i=0;i<3;++i) {
        f->parameters[i][0]=origin_type(0);
        f->operations[i][0]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
        f->blocks[i][0]=(XrXirBlock){.count=1};
        f->functions[i]=(XrXirFunction){.name="origin",.name_length=6,.parameters=f->parameters[i],
            .parameter_count=1,.result=origin_type(0),.blocks=f->blocks[i],.block_count=1,
            .instructions=f->operations[i],.instruction_count=1};
    }
    f->operations[3][0]=(XrXirInstruction){.op=XR_XIR_RETURN};f->blocks[3][0]=(XrXirBlock){.count=1};
    f->functions[3]=(XrXirFunction){.name="init",.name_length=4,.blocks=f->blocks[3],.block_count=1,
        .instructions=f->operations[3],.instruction_count=1};
    f->source=(XrXirSourceModule){.name="origins",.name_length=7,.initializer=3};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,.types=&f->types,
        .declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
}
static void origin_captured(OriginFixture *f) {
    origin_fixture(f);
    f->parameters[0][1]=origin_type(0);f->functions[0].parameter_count=2;
    f->functions[1].result=XR_XIR_UNIT;f->functions[1].instruction_count=2;f->blocks[1][0].count=2;
    f->operations[1][0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=origin_type(4),.args={0,1},.immediate=0};
    f->operations[1][1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->operands[1][0]=0;f->functions[1].operands=f->operands[1];f->functions[1].operand_count=1;
}
static XrXirStatus origin_prove(const XrXirCompileContext *c,const OriginFixture *f) {
    return xir_compile_view_return_origins_verified(c,&f->module,NULL);
}
static void origin_expect(const XrXirCompileContext *c,const OriginFixture *f,XrXirStatus expected) {
    uint64_t baseline=stats(c).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(c,&f->types)==XR_XIR_OK);
    CHECK(origin_prove(c,f)==expected && stats(c).live_bytes==baseline);
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus actual=xir_fixture_check(c,&f->module,&checked,&diagnostic);
    if (actual!=(expected==XR_XIR_BAD_VALUE ? XR_XIR_BAD_VALUE : XR_XIR_BAD_TYPE))
        fprintf(stderr,"origin status %u, common status %u\n",(unsigned)expected,(unsigned)actual);
    CHECK(actual==(expected==XR_XIR_BAD_VALUE ? XR_XIR_BAD_VALUE : XR_XIR_BAD_TYPE) && !checked);
    if (actual==XR_XIR_BAD_VALUE) CHECK(diagnostic.function==0 && diagnostic.instruction!=UINT32_MAX);
    CHECK(stats(c).live_bytes==baseline);
}
static void origin_candidates_and_wrappers(const XrXirCompileContext *c) {
    OriginFixture f;origin_fixture(&f);origin_expect(c,&f,XR_XIR_OK);
    f.parameters[0][0]=origin_type(1);f.functions[0].result=origin_type(1);origin_expect(c,&f,XR_XIR_OK);
    origin_fixture(&f);f.functions[0].result=origin_type(2);f.functions[0].instruction_count=4;f.blocks[0][0].count=4;
    f.operations[0][0]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=origin_type(2),.args={0,0}};
    f.operations[0][1]=(XrXirInstruction){.op=XR_XIR_NULLABLE_UNWRAP,.type=origin_type(0),.args={1,0}};
    f.operations[0][2]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=origin_type(2),.args={2,0}};
    f.operations[0][3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
    origin_expect(c,&f,XR_XIR_OK);
    f.operations[0][0]=(XrXirInstruction){.op=XR_XIR_NULLABLE_NONE,.type=origin_type(2)};
    origin_expect(c,&f,XR_XIR_OK);
    f.functions[0].parameters=NULL;f.functions[0].parameter_count=0;f.functions[0].instruction_count=2;f.blocks[0][0].count=2;
    f.operations[0][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    origin_expect(c,&f,XR_XIR_BAD_TYPE);
    origin_fixture(&f);f.parameters[0][1]=origin_type(0);f.functions[0].parameter_count=2;
    origin_expect(c,&f,XR_XIR_BAD_TYPE);
    origin_captured(&f);origin_expect(c,&f,XR_XIR_BAD_VALUE);
    f.operations[0][0].args[0]=1;origin_expect(c,&f,XR_XIR_OK);
}
static void origin_tuple_paths(const XrXirCompileContext *c) {
    OriginFixture f;origin_captured(&f);f.functions[0].instruction_count=3;f.blocks[0][0].count=3;
    f.functions[0].operands=f.operands[0];f.functions[0].operand_count=2;
    f.operands[0][0]=1;f.operands[0][1]=0;
    f.operations[0][0]=(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=origin_type(3),.args={0,2}};
    f.operations[0][1]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=origin_type(0),.args={2,0},.immediate=0};
    f.operations[0][2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
    origin_expect(c,&f,XR_XIR_OK);
    f.operations[0][1].immediate=1;origin_expect(c,&f,XR_XIR_BAD_VALUE);
    f.types.count=6;f.nodes[5]=f.nodes[4];f.nodes[5].result=origin_type(3);
    f.functions[0].result=origin_type(3);f.functions[0].instruction_count=2;f.blocks[0][0].count=2;
    f.operations[1][0].type=origin_type(5);
    f.operations[0][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    origin_expect(c,&f,XR_XIR_BAD_VALUE);
    f.operands[0][1]=1;origin_expect(c,&f,XR_XIR_OK);
}
static void origin_calls(const XrXirCompileContext *c) {
    OriginFixture f;origin_captured(&f);
    f.functions[0].instruction_count=2;f.blocks[0][0].count=2;
    f.functions[0].operands=f.operands[0];f.functions[0].operand_count=1;f.operands[0][0]=1;
    f.operations[0][0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=origin_type(0),.args={0,1},.immediate=2};
    f.operations[0][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    origin_expect(c,&f,XR_XIR_OK);f.operands[0][0]=0;origin_expect(c,&f,XR_XIR_BAD_VALUE);
    f.types.count=6;f.nodes[5]=f.nodes[4];
    f.call_parameters[1]=(XrXirCallableParameter){origin_type(4),0};f.nodes[5].parameter_count=2;
    f.parameters[0][2]=origin_type(4);f.functions[0].parameter_count=3;f.operations[1][0].type=origin_type(5);
    f.operations[0][0].op=XR_XIR_CALL_INDIRECT;f.operations[0][1].args[0]=3;
    f.operands[0][0]=1;origin_expect(c,&f,XR_XIR_OK);
    f.operands[0][0]=0;origin_expect(c,&f,XR_XIR_BAD_VALUE);
}
static void origin_loop(const XrXirCompileContext *c) {
    OriginFixture f;origin_captured(&f);f.types.count=6;f.nodes[5]=f.nodes[4];
    f.call_parameters[1]=(XrXirCallableParameter){XR_XIR_BOOL,0};f.nodes[5].parameter_count=2;
    f.parameters[0][2]=XR_XIR_BOOL;f.functions[0].parameter_count=3;
    f.functions[0].instruction_count=6;f.functions[0].block_count=4;
    f.blocks[0][0]=(XrXirBlock){.first=0,.count=1};f.blocks[0][1]=(XrXirBlock){.first=1,.count=2};
    f.blocks[0][2]=(XrXirBlock){.first=3,.count=2};f.blocks[0][3]=(XrXirBlock){.first=5,.count=1};
    f.functions[0].operands=f.operands[0];f.functions[0].operand_count=4;
    f.operands[0][0]=0;f.operands[0][1]=1;f.operands[0][2]=2;f.operands[0][3]=6;
    f.operations[0][0]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1,0}};
    f.operations[0][1]=(XrXirInstruction){.op=XR_XIR_PHI,.type=origin_type(0),.args={0,4}};
    f.operations[0][2]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={2,0},.targets={2,3}};
    f.operations[0][3]=(XrXirInstruction){.op=XR_XIR_COPY,.type=origin_type(0),.args={4,0}};
    f.operations[0][4]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1,0}};
    f.operations[0][5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4,0}};
    f.operations[1][0].type=origin_type(5);
    origin_expect(c,&f,XR_XIR_OK);
    f.operations[0][3].args[0]=0;origin_expect(c,&f,XR_XIR_BAD_VALUE);
}
static uint64_t origin_dag(const XrXirCompileContext *c) {
    XrXirTypeNode nodes[49]={0};XrXirCallableParameter fields[48][2]={0};
    XrXirInstruction ops[49]={0};uint32_t operands[96]={0};
    nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    for (uint32_t i=1;i<49;++i) {
        fields[i-1][0]=(XrXirCallableParameter){origin_type(i-1),0};fields[i-1][1]=fields[i-1][0];
        nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[i-1],.parameter_count=2};
        operands[(i-1)*2]=i-1;operands[(i-1)*2+1]=i-1;
        ops[i-1]=(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=origin_type(i),.args={(i-1)*2,2}};
    }
    ops[48]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={48,0}};
    const XrXirTypes types={nodes,49,NULL,NULL};const XrXirType parameter=origin_type(0);
    const XrXirBlock block={.count=49};
    const XrXirFunction function={.name="dag",.name_length=3,.parameters=&parameter,.parameter_count=1,
        .result=origin_type(48),.blocks=&block,.block_count=1,.instructions=ops,.instruction_count=49,
        .operands=operands,.operand_count=96};
    const XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.types=&types};
    CHECK(xr_xir_compile_types_structure_verify(c,&types)==XR_XIR_OK);
    uint64_t before=stats(c).work;
    CHECK(xir_compile_view_return_origins_verified(c,&module,NULL)==XR_XIR_OK);
    uint64_t work=stats(c).work-before;CHECK(work<10000);return work;
}
static void origin_fault_fixture(OriginFixture *f) {
    origin_captured(f);f->types.count=8;
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=origin_type(3)};
    for (uint32_t i=6;i<8;++i)
        f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=origin_type(i-1)};
    f->functions[0].instruction_count=9;f->blocks[0][0].count=9;
    f->functions[0].operands=f->operands[0];f->functions[0].operand_count=2;
    f->operands[0][0]=1;f->operands[0][1]=0;
    f->operations[0][0]=(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=origin_type(3),.args={0,2}};
    for (uint32_t i=1;i<4;++i)
        f->operations[0][i]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=origin_type(i+4),.args={i+1,0}};
    for (uint32_t i=4;i<7;++i)
        f->operations[0][i]=(XrXirInstruction){.op=XR_XIR_NULLABLE_UNWRAP,
            .type=origin_type(i==6?3:10-i),.args={i+1,0}};
    f->operations[0][7]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=origin_type(0),.args={8,0}};
    f->operations[0][8]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={9,0}};
}
static void origin_bounds(void) {
    OriginFixture f;origin_fault_fixture(&f);
    XrXirCompileContext c=origin_owner();uint64_t baseline=stats(&c).live_bytes;
    CHECK(origin_prove(&c,&f)==XR_XIR_OK);XrCompileResourceStats measured=stats(&c);owner_free(&c,baseline);
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits={1048576,1048576,4000000};
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;
        else limits.work=measured.work-minus;
        c=owner_new(limits);baseline=stats(&c).live_bytes;
        CHECK(origin_prove(&c,&f)==(minus?XR_XIR_BUDGET:XR_XIR_OK));owner_free(&c,baseline);
    }
    c=origin_owner();baseline=stats(&c).live_bytes;injected=false;fail_at=attempts;
    CHECK(origin_prove(&c,&f)==XR_XIR_OUT_OF_MEMORY && injected);fail_at=SIZE_MAX;owner_free(&c,baseline);
}
static void origin_pipeline(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    OriginFixture f;origin_fixture(&f);XrXirArtifact *checked=NULL;
    CHECK(xir_fixture_check(&c,&f.module,&checked,NULL)==XR_XIR_BAD_TYPE && !checked);
    origin_captured(&f);
    CHECK(xir_fixture_check(&c,&f.module,&checked,NULL)==XR_XIR_BAD_VALUE && !checked);
    f.operations[0][0].args[0]=1;
    CHECK(xir_fixture_check(&c,&f.module,&checked,NULL)==XR_XIR_BAD_TYPE && !checked);
    owner_free(&c,baseline);
}
static void origin_faults(void) {
    OriginFixture f;origin_fault_fixture(&f);
    XrXirCompileContext c=origin_owner();uint64_t baseline=stats(&c).live_bytes;size_t start=attempts;
    CHECK(origin_prove(&c,&f)==XR_XIR_OK);size_t count=attempts-start;owner_free(&c,baseline);
    CHECK(count>4 && count<128);
    for (size_t at=0;at<count;++at) {
        c=origin_owner();baseline=stats(&c).live_bytes;injected=false;fail_at=attempts+at;
        CHECK(origin_prove(&c,&f)==XR_XIR_OUT_OF_MEMORY && injected);
        fail_at=SIZE_MAX;owner_free(&c,baseline);
    }
    printf("returned view origin allocation failures=%zu, physical0 PASS\n",count);
}
int main(void) {
    XrXirCompileContext c=origin_owner();uint64_t baseline=stats(&c).live_bytes;
    origin_candidates_and_wrappers(&c);origin_tuple_paths(&c);origin_calls(&c);origin_loop(&c);
    uint64_t work=origin_dag(&c);owner_free(&c,baseline);origin_bounds();origin_pipeline();origin_faults();
    printf("returned view origins: signatures/captures/each leaf/calls/PHI cycle, DAG work=%llu, finite bounds and physical0 PASS\n",
        (unsigned long long)work);return 0;
}
