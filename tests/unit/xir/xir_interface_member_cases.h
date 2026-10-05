/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_interface_member_cases.h - Inherited substitution and ambiguity cases
 */
#ifndef XIR_INTERFACE_MEMBER_CASES_H
#define XIR_INTERFACE_MEMBER_CASES_H
#include "xir/xxir_interface_members.h"

typedef struct MemberFixture {
    XrXirConstraint constraint;
    XrXirType parameter, array_parameter, concrete;
    XrXirTypeNode nodes[8];
    XrXirCallableParameter parameter_modes[2];
    XrXirTypes types;
    XrXirInterfaceMethod methods[2];
    XrXirInterfaceApplication parents[6];
    XrXirInterfaceDeclaration declarations[6];
    XrXirInterfaceTable table;
} MemberFixture;
static XrXirType member_case_type(uint32_t index) {
    return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + index);
}
static void member_fixture(MemberFixture *f) {
    memset(f, 0, sizeof(*f));
    f->parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->array_parameter = member_case_type(0); f->concrete = XR_XIR_I64;
    f->nodes[0].kind = XR_XIR_TYPE_ARRAY; f->nodes[0].element = f->parameter; f->nodes[0].parameter_span = 1;
    f->nodes[1].kind = XR_XIR_TYPE_CALLABLE; f->nodes[1].result = f->parameter; f->nodes[1].parameter_span = 1;
    f->nodes[2].kind = XR_XIR_TYPE_CALLABLE; f->nodes[2].result = member_case_type(0); f->nodes[2].parameter_span = 1;
    f->nodes[3].kind = XR_XIR_TYPE_CALLABLE; f->nodes[3].result = XR_XIR_I64;
    f->nodes[4].kind = XR_XIR_TYPE_ARRAY; f->nodes[4].element = XR_XIR_I64;
    f->nodes[5].kind = XR_XIR_TYPE_CALLABLE; f->nodes[5].result = member_case_type(4);
    f->parameter_modes[0].type = XR_XIR_I64; f->parameter_modes[1].type = XR_XIR_BOOL;
    f->nodes[6] = f->nodes[5]; f->nodes[6].parameters = f->parameter_modes; f->nodes[6].parameter_count = 1;
    f->nodes[7] = f->nodes[6]; f->nodes[7].parameters = f->parameter_modes + 1;
    f->types = (XrXirTypes){f->nodes,8,NULL,NULL};
    f->methods[0] = (XrXirInterfaceMethod){{"get",3},member_case_type(1),0,0,NULL};
    f->methods[1] = (XrXirInterfaceMethod){{"get",3},member_case_type(5),0,0,NULL};
    f->parents[0] = (XrXirInterfaceApplication){0,&f->array_parameter,1};
    f->parents[1] = (XrXirInterfaceApplication){0,&f->array_parameter,1};
    f->parents[2] = (XrXirInterfaceApplication){1,&f->parameter,1};
    f->parents[3] = (XrXirInterfaceApplication){2,&f->parameter,1};
    f->parents[4] = (XrXirInterfaceApplication){3,&f->concrete,1};
    f->parents[5] = (XrXirInterfaceApplication){5,NULL,0};
    f->declarations[0] = (XrXirInterfaceDeclaration){{"m",1},{"A",1},1,&f->constraint,1,NULL,0,f->methods,1};
    f->declarations[1] = (XrXirInterfaceDeclaration){{"m",1},{"B",1},1,&f->constraint,1,f->parents,1,NULL,0};
    f->declarations[2] = (XrXirInterfaceDeclaration){{"m",1},{"C",1},1,&f->constraint,1,f->parents+1,1,NULL,0};
    f->declarations[3] = (XrXirInterfaceDeclaration){{"m",1},{"D",1},1,&f->constraint,1,f->parents+2,2,NULL,0};
    f->declarations[4] = (XrXirInterfaceDeclaration){{"m",1},{"E",1},1,NULL,0,f->parents+4,2,NULL,0};
    f->declarations[5] = (XrXirInterfaceDeclaration){{"m",1},{"X",1},1,NULL,0,NULL,0,f->methods+1,1};
    f->table = (XrXirInterfaceTable){f->declarations,6};
}
static void interface_member_semantics(void) {
    for (unsigned order = 0; order < 2; ++order) {
        for (unsigned attack = 0; attack < 5; ++attack) {
            MemberFixture f; member_fixture(&f);
            if (order) {
                XrXirInterfaceApplication tmp = f.parents[4]; f.parents[4] = f.parents[5]; f.parents[5] = tmp;
                tmp = f.parents[2]; f.parents[2] = f.parents[3]; f.parents[3] = tmp;
            }
            if (attack == 1) f.methods[1].signature = member_case_type(3);
            if (attack == 2) f.nodes[5].flags = XR_XIR_CALLABLE_NO_SUSPEND;
            if (attack == 3) f.methods[1].receiver = 1;
            if (attack == 4) {
                f.parameter_modes[1].type = XR_XIR_I64; f.parameter_modes[1].mode = 1;
                f.methods[0].signature = member_case_type(6); f.methods[1].signature = member_case_type(7);
            }
            XrXirCompileContext budget = interface_context_default();
            CHECK(xr_xir_compile_interfaces_verify_members_verified(&budget, &f.table, &f.types)==
                (attack ? XR_XIR_BAD_TYPE : XR_XIR_OK));
            if (attack) interface_temporary_clean(&budget);
            CHECK(f.declarations[0].method_count==1 && f.declarations[5].method_count==1);
            CHECK(interface_live == stage_owner_count);
        }
    }
    MemberFixture f; member_fixture(&f);
    /* The same original requirement under different substitutions is ambiguous. */
    f.parents[1].arguments = &f.parameter;
    XrXirCompileContext budget = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_members_verified(&budget, &f.table, &f.types)==XR_XIR_BAD_TYPE && interface_live == stage_owner_count);
    member_fixture(&f);
    /* A direct redeclaration must agree after composing both inheritance edges. */
    XrXirInterfaceMethod direct = {{"get",3},member_case_type(2),0,0,NULL};
    f.declarations[3].methods = &direct; f.declarations[3].method_count = 1;
    budget = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_members_verified(&budget, &f.table, &f.types)==XR_XIR_OK && interface_live == stage_owner_count);
    direct.signature = member_case_type(1); budget = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_members_verified(&budget, &f.table, &f.types)==XR_XIR_BAD_TYPE && interface_live == stage_owner_count);
}
static void interface_member_resources(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        MemberFixture f; member_fixture(&f);
        XrXirCompileContext context = interface_context_default();
        attempts = 0; fail_at = site ? site - 1 : SIZE_MAX;
        CHECK(xr_xir_compile_interfaces_verify_members_verified(&context,&f.table,&f.types)==
            (site ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        if (!site) sites = attempts;
        interface_temporary_clean(&context);
    }
    fail_at = SIZE_MAX;
    MemberFixture f; member_fixture(&f);
    XrXirCompileContext measured = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_members_verified(&measured,&f.table,&f.types)==XR_XIR_OK);
    XrCompileResourceStats stats = stage_stats(&measured);
    interface_temporary_clean(&measured);
    for (unsigned boundary = 0; boundary < 4; ++boundary) {
        XrXirCompileContext context = interface_exact_context(stats,boundary);
        CHECK(xr_xir_compile_interfaces_verify_members_verified(&context,&f.table,&f.types)==
            (boundary ? XR_XIR_BUDGET : XR_XIR_OK));
        interface_temporary_clean(&context);
    }
    printf("Interface member composition: %zu allocation failure sites\n",sites);
}
static void interface_member_parameter_order(void) {
    MemberFixture f; member_fixture(&f);
    XrXirConstraint constraints[2] = {{0},{0}};
    XrXirType p0 = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType p1 = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + 1);
    XrXirType swapped[] = {p1,p0}, identity[] = {p0,p1};
    f.nodes[1].result = p1; f.nodes[1].parameter_span = 2;
    f.types.count = 2; f.table.count = 4;
    for (uint32_t d = 0; d < 4; ++d) {
        f.declarations[d].constraints = constraints; f.declarations[d].parameter_count = 2;
    }
    /* B swaps A's parameters. C and D use opposite paths to that same A<P1,P0>. */
    f.parents[0] = (XrXirInterfaceApplication){0,swapped,2};
    f.parents[1] = (XrXirInterfaceApplication){0,identity,2};
    f.parents[2] = (XrXirInterfaceApplication){1,identity,2};
    f.parents[3] = (XrXirInterfaceApplication){2,swapped,2};
    XrXirCompileContext budget = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_members_verified(&budget, &f.table, &f.types)==XR_XIR_OK && interface_live == stage_owner_count);
    f.parents[3].arguments = identity; budget = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_members_verified(&budget, &f.table, &f.types)==XR_XIR_BAD_TYPE && interface_live == stage_owner_count);
}
static void interface_member_sparse_substitution(void) {
    MemberFixture f; member_fixture(&f); f.table.count = 1;
    f.declarations[0].methods = NULL; f.declarations[0].method_count = 0;
    XrXirCompileContext short_pool = interface_context_default(), long_pool = interface_context_default();
    f.types.count = 2;
    CHECK(xr_xir_compile_interfaces_verify_members_verified(&short_pool,&f.table,&f.types)==XR_XIR_OK);
    f.types.count = 8;
    CHECK(xr_xir_compile_interfaces_verify_members_verified(&long_pool,&f.table,&f.types)==XR_XIR_OK);
    XrCompileResourceStats short_stats = stage_stats(&short_pool), long_stats = stage_stats(&long_pool);
    CHECK(!memcmp(&short_stats,&long_stats,sizeof(short_stats)));
    interface_temporary_clean(&short_pool); interface_temporary_clean(&long_pool);
}
static void interface_member_cases(void) {
    interface_member_semantics(); interface_member_resources(); interface_member_parameter_order();
    interface_member_sparse_substitution();
}
#endif // XIR_INTERFACE_MEMBER_CASES_H
