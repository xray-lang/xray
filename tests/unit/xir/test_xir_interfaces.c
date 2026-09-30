/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_interfaces.c - Abstract declaration structure and owned failure paths
 */
#include "xir/xxir_interface.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at = SIZE_MAX, live;
static void *counted_calloc(size_t count, size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *p = xr_calloc(count, size); if (p) ++live; return p;
}
static void *counted_malloc(size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *p = xr_malloc(size); if (p) ++live; return p;
}
static void counted_free(void *p) { if (p) { CHECK(live); --live; } xr_free(p); }
#undef xr_calloc
#undef xr_malloc
#undef xr_free
#define xr_calloc(n, s) counted_calloc(n, s)
#define xr_malloc(s) counted_malloc(s)
#define xr_free(p) counted_free(p)
#include "xir/xxir_interface.c"
#include "xir/xxir_interface_members.c"
#include "xir_interface_member_cases.h"
#include "xir_interface_access_cases.h"

typedef struct Fixture {
    char name[8], method[4];
    XrXirConstraint constraint;
    XrXirType parameter, concrete;
    XrXirTypeNode signature;
    XrXirTypes types;
    XrXirInterfaceMethod member;
    XrXirInterfaceApplication parents[2];
    XrXirInterfaceDeclaration declarations[3];
    XrXirInterfaceTable table;
} Fixture;
static void fixture(Fixture *f) {
    memset(f, 0, sizeof(*f)); memcpy(f->name, "Measure", 8); memcpy(f->method, "get", 4);
    f->parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; f->concrete = XR_XIR_I64;
    f->signature.kind = XR_XIR_TYPE_CALLABLE; f->signature.result = f->parameter; f->signature.parameter_span = 1;
    f->types = (XrXirTypes){&f->signature, 1, NULL, NULL};
    f->member = (XrXirInterfaceMethod){{f->method, 3}, (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE, 0};
    f->parents[0] = (XrXirInterfaceApplication){0, &f->parameter, 1};
    f->parents[1] = (XrXirInterfaceApplication){0, &f->concrete, 1};
    f->declarations[0] = (XrXirInterfaceDeclaration){{"alpha",5},{f->name,7},1,&f->constraint,1,NULL,0,&f->member,1};
    f->declarations[1] = (XrXirInterfaceDeclaration){{"alpha",5},{"Child",5},1,&f->constraint,1,f->parents,1,NULL,0};
    f->declarations[2] = (XrXirInterfaceDeclaration){{"other",5},{"Concrete",8},1,NULL,0,f->parents+1,1,NULL,0};
    f->table = (XrXirInterfaceTable){f->declarations,3};
}
static void ownership_and_oom(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        Fixture f; fixture(&f); attempts = 0; fail_at = site ? site - 1 : SIZE_MAX;
        XrXirBudget budget = xr_xir_default_budget(), original = budget;
        XrXirInterfaceTable *copy = NULL;
        XrXirStatus status = xr_xir_interfaces_clone(&f.table, &f.types, &budget, &copy);
        if (!site) {
            CHECK(status == XR_XIR_OK && copy); sites = attempts;
            memset(f.name, 0xcc, sizeof(f.name)); memset(f.method, 0xcc, sizeof(f.method));
            f.parameter = XR_XIR_UNIT; f.concrete = XR_XIR_UNIT; f.constraint.markers = UINT32_MAX;
            memset(f.parents, 0xcc, sizeof(f.parents)); memset(f.declarations, 0xcc, sizeof(f.declarations));
            CHECK(copy->count == 3 && !memcmp(copy->declarations[0].name.bytes,"Measure",7));
            CHECK(!memcmp(copy->declarations[0].methods[0].name.bytes,"get",3));
            CHECK(copy->declarations[1].parents[0].arguments[0] == XR_XIR_TYPE_PARAMETER_BASE);
            CHECK(copy->declarations[2].parents[0].arguments[0] == XR_XIR_I64);
            CHECK(!copy->declarations[0].constraints[0].markers);
            budget = original; CHECK(xr_xir_interfaces_verify_structure(copy,&f.types,&budget)==XR_XIR_OK);
        } else {
            CHECK(status == XR_XIR_OUT_OF_MEMORY && !copy);
            CHECK(!memcmp(&budget,&original,sizeof(budget)));
        }
        xr_xir_interfaces_free(copy); CHECK(!live);
    }
    fail_at = SIZE_MAX; printf("Interface structure ownership: %zu allocation failure sites\n", sites);
}
static void rejection_and_budget(void) {
    for (unsigned attack = 0; attack < 10; ++attack) {
        Fixture f; fixture(&f);
        if (attack == 0) f.parents[0].declaration = 1;
        if (attack == 1) f.parents[0].declaration = UINT32_MAX;
        if (attack == 2) f.member.receiver = 1;
        if (attack == 3) f.member.signature = XR_XIR_I64;
        if (attack == 4) f.parents[0].argument_count = 0;
        if (attack == 5) f.constraint.markers = 4;
        if (attack == 6) f.declarations[0].exported = 0;
        if (attack == 7) f.declarations[1].name = f.declarations[0].name;
        if (attack == 8) f.parameter = XR_XIR_UNIT;
        if (attack == 9) f.declarations[0].parents = f.parents, f.declarations[0].parent_count = 1;
        XrXirBudget budget = xr_xir_default_budget(), original = budget;
        XrXirInterfaceTable *copy = NULL;
        CHECK(xr_xir_interfaces_clone(&f.table,&f.types,&budget,&copy)!=XR_XIR_OK && !copy && !live);
        CHECK(!memcmp(&budget,&original,sizeof(budget)));
    }
    Fixture f; fixture(&f); XrXirBudget original = xr_xir_default_budget(), spent = original;
    CHECK(xr_xir_interfaces_verify_structure(&f.table,&f.types,&spent)==XR_XIR_OK && !live);
    XrXirBudget exact = original;
    exact.metadata_bytes -= spent.metadata_bytes; exact.scratch_bytes -= spent.scratch_bytes;
    exact.work -= spent.work; exact.parameters -= spent.parameters;
    for (unsigned boundary = 0; boundary < 5; ++boundary) {
        XrXirBudget budget = exact;
        if (boundary == 1) --budget.metadata_bytes;
        if (boundary == 2) --budget.scratch_bytes;
        if (boundary == 3) --budget.work;
        if (boundary == 4) --budget.parameters;
        XrXirInterfaceTable *copy = NULL;
        CHECK(xr_xir_interfaces_clone(&f.table,&f.types,&budget,&copy)==(boundary ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK((copy!=NULL)==!boundary); xr_xir_interfaces_free(copy); CHECK(!live);
    }
}
static XrXirStatus check_interface_module(const XrXirTypes *types, XrXirFunction function,
    XrXirArtifact **checked, XrXirDiagnostic *diagnostic) {
    XrXirInstruction ret = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirBlock block = {0,1,0,0};
    XrXirFunction functions[] = {function,
        {"init",4,NULL,0,XR_XIR_UNIT,&block,1,&ret,1,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&block,1,&ret,1,NULL,0}};
    uint32_t dependency = 0;
    XrXirSourceModule modules[] = {{"alpha",5,NULL,0,1},{"other",5,&dependency,1,2}};
    XrXirFunctionIdentity identities[] = {{0},{0},{1,0,0,0,0,0}};
    XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,0,0};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,NULL,types,NULL};
    return xr_xir_check(&module,NULL,checked,diagnostic);
}
static void checked_owner_lifetime(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        Fixture f; fixture(&f); f.types.interfaces = &f.table;
        XrXirInstruction instructions[] = {
            {XR_XIR_CONST_INT, XR_XIR_I64, {0,0}, {0,0}, 41, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {0,0}, {0,0}, 0, {0}}
        };
        XrXirBlock block = {0,2,0,0};
        XrXirFunction function = {"entry",5,NULL,0,XR_XIR_I64,&block,1,instructions,2,NULL,0};
        attempts = 0; fail_at = site ? site - 1 : SIZE_MAX;
        XrXirArtifact *checked = NULL;
        XrXirStatus status = check_interface_module(&f.types,function,&checked,NULL);
        if (!site) {
            CHECK(status == XR_XIR_OK && checked); sites = attempts;
            const XrXirModule *owned = xr_xir_artifact_module(checked);
            CHECK(owned->types != &f.types && owned->types->interfaces != &f.table);
            memset(&f,0xcc,sizeof(f)); memset(instructions,0xcc,sizeof(instructions));
            memset(&block,0xcc,sizeof(block)); memset(&function,0xcc,sizeof(function));
            CHECK(xr_xir_artifact_verify(checked,NULL,NULL) == XR_XIR_OK);
            const XrXirInterfaceDeclaration *decls = owned->types->interfaces->declarations;
            CHECK(owned->types->interfaces->count == 3);
            CHECK(!memcmp(decls[0].name.bytes,"Measure",7));
            CHECK(!memcmp(decls[0].methods[0].name.bytes,"get",3));
            CHECK(decls[1].parents[0].arguments[0] == XR_XIR_TYPE_PARAMETER_BASE);
            CHECK(owned->functions[0].instructions[0].immediate == 41);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !checked);
        xr_xir_artifact_free(checked); CHECK(!live);
    }
    fail_at = SIZE_MAX;
    printf("Checked interface owner: %zu allocation failure sites\n",sites);
}
static void inherited_contexts(void) {
    Fixture f; fixture(&f);
    XrXirConstraint parent_constraint = {XR_XIR_CONSTRAINT_SENDABLE};
    f.declarations[0].constraints = &parent_constraint;
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_interfaces_verify_structure(&f.table,&f.types,&budget)==XR_XIR_BAD_TYPE);
    f.constraint.markers = XR_XIR_CONSTRAINT_SENDABLE;
    budget = xr_xir_default_budget();
    CHECK(xr_xir_interfaces_verify_structure(&f.table,&f.types,&budget)==XR_XIR_OK);
    XrXirInterfaceApplication cycle = {1,&f.parameter,1};
    f.declarations[0].parents = &cycle; f.declarations[0].parent_count = 1;
    budget = xr_xir_default_budget();
    CHECK(xr_xir_interfaces_verify_structure(&f.table,&f.types,&budget)==XR_XIR_BAD_STRUCTURE);
    f.declarations[0].parents = NULL; f.declarations[0].parent_count = 0;
    XrXirInterfaceApplication repeated[] = {{0,&f.parameter,1},{0,&f.parameter,1}};
    f.declarations[1].parents = repeated; f.declarations[1].parent_count = 2;
    budget = xr_xir_default_budget();
    CHECK(xr_xir_interfaces_verify_structure(&f.table,&f.types,&budget)==XR_XIR_OK);
    XrXirInterfaceMethod duplicate[] = {f.member,f.member};
    f.declarations[0].methods = duplicate; f.declarations[0].method_count = 2;
    budget = xr_xir_default_budget();
    CHECK(xr_xir_interfaces_verify_structure(&f.table,&f.types,&budget)==XR_XIR_BAD_STRUCTURE && !live);
}
static void interface_parameters_remain_declaration_scoped(void) {
    for (unsigned attack = 0; attack < 7; ++attack) {
        Fixture f; fixture(&f); f.types.interfaces = &f.table;
        XrXirType leaked = attack <= 3 ? f.parameter : f.member.signature;
        XrXirInstruction instructions[] = {
            {XR_XIR_CONST_INT, XR_XIR_I64, {0,0}, {0,0}, 41, {0}},
            {XR_XIR_COPY, XR_XIR_I64, {0,0}, {0,0}, 0, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {0,0}, {0,0}, 0, {0}}
        };
        XrXirBlock block = {0,3,0,0};
        XrXirFunction function = {"entry",5,NULL,0,XR_XIR_I64,&block,1,instructions,3,NULL,0};
        if (attack == 1 || attack == 4) {
            function.parameters = &leaked; function.parameter_count = 1;
            instructions[1].args[0] = 1; instructions[2].args[0] = 1;
        }
        if (attack == 2 || attack == 5) function.result = leaked;
        if (attack == 3 || attack == 6)
            instructions[1] = (XrXirInstruction){XR_XIR_LOCAL_UNINIT,leaked,{0,0},{0,0},0,{0}};
        XrXirArtifact *checked = NULL;
        XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = check_interface_module(&f.types,function,&checked,&diagnostic);
        CHECK(status == (attack ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        CHECK((checked != NULL) == !attack);
        if (attack == 3 || attack == 6) CHECK(diagnostic.instruction == 1);
        xr_xir_artifact_free(checked); CHECK(!live);
    }
}
int main(void) {
    interface_access_cases();
    interface_member_cases();
    ownership_and_oom(); rejection_and_budget(); inherited_contexts(); checked_owner_lifetime();
    interface_parameters_remain_declaration_scoped();
    puts("Interface structure, explicit inheritance, budgets and owned cloning passed"); return 0;
}
