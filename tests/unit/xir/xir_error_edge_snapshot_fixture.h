/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_edge_snapshot_fixture.h - Independent CFG and simultaneous PHI facts
 */
#ifndef XIR_ERROR_EDGE_SNAPSHOT_FIXTURE_H
#define XIR_ERROR_EDGE_SNAPSHOT_FIXTURE_H
#include "xir/xxir_nominal.h"
enum { EE_ENUM=256, EE_CELL=257, EE_SCALARS=512, EE_BLOCKS=32, EE_MAX_OPS=576 };
typedef enum EdgeCase {
    EE_JUMP, EE_BRANCH, EE_FILTER, EE_INVOKE, EE_CLEANUP, EE_PANIC,
    EE_TEMPORARY, EE_PHI, EE_BULK
} EdgeCase;
typedef struct EdgeFixture {
    XrXirNominalVariant variants[2];
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirTypeNode nodes[2];
    XrXirTypes types;
    XrXirType parameters[2], cleanup_parameter;
    XrXirInstruction init, subject[EE_MAX_OPS], thrower[2], cleanup[3];
    XrXirBlock one, blocks[EE_BLOCKS], throw_block, cleanup_block;
    XrXirFunction functions[4];
    uint32_t operands[8];
    XrXirSourceModule source;
    XrXirFunctionIdentity identities[4];
    XrXirDeclarations declarations;
    XrXirModule module;
} EdgeFixture;
static void ee_cleanup_fixture(EdgeFixture *f, EdgeCase mode, bool snapshot) {
    f->cleanup_parameter=(XrXirType)EE_CELL;
    f->cleanup[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM,.immediate=1};
    f->cleanup[1]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={0,1}};
    f->cleanup[2]=f->init;f->cleanup_block=(XrXirBlock){.count=3};
    f->functions[3]=(XrXirFunction){.name="cleanup",.name_length=7,
        .parameters=&f->cleanup_parameter,.parameter_count=1,.result=XR_XIR_UNIT,
        .blocks=&f->cleanup_block,.block_count=1,.instructions=f->cleanup,.instruction_count=3};
    f->identities[3].cleanup_owner=2;f->module.function_count=4;
    f->subject[4]=(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.args={0,1},.targets={1},.immediate=3};
    f->operands[0]=3;f->functions[1].operands=f->operands;f->functions[1].operand_count=1;
    f->subject[5]=(XrXirInstruction){.op=XR_XIR_CLEANUP_LEAVE,.targets={2}};
    f->subject[6]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)EE_ENUM,.args={3}};
    f->subject[7]=(XrXirInstruction){.op=XR_XIR_THROW,.args={snapshot?5u:8u}};
    f->blocks[0]=(XrXirBlock){.count=5};f->blocks[1]=(XrXirBlock){.first=5,.count=1,.frontier=5};
    f->blocks[2]=(XrXirBlock){.first=6,.count=2};
    f->functions[1].block_count=3;f->functions[1].instruction_count=8;
    if (mode==EE_PANIC) {
        f->subject[5].targets[0]=3;f->blocks[1].panic=2;
        f->subject[6]=(XrXirInstruction){.op=XR_XIR_PANIC_CATCH};
        f->subject[7]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)EE_ENUM,.args={3}};
        f->subject[8]=(XrXirInstruction){.op=XR_XIR_THROW,.args={snapshot?5u:9u}};
        f->subject[9]=f->init;f->blocks[2].count=3;f->blocks[3]=(XrXirBlock){.first=9,.count=1};
        f->functions[1].block_count=4;f->functions[1].instruction_count=10;
    }
}
static void ee_phi_fixture(EdgeFixture *f) {
    f->subject[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
    f->subject[1]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM,.immediate=1};
    f->subject[2]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    f->subject[3]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)EE_ENUM,.args={0,4}};
    f->subject[4]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)EE_ENUM,.args={4,4}};
    f->subject[5]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={2,3}};
    f->subject[6]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    f->subject[7]=(XrXirInstruction){.op=XR_XIR_THROW,.args={5}};
    static const uint32_t pairs[8]={0,2,2,6,0,3,2,5};
    memcpy(f->operands,pairs,sizeof(pairs));
    f->blocks[0]=(XrXirBlock){.count=3};f->blocks[1]=(XrXirBlock){.first=3,.count=3};
    f->blocks[2]=(XrXirBlock){.first=6,.count=1};f->blocks[3]=(XrXirBlock){.first=7,.count=1};
    f->functions[1].block_count=4;f->functions[1].instruction_count=8;
    f->functions[1].operands=f->operands;f->functions[1].operand_count=8;
}
static void ee_bulk_fixture(EdgeFixture *f) {
    for (uint32_t i=0;i<EE_SCALARS;++i)
        f->subject[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
    uint32_t n=EE_SCALARS;
    f->subject[n++]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
    f->subject[n++]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    f->blocks[0]=(XrXirBlock){.count=n};
    for (uint32_t b=1;b<EE_BLOCKS-1;++b) {
        f->blocks[b]=(XrXirBlock){.first=n,.count=1};
        f->subject[n++]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={b+1}};
    }
    f->blocks[EE_BLOCKS-1]=(XrXirBlock){.first=n,.count=1};
    f->subject[n++]=(XrXirInstruction){.op=XR_XIR_THROW,.args={EE_SCALARS}};
    f->functions[1].parameters=NULL;f->functions[1].parameter_count=0;
    f->functions[1].block_count=EE_BLOCKS;f->functions[1].instruction_count=n;
}
static void ee_fixture(EdgeFixture *f, EdgeCase mode, bool snapshot) {
    memset(f,0,sizeof(*f));
    f->variants[0]=(XrXirNominalVariant){{"First",5},0,0};
    f->variants[1]=(XrXirNominalVariant){{"Second",6},0,0};
    f->nominal=(XrXirNominalDeclaration){.module={"edges",5},.name={"Failure",7},
        .exported=1,.kind=XR_XIR_NOMINAL_ENUM,.variants=f->variants,.variant_count=2};
    f->nominals=(XrXirNominalTable){.declarations=&f->nominal,.count=1};
    f->nodes[0].kind=XR_XIR_TYPE_NOMINAL;
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)EE_ENUM};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2,.nominals=&f->nominals};
    f->parameters[0]=(XrXirType)EE_CELL;f->parameters[1]=XR_XIR_BOOL;
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN};f->one=(XrXirBlock){.count=1};
    f->source=(XrXirSourceModule){.name="edges",.name_length=5,.initializer=0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->one,.block_count=1,.instructions=&f->init,.instruction_count=1};
    f->thrower[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM,.immediate=1};
    f->thrower[1]=(XrXirInstruction){.op=XR_XIR_THROW,.args={0}};
    f->throw_block=(XrXirBlock){.count=2};
    f->functions[2]=(XrXirFunction){.name="thrower",.name_length=7,.result=XR_XIR_UNIT,
        .blocks=&f->throw_block,.block_count=1,.instructions=f->thrower,.instruction_count=2};
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=f->functions,.function_count=3,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
    f->subject[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
    f->subject[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)EE_CELL,.args={2}};
    f->subject[2]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)EE_CELL,.args={3}};
    f->subject[3]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)EE_ENUM,.args={4}};
    f->subject[4]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    f->subject[5]=(XrXirInstruction){.op=XR_XIR_THROW,.args={5}};
    f->blocks[0]=(XrXirBlock){.count=5};f->blocks[1]=(XrXirBlock){.first=5,.count=1};
    f->functions[1]=(XrXirFunction){.name="subject",.name_length=7,.parameters=f->parameters,
        .parameter_count=2,.result=XR_XIR_UNIT,.blocks=f->blocks,.block_count=2,
        .instructions=f->subject,.instruction_count=6};
    if (mode==EE_BRANCH) {
        f->subject[4]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,2}};
        f->subject[6]=f->subject[5];f->blocks[2]=(XrXirBlock){.first=6,.count=1};
        f->functions[1].block_count=3;f->functions[1].instruction_count=7;
    }
    if (mode==EE_INVOKE) {
        f->subject[4]=(XrXirInstruction){.op=XR_XIR_INVOKE,.targets={1,2},.immediate=2};
        f->subject[6]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=4};
        f->subject[7]=(XrXirInstruction){.op=XR_XIR_THROW,.args={8}};
        f->blocks[2]=(XrXirBlock){.first=6,.count=2};
        f->functions[1].block_count=3;f->functions[1].instruction_count=8;
    }
    if (mode==EE_FILTER) {
        f->subject[4]=(XrXirInstruction){.op=XR_XIR_ENUM_TAG,.type=XR_XIR_I64,.args={2}};
        f->subject[5]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        f->subject[6]=(XrXirInstruction){.op=XR_XIR_EQ_INT,.type=XR_XIR_BOOL,.args={6,7}};
        f->subject[7]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={8},.targets={1,2}};
        f->subject[8]=(XrXirInstruction){.op=XR_XIR_THROW,.args={5}};
        f->subject[9]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM,.immediate=1};
        f->subject[10]=(XrXirInstruction){.op=XR_XIR_THROW,.args={11}};
        f->blocks[0].count=8;f->blocks[1]=(XrXirBlock){.first=8,.count=1};
        f->blocks[2]=(XrXirBlock){.first=9,.count=2};
        f->functions[1].block_count=3;f->functions[1].instruction_count=11;
    }
    if (mode==EE_TEMPORARY) {
        f->functions[2]=f->functions[0];f->functions[2].name="pure";
        f->subject[5]=(XrXirInstruction){.op=XR_XIR_CALL,.immediate=2};
        f->subject[6]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM,.immediate=1};
        f->subject[7]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={4,8}};
        f->subject[8]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)EE_ENUM,.args={3}};
        f->subject[9]=(XrXirInstruction){.op=XR_XIR_THROW,.args={snapshot?5u:10u}};
        f->blocks[1].count=5;f->functions[1].instruction_count=10;
    }
    if (mode==EE_CLEANUP || mode==EE_PANIC) ee_cleanup_fixture(f,mode,snapshot);
    if (mode==EE_PHI) ee_phi_fixture(f);
    if (mode==EE_BULK) ee_bulk_fixture(f);
}
/* Literal bits: unknown=1, unidentified=2, First=4, Second=8. */
static uint64_t ee_expected(EdgeCase mode, bool snapshot) {
    if (mode==EE_INVOKE || mode==EE_PHI) return UINT64_C(12);
    if (mode==EE_CLEANUP || mode==EE_PANIC) return snapshot?UINT64_C(4):UINT64_C(12);
    if (mode==EE_TEMPORARY) return snapshot?UINT64_C(4):UINT64_C(8);
    return UINT64_C(4);
}
#endif // XIR_ERROR_EDGE_SNAPSHOT_FIXTURE_H
