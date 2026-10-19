/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_method_owner_cases.h - Declaration-owned receiver admission
 *
 * KEY CONCEPT:
 *   Equal storage layout cannot substitute a method's declaration authority.
 */
#ifndef XIR_METHOD_OWNER_CASES_H
#define XIR_METHOD_OWNER_CASES_H
#include "xir_construction_fixture.h"
#include "xir/xxir_nominal.h"
static void method_owner_cases(void) {
    XrXirConstraint constraints[2] = {{0},{0}};
    XrXirNominalVariant variant = {{"Only",4},0,0};
    XrXirNominalDeclaration nominal[2] = {
        {{"alpha",5},{"Box",3},0,constraints,1,NULL,0,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}},
        {{"alpha",5},{"Twin",4},0,constraints,1,NULL,0,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}}};
    XrXirNominalTable table = {nominal,2,NULL};
    XrXirType argument = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirTypeNode node = {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,1,{0,&argument,1,NULL,0}};
    XrXirTypes types = {&node,1,&table,NULL};
    XrXirType receiver = (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirInstruction code[2] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction member_code[2] = {code[0],code[1]}; member_code[1].args[0] = 1;
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirBlock block = {0,2,0,0}, init_block = {0,1,0,0};
    XrXirFunction functions[3] = {
        {"main",4,NULL,0,XR_XIR_I64,&block,1,code,2,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0},
        {"read",4,&receiver,1,XR_XIR_I64,&block,1,member_code,2,NULL,0}};
    XrXirGeneric generic[3] = {{0},{0},{constraints,2,NULL,0, NULL}};
    XrXirFunctionIdentity identities[3] = {{0,1,0,0,0,0,XR_XIR_NON_MEMBER, 0, 0},
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER, 0, 0},{0,0,1,XR_XIR_MEMBER_PRIVATE,0,0,XR_XIR_READ_METHOD, 0, 0}};
    XrXirSourceModule source = {"alpha",5,NULL,0,1};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,NULL,0,0,0,NULL};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,generic,&types,NULL, XR_XIR_PROGRAM, NULL};
    for (unsigned kind = 0; kind < 3; ++kind) {
        nominal[0].kind = kind;
        nominal[0].flags = kind == XR_XIR_NOMINAL_CLASS ? XR_XIR_NOMINAL_FINAL : 0;
        nominal[0].variants = kind == XR_XIR_NOMINAL_ENUM ? &variant : NULL;
        nominal[0].variant_count = kind == XR_XIR_NOMINAL_ENUM ? 1 : 0;
        XrXirArtifact *checked = NULL;
        CHECK(xir_fixture_check(&stage_context, &module, &checked, NULL) == XR_XIR_OK && checked);
        xr_xir_compile_artifact_free(checked); checked = NULL;
    }
    nominal[0].kind = XR_XIR_NOMINAL_STRUCT; nominal[0].flags = 0;
    nominal[0].variants = NULL; nominal[0].variant_count = 0;
    /* Ordinary non-witness method; phantom owner args have identical storage. */
    {
        identities[2].member_access=XR_XIR_MEMBER_PUBLIC;
        XrXirType owner_arg=XR_XIR_I64,call_args[]={XR_XIR_I64,XR_XIR_STRING};
        XrXirTypeNode pair[]={node,node};pair[1].parameter_span=0;pair[1].nominal.arguments=&owner_arg;
        XrXirInstruction entry_ops[]={
            {XR_XIR_STRUCT_NEW,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),{0},{0},0,{0}},
            {XR_XIR_CALL,XR_XIR_I64,{0,1},{0},2,{0,2}},
            {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
        XrXirBlock entry_block={0,3,0,0};uint32_t operands[]={0};
        XrXirFunction original_main=functions[0];
        functions[0]=(XrXirFunction){"main",4,NULL,0,XR_XIR_I64,&entry_block,1,entry_ops,3,operands,1};
        generic[0]=(XrXirGeneric){NULL,0,call_args,2, NULL};types.nodes=pair;types.count=2;
        XrXirArtifact *original=NULL,*special=NULL;
        CHECK(xir_fixture_check(&stage_context, &module, &original, NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_specialize(original, &special, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(original);
        XrXirModule *closed=&special->module;uint32_t method=UINT32_MAX;
        for(uint32_t i=0;i<closed->function_count;++i)
            if(closed->declarations->functions[i].method_kind==XR_XIR_READ_METHOD)method=i;
        CHECK(method!=UINT32_MAX);XrXirType subject=closed->functions[method].parameters[0];
        XrXirTypeNode *actual=(XrXirTypeNode *)xr_xir_type_node(closed->types,subject);
        CHECK(actual&&actual->nominal.argument_count==1&&actual->nominal.arguments[0]==XR_XIR_I64);
        XrXirType *arguments=(XrXirType *)actual->nominal.arguments;arguments[0]=XR_XIR_BOOL;
        XrXirCompileContext role=stage_context_default();
        CHECK(xr_xir_compile_method_signature_verify(&role, closed, method)==XR_XIR_OK);
        CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_BAD_TYPE);
        XrXirCheckedPacket denied={0};CHECK(xr_xir_compile_checked_write(special, &denied, NULL)==XR_XIR_BAD_TYPE&&!denied.bytes);
        arguments[0]=XR_XIR_I64;CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_OK);
        actual->nominal.declaration=1;role=stage_context_default();
        CHECK(xr_xir_compile_method_signature_verify(&role, closed, method)==XR_XIR_BAD_TYPE);
        CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_BAD_TYPE);
        actual->nominal.declaration=0;CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(special);functions[0]=original_main;generic[0]=(XrXirGeneric){0};types.nodes=&node;types.count=1;
        identities[2].member_access=XR_XIR_MEMBER_PRIVATE;
        puts("ordinary receiver: same-layout wrong owner and wrong phantom prefix rejected");
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        XrXirArtifact *checked = NULL;
        if (mode == 0) identities[2].nominal_owner = 2; /* Same-layout Twin. */
        if (mode == 1) { argument = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1); node.parameter_span = 2; }
        if (mode == 2) receiver = XR_XIR_I64;
        if (mode == 3) identities[2].method_kind = XR_XIR_NON_MEMBER;
        XrXirStatus expected = mode == 3 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE;
        CHECK(xir_fixture_check(&stage_context, &module, &checked, NULL) == expected && !checked);
        identities[2].nominal_owner = 1; identities[2].method_kind = XR_XIR_READ_METHOD;
        argument = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; node.parameter_span = 1;
        receiver = (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    }
    /* Constructor result is also an owner-bearing application. */
    functions[2].parameters = NULL; functions[2].parameter_count = 0;
    functions[2].result = receiver; identities[2].method_kind = XR_XIR_CONSTRUCTOR;
    member_code[0] = (XrXirInstruction){XR_XIR_STRUCT_NEW,receiver,{0},{0},0,{0}};
    member_code[1].args[0] = 0;
    for (unsigned mode = 0; mode < 3; ++mode) {
        XrXirArtifact *checked = NULL;
        if (mode == 1) identities[2].nominal_owner = 2;
        if (mode == 2) { argument = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1); node.parameter_span = 2; }
        CHECK(xir_fixture_check(&stage_context, &module, &checked, NULL) == (mode ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        CHECK(mode ? !checked : !!checked); xr_xir_compile_artifact_free(checked); checked = NULL;
        identities[2].nominal_owner = 1;
        argument = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; node.parameter_span = 1;
    }
    functions[2].result = XR_XIR_I64; member_code[0] = code[0];
    identities[2].method_kind = XR_XIR_STATIC_METHOD;
    XrXirArtifact *checked = NULL;
    CHECK(xir_fixture_check(&stage_context, &module, &checked, NULL) == XR_XIR_OK && checked); xr_xir_compile_artifact_free(checked); checked = NULL;
    XrXirConstraint parent_condition = {XR_XIR_CONSTRAINT_SENDABLE,NULL,0};
    nominal[0].constraints = &parent_condition;
    CHECK(xir_fixture_check(&stage_context, &module, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    constraints[0].markers = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xir_fixture_check(&stage_context, &module, &checked, NULL) == XR_XIR_OK && checked); xr_xir_compile_artifact_free(checked); checked = NULL;
    nominal[0].constraints = constraints; constraints[0].markers = 0;
    generic[2].parameter_count = 0; generic[2].constraints = NULL;
    XrXirCompileContext role_budget = stage_context_default();
    CHECK(xr_xir_compile_method_signature_verify(&role_budget, &module, 2) == XR_XIR_BAD_TYPE);
    /* Lexical helpers have authority but no implicit nominal parameter prefix. */
    identities[2].method_kind = XR_XIR_MEMBER_HELPER;
    CHECK(xr_xir_compile_method_signature_verify(&role_budget, &module, 2) == XR_XIR_OK);
    generic[2].parameter_count = 2; generic[2].constraints = constraints;
    identities[2].method_kind = XR_XIR_READ_METHOD;
    functions[2].parameters = &receiver; functions[2].parameter_count = 1; member_code[1].args[0] = 1;
    XrXirCompileContext budget = stage_context_limited(STAGE_ALLOCATED_BYTES, STAGE_LIVE_BYTES, 1);
    CHECK(xr_xir_compile_method_signature_verify(&budget, &module, 2) == XR_XIR_BUDGET);
    budget = stage_context_limited(STAGE_ALLOCATED_BYTES, STAGE_LIVE_BYTES, 2);
    XrXirCompileContext unchanged = budget;
    XrCompileResourceStats baseline = stage_stats(&budget);
    CHECK(xr_xir_compile_method_signature_verify(&budget, &module, 2) == XR_XIR_OK);
    CHECK(stage_stats(&budget).work == baseline.work + 2);
    CHECK(stage_stats(&budget).live_bytes == baseline.live_bytes);
    CHECK(!memcmp(&budget, &unchanged, sizeof(budget)));
}
#endif // XIR_METHOD_OWNER_CASES_H
