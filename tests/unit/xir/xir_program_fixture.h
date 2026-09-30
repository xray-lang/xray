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
#include "xir/xxir.h"
#include "xir/xxir_generic.h"
#include "xir_error_fixture.h"
static XrXirArtifact *program_fixture(uint32_t mode) {
    XrXirInstruction alpha[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 10, {0}},
        {XR_XIR_ATOMIC_I64_NEW, XR_XIR_ATOMIC_I64, {0}, {0}, 0, {0}},
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
        {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {0}, {0}, 4, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {0}, {0}, 2, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    XrXirInstruction get_alpha[] = {
        {XR_XIR_SLOT_LOAD, XR_XIR_ATOMIC_I64, {0}, {0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 1, {0}},
        {XR_XIR_ATOMIC_I64_FETCH_ADD, XR_XIR_I64, {0, 1}, {0}, 0, {0}},
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
        {XR_XIR_SLOT_LOAD, XR_XIR_STRING, {0}, {0}, 4, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {6}, {0}, 1, {0}},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 3, {0}},
        {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {8}, {0}, 4, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}
    };
    XrXirInstruction get_string[] = {
        {XR_XIR_SLOT_LOAD, XR_XIR_STRING, {0}, {0}, 4, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    XrXirInstruction get_atomic[] = {
        {XR_XIR_SLOT_LOAD, XR_XIR_ATOMIC_I64, {0}, {0}, 0, {0}},
        {XR_XIR_ATOMIC_I64_LOAD, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {1}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    const XrXirBlock blocks[] = {{0, 4, 0, 0}, {0, 10, 0, 0}, {0, 11, 0, 0}, {0, 6, 0, 0}, {0, 2, 0, 0}};
    const XrXirFunction functions[] = {
        {"init_root", 9, NULL, 0, XR_XIR_UNIT, &blocks[0], 1, init, 4, NULL, 0},
        {"init_beta", 9, NULL, 0, XR_XIR_UNIT, &blocks[1], 1, beta, 10, NULL, 0},
        {"init_alpha", 10, NULL, 0, XR_XIR_UNIT, &blocks[1], 1, alpha, 10, mode == 2 ? &error_operand : NULL, mode == 2 ? 1u : 0u},
        {"main", 4, NULL, 0, XR_XIR_I64, &blocks[2], 1, root, 11, NULL, 0},
        {"alpha_next", 10, NULL, 0, XR_XIR_I64, &blocks[3], 1, get_alpha, 6, NULL, 0},
        {"beta_next", 9, NULL, 0, XR_XIR_I64, &blocks[3], 1, get_beta, 6, NULL, 0},
        {"root_string", 11, NULL, 0, XR_XIR_STRING, &blocks[4], 1, get_string, 2, NULL, 0},
        {"alpha_cell", 10, NULL, 0, XR_XIR_ATOMIC_I64, &blocks[0], 1, get_atomic, 4, NULL, 0}
    };
    const uint32_t imports[] = {1, 2};
    const XrXirSourceModule modules[] = {
        {"root", 4, imports, 2, 0}, {"beta", 4, NULL, 0, 1}, {"alpha", 5, NULL, 0, 2}
    };
    const XrXirFunctionIdentity identities[] = {{0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {1, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {2, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {2, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {1, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {0, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {2, 1, 0, 0, 0, 0, XR_XIR_NON_MEMBER}};
    const XrXirSlot slots[] = {{2, XR_XIR_ATOMIC_I64, 0}, {1, XR_XIR_ATOMIC_I64, 0},
        {2, XR_XIR_STRING, 0}, {1, XR_XIR_STRING, 0}, {0, XR_XIR_STRING, 1}};
    const XrXirLiteral literals[] = {{"A\0\xe4\xb8\xad", 5}, {"B\xf0\x9f\x98\x80", 5}, {"root", 4}, {"updated", 7}, {NULL, 0}};
    const XrXirDeclarations declarations = {modules, 3, identities, slots, 5, literals, 5, 0, 3, NULL};
    ErrorFixture error; error_fixture_init(&error,false);
    const XrXirModule built = {XR_XIR_BUILT, functions, 8, &declarations, NULL, mode == 2 ? &error.types : NULL, NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    XrXirArtifact *closed=NULL; CHECK(xr_xir_specialize(checked,NULL,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif
