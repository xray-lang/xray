/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_interfaces.c - Abstract declaration structure and owned failure paths
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_interface.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at = SIZE_MAX, interface_live;
static void *counted_calloc(size_t count, size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *p = xr_calloc(count, size); if (p) ++interface_live; return p;
}
static void *counted_malloc(size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *p = xr_malloc(size); if (p) ++interface_live; return p;
}
static void counted_free(void *p) { if (p) { CHECK(interface_live); --interface_live; } xr_free(p); }
#undef xr_calloc
#undef xr_malloc
#undef xr_free
#define xr_calloc(n, s) counted_calloc(n, s)
#define xr_malloc(s) counted_malloc(s)
#define xr_free(p) counted_free(p)
#include "xir_interface_context_owner.h"
#define xr_malloc(s) counted_malloc(s)
#define xr_free(p) counted_free(p)
#include "xir/xxir_interface.c"
#include "xir/xxir_interface_members.c"
#include "xir/xxir_constraints.c"
#include "xir/xxir_constraint_proof.c"
#include "xir/xxir_implementation.c"
#include "xir/xxir_implementation_verify.c"
#include "xir/xxir_type_inference.c"
#include "xir_interface_member_cases.h"
#include "xir_interface_access_cases.h"
#include "xir_generic_method_access_cases.h"
#include "xir_interface_closure_cases.h"
#include "xir_constraint_proof_cases.h"
#include "xir_implementation_semantic_cases.h"
#include "xir_generic_method_proof_cases.h"
#include "xir_type_inference_cases.h"
#include "xir_generic_method_authority_cases.h"

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
    f->member = (XrXirInterfaceMethod){{f->method, 3}, (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE, 0,0,NULL};
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
        XrXirCompileContext budget = interface_context_default();
        XrXirInterfaceTable *copy = NULL;
        XrXirStatus status = xr_xir_compile_interfaces_clone(&budget, &f.table, &f.types, &copy);
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
            CHECK(xr_xir_compile_interfaces_verify_structure(&budget, copy, &f.types)==XR_XIR_OK);
        } else {
            CHECK(status == XR_XIR_OUT_OF_MEMORY && !copy);
            interface_temporary_clean(&budget);
        }
        xr_xir_compile_interfaces_free(copy); CHECK(interface_live == stage_owner_count);
    }
    fail_at = SIZE_MAX; printf("Interface structure ownership: %zu allocation failure sites\n", sites);
}
static void rejection_and_budget(void) {
    for (unsigned attack = 0; attack < 11; ++attack) {
        Fixture f; fixture(&f);
        if (attack == 0) f.parents[0].declaration = 1;
        if (attack == 1) f.parents[0].declaration = UINT32_MAX;
        if (attack == 2) f.member.receiver = 1;
        if (attack == 3) f.member.signature = XR_XIR_I64;
        if (attack == 4) f.parents[0].argument_count = 0;
        if (attack == 5) f.constraint.markers = 8;
        if (attack == 6) f.declarations[0].exported = 0;
        if (attack == 7) f.declarations[1].name = f.declarations[0].name;
        if (attack == 8) f.parameter = XR_XIR_UNIT;
        if (attack == 9) f.declarations[0].parents = f.parents, f.declarations[0].parent_count = 1;
        if (attack == 10) f.constraint.markers = 64;
        XrXirCompileContext context = interface_context_default();
        XrXirInterfaceTable *copy = NULL;
        XrXirStatus status = xr_xir_compile_interfaces_clone(&context,&f.table,&f.types,&copy);
        if (attack == 5) {
            CHECK(status == XR_XIR_OK && copy);
            CHECK(copy->declarations[0].constraints[0].markers == XR_XIR_CONSTRAINT_ATOMIC_VALUE);
            xr_xir_compile_interfaces_free(copy); copy = NULL;
        } else CHECK(status != XR_XIR_OK && !copy);
        interface_temporary_clean(&context);
    }
    Fixture f; fixture(&f);
    XrXirCompileContext measured = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_structure(&measured,&f.table,&f.types)==XR_XIR_OK);
    interface_temporary_clean(&measured);
    measured = interface_context_default();
    XrXirInterfaceTable *copy = NULL;
    CHECK(xr_xir_compile_interfaces_clone(&measured,&f.table,&f.types,&copy)==XR_XIR_OK && copy);
    XrCompileResourceStats stats = stage_stats(&measured);
    xr_xir_compile_interfaces_free(copy); copy = NULL;
    interface_temporary_clean(&measured);
    for (unsigned boundary = 0; boundary < 5; ++boundary) {
        XrXirCompileContext context = interface_exact_context(stats,boundary);
        if (boundary == 4) context.limits.parameters = 1;
        CHECK(xr_xir_compile_interfaces_clone(&context,&f.table,&f.types,&copy)==(boundary ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK((copy!=NULL)==!boundary);
        xr_xir_compile_interfaces_free(copy); copy = NULL;
        interface_temporary_clean(&context);
    }
    XrXirCompileContext no_storage = interface_context_limited(STAGE_ALLOCATED_BYTES,0,STAGE_WORK);
    CHECK(xr_xir_compile_interfaces_verify_structure(&no_storage,&f.table,&f.types)==XR_XIR_BUDGET);
    interface_temporary_clean(&no_storage);
}
static XrXirStatus check_interface_module(const XrXirCompileContext *context, const XrXirTypes *types, XrXirFunction function,
    XrXirArtifact **checked, XrXirDiagnostic *diagnostic) {
    XrXirInstruction ret = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirBlock block = {0,1,0,0};
    XrXirFunction functions[] = {function,
        {"init",4,NULL,0,XR_XIR_UNIT,&block,1,&ret,1,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&block,1,&ret,1,NULL,0}};
    uint32_t dependency = 0;
    XrXirSourceModule modules[] = {{"alpha",5,NULL,0,1},{"other",5,&dependency,1,2}};
    XrXirFunctionIdentity identities[] = {{0},{0},{1,0,0,0,0,0, XR_XIR_NON_MEMBER, 0, 0}};
    XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,0,0, NULL};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,NULL,types,NULL, XR_XIR_PROGRAM, NULL};
    return xir_fixture_check(context, &module, checked, diagnostic);
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
        XrXirCompileContext context = interface_context_default();
        XrXirStatus status = check_interface_module(&context,&f.types,function,&checked,NULL);
        if (!site) {
            CHECK(status == XR_XIR_OK && checked); sites = attempts;
            const XrXirModule *owned = xr_xir_compile_artifact_module(checked);
            CHECK(owned->types != &f.types && owned->types->interfaces != &f.table);
            memset(&f,0xcc,sizeof(f)); memset(instructions,0xcc,sizeof(instructions));
            memset(&block,0xcc,sizeof(block)); memset(&function,0xcc,sizeof(function));
            CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
            const XrXirInterfaceDeclaration *decls = owned->types->interfaces->declarations;
            CHECK(owned->types->interfaces->count == 3);
            CHECK(!memcmp(decls[0].name.bytes,"Measure",7));
            CHECK(!memcmp(decls[0].methods[0].name.bytes,"get",3));
            CHECK(decls[1].parents[0].arguments[0] == XR_XIR_TYPE_PARAMETER_BASE);
            CHECK(owned->functions[0].instructions[0].immediate == 41);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !checked);
        xr_xir_compile_artifact_free(checked); CHECK(interface_live == stage_owner_count);
    }
    fail_at = SIZE_MAX;
    printf("Checked interface owner: %zu allocation failure sites\n",sites);
}
static XrXirStatus inherited_module_verify(Fixture *f, XrXirCompileContext *budget) {
    XrXirInstruction op = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirBlock block = {0,1,0,0}, entry_block = {0,2,0,0};
    XrXirInstruction entry[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&entry_block,1,entry,2,NULL,0}};
    uint32_t dependency = 0;
    XrXirSourceModule modules[] = {{"alpha",5,NULL,0,0},{"other",5,&dependency,1,1}};
    XrXirFunctionIdentity identities[] = {{0},{.module=1},{0}};
    XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,0,2,NULL};
    XrXirTypes types = f->types; types.interfaces = &f->table;
    XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
    return xir_fixture_verify(budget, &module, NULL);
}
static void inherited_contexts(void) {
    Fixture f; fixture(&f);
    XrXirConstraint parent_constraint = {.markers = XR_XIR_CONSTRAINT_SENDABLE};
    f.declarations[0].constraints = &parent_constraint;
    XrXirCompileContext budget = interface_context_default();
    CHECK(inherited_module_verify(&f,&budget)==XR_XIR_BAD_TYPE);
    f.constraint.markers = XR_XIR_CONSTRAINT_SENDABLE;
    budget = interface_context_default();
    CHECK(inherited_module_verify(&f,&budget)==XR_XIR_OK);
    XrXirInterfaceApplication cycle = {1,&f.parameter,1};
    f.declarations[0].parents = &cycle; f.declarations[0].parent_count = 1;
    budget = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_structure(&budget, &f.table, &f.types)==XR_XIR_BAD_STRUCTURE);
    f.declarations[0].parents = NULL; f.declarations[0].parent_count = 0;
    XrXirInterfaceApplication repeated[] = {{0,&f.parameter,1},{0,&f.parameter,1}};
    f.declarations[1].parents = repeated; f.declarations[1].parent_count = 2;
    budget = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_structure(&budget, &f.table, &f.types)==XR_XIR_OK);
    XrXirInterfaceMethod duplicate[] = {f.member,f.member};
    f.declarations[0].methods = duplicate; f.declarations[0].method_count = 2;
    budget = interface_context_default();
    CHECK(xr_xir_compile_interfaces_verify_structure(&budget, &f.table, &f.types)==XR_XIR_BAD_STRUCTURE && interface_live == stage_owner_count);
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
        XrXirCompileContext context = interface_context_default();
        XrXirStatus status = check_interface_module(&context,&f.types,function,&checked,&diagnostic);
        CHECK(status == (attack ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        CHECK((checked != NULL) == !attack);
        if (attack == 3 || attack == 6) CHECK(diagnostic.instruction == 1);
        xr_xir_compile_artifact_free(checked); CHECK(interface_live == stage_owner_count);
    }
}

