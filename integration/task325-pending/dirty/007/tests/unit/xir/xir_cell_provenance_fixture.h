/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_provenance_fixture.h - Real producer, call, capture and cleanup edges
 */
#ifndef XIR_CELL_PROVENANCE_FIXTURE_H
#define XIR_CELL_PROVENANCE_FIXTURE_H
typedef struct CellProofFixture {
    XrXirTypeNode nodes[2]; XrXirTypes types; XrXirType parameter;
    XrXirInstruction done, entry[8], read[2], cleanup[3];
    XrXirBlock one, pair, three, entry_blocks[3];
    uint32_t operands[3]; XrXirFunction functions[6];
    XrXirFunctionIdentity identities[6]; XrXirSourceModule source;
    XrXirDeclarations declarations; XrXirModule module;
} CellProofFixture;
static void cell_proof_fixture(CellProofFixture *f) {
    memset(f,0,sizeof(*f)); f->parameter = (XrXirType)256;
    f->nodes[0] = (XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->nodes[1] = (XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types = (XrXirTypes){f->nodes,2,NULL,NULL};
    f->done = (XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->entry[0] = (XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}};
    f->entry[1] = (XrXirInstruction){XR_XIR_CELL_NEW,(XrXirType)256,{0},{0},0,{0}};
    f->entry[2] = (XrXirInstruction){XR_XIR_FUNCTION_REF,(XrXirType)257,{0,1},{0},3,{0}};
    f->entry[3] = (XrXirInstruction){XR_XIR_CALL,XR_XIR_I64,{1,1},{0},2,{0}};
    f->entry[4] = (XrXirInstruction){XR_XIR_CALL_INDIRECT,XR_XIR_I64,{0},{0},2,{0}};
    f->entry[5] = (XrXirInstruction){XR_XIR_CLEANUP_REGISTER,XR_XIR_UNIT,{2,1},{1},4,{0}};
    f->entry[6] = (XrXirInstruction){XR_XIR_CLEANUP_LEAVE,XR_XIR_UNIT,{0},{2},0,{0}};
    f->entry[7] = (XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{4},{0},0,{0}};
    f->read[0] = (XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_I64,{0},{0},0,{0}};
    f->read[1] = (XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}};
    f->cleanup[0] = (XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},9,{0}};
    f->cleanup[1] = (XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{0,1},{0},0,{0}};
    f->cleanup[2] = f->done;
    f->one = (XrXirBlock){0,1,0,0}; f->pair = (XrXirBlock){0,2,0,0}; f->three = (XrXirBlock){0,3,0,0};
    f->entry_blocks[0] = (XrXirBlock){0,6,0,0};
    f->entry_blocks[1] = (XrXirBlock){6,1,0,6}; f->entry_blocks[2] = (XrXirBlock){7,1,0,0};
    f->operands[0] = f->operands[1] = f->operands[2] = 1;
    f->functions[0] = (XrXirFunction){"init",4,NULL,0,XR_XIR_UNIT,&f->one,1,&f->done,1,NULL,0};
    f->functions[1] = (XrXirFunction){"entry",5,NULL,0,XR_XIR_I64,f->entry_blocks,3,f->entry,8,f->operands,3};
    for (uint32_t i = 2; i < 6; ++i)
        f->functions[i] = (XrXirFunction){"read",4,&f->parameter,1,XR_XIR_I64,&f->pair,1,f->read,2,NULL,0};
    f->functions[4] = (XrXirFunction){"cleanup",7,&f->parameter,1,XR_XIR_UNIT,&f->three,1,f->cleanup,3,NULL,0};
    f->identities[4].cleanup_owner = 2;
    f->source = (XrXirSourceModule){"root",4,NULL,0,0};
    f->declarations = (XrXirDeclarations){&f->source,1,f->identities,NULL,0,NULL,0,0,1,NULL};
    f->module = (XrXirModule){XR_XIR_BUILT,f->functions,6,&f->declarations,NULL,&f->types,NULL,XR_XIR_PROGRAM,NULL};
}
#endif // XIR_CELL_PROVENANCE_FIXTURE_H
