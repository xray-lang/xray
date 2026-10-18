/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_affine.c - Independent logical transfer and CFG rejection
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_types_internal.h"
#include "tuple_owner_observer.h"

#define MUT ((XrXirType)256)
#define AFFINE_OPTIONAL ((XrXirType)257)
#define PAIR ((XrXirType)258)
#define SHARED ((XrXirType)259)
#define SHARED_OPTIONAL ((XrXirType)260)
typedef struct AffineFixture {
    XrXirTypeNode nodes[6];XrXirCallableParameter fields[2],call_parameter;
    XrXirTypes types;XrXirType parameters[3],callee_parameters[2];
    XrXirInstruction instructions[32],callee[1],init[1];XrXirBlock blocks[8],one;
    uint32_t operands[64];XrXirFunction functions[3];XrXirModule module;
    XrXirSourceModule source;XrXirFunctionIdentity identities[3];XrXirDeclarations declarations;
} AffineFixture;
static uint32_t value(uint32_t i) { return i+3; }
static XrXirCompileContext affine_owner(void) {
    XrCompileResourceLimits limits=caps();limits.allocated_bytes=1048576;
    limits.live_bytes=1048576;limits.work=4000000;return owner_new(limits);
}
static void affine_fixture(AffineFixture *f) {
    *f=(AffineFixture){0};f->fields[0]=(XrXirCallableParameter){MUT,0};f->fields[1]=f->fields[0];
    f->call_parameter=f->fields[0];
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=MUT};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields,.parameter_count=2};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=SHARED};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->call_parameter,
        .parameter_count=1,.result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->types=(XrXirTypes){f->nodes,6,NULL,NULL};
    f->parameters[0]=XR_XIR_BOOL;f->parameters[1]=MUT;f->parameters[2]=MUT;
    f->callee_parameters[0]=MUT;f->callee_parameters[1]=MUT;
    f->instructions[0]=(XrXirInstruction){.op=XR_XIR_RETURN};f->blocks[0]=(XrXirBlock){.count=1};
    f->one=(XrXirBlock){.count=1};f->init[0]=f->instructions[0];f->callee[0]=f->instructions[0];
    f->functions[0]=(XrXirFunction){.name="affine",.name_length=6,.parameters=f->parameters,.parameter_count=3,
        .instructions=f->instructions,.instruction_count=1,.blocks=f->blocks,.block_count=1};
    f->functions[1]=(XrXirFunction){.name="init",.name_length=4,.instructions=f->init,.instruction_count=1,
        .blocks=&f->one,.block_count=1};
    f->functions[2]=(XrXirFunction){.name="callee",.name_length=6,.parameters=f->callee_parameters,.parameter_count=1,
        .instructions=f->callee,.instruction_count=1,.blocks=&f->one,.block_count=1};
    f->source=(XrXirSourceModule){.name="affine",.name_length=6,.initializer=1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=3,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
}
static void affine_straight(AffineFixture *f,uint32_t count) {
    f->functions[0].instruction_count=count;f->blocks[0].count=count;
    f->instructions[count-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
}
static void affine_expect(const XrXirCompileContext *c,const AffineFixture *f,XrXirStatus expected,uint32_t instruction) {
    uint64_t baseline=stats(c).live_bytes;XrXirDiagnostic d={0};
    CHECK(xr_xir_compile_types_structure_verify(c,&f->types)==XR_XIR_OK);
    CHECK(xir_compile_view_affine_verified(c,&f->module,&d)==expected && stats(c).live_bytes==baseline);
    if (expected==XR_XIR_BAD_VALUE) CHECK(d.function==0 && d.instruction==instruction);
    XrXirArtifact *checked=NULL;
    XrXirStatus actual=f->module.stage==XR_XIR_BUILT ? xir_fixture_check(c,&f->module,&checked,&d) :
        xir_fixture_verify(c,&f->module,&d);
    if (actual!=(expected==XR_XIR_OK ? XR_XIR_BAD_TYPE : expected))
        fprintf(stderr,"affine expected=%u actual common=%u stage=%u first-op=%u at %u/%u\n",
            expected,actual,f->module.stage,f->instructions[0].op,d.block,d.instruction);
    CHECK(actual==(expected==XR_XIR_OK ? XR_XIR_BAD_TYPE : expected) && !checked);
    if (expected==XR_XIR_BAD_VALUE) CHECK(d.function==0 && d.instruction==instruction);
    CHECK(stats(c).live_bytes==baseline);
}
static void affine_straight_cases(const XrXirCompileContext *c) {
    AffineFixture f;affine_fixture(&f);affine_straight(&f,3);
    f.instructions[0]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=AFFINE_OPTIONAL,.args={1,0}};
    f.instructions[1]=(XrXirInstruction){.op=XR_XIR_NULLABLE_IS_SOME,.type=XR_XIR_BOOL,.args={value(0),0}};
    affine_expect(c,&f,XR_XIR_OK,0);
    f.instructions[1]=f.instructions[0];affine_expect(c,&f,XR_XIR_BAD_VALUE,1);
    f.instructions[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=MUT,.args={1,0}};
    affine_expect(c,&f,XR_XIR_BAD_VALUE,0);
    f.module.stage=XR_XIR_CHECKED;affine_expect(c,&f,XR_XIR_BAD_VALUE,0);f.module.stage=XR_XIR_BUILT;
    affine_fixture(&f);affine_straight(&f,2);f.functions[0].operand_count=2;f.functions[0].operands=f.operands;
    f.instructions[0]=(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=PAIR,.args={0,2}};
    f.operands[0]=1;f.operands[1]=2;affine_expect(c,&f,XR_XIR_OK,0);
    f.operands[1]=1;affine_expect(c,&f,XR_XIR_BAD_VALUE,0);
    f.operands[1]=2;affine_straight(&f,3);
    f.instructions[1]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=MUT,.args={value(0),0}};
    affine_expect(c,&f,XR_XIR_BAD_VALUE,1);
    affine_fixture(&f);affine_straight(&f,3);
    f.instructions[0]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=AFFINE_OPTIONAL,.args={1,0}};
    f.instructions[1]=(XrXirInstruction){.op=XR_XIR_NULLABLE_UNWRAP,.type=MUT,.args={value(0),0}};
    affine_expect(c,&f,XR_XIR_BAD_VALUE,1);
    affine_fixture(&f);affine_straight(&f,2);
    f.instructions[0]=(XrXirInstruction){.op=XR_XIR_OWNED_RETAIN,.type=MUT,.args={1,0}};
    f.module.stage=XR_XIR_LOWERED;f.module.linkage_kind=XR_XIR_PROGRAM;
    f.declarations.root_module=0;f.declarations.entry_function=2;
    f.functions[2].parameter_count=0;f.functions[2].parameters=NULL;
    const XrXirInstruction entry_ops[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_RETURN,.args={0,0}}};const XrXirBlock entry_block={.count=2};
    f.functions[2].result=XR_XIR_I64;f.functions[2].instructions=entry_ops;f.functions[2].instruction_count=2;
    f.functions[2].blocks=&entry_block;
    affine_expect(c,&f,XR_XIR_BAD_VALUE,0);
    affine_fixture(&f);affine_straight(&f,2);f.parameters[1]=SHARED;
    f.instructions[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=SHARED,.args={1,0}};
    affine_expect(c,&f,XR_XIR_OK,0);
    affine_fixture(&f);f.parameters[2]=XR_XIR_I64;f.functions[0].result=MUT;
    f.instructions[0].args[0]=1;affine_expect(c,&f,XR_XIR_OK,0);
    affine_straight(&f,2);f.instructions[0]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,
        .type=AFFINE_OPTIONAL,.args={1,0}};f.instructions[1].args[0]=1;
    affine_expect(c,&f,XR_XIR_BAD_VALUE,1);
}
static void affine_diamond(AffineFixture *f,bool consume) {
    affine_fixture(f);f->functions[0].block_count=4;f->functions[0].instruction_count=8;
    f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=2};
    f->blocks[2]=(XrXirBlock){.first=3,.count=2};f->blocks[3]=(XrXirBlock){.first=5,.count=3};
    f->instructions[0]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0,0},.targets={1,2}};
    f->instructions[1]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=AFFINE_OPTIONAL,.args={1,0}};
    f->instructions[2]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3,0}};
    f->instructions[3]=(XrXirInstruction){.op=consume?XR_XIR_NULLABLE_SOME:XR_XIR_NULLABLE_NONE,
        .type=AFFINE_OPTIONAL,.args={consume?2u:0u,0}};
    f->instructions[4]=f->instructions[2];
    f->instructions[5]=(XrXirInstruction){.op=XR_XIR_PHI,.type=AFFINE_OPTIONAL,.args={0,4}};
    f->operands[0]=1;f->operands[1]=value(1);f->operands[2]=2;f->operands[3]=value(3);
    f->functions[0].operand_count=4;f->functions[0].operands=f->operands;
    f->instructions[6]=(XrXirInstruction){.op=XR_XIR_NULLABLE_IS_SOME,.type=XR_XIR_BOOL,.args={value(5),0}};
    f->instructions[7]=(XrXirInstruction){.op=XR_XIR_RETURN};
}
static void affine_branches(const XrXirCompileContext *c) {
    AffineFixture f;affine_diamond(&f,true);affine_expect(c,&f,XR_XIR_OK,0);
    f.instructions[6]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=AFFINE_OPTIONAL,.args={1,0}};
    affine_expect(c,&f,XR_XIR_BAD_VALUE,6);
    affine_diamond(&f,false);affine_expect(c,&f,XR_XIR_OK,0);
    f.instructions[6]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=AFFINE_OPTIONAL,.args={1,0}};
    affine_expect(c,&f,XR_XIR_BAD_VALUE,6);
    affine_diamond(&f,true);f.instructions[6]=f.instructions[5];
    f.instructions[6].args[0]=4;f.functions[0].operand_count=8;
    memcpy(f.operands+4,f.operands,4*sizeof(*f.operands));
    affine_expect(c,&f,XR_XIR_BAD_VALUE,6);
}
static void affine_loop(AffineFixture *f,bool carried) {
    affine_fixture(f);f->functions[0].block_count=3;f->functions[0].instruction_count=5;
    f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=3};
    f->blocks[2]=(XrXirBlock){.first=4,.count=1};
    f->instructions[0]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1,0}};
    f->instructions[1]=(XrXirInstruction){.op=XR_XIR_PHI,.type=MUT,.args={0,4}};
    f->operands[0]=0;f->operands[1]=1;f->operands[2]=1;f->operands[3]=carried?value(1):1;
    f->functions[0].operand_count=4;f->functions[0].operands=f->operands;
    f->instructions[2]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
    f->instructions[3]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={value(2),0},.targets={1,2}};
    f->instructions[4]=(XrXirInstruction){.op=XR_XIR_RETURN};
}
static void affine_loops_and_panic(const XrXirCompileContext *c) {
    AffineFixture f;affine_loop(&f,true);affine_expect(c,&f,XR_XIR_OK,0);
    affine_loop(&f,false);affine_expect(c,&f,XR_XIR_BAD_VALUE,1);
    affine_fixture(&f);f.functions[0].block_count=3;f.functions[0].instruction_count=6;
    f.blocks[0]=(XrXirBlock){.count=1};f.blocks[1]=(XrXirBlock){.first=1,.count=2,.panic=2};
    f.blocks[2]=(XrXirBlock){.first=3,.count=3};
    f.instructions[0]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1,0}};
    f.instructions[1]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=AFFINE_OPTIONAL,.args={1,0}};
    f.instructions[2]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f.instructions[3]=(XrXirInstruction){.op=XR_XIR_PANIC_CATCH,.type=XR_XIR_PANIC_INFO};
    f.instructions[4]=f.instructions[1];f.instructions[5]=f.instructions[2];
    affine_expect(c,&f,XR_XIR_BAD_VALUE,4);
    f.instructions[4].args[0]=2;affine_expect(c,&f,XR_XIR_OK,0);
}
static void affine_calls(const XrXirCompileContext *c) {
    AffineFixture f;affine_fixture(&f);affine_straight(&f,3);f.functions[0].operand_count=2;f.functions[0].operands=f.operands;
    f.operands[0]=1;f.instructions[0]=(XrXirInstruction){.op=XR_XIR_CALL,.args={0,1},.immediate=2};
    f.instructions[1]=f.instructions[0];f.instructions[1].args[0]=1;f.operands[1]=1;
    affine_expect(c,&f,XR_XIR_OK,0);
    f.functions[2].parameter_count=2;f.functions[0].operand_count=4;f.functions[0].operands=f.operands;
    f.instructions[0].args[1]=2;f.instructions[1].args[0]=2;f.instructions[1].args[1]=2;
    f.operands[1]=2;f.operands[2]=1;f.operands[3]=2;
    affine_expect(c,&f,XR_XIR_OK,0);f.operands[1]=1;affine_expect(c,&f,XR_XIR_BAD_VALUE,0);
    affine_fixture(&f);affine_straight(&f,4);f.functions[0].operand_count=1;f.functions[0].operands=f.operands;f.operands[0]=1;
    f.instructions[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)261,.immediate=2};
    f.instructions[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.args={0,1},.immediate=3};
    f.instructions[2]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=AFFINE_OPTIONAL,.args={1,0}};
    affine_expect(c,&f,XR_XIR_OK,0);f.instructions[3]=f.instructions[1];
    f.instructions[3].args[0]=1;f.operands[1]=1;f.functions[0].operand_count=2;
    f.instructions[4]=(XrXirInstruction){.op=XR_XIR_RETURN};f.functions[0].instruction_count=5;f.blocks[0].count=5;
    affine_expect(c,&f,XR_XIR_BAD_VALUE,3);
}
static void affine_simultaneous(const XrXirCompileContext *c) {
    AffineFixture f;affine_loop(&f,true);f.functions[0].instruction_count=6;
    f.blocks[1].count=4;f.blocks[2].first=5;f.functions[0].operand_count=8;f.functions[0].operands=f.operands;
    f.instructions[2]=f.instructions[1];f.instructions[2].args[0]=4;
    f.operands[3]=value(2);f.operands[4]=0;f.operands[5]=2;f.operands[6]=1;f.operands[7]=value(1);
    f.instructions[3]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
    f.instructions[4]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={value(3),0},.targets={1,2}};
    f.instructions[5]=(XrXirInstruction){.op=XR_XIR_RETURN};affine_expect(c,&f,XR_XIR_OK,0);
    f.operands[5]=1;affine_expect(c,&f,XR_XIR_BAD_VALUE,2);
}
static void affine_invoke(const XrXirCompileContext *c) {
    AffineFixture f;affine_fixture(&f);f.functions[0].block_count=3;f.functions[0].instruction_count=6;
    f.blocks[0]=(XrXirBlock){.count=1};f.blocks[1]=(XrXirBlock){.first=1,.count=2};
    f.blocks[2]=(XrXirBlock){.first=3,.count=3};
    f.instructions[0]=(XrXirInstruction){.op=XR_XIR_INVOKE,.args={0,1},.targets={1,2},.immediate=2};
    f.instructions[1]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=AFFINE_OPTIONAL,.args={1,0}};
    f.instructions[2]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f.instructions[3]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR};
    f.instructions[4]=f.instructions[1];f.instructions[5]=f.instructions[2];
    f.functions[0].operand_count=1;f.functions[0].operands=f.operands;f.operands[0]=1;
    f.functions[2].instruction_count=1;f.callee[0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    affine_expect(c,&f,XR_XIR_OK,0);
    f.instructions[4].op=XR_XIR_COPY;f.instructions[4].type=MUT;
    XrXirDiagnostic d={0};CHECK(xir_compile_view_affine_verified(c,&f.module,&d)==XR_XIR_BAD_VALUE);
    CHECK(d.block==2 && d.instruction==4);
}
static void affine_dense(const XrXirCompileContext *c) {
    XrXirType parameters[130];XrXirInstruction instructions[131]={0};
    const XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=MUT}};
    const XrXirTypes types={nodes,2,NULL,NULL};
    for (uint32_t i=0;i<130;++i) {parameters[i]=MUT;
        instructions[i]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=AFFINE_OPTIONAL,.args={i,0}};}
    instructions[130]=(XrXirInstruction){.op=XR_XIR_RETURN};
    const XrXirBlock block={.count=131};
    const XrXirFunction function={.name="dense",.name_length=5,.parameters=parameters,.parameter_count=130,.instructions=instructions,
        .instruction_count=131,.blocks=&block,.block_count=1};
    AffineFixture f;affine_fixture(&f);f.types=types;f.functions[0]=function;
    CHECK(xr_xir_compile_types_structure_verify(c,&types)==XR_XIR_OK);
    uint64_t start=stats(c).work;CHECK(xir_compile_view_affine_verified(c,&f.module,NULL)==XR_XIR_OK);
    CHECK(stats(c).work-start<5000);
    instructions[129].args[0]=65;XrXirDiagnostic d={0};
    CHECK(xir_compile_view_affine_verified(c,&f.module,&d)==XR_XIR_BAD_VALUE && d.instruction==129);
}
static void affine_type_dag(const XrXirCompileContext *c) {
    XrXirTypeNode nodes[71]={0};XrXirCallableParameter fields[69][2]={0};
    nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    for (uint32_t i=1;i<70;++i) {
        fields[i-1][0]=(XrXirCallableParameter){(XrXirType)(255+i),0};fields[i-1][1]=fields[i-1][0];
        nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[i-1],.parameter_count=2};
    }
    nodes[70]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)325};
    const XrXirTypes types={nodes,71,NULL,NULL};
    const XrXirInstruction operations[]={{.op=XR_XIR_NULLABLE_NONE,.type=(XrXirType)326},
        {.op=XR_XIR_NULLABLE_IS_SOME,.type=XR_XIR_BOOL,.args={0,0}},{.op=XR_XIR_RETURN}};
    const XrXirBlock block={.count=3};
    const XrXirFunction function={.name="dag",.name_length=3,.instructions=operations,
        .instruction_count=3,.blocks=&block,.block_count=1};
    AffineFixture f;affine_fixture(&f);f.types=types;f.functions[0]=function;
    CHECK(xr_xir_compile_types_structure_verify(c,&types)==XR_XIR_OK);uint64_t start=stats(c).work;
    CHECK(xir_compile_view_affine_verified(c,&f.module,NULL)==XR_XIR_OK);
    CHECK(stats(c).work-start<2000);
}
static XrXirStatus affine_prove(const XrXirCompileContext *c,const AffineFixture *f) {
    return xir_compile_view_affine_verified(c,&f->module,NULL);
}
static void affine_resources(void) {
    AffineFixture f;affine_loop(&f,true);XrXirCompileContext c=affine_owner();
    uint64_t baseline=stats(&c).live_bytes;size_t start=attempts;
    CHECK(affine_prove(&c,&f)==XR_XIR_OK);XrCompileResourceStats measured=stats(&c);
    size_t count=attempts-start;CHECK(count>=5 && count<32);
    CHECK(affine_prove(&c,&f)==XR_XIR_OK && stats(&c).live_bytes==baseline);owner_free(&c,baseline);
    for (size_t point=0;point<count;++point) {
        c=affine_owner();baseline=stats(&c).live_bytes;injected=false;fail_at=attempts+point;
        CHECK(affine_prove(&c,&f)==XR_XIR_OUT_OF_MEMORY && injected);
        fail_at=SIZE_MAX;owner_free(&c,baseline);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits={1048576,1048576,4000000};
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;
        else limits.work=measured.work-minus;
        c=owner_new(limits);baseline=stats(&c).live_bytes;
        CHECK(affine_prove(&c,&f)==(minus?XR_XIR_BUDGET:XR_XIR_OK));owner_free(&c,baseline);
    }
    c=affine_owner();baseline=stats(&c).live_bytes;c.limits.frame_bytes=1;
    CHECK(affine_prove(&c,&f)==XR_XIR_BUDGET);owner_free(&c,baseline);
    printf("affine CFG: %zu complete allocation failures, three axes/same owner/physical0 PASS\n",count);
}
int main(void) {
    XrXirCompileContext c=affine_owner();uint64_t baseline=stats(&c).live_bytes;
    affine_straight_cases(&c);affine_branches(&c);affine_loops_and_panic(&c);affine_calls(&c);
    affine_simultaneous(&c);affine_invoke(&c);affine_dense(&c);affine_type_dag(&c);
    owner_free(&c,baseline);affine_resources();
    puts("affine CFG: copies/transfers/branch meet/loop PHIs/panic/call borrows and closed common boundary PASS");return 0;
}
