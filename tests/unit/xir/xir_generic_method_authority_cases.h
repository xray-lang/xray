/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_authority_cases.h - Self bounds and sibling method authority
 *
 * KEY CONCEPT:
 *   An authentic symbolic self bound is a fact, while a concrete call must supply
 *   independent evidence and a sibling method cannot lend its own conditions.
 */
#ifndef XIR_GENERIC_METHOD_AUTHORITY_CASES_H
#define XIR_GENERIC_METHOD_AUTHORITY_CASES_H
/* Include after xir_generic_method_proof_cases.h. */
static void generic_method_self_bound_cases(void) {
    GenericMethodProofFixture g; generic_method_proof_fixture(&g);
    ImplementationSemanticFixture *f = &g.base;
    /* Measure.map<U:Measure>(U)->U is a finite symbolic self bound. */
    g.own = (XrXirConstraint){0,&f->application,1};
    g.function_constraints[1] = g.own;
    CHECK(implementation_semantic_status(f)==XR_XIR_OK);
    XrXirProofContext context = {&f->module,{XR_XIR_CONTEXT_CONFORMANCE_METHOD,1,0}};
    XrXirConstraintUse use = {&f->module,{XR_XIR_CONTEXT_FUNCTION,1,0},1,g.arguments,2};
    XrXirBudget budget = xr_xir_default_budget(); uint64_t scratch = budget.scratch_bytes;
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_OK);
    CHECK(!live && budget.scratch_bytes==scratch);
    /* No scalar implementation exists; the requirement's self bound is no fact. */
    XrXirType scalar = XR_XIR_I64;
    context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_CLOSED,0,0};
    use = (XrXirConstraintUse){&f->module,{XR_XIR_CONTEXT_INTERFACE_METHOD,0,0},0,&scalar,1};
    budget = xr_xir_default_budget();
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_BAD_TYPE);
    CHECK(!live && budget.scratch_bytes==scratch);
    /* Candidate-only marker cannot be inferred from a declared interface fact. */
    g.function_constraints[1].markers = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(implementation_semantic_status(f)==XR_XIR_BAD_TYPE);
}
static void generic_method_sibling_cases(void) {
    GenericMethodProofFixture g; generic_method_proof_fixture(&g);
    XrXirConstraint other_own = {XR_XIR_CONSTRAINT_ERROR,NULL,0};
    XrXirInterfaceMethod methods[2] = {g.base.method,g.base.method};
    methods[1].name = (XrXirLiteral){"other",5}; methods[1].constraints = &other_own;
    g.base.interface.methods = methods; g.base.interface.method_count = 2;
    XrXirProofContext context = {&g.base.module,{XR_XIR_CONTEXT_INTERFACE_METHOD,0,0}};
    XrXirConstraintUse use = {&g.base.module,{XR_XIR_CONTEXT_INTERFACE_METHOD,0,0},0,g.arguments,1};
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_OK);
    context.owner.member = 1; budget = xr_xir_default_budget();
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_BAD_TYPE);
    use.declaration.member = 1; budget = xr_xir_default_budget();
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_OK);
    context.owner.member = 0; budget = xr_xir_default_budget();
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_BAD_TYPE);
    CHECK(!live);
}
static void generic_method_work_boundary_cases(void) {
    GenericMethodProofFixture g; generic_method_proof_fixture(&g);
    XrXirProofContext context = {&g.base.module,{XR_XIR_CONTEXT_CONFORMANCE_METHOD,1,0}};
    XrXirConstraintUse use = {&g.base.module,{XR_XIR_CONTEXT_FUNCTION,1,0},1,g.arguments,2};
    XrXirBudget initial = xr_xir_default_budget(), budget = initial;
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_OK);
    uint64_t exact = initial.work-budget.work; CHECK(exact>0);
    budget = initial; budget.work = exact;
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_OK);
    CHECK(budget.work==0 && budget.scratch_bytes==initial.scratch_bytes && !live);
    budget = initial; budget.work = exact-1;
    CHECK(xr_xir_constraints_prove(&context,&use,&budget)==XR_XIR_BUDGET);
    CHECK(budget.work<=exact-1 && budget.scratch_bytes==initial.scratch_bytes && !live);
}
static void generic_method_authority_cases(void) {
    generic_method_self_bound_cases(); generic_method_sibling_cases();
    generic_method_work_boundary_cases();
}
#endif // XIR_GENERIC_METHOD_AUTHORITY_CASES_H
