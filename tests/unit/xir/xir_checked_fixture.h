/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_checked_fixture.h - Declaration-rich packet ownership fixture
 *
 * KEY CONCEPT:
 *   Tests reconstruct Built views explicitly; the reader accepts only Checked.
 */
#ifndef XIR_CHECKED_FIXTURE_H
#define XIR_CHECKED_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir_program_fixture.h"
typedef struct CheckedAtomicPool {
    XrXirTypeNode nodes[512];
    XrXirTypes types;
    XrXirFunction functions[9];
    XrXirInstruction instructions[8][12];
    XrXirSlot slots[5];
    XrXirDeclarations declarations;
} CheckedAtomicPool;
static void checked_atomic_pool(XrXirModule *built,CheckedAtomicPool *pool) {
    CHECK(built && built->types && built->types->count<=510 && built->function_count==9);
    XrXirType old=built->declarations->slots[0].type;
    XrXirType old_cell=built->declarations->slots[4].type;
    CHECK(old!=old_cell && built->declarations->slots[4].mutable==1 &&
        built->functions[0].instruction_count==5 &&
        built->functions[0].instructions[1].op==XR_XIR_CELL_NEW &&
        built->functions[0].instructions[1].type==old_cell);
    XrXirType atomic=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+built->types->count);
    XrXirType cell=XR_XIR_UNIT;
    pool->types=*built->types;
    if(pool->types.count)memcpy(pool->nodes,pool->types.nodes,pool->types.count*sizeof(*pool->nodes));
    for(uint32_t i=0;i<pool->types.count;++i) {
        const XrXirTypeNode *node=&pool->nodes[i];
        if(node->kind==XR_XIR_TYPE_CELL && node->element==XR_XIR_STRING &&
            !node->parameters && !node->parameter_count && node->result==XR_XIR_UNIT &&
            !node->flags && !node->parameter_span && !node->nominal.declaration &&
            !node->nominal.arguments && !node->nominal.argument_count &&
            !node->nominal.fields && !node->nominal.field_count) {
            cell=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
            break;
        }
    }
    pool->nodes[pool->types.count++]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_I64};
    if(cell==XR_XIR_UNIT) {
        cell=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+pool->types.count);
        pool->nodes[pool->types.count++]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_STRING};
    }
    pool->types.nodes=pool->nodes;
    memcpy(pool->functions,built->functions,sizeof(pool->functions));
    for(uint32_t f=0;f<8;++f){
        CHECK(pool->functions[f].instruction_count<=12);
        memcpy(pool->instructions[f],pool->functions[f].instructions,
            pool->functions[f].instruction_count*sizeof(XrXirInstruction));
        pool->functions[f].instructions=pool->instructions[f];
        if(pool->functions[f].result==old)pool->functions[f].result=atomic;
        else if(pool->functions[f].result==old_cell)pool->functions[f].result=cell;
        for(uint32_t i=0;i<pool->functions[f].instruction_count;++i) {
            if(pool->instructions[f][i].type==old)pool->instructions[f][i].type=atomic;
            else if(pool->instructions[f][i].type==old_cell)pool->instructions[f][i].type=cell;
        }
    }
    pool->declarations=*built->declarations;
    CHECK(pool->declarations.slot_count==5);
    memcpy(pool->slots,pool->declarations.slots,sizeof(pool->slots));
    for(uint32_t i=0;i<5;++i) {
        if(pool->slots[i].type==old)pool->slots[i].type=atomic;
        else if(pool->slots[i].type==old_cell)pool->slots[i].type=cell;
    }
    pool->declarations.slots=pool->slots;
    built->functions=pool->functions;built->declarations=&pool->declarations;built->types=&pool->types;
}
static XrXirArtifact *checked_fixture(const XrXirCompileContext *context) {
    (void)&program_fixture;
    XrXirArtifact *definition = program_fixture_checked(context, 0), *checked = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(definition);
    CHECK(built.stage == XR_XIR_CHECKED && !built.provenance);
    built.stage = XR_XIR_BUILT;
    XrXirFunction functions[9];
    memcpy(functions, built.functions, 8 * sizeof(*functions));
    const XrXirType parameters[] = {XR_XIR_I64, XR_XIR_STRING};
    const uint32_t operands[] = {0, 2};
    const XrXirInstruction ops[] = {
        {XR_XIR_COPY, XR_XIR_STRING, {1}, {0}, 0, {0}},
        {XR_XIR_PRINT, XR_XIR_UNIT, {0, 2}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    const XrXirBlock block = {0, 3, 0, 0};
    functions[8] = (XrXirFunction) {"args", 4, parameters, 2, XR_XIR_I64, &block, 1, ops, 3, operands, 2};
    XrXirFunctionIdentity identities[9];
    memcpy(identities, built.declarations->functions, 8 * sizeof(*identities));
    identities[8] = (XrXirFunctionIdentity) {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0};
    XrXirDeclarations declarations = *built.declarations;
    declarations.functions = identities;
    built.functions = functions; built.function_count = 9; built.declarations = &declarations;
    CHECK(xir_fixture_check(context, &built, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(definition);definition=NULL;
    return checked;
}
#endif
