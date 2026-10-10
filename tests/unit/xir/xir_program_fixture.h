/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_program_fixture.h - Three module program with private synchronized state
 *
 * KEY CONCEPT:
 *   Construction buffers expire before either execution consumer is invoked.
 */
#ifndef XIR_PROGRAM_FIXTURE_H
#define XIR_PROGRAM_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir/xxir.h"
#include "xir/xxir_generic.h"
#include "xir_error_fixture.h"
static XrXirArtifact *program_fixture_checked(const XrXirCompileContext *context,uint32_t mode) {
    const XrXirType atomic=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+(mode==2));
    const XrXirType string_cell=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1+(mode==2));
    const uint32_t atomic_operands[]={0,1};
    XrXirInstruction alpha[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 10, {0}},
        {XR_XIR_ATOMIC_NEW, atomic, {0}, {0}, 0, {0}},
        {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {1}, {0}, 0, {0}},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 0, {0}},
        {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {3}, {0}, 2, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {3}, {0}, 2, {0}},
        {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 91, {0}},
        {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    XrXirInstruction beta[10]; memcpy(beta, alpha, sizeof(beta));
    beta[0].immediate = 20; beta[2].immediate = 1; beta[3].immediate = 1; beta[4].immediate = 3;
    if (mode) alpha[6] = (XrXirInstruction) {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    const uint32_t error_operand=7;
    if (mode == 2) {
        alpha[8]=(XrXirInstruction){XR_XIR_ENUM_NEW,(XrXirType)256,{0,1},{0},0, {0}};
        alpha[9]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{8},{0},0, {0}};
    }
    XrXirInstruction init[] = {
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 2, {0}},
        {XR_XIR_CELL_NEW, string_cell, {0}, {0}, 0, {0}},
        {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {1}, {0}, 4, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {0}, {0}, 2, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    XrXirInstruction get_alpha[] = {
        {XR_XIR_SLOT_LOAD, atomic, {0}, {0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 1, {0}},
        {XR_XIR_ATOMIC_FETCH_ADD, XR_XIR_I64, {0, 2}, {0}, 0, {0}},
        {XR_XIR_SLOT_LOAD, XR_XIR_STRING, {0}, {0}, 2, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {3}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}
    };
    XrXirInstruction get_beta[6]; memcpy(get_beta, get_alpha, sizeof(get_beta));
    get_beta[0].immediate = 1; get_beta[3].immediate = 3;
    XrXirInstruction root[] = {
        {XR_XIR_CALL, XR_XIR_I64, {0}, {0}, 4, {0}},
        {XR_XIR_CALL, XR_XIR_I64, {0}, {0}, 5, {0}},
        {XR_XIR_ADD_INT, XR_XIR_I64, {0, 1}, {0}, 0, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {2}, {0}, 1, {0}},
        {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {4}, {0}, 2, {0}},
        {XR_XIR_SLOT_LOAD, string_cell, {0}, {0}, 4, {0}},
        {XR_XIR_CELL_READ, XR_XIR_STRING, {6}, {0}, 0, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {7}, {0}, 1, {0}},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 3, {0}},
        {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {6,9}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}
    };
    XrXirInstruction get_string[] = {
        {XR_XIR_SLOT_LOAD, string_cell, {0}, {0}, 4, {0}},
        {XR_XIR_CELL_READ, XR_XIR_STRING, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}
    };
    XrXirInstruction get_atomic[] = {
        {XR_XIR_SLOT_LOAD, atomic, {0}, {0}, 0, {0}},
        {XR_XIR_ATOMIC_LOAD, XR_XIR_I64, {0,1}, {0}, 0, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {1}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    const XrXirBlock blocks[] = {{0, 5, 0, 0}, {0, 10, 0, 0}, {0, 12, 0, 0}, {0, 6, 0, 0}, {0, 3, 0, 0}, {0, 4, 0, 0}};
    const XrXirFunction functions[] = {
        {"init_root", 9, NULL, 0, XR_XIR_UNIT, &blocks[0], 1, init, 5, NULL, 0},
        {"init_beta", 9, NULL, 0, XR_XIR_UNIT, &blocks[1], 1, beta, 10, NULL, 0},
        {"init_alpha", 10, NULL, 0, XR_XIR_UNIT, &blocks[1], 1, alpha, 10, mode == 2 ? &error_operand : NULL, mode == 2 ? 1u : 0u},
        {"main", 4, NULL, 0, XR_XIR_I64, &blocks[2], 1, root, 12, NULL, 0},
        {"alpha_next", 10, NULL, 0, XR_XIR_I64, &blocks[3], 1, get_alpha, 6, atomic_operands, 2},
        {"beta_next", 9, NULL, 0, XR_XIR_I64, &blocks[3], 1, get_beta, 6, atomic_operands, 2},
        {"root_string", 11, NULL, 0, XR_XIR_STRING, &blocks[4], 1, get_string, 3, NULL, 0},
        {"alpha_cell", 10, NULL, 0, atomic, &blocks[5], 1, get_atomic, 4, atomic_operands, 1}
    };
    const uint32_t imports[] = {1, 2};
    const XrXirSourceModule modules[] = {
        {"root", 4, imports, 2, 0}, {"beta", 4, NULL, 0, 1}, {"alpha", 5, NULL, 0, 2}
    };
    const XrXirFunctionIdentity identities[] = {{0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {1, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {2, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {2, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {1, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {0, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {2, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}};
    const XrXirSlot slots[] = {{2, atomic, 0}, {1, atomic, 0},
        {2, XR_XIR_STRING, 0}, {1, XR_XIR_STRING, 0}, {0, string_cell, 1}};
    const XrXirLiteral literals[] = {{"A\0\xe4\xb8\xad", 5}, {"B\xf0\x9f\x98\x80", 5}, {"root", 4}, {"updated", 7}, {NULL, 0}};
    const XrXirDeclarations declarations = {modules, 3, identities, slots, 5, literals, 5, 0, 3, NULL};
    ErrorFixture error; error_fixture_init(&error,false);
    XrXirTypeNode nodes[3]={{.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_STRING}};
    XrXirTypes types={nodes,2,NULL,NULL};
    if(mode==2){nodes[2]=nodes[1];nodes[1]=nodes[0];nodes[0]=error.node;types.count=3;types.nominals=&error.table;}
    const XrXirModule built = {XR_XIR_BUILT, functions, 8, &declarations, NULL, &types, NULL, XR_XIR_PROGRAM, NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked=NULL;
    CHECK(xir_fixture_check(context, &built, &checked, NULL)==XR_XIR_OK);
    (void)target;return checked;
}
static XrXirArtifact *program_fixture(const XrXirCompileContext *context,uint32_t mode) {
    XrXirArtifact *checked=program_fixture_checked(context,mode),*closed=NULL,*lowered=NULL;
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL;xr_xir_compile_artifact_free(checked);checked=NULL;return lowered;
}

#endif
