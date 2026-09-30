/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_interface_closure_cases.h - Arbitrary application closure ownership
 */
#ifndef XIR_INTERFACE_CLOSURE_CASES_H
#define XIR_INTERFACE_CLOSURE_CASES_H
#include "xir_interface_member_cases.h"

static void interface_closure_multiple_roots(void) {
    for (unsigned order = 0; order < 2; ++order) {
        MemberFixture f; member_fixture(&f);
        XrXirType actual = XR_XIR_I64;
        XrXirInterfaceApplication roots[] = {{3,&actual,1},{5,NULL,0},{3,&actual,1}};
        if (order) { XrXirInterfaceApplication tmp = roots[0]; roots[0] = roots[1]; roots[1] = tmp; }
        XrXirBudget budget = xr_xir_default_budget(); XrXirInterfaceClosure *closure = NULL;
        CHECK(xr_xir_interface_closure_build(&f.table,&f.types,roots,3,&budget,&closure)==XR_XIR_OK);
        actual = XR_XIR_BOOL; memset(roots,0xcc,sizeof(roots));
        CHECK(xr_xir_interface_closure_application_count(closure)==5);
        CHECK(xr_xir_interface_closure_requirement_count(closure)==2);
        unsigned origins = 0;
        const XrXirTypes *types = xr_xir_interface_closure_types(closure);
        for (uint32_t i = 0; i < 2; ++i) {
            const XrXirInterfaceRequirement *r = xr_xir_interface_closure_requirement(closure,i);
            const XrXirInterfaceApplication *app = xr_xir_interface_closure_application(closure,r->application);
            CHECK(app && app->declaration==r->origin_interface && r->member==0);
            CHECK(r->name.length==3 && !memcmp(r->name.bytes,"get",3));
            budget = xr_xir_default_budget();
            CHECK(xr_xir_type_substitution_matches_between(types,&f.types,NULL,0,r->signature,
                member_case_type(5),&budget)==XR_XIR_OK);
            if (r->origin_interface==0) {
                origins |= 1; CHECK(app->argument_count==1);
                CHECK(xr_xir_array_element(types,app->arguments[0])==XR_XIR_I64);
            } else { CHECK(r->origin_interface==5); origins |= 2; }
        }
        CHECK(origins==3 && !xr_xir_interface_closure_application(closure,5));
        CHECK(!xr_xir_interface_closure_requirement(closure,2));
        xr_xir_interface_closure_free(closure); CHECK(!live);
        member_fixture(&f); actual = XR_XIR_I64;
        XrXirInterfaceApplication conflict[] = {{3,&actual,1},{5,NULL,0}};
        if (order) { XrXirInterfaceApplication tmp = conflict[0]; conflict[0] = conflict[1]; conflict[1] = tmp; }
        f.methods[1].signature = member_case_type(3); budget = xr_xir_default_budget();
        CHECK(xr_xir_interface_closure_build(&f.table,&f.types,conflict,2,&budget,&closure)==XR_XIR_BAD_TYPE);
        CHECK(!closure && !live);
    }
}
static void interface_closure_distinct_applications(void) {
    MemberFixture f; member_fixture(&f);
    XrXirType values[] = {XR_XIR_I64,XR_XIR_BOOL};
    XrXirInterfaceApplication roots[] = {{0,values,1},{0,values+1,1},{0,values,1}};
    XrXirBudget budget = xr_xir_default_budget(); XrXirInterfaceClosure *closure = NULL;
    CHECK(xr_xir_interface_closure_build(&f.table,&f.types,roots,3,&budget,&closure)==XR_XIR_BAD_TYPE);
    CHECK(!closure && !live);
    f.methods[0].signature = member_case_type(3); budget = xr_xir_default_budget();
    CHECK(xr_xir_interface_closure_build(&f.table,&f.types,roots,3,&budget,&closure)==XR_XIR_OK);
    CHECK(xr_xir_interface_closure_application_count(closure)==2);
    CHECK(xr_xir_interface_closure_requirement_count(closure)==2);
    CHECK(xr_xir_interface_closure_requirement(closure,0)->application !=
        xr_xir_interface_closure_requirement(closure,1)->application);
    xr_xir_interface_closure_free(closure); CHECK(!live);
}
static void interface_closure_parameter_order(void) {
    MemberFixture f; member_fixture(&f);
    XrXirConstraint constraints[2] = {{0},{0}};
    XrXirType p0 = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType p1 = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);
    XrXirType swapped[] = {p1,p0}, identity[] = {p0,p1};
    f.nodes[1].result = p1; f.nodes[1].parameter_span = 2; f.types.count = 2; f.table.count = 4;
    for (uint32_t d = 0; d < 4; ++d) {
        f.declarations[d].constraints = constraints; f.declarations[d].parameter_count = 2;
    }
    f.parents[0] = (XrXirInterfaceApplication){0,swapped,2};
    f.parents[1] = (XrXirInterfaceApplication){0,identity,2};
    f.parents[2] = (XrXirInterfaceApplication){1,identity,2};
    f.parents[3] = (XrXirInterfaceApplication){2,swapped,2};
    XrXirInterfaceApplication root = {3,swapped,2};
    XrXirBudget budget = xr_xir_default_budget(); XrXirInterfaceClosure *closure = NULL;
    CHECK(xr_xir_interface_closure_build(&f.table,&f.types,&root,1,&budget,&closure)==XR_XIR_OK);
    CHECK(xr_xir_interface_closure_requirement_count(closure)==1);
    const XrXirInterfaceRequirement *r = xr_xir_interface_closure_requirement(closure,0);
    const XrXirInterfaceApplication *app = xr_xir_interface_closure_application(closure,r->application);
    CHECK(app->arguments[0]==p0 && app->arguments[1]==p1);
    CHECK(xr_xir_callable_signature(xr_xir_interface_closure_types(closure),r->signature)->result==p1);
    xr_xir_interface_closure_free(closure); CHECK(!live);
}
static void interface_closure_resources(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        MemberFixture f; member_fixture(&f);
        XrXirInterfaceApplication roots[] = {{3,&f.concrete,1},{5,NULL,0}};
        attempts = 0; fail_at = site ? site - 1 : SIZE_MAX;
        XrXirBudget budget = xr_xir_default_budget(), original = budget;
        XrXirInterfaceClosure *closure = (XrXirInterfaceClosure *)(uintptr_t)1;
        CHECK(xr_xir_interface_closure_build(&f.table,&f.types,roots,2,&budget,&closure)==
            (site ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        if (site) CHECK(!closure && !memcmp(&budget,&original,sizeof(budget))); else sites = attempts;
        xr_xir_interface_closure_free(closure); CHECK(!live);
    }
    fail_at = SIZE_MAX;
    MemberFixture f; member_fixture(&f); XrXirInterfaceApplication root = {3,&f.concrete,1};
    XrXirBudget original = xr_xir_default_budget(), spent = original;
    XrXirInterfaceClosure *closure = NULL;
    CHECK(xr_xir_interface_closure_build(&f.table,&f.types,&root,1,&spent,&closure)==XR_XIR_OK);
    xr_xir_interface_closure_free(closure);
    XrXirBudget exact = original; exact.work -= spent.work; exact.scratch_bytes -= spent.scratch_bytes;
    for (unsigned boundary = 0; boundary < 3; ++boundary) {
        XrXirBudget budget = exact;
        if (boundary==1) --budget.work;
        if (boundary==2) --budget.scratch_bytes;
        XrXirBudget before = budget;
        CHECK(xr_xir_interface_closure_build(&f.table,&f.types,&root,1,&budget,&closure)==
            (boundary ? XR_XIR_BUDGET : XR_XIR_OK));
        if (boundary) CHECK(!closure && !memcmp(&budget,&before,sizeof(budget)));
        xr_xir_interface_closure_free(closure); CHECK(!live);
    }
    XrXirBudget budget = original;
    CHECK(xr_xir_interface_closure_build(NULL,NULL,NULL,0,&budget,&closure)==XR_XIR_OK);
    CHECK(!xr_xir_interface_closure_application_count(closure) && !xr_xir_interface_closure_requirement_count(closure));
    xr_xir_interface_closure_free(closure); CHECK(!live);
    printf("Interface closure owned allocations: %zu failure sites\n",sites);
}
static void interface_closure_deep(void) {
    enum { DEPTH = 512 };
    XrXirInterfaceDeclaration declarations[DEPTH] = {0};
    XrXirInterfaceApplication parents[DEPTH] = {0};
    char names[DEPTH][12];
    MemberFixture f; member_fixture(&f); f.methods[0].signature = member_case_type(3);
    for (uint32_t i = 0; i < DEPTH; ++i) {
        int length = snprintf(names[i],sizeof(names[i]),"I%u",i); CHECK(length>0 && length<12);
        declarations[i].module = (XrXirLiteral){"m",1};
        declarations[i].name = (XrXirLiteral){names[i],(uint32_t)length}; declarations[i].exported = 1;
        if (i) {
            parents[i].declaration = i-1; declarations[i].parents = &parents[i]; declarations[i].parent_count = 1;
        }
    }
    declarations[0].methods = f.methods; declarations[0].method_count = 1;
    XrXirInterfaceTable table = {declarations,DEPTH};
    XrXirInterfaceApplication root = {DEPTH-1,NULL,0}; XrXirInterfaceClosure *closure = NULL;
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_interface_closure_build(&table,&f.types,&root,1,&budget,&closure)==XR_XIR_OK);
    CHECK(xr_xir_interface_closure_application_count(closure)==DEPTH);
    CHECK(xr_xir_interface_closure_requirement_count(closure)==1);
    xr_xir_interface_closure_free(closure); CHECK(!live);
    budget = xr_xir_default_budget(); budget.work = 128;
    CHECK(xr_xir_interface_closure_build(&table,&f.types,&root,1,&budget,&closure)==XR_XIR_BUDGET);
    CHECK(!closure && !live);
}
static void interface_closure_cases(void) {
    interface_closure_multiple_roots(); interface_closure_distinct_applications();
    interface_closure_parameter_order(); interface_closure_resources(); interface_closure_deep();
}
#endif // XIR_INTERFACE_CLOSURE_CASES_H
