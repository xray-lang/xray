/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_cell_index_fixture.h - Independent typed Cell invalidation witnesses
 *
 * KEY CONCEPT:
 *   A copied enum snapshot keeps its original variant while mutable aliases
 *   recover the complete type after an execution or host boundary.
 */
#ifndef XIR_ERROR_CELL_INDEX_FIXTURE_H
#define XIR_ERROR_CELL_INDEX_FIXTURE_H
#include "xir/xxir_nominal.h"
enum { CI_ENUM=256, CI_CELL=257, CI_TASK=258, CI_FN=259,
       CI_SCALARS=512, CI_BOUNDARIES=256, CI_MAX_OPS=CI_SCALARS+CI_BOUNDARIES+16 };
typedef enum CellBoundary {
    CI_CALL, CI_UNKNOWN, CI_GO, CI_OUTPUT, CI_PRINT, CI_STREAM,
    CI_SUSPEND, CI_TIMER, CI_WRITE, CI_CLEANUP, CI_PANIC, CI_AWAIT, CI_INVOKE, CI_INVOKE_UNKNOWN, CI_PHI
} CellBoundary;
typedef struct CellIndexFixture {
    XrXirNominalVariant variants[2];
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirTypeNode nodes[4];
    XrXirTypes types;
    XrXirType parameters[4], cleanup_parameter;
    XrXirInstruction init, subject[CI_MAX_OPS], cleanup[3];
    XrXirBlock one, blocks[4], cleanup_block;
    XrXirFunction functions[4];
    uint32_t operands[4];
    XrXirSourceModule source;
    XrXirFunctionIdentity identities[4];
    XrXirLiteral literal;
    XrXirDeclarations declarations;
    XrXirModule module;
    uint32_t snapshot, new_cell, alias, read_after;
    uint32_t expected_ids[3], expected_count;
} CellIndexFixture;
static void ci_invoke_fixture(CellIndexFixture *f,CellBoundary boundary,bool snapshot){
    XrXirInstruction invoke={.op=XR_XIR_INVOKE,.targets={1,2},.immediate=2};
    if(boundary==CI_INVOKE_UNKNOWN)invoke=(XrXirInstruction){.op=XR_XIR_INVOKE_INDIRECT,.targets={1,2},.immediate=1};
    if(boundary==CI_AWAIT){
        f->parameters[1]=(XrXirType)CI_TASK;
        invoke=(XrXirInstruction){.op=XR_XIR_TASK_AWAIT,.args={1},.targets={1,2}};
    }
    f->subject[4]=invoke;
    f->subject[5]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)CI_ENUM,.args={f->new_cell}};
    f->subject[6]=(XrXirInstruction){.op=XR_XIR_THROW,.args={snapshot?f->snapshot:9}};
    f->subject[7]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=4};
    f->subject[8]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)CI_ENUM,.args={f->new_cell}};
    f->subject[9]=(XrXirInstruction){.op=XR_XIR_THROW,.args={snapshot?f->snapshot:12}};
    f->blocks[0]=(XrXirBlock){.count=5};f->blocks[1]=(XrXirBlock){.first=5,.count=2};
    f->blocks[2]=(XrXirBlock){.first=7,.count=3};
    f->functions[1].block_count=3;f->functions[1].instruction_count=10;
}
static void ci_fixture(CellIndexFixture *f, CellBoundary boundary, bool snapshot, bool large) {
    memset(f,0,sizeof(*f));
    f->variants[0]=(XrXirNominalVariant){{"First",5},0,0};
    f->variants[1]=(XrXirNominalVariant){{"Second",6},0,0};
    f->nominal=(XrXirNominalDeclaration){.module={"cells",5},.name={"Failure",7},
        .exported=1,.kind=XR_XIR_NOMINAL_ENUM,.variants=f->variants,.variant_count=2};
    f->nominals=(XrXirNominalTable){.declarations=&f->nominal,.count=1};
    f->nodes[0].kind=XR_XIR_TYPE_NOMINAL;
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)CI_ENUM};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_UNIT};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=4,.nominals=&f->nominals};
    f->parameters[0]=(XrXirType)CI_CELL; f->parameters[1]=(XrXirType)CI_FN;
    f->parameters[2]=XR_XIR_I64; f->parameters[3]=XR_XIR_STRING;
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN}; f->one=(XrXirBlock){.count=1};
    f->source=(XrXirSourceModule){.name="cells",.name_length=5,.initializer=0};
    f->literal=(XrXirLiteral){"",0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,
        .functions=f->identities,.literals=&f->literal,.literal_count=1,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->one,.block_count=1,.instructions=&f->init,.instruction_count=1};
    f->functions[2]=f->functions[0];f->functions[2].name="pure";
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=f->functions,.function_count=3,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
    uint32_t p=4;
    f->subject[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)CI_ENUM};
    f->subject[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)CI_CELL,.args={p}};
    f->subject[2]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)CI_CELL,.args={p+1}};
    f->subject[3]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)CI_ENUM,.args={p+2}};
    f->new_cell=p+1;f->alias=p+2;f->snapshot=p+3;
    f->expected_ids[0]=0;f->expected_ids[1]=p+1;f->expected_ids[2]=p+2;f->expected_count=3;
    uint32_t n=4;
    if(large)for(uint32_t i=0;i<CI_SCALARS;++i)
        f->subject[n++]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
    uint32_t repeat=large?CI_BOUNDARIES:1;
    for(uint32_t i=0;i<repeat;++i){
        XrXirInstruction op={.op=XR_XIR_CALL,.immediate=2};
        if(boundary==CI_UNKNOWN)op=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.immediate=1};
        if(boundary==CI_GO)op=(XrXirInstruction){.op=XR_XIR_GO,.type=(XrXirType)CI_TASK,.immediate=2};
        if(boundary==CI_OUTPUT)op=(XrXirInstruction){.op=XR_XIR_OUTPUT,.args={2},.immediate=1};
        if(boundary==CI_PRINT)op=(XrXirInstruction){.op=XR_XIR_PRINT,.args={0,1}};
        if(boundary==CI_STREAM)op=(XrXirInstruction){.op=XR_XIR_WRITE_STREAM,.type=XR_XIR_BOOL,.args={3},.immediate=1};
        if(boundary==CI_SUSPEND)op=(XrXirInstruction){.op=XR_XIR_SUSPEND};
        if(boundary==CI_TIMER)op=(XrXirInstruction){.op=XR_XIR_TIMER_AFTER_MS,.args={2}};
        if(boundary==CI_WRITE){
            f->subject[n++]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)CI_ENUM,.immediate=1};
            op=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={f->alias,p+n-1}};
        }
        f->subject[n++]=op;
    }
    f->read_after=p+n;
    f->subject[n++]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)CI_ENUM,.args={f->new_cell}};
    f->subject[n++]=(XrXirInstruction){.op=XR_XIR_THROW,.args={snapshot?f->snapshot:f->read_after}};
    f->blocks[0]=(XrXirBlock){.count=n};
    f->functions[1]=(XrXirFunction){.name="subject",.name_length=7,.parameters=f->parameters,
        .parameter_count=p,.result=XR_XIR_UNIT,.blocks=f->blocks,.block_count=1,
        .instructions=f->subject,.instruction_count=n};
    if(boundary==CI_PRINT){f->operands[0]=2;f->functions[1].operands=f->operands;f->functions[1].operand_count=1;}
    if(boundary==CI_CLEANUP || boundary==CI_PANIC){
        f->cleanup_parameter=(XrXirType)CI_CELL;
        f->cleanup[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)CI_ENUM,.immediate=1};
        f->cleanup[1]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={0,1}};
        f->cleanup[2]=f->init;f->cleanup_block=(XrXirBlock){.count=3};
        f->functions[3]=(XrXirFunction){.name="cleanup",.name_length=7,.parameters=&f->cleanup_parameter,
            .parameter_count=1,.result=XR_XIR_UNIT,.blocks=&f->cleanup_block,.block_count=1,
            .instructions=f->cleanup,.instruction_count=3};
        f->identities[3].cleanup_owner=2;f->module.function_count=4;
        f->subject[4]=(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.args={0,1},.targets={1},.immediate=3};
        f->operands[0]=f->new_cell;f->functions[1].operands=f->operands;f->functions[1].operand_count=1;
        f->subject[5]=(XrXirInstruction){.op=XR_XIR_CLEANUP_LEAVE,.targets={2}};
        f->blocks[0]=(XrXirBlock){.count=5};
        f->blocks[1]=(XrXirBlock){.first=5,.count=1,.frontier=5};
        f->blocks[2]=(XrXirBlock){.first=6,.count=2};
        f->functions[1].block_count=3;f->functions[1].instruction_count=8;
        f->subject[6]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)CI_ENUM,.args={f->new_cell}};
        f->read_after=p+6;
        f->subject[7]=(XrXirInstruction){.op=XR_XIR_THROW,.args={snapshot?f->snapshot:f->read_after}};
        if(boundary==CI_PANIC){
            f->subject[5].targets[0]=3;f->blocks[1].panic=2;
            f->subject[6]=(XrXirInstruction){.op=XR_XIR_PANIC_CATCH};
            f->subject[7]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)CI_ENUM,.args={f->new_cell}};
            f->read_after=p+7;
            f->subject[8]=(XrXirInstruction){.op=XR_XIR_THROW,.args={snapshot?f->snapshot:f->read_after}};
            f->subject[9]=f->init;f->blocks[2].count=3;f->blocks[3]=(XrXirBlock){.first=9,.count=1};
            f->functions[1].block_count=4;f->functions[1].instruction_count=10;
        }
    }
    if(boundary==CI_AWAIT || boundary==CI_INVOKE || boundary==CI_INVOKE_UNKNOWN)ci_invoke_fixture(f,boundary,snapshot);
    if(boundary==CI_PHI){
        f->parameters[1]=XR_XIR_BOOL;p=2;f->functions[1].parameter_count=p;
        f->subject[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)CI_ENUM};
        f->subject[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)CI_CELL,.args={2}};
        f->subject[2]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,2}};
        f->subject[3]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}};
        f->subject[4]=f->subject[3];
        f->subject[5]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)CI_CELL,.args={0,4}};
        f->operands[0]=1;f->operands[1]=3;f->operands[2]=2;f->operands[3]=0;
        f->subject[6]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)CI_ENUM,.args={7}};
        f->subject[7]=(XrXirInstruction){.op=XR_XIR_THROW,.args={8}};
        f->blocks[0]=(XrXirBlock){.count=3};f->blocks[1]=(XrXirBlock){.first=3,.count=1};
        f->blocks[2]=(XrXirBlock){.first=4,.count=1};f->blocks[3]=(XrXirBlock){.first=5,.count=3};
        f->functions[1].block_count=4;f->functions[1].instruction_count=8;
        f->functions[1].operands=f->operands;f->functions[1].operand_count=4;
        f->expected_ids[1]=3;f->expected_ids[2]=7;
    }
}
/* Literal bit positions: unknown=1, unidentified=2, First=4, Second=8. */
static uint64_t ci_expected(CellBoundary boundary,bool snapshot){
    uint64_t variants=boundary==CI_PHI?UINT64_C(12):snapshot?UINT64_C(4):boundary==CI_WRITE?UINT64_C(8):UINT64_C(12);
    return variants|(boundary==CI_UNKNOWN?UINT64_C(1):0);
}
#endif // XIR_ERROR_CELL_INDEX_FIXTURE_H
