/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_types_fixture.h - Higher-order contracts through generic identity
 *
 * KEY CONCEPT:
 *   Nested function types preserve their contract without selecting a target.
 */
#ifndef XIR_TYPES_FIXTURE_H
#define XIR_TYPES_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *callable_fixture(const XrXirCompileContext *context) {
    XrXirType fn0 = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType fn1 = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + 1);
    XrXirType generic = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    XrXirCallableParameter parameters[] = {{XR_XIR_I64, 0}, {fn0, 0}};
    XrXirTypeNode signatures[] = {{XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, parameters, 1, XR_XIR_STRING, XR_XIR_CALLABLE_ROOT_UNRESOLVED,0, {0}},
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, parameters + 1, 1, fn0, XR_XIR_CALLABLE_ROOT_UNRESOLVED,0, {0}},
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, XR_XIR_CALLABLE_ROOT_UNRESOLVED,0, {0}}};
    XrXirTypes types = {signatures, 3, NULL, NULL};
    XrXirInstruction caller_ops[] = {{XR_XIR_CALL, fn1, {0, 1}, {0}, 1, {0, 1}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0}, 0, {0}}};
    XrXirInstruction generic_ops[] = {{XR_XIR_COPY, generic, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0}, 0, {0}}};
    XrXirBlock block = {0, 2, 0, 0}; uint32_t operand = 0;
    XrXirConstraint constraint = {0};
    XrXirFunction functions[] = {
        {"forward", 7, &fn1, 1, fn1, &block, 1, caller_ops, 2, &operand, 1},
        {"identity", 8, &generic, 1, generic, &block, 1, generic_ops, 2, NULL, 0}
    };
    XrXirGeneric generics[] = {{NULL, 0, &fn1, 1, NULL}, {&constraint, 1, NULL, 0, NULL}};
    XrXirModule built = {XR_XIR_BUILT, functions, 2, NULL, generics, &types, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xir_fixture_check(context, &built, &checked, NULL) == XR_XIR_OK && checked);
    memset(signatures, 0xCC, sizeof(signatures)); memset(parameters, 0xCC, sizeof(parameters));
    const XrXirTypes *owned = xr_xir_compile_artifact_module(checked)->types;
    CHECK(owned && owned->count == 3);
    for (uint32_t i = 0; i < 3; ++i) CHECK(owned->nodes[i].flags == 8u);
    return checked;
}
static XrXirArtifact *function_ir_fixture(const XrXirCompileContext *context) {
    XrXirType parameter = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE, concrete = XR_XIR_I64;
    XrXirConstraint constraint = {0};
    XrXirGeneric generics[] = {{0}, {NULL,0,&concrete,1, NULL}, {&constraint,1,NULL,0, NULL}};
    XrXirCallableParameter input = {XR_XIR_I64, 0};
    XrXirTypeNode signature = {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, &input, 1, XR_XIR_I64, XR_XIR_CALLABLE_ROOT_UNRESOLVED,0, {0}};
    XrXirTypes types = {&signature, 1, NULL, NULL};
    XrXirInstruction init[] = {{XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction root[] = {{XR_XIR_FUNCTION_REF, (XrXirType) 256, {0}, {0}, 2, {0,1}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 7, {0}},
        {XR_XIR_CALL_INDIRECT, XR_XIR_I64, {0, 1}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}};
    XrXirInstruction target[] = {{XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock blocks[] = {{0,1, 0, 0}, {0,4, 0, 0}, {0,2, 0, 0}};
    uint32_t argument = 1;
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,1,NULL,0},
        {"root",4,NULL,0,XR_XIR_I64,&blocks[1],1,root,4,&argument,1},
        {"echo",4,&parameter,1,parameter,&blocks[2],1,target,2,NULL,0}
    };
    XrXirSourceModule source = {"root",4,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0,0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {0,1, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {0,0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,NULL,0,0,1, NULL};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,generics,&types, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xir_fixture_check(context, &built, &checked, NULL) == XR_XIR_OK && checked);
    return checked;
}
static XrXirArtifact *generic_callable_fixture(const XrXirCompileContext *context) {
    XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    XrXirCallableParameter components[] = {{t,0}, {XR_XIR_STRING,0}};
    XrXirTypeNode signatures[] = {{XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,components,1,t, XR_XIR_CALLABLE_ROOT_UNRESOLVED,1, {0}},
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,components+1,1,XR_XIR_STRING, XR_XIR_CALLABLE_ROOT_UNRESOLVED,0, {0}}};
    XrXirTypes table = {signatures,2, NULL, NULL};
    XrXirType parameters[] = {(XrXirType)257,XR_XIR_STRING,(XrXirType)256,t}, argument = XR_XIR_STRING;
    XrXirConstraint constraint = {0};
    uint32_t arguments[] = {0,1}, indirect = 1;
    XrXirGeneric generics[] = {{NULL,0,&argument,1, NULL}, {&constraint,1,NULL,0, NULL}};
    XrXirInstruction caller[] = {{XR_XIR_CALL,XR_XIR_STRING,{0,2},{0},1, {0,1}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0, {0}}};
    XrXirInstruction body[] = {{XR_XIR_CALL_INDIRECT,t,{0,1},{0},0, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0, {0}}};
    XrXirBlock block = {0,2, 0, 0};
    XrXirFunction functions[] = {
        {"caller",6,parameters,2,XR_XIR_STRING,&block,1,caller,2,arguments,2},
        {"apply",5,parameters+2,2,t,&block,1,body,2,&indirect,1}
    };
    XrXirModule built = {XR_XIR_BUILT,functions,2,NULL,generics,&table, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xir_fixture_check(context, &built, &checked, NULL) == XR_XIR_OK && checked);
    memset(signatures,0xCC,sizeof(signatures)); memset(components,0xCC,sizeof(components));
    const XrXirTypes *owned = xr_xir_compile_artifact_module(checked)->types;
    CHECK(owned && owned->count == 2 && owned->nodes[0].flags == 8u && owned->nodes[1].flags == 8u);
    return checked;
}
/* Two generic owners reuse the same abstract nodes with different substitutions. */
static XrXirArtifact *constructed_fixture(const XrXirCompileContext *context) {
    XrXirType t = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirCallableParameter inputs[] = {{(XrXirType)257,0}, {(XrXirType)261,0}, {(XrXirType)265,0}};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_ARRAY,t,NULL,0,XR_XIR_UNIT,0,1, {0}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)256,NULL,0,XR_XIR_UNIT,0,1, {0}},
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,inputs,1,(XrXirType)256, XR_XIR_CALLABLE_ROOT_UNRESOLVED,1, {0}},
        {XR_XIR_TYPE_CELL,(XrXirType)257,NULL,0,XR_XIR_UNIT,0,1, {0}},
        {XR_XIR_TYPE_ARRAY,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)260,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,inputs+1,1,(XrXirType)260, XR_XIR_CALLABLE_ROOT_UNRESOLVED,0, {0}},
        {XR_XIR_TYPE_CELL,(XrXirType)261,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)264,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,inputs+2,1,(XrXirType)264, XR_XIR_CALLABLE_ROOT_UNRESOLVED,0, {0}},
        {XR_XIR_TYPE_CELL,(XrXirType)265,NULL,0,XR_XIR_UNIT,0,0, {0}}
    };
    XrXirTypes types = {nodes,12, NULL, NULL};
    XrXirType parameters[] = {(XrXirType)261,(XrXirType)262,(XrXirType)265,(XrXirType)266,
        (XrXirType)257,(XrXirType)258};
    XrXirType arguments[] = {XR_XIR_I64,XR_XIR_STRING};
    XrXirConstraint constraint = {.markers = XR_XIR_CONSTRAINT_SENDABLE};
    uint32_t operands[] = {0,1,2,3};
    XrXirGeneric generics[] = {{NULL,0,arguments,2, NULL}, {&constraint,1,NULL,0, NULL}, {&constraint,1,NULL,0, NULL}};
    XrXirInstruction caller[] = {{XR_XIR_CALL,(XrXirType)261,{0,2},{0},1, {0,1}},
        {XR_XIR_CALL,(XrXirType)265,{2,2},{0},2, {1,1}}, {XR_XIR_RETURN,XR_XIR_UNIT,{5},{0},0, {0}}};
    XrXirInstruction body[] = {{XR_XIR_COPY,(XrXirType)257,{0},{0},0, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0, {0}}};
    XrXirBlock blocks[] = {{0,3, 0, 0},{0,2, 0, 0}};
    XrXirFunction functions[] = {
        {"caller",6,parameters,4,(XrXirType)265,blocks,1,caller,3,operands,4},
        {"first",5,parameters+4,2,(XrXirType)257,blocks+1,1,body,2,NULL,0},
        {"second",6,parameters+4,2,(XrXirType)257,blocks+1,1,body,2,NULL,0}
    };
    XrXirModule built = {XR_XIR_BUILT,functions,3,NULL,generics,&types, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xir_fixture_check(context, &built, &checked, NULL) == XR_XIR_OK && checked);
    memset(nodes,0xCC,sizeof(nodes)); memset(inputs,0xCC,sizeof(inputs));
    const XrXirTypes *owned = xr_xir_compile_artifact_module(checked)->types;
    CHECK(owned && owned->nodes[2].flags == 8u && owned->nodes[6].flags == 8u && owned->nodes[10].flags == 8u);
    return checked;
}
#endif // XIR_TYPES_FIXTURE_H
