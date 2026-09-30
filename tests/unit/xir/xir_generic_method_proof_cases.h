/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_proof_cases.h - Authentic mixed method proof environments
 *
 * KEY CONCEPT:
 *   Implementation conditions are goals; nominal and original method conditions
 *   supply independent facts, with distinct parent and own parameter positions.
 */
#ifndef XIR_GENERIC_METHOD_PROOF_CASES_H
#define XIR_GENERIC_METHOD_PROOF_CASES_H
/* Include after xir_implementation_semantic_cases.h in test_xir_interfaces.c. */
typedef struct GenericMethodProofFixture {
    ImplementationSemanticFixture base;
    XrXirConstraint own, function_constraints[2];
    XrXirCallableParameter parameter;
    XrXirType parameters[2][2], arguments[2];
} GenericMethodProofFixture;
static void generic_method_proof_fixture(GenericMethodProofFixture *g) {
    memset(g,0,sizeof(*g)); implementation_semantic_fixture(&g->base);
    ImplementationSemanticFixture *f = &g->base;
    g->arguments[0] = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    g->arguments[1] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);
    g->own.markers = XR_XIR_CONSTRAINT_SENDABLE;
    f->method.own_parameter_count = 1; f->method.constraints = &g->own;
    g->parameter = (XrXirCallableParameter){g->arguments[0],0};
    f->nodes[0].parameters = &g->parameter; f->nodes[0].parameter_count = 1;
    f->nodes[0].result = g->arguments[0]; f->nodes[0].parameter_span = 1;
    g->function_constraints[0] = f->bound; g->function_constraints[1] = g->own;
    f->generics[0] = (XrXirGeneric){&g->own,1,NULL,0};
    f->generics[1] = (XrXirGeneric){g->function_constraints,2,NULL,0};
    for (uint32_t n = 0; n < 2; ++n) {
        g->parameters[n][0] = n ? f->box_parameter : f->meter;
        g->parameters[n][1] = g->arguments[n];
        f->functions[n].parameters = g->parameters[n]; f->functions[n].parameter_count = 2;
        f->functions[n].result = g->arguments[n];
    }
}
static void generic_method_proof_authority(void) {
    GenericMethodProofFixture g; generic_method_proof_fixture(&g);
    ImplementationSemanticFixture *f = &g.base;
    CHECK(implementation_semantic_status(f)==XR_XIR_OK);
    /* Box<T:Measure>.map<U:Sendable>: T and U cannot exchange authority. */
    XrXirProofContext context = {&f->module,{XR_XIR_CONTEXT_CONFORMANCE_METHOD,1,0}};
    XrXirConstraintUse use = {&f->module,{XR_XIR_CONTEXT_FUNCTION,1,0},1,g.arguments,2};
    XrXirBudget budget = xr_xir_default_budget(); uint64_t scratch = budget.scratch_bytes;
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_OK);
    CHECK(budget.scratch_bytes==scratch && !live);
    g.function_constraints[1].markers |= XR_XIR_CONSTRAINT_ERROR;
    budget = xr_xir_default_budget();
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_BAD_TYPE);
    CHECK(budget.scratch_bytes==scratch && !live);
    CHECK(implementation_semantic_status(f)==XR_XIR_BAD_TYPE);
    g.function_constraints[1] = g.own;
    /* A real body has only its own declaration constraints, not hidden seed facts. */
    g.function_constraints[1].markers = 0;
    context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,1,0};
    XrXirConstraintUse own_use = {&f->module,{XR_XIR_CONTEXT_INTERFACE_METHOD,0,0},0,&g.arguments[1],1};
    budget = xr_xir_default_budget();
    CHECK(xr_xir_constraints_prove(&context,&own_use,&budget)==XR_XIR_BAD_TYPE);
    context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_CONFORMANCE_METHOD,1,0};
    budget = xr_xir_default_budget();
    CHECK(xr_xir_constraints_prove(&context,&own_use,&budget)==XR_XIR_OK);
    own_use.declaration = context.owner;
    budget = xr_xir_default_budget();
    CHECK(xr_xir_constraints_prove(&context,&own_use,&budget)==XR_XIR_BAD_STRUCTURE);
    CHECK(budget.scratch_bytes==scratch && !live);
    context.owner.member = 1; budget = xr_xir_default_budget();
    CHECK(xr_xir_type_use_verify(&context,XR_XIR_I64,&budget)==XR_XIR_BAD_STRUCTURE);
    context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,1,1};
    budget = xr_xir_default_budget();
    CHECK(xr_xir_type_use_verify(&context,XR_XIR_I64,&budget)==XR_XIR_BAD_STRUCTURE);
    context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_INTERFACE_METHOD,0,1};
    budget = xr_xir_default_budget();
    CHECK(xr_xir_type_use_verify(&context,XR_XIR_I64,&budget)==XR_XIR_BAD_STRUCTURE);
    CHECK(!live);
}
static void generic_method_proof_resources(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        GenericMethodProofFixture g; generic_method_proof_fixture(&g);
        XrXirProofContext context = {&g.base.module,{XR_XIR_CONTEXT_CONFORMANCE_METHOD,1,0}};
        XrXirConstraintUse use = {&g.base.module,{XR_XIR_CONTEXT_FUNCTION,1,0},1,g.arguments,2};
        XrXirBudget budget = xr_xir_default_budget(); uint64_t scratch = budget.scratch_bytes;
        attempts = 0; fail_at = site ? site-1 : SIZE_MAX;
        XrXirStatus status = xr_xir_constraints_prove(&context,&use,&budget);
        CHECK(status==(site ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        CHECK(!live && budget.scratch_bytes==scratch);
        if (!site) sites = attempts;
    }
    fail_at = SIZE_MAX;
    for (uint32_t mode = 0; mode < 2; ++mode) {
        GenericMethodProofFixture g; generic_method_proof_fixture(&g);
        XrXirProofContext context = {&g.base.module,{XR_XIR_CONTEXT_CONFORMANCE_METHOD,1,0}};
        XrXirConstraintUse use = {&g.base.module,{XR_XIR_CONTEXT_FUNCTION,1,0},1,g.arguments,2};
        XrXirBudget budget = xr_xir_default_budget();
        if (mode) budget.scratch_bytes = 0; else budget.work = 0;
        uint64_t scratch = budget.scratch_bytes;
        CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_BUDGET);
        CHECK(!live && budget.scratch_bytes==scratch);
    }
}
static void generic_method_proof_alpha(void) {
    GenericMethodProofFixture g; generic_method_proof_fixture(&g);
    XrXirTypeNode nodes[5]; memcpy(nodes,g.base.nodes,sizeof(g.base.nodes));
    nodes[4] = nodes[0];
    XrXirCallableParameter other_parameter = {(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),0};
    nodes[4].parameters = &other_parameter; nodes[4].result = other_parameter.type;
    nodes[4].parameter_span = 2;
    XrXirConstraint empty = {0}, own[2] = {g.own,{0}};
    XrXirInterfaceMethod methods[2] = {g.base.method,g.base.method};
    methods[1].signature = member_case_type(4); methods[1].constraints = own;
    XrXirInterfaceDeclaration declarations[2] = {g.base.interface,g.base.interface};
    declarations[0].methods = &methods[0]; declarations[1].methods = &methods[1];
    declarations[1].name = (XrXirLiteral){"Other",5};
    declarations[1].parameter_count = 1; declarations[1].constraints = &empty;
    XrXirInterfaceTable table = {declarations,2};
    XrXirTypes types = {nodes,5,&g.base.nominal_table,&table};
    XrXirType actual = XR_XIR_STRING;
    XrXirInterfaceApplication apps[2] = {{0,NULL,0},{1,&actual,1}};
    XrXirInterfaceClosureRoots request = {&table,&types,apps,2,3};
    for (uint32_t attack = 0; attack < 3; ++attack) {
        own[0] = g.own; methods[1].own_parameter_count = 1;
        if (attack==1) own[0].markers |= XR_XIR_CONSTRAINT_ERROR;
        if (attack==2) methods[1].own_parameter_count = 2;
        XrXirBudget budget = xr_xir_default_budget();
        XrXirInterfaceClosure *closure = NULL;
        XrXirStatus status = xr_xir_interface_closure_build(&request,&budget,&closure);
        CHECK(status==(attack ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        if (!attack) {
            CHECK(xr_xir_interface_closure_requirement_count(closure)==2);
            const XrXirInterfaceRequirement *required = xr_xir_interface_closure_requirement(closure,0);
            const XrXirTypeNode *signature = xr_xir_callable_signature(xr_xir_interface_closure_types(closure),required->signature);
            CHECK(signature && signature->result==(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+3));
        } else CHECK(!closure);
        xr_xir_interface_closure_free(closure); CHECK(!live);
    }
}
static void generic_method_proof_cases(void) {
    generic_method_proof_authority(); generic_method_proof_resources(); generic_method_proof_alpha();
}
#endif // XIR_GENERIC_METHOD_PROOF_CASES_H