static void checked_owner_outlives_producer(void) {
    size_t blocks_before = stage_physical_count, bytes_before = stage_physical_bytes;
    XrCompileResourceLimits limits = {STAGE_ALLOCATED_BYTES,STAGE_LIVE_BYTES,STAGE_WORK};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    Fixture f; fixture(&f); f.types.interfaces = &f.table;
    XrXirInstruction instructions[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock block = {0,2,0,0};
    XrXirFunction function = {"entry",5,NULL,0,XR_XIR_I64,&block,1,instructions,2,NULL,0};
    XrXirArtifact *checked = NULL;
    CHECK(check_interface_module(&context,&f.types,function,&checked,NULL)==XR_XIR_OK && checked);
    xr_compile_resources_release(context.resources); context = (XrXirCompileContext){0};
    memset(&f,0xcc,sizeof(f)); memset(instructions,0xcc,sizeof(instructions));
    memset(&block,0xcc,sizeof(block)); memset(&function,0xcc,sizeof(function));
    CHECK(stage_physical_count>blocks_before && stage_physical_bytes>bytes_before);
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    const XrXirModule *owned = xr_xir_compile_artifact_module(checked);
    CHECK(owned->types->interfaces->count==3);
    CHECK(!memcmp(owned->types->interfaces->declarations[0].name.bytes,"Measure",7));
    CHECK(owned->functions[0].instructions[0].immediate==41);
    xr_xir_compile_artifact_free(checked);
    CHECK(stage_physical_count==blocks_before && stage_physical_bytes==bytes_before);
    CHECK(interface_live==stage_owner_count);
    puts("Checked interface owner survives producer release and poisoned inputs; physical delta=0/0");
}

int main(void) {
    type_inference_cases();
    implementation_semantic_cases(); generic_method_proof_cases(); generic_method_authority_cases();
    interface_closure_cases();
    constraint_proof_cases();
    interface_access_cases();
    generic_method_access_cases();
    interface_member_cases();
    ownership_and_oom(); rejection_and_budget(); inherited_contexts(); checked_owner_lifetime();
    interface_parameters_remain_declaration_scoped(); checked_owner_outlives_producer();
    stage_contexts_free(); CHECK(!interface_live);
    puts("Interface structure, explicit inheritance, budgets and owned cloning passed"); return 0;
}
