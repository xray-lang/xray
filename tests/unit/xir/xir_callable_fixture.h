/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_callable_fixture.h - Higher-order contracts through generic identity
 *
 * KEY CONCEPT:
 *   Nested function types preserve their contract without selecting a target.
 */
#ifndef XIR_CALLABLE_FIXTURE_H
#define XIR_CALLABLE_FIXTURE_H
#include "xir/xxir_callable.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *callable_fixture(void) {
    XrXirType fn0 = (XrXirType) XR_XIR_CALLABLE_TYPE_BASE;
    XrXirType fn1 = (XrXirType) (XR_XIR_CALLABLE_TYPE_BASE + 1);
    XrXirType generic = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    XrXirCallableParameter parameters[] = {{XR_XIR_I64, 0}, {fn0, 0}};
    XrXirCallableSignature signatures[] = {{parameters, 1, XR_XIR_STRING, 0},
        {parameters + 1, 1, fn0, 0}, {NULL, 0, XR_XIR_UNIT, 0}};
    XrXirCallableTypes types = {signatures, 3};
    XrXirInstruction caller_ops[] = {{XR_XIR_CALL, fn1, {0, 1}, {0, 1}, 1},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0}, 0}};
    XrXirInstruction generic_ops[] = {{XR_XIR_COPY, generic, {0}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0}, 0}};
    XrXirBlock block = {0, 2}; uint32_t operand = 0, constraint = 0;
    XrXirFunction functions[] = {
        {"forward", 7, &fn1, 1, fn1, &block, 1, caller_ops, 2, &operand, 1},
        {"identity", 8, &generic, 1, generic, &block, 1, generic_ops, 2, NULL, 0}
    };
    XrXirGeneric generics[] = {{NULL, 0, &fn1, 1}, {&constraint, 1, NULL, 0}};
    XrXirModule built = {XR_XIR_BUILT, functions, 2, NULL, generics, &types};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK && checked);
    memset(signatures, 0xCC, sizeof(signatures)); memset(parameters, 0xCC, sizeof(parameters));
    return checked;
}
static XrXirArtifact *function_ir_fixture(void) {
    XrXirType parameter = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE, concrete = XR_XIR_I64;
    uint32_t constraint = 0;
    XrXirGeneric generics[] = {{0}, {NULL,0,&concrete,1}, {&constraint,1,NULL,0}};
    XrXirCallableParameter input = {XR_XIR_I64, 0};
    XrXirCallableSignature signature = {&input, 1, XR_XIR_I64, 0};
    XrXirCallableTypes types = {&signature, 1};
    XrXirInstruction init[] = {{XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0}};
    XrXirInstruction root[] = {{XR_XIR_FUNCTION_REF, (XrXirType) 256, {0}, {0,1}, 2},
        {XR_XIR_CONST_I64, XR_XIR_I64, {0}, {0}, 7},
        {XR_XIR_CALL_INDIRECT, XR_XIR_I64, {0, 1}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0}};
    XrXirInstruction target[] = {{XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0}};
    XrXirBlock blocks[] = {{0,1}, {0,4}, {0,2}};
    uint32_t argument = 1;
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,1,NULL,0},
        {"root",4,NULL,0,XR_XIR_I64,&blocks[1],1,root,4,&argument,1},
        {"echo",4,&parameter,1,parameter,&blocks[2],1,target,2,NULL,0}
    };
    XrXirSourceModule source = {"root",4,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0,0}, {0,1}, {0,0}};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,NULL,0,0,1};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,generics,&types};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK && checked);
    return checked;
}
#endif // XIR_CALLABLE_FIXTURE_H
