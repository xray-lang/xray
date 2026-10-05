/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_allocations.c - Fail each source-owner metadata allocation
 *
 * KEY CONCEPT:
 *   The complete source pipeline and its escaped owners share one finite ledger.
 */
#include "base/xmalloc.h"
#include "base/xhash.h"
#include "toolchain/xcompiler_session.h"
#include "module/xmodule_resolver.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#ifdef interface
#undef interface
#endif
static unsigned resolver_fault;
static XrModuleStatus source_fault_resolve(XrModuleResolver *resolver, const char *specifier,
    const char *importer, const XrModuleIdentityAuthority *authority, XrModuleId *id, char **error) {
    if (resolver_fault == 1) return XR_MODULE_OK;
    if (resolver_fault == 2) return XR_MODULE_INVALID;
    XrModuleStatus status = xr_compile_module_resolver_resolve(resolver,specifier,importer,authority,id,error);
    char **field = NULL;
    if (resolver_fault == 3) field = &id->canonical;
    if (resolver_fault == 4) field = &id->logical_path;
    if (resolver_fault == 5) {
        CHECK(status == XR_MODULE_OK && id->authority.physical_root);
        xr_compile_resources_free((char *)id->authority.physical_root);
        id->authority.physical_root = NULL;
    }
    if (resolver_fault == 6) field = &id->source_path;
    if (field) { CHECK(status == XR_MODULE_OK && *field); xr_compile_resources_free(*field); *field = NULL; }
    return status;
}
#include "xir/xxir_source_query.c"
#define xr_compile_module_resolver_resolve source_fault_resolve
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#undef xr_compile_module_resolver_resolve
#include "xir_source_allocation_context.h"
#include "xir_source_allocation_shards.h"
#include "xir_nominal_fixture.h"
static void snapshot_nominal_allocations(void) {
    for (unsigned declaration_only = 0; declaration_only < 2; ++declaration_only) {
        size_t sites = allocation_case_begin(14+declaration_only);
        for(size_t attempt=allocation_first();allocation_more(attempt,sites);attempt=allocation_next(attempt)) {
            NominalFixture fixture; nominal_fixture(&fixture);
            XrXirType argument = XR_XIR_I64, fields[] = {XR_XIR_I64, XR_XIR_STRING};
            XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
            node.nominal = (XrXirNominalType) {0, &argument, 1, fields, 2};
            XrXirTypes types = {declaration_only ? NULL : &node, declaration_only ? 0 : 1, &fixture.table, NULL};
            XrXirSourceView view = {0}; view.types = &types; view.diagnostic.status = XR_XIR_BAD_TYPE;
            XrCompileResourceLimits limits = allocation_limits();
            source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = attempt ? attempt - 1 : SIZE_MAX; source_fixture_compile_injected=false;
            XrXirSourceSnapshot *snapshot = NULL;
            XrXirStatus status = allocation_snapshot_copy(&view,&limits,&snapshot);
            if (!attempt) {
                CHECK(status == XR_XIR_OK && snapshot); sites = source_fixture_compile_attempts;
                allocation_snapshot_boundaries(&view,allocation_last_stats);
                memset(&fixture, 0xcc, sizeof(fixture)); memset(&node, 0xcc, sizeof(node));
                argument = XR_XIR_UNIT; fields[0] = fields[1] = XR_XIR_UNIT;
                const XrXirSourceView *copy = xr_xir_compile_source_snapshot_view(snapshot);
                CHECK(!copy->complete && copy->diagnostic.status == XR_XIR_BAD_TYPE);
                CHECK(copy->types->count == (declaration_only ? 0u : 1u));
                const XrXirNominalTable *table = copy->types->nominals;
                CHECK(table && table->count == 2 && !table->identities);
                const XrXirNominalDeclaration *decl = table->declarations;
                CHECK(decl[0].module.length == 5 && !memcmp(decl[0].module.bytes, "alpha", 5));
                CHECK(decl[0].name.length == 4 && !memcmp(decl[0].name.bytes, "Pair", 4));
                CHECK(decl[0].constraints[0].markers == XR_XIR_CONSTRAINT_SENDABLE);
                CHECK(decl[0].fields[0].type == XR_XIR_TYPE_PARAMETER_BASE);
                CHECK(decl[0].fields[0].flags == XR_XIR_FIELD_MUTABLE);
                CHECK(!memcmp(decl[0].fields[0].name.bytes, "value", 5));
                CHECK(decl[1].fields[1].flags == XR_XIR_FIELD_PRIVATE);
                if (!declaration_only) {
                    CHECK(copy->types->nodes[0].nominal.arguments[0] == XR_XIR_I64);
                    CHECK(copy->types->nodes[0].nominal.fields[0] == XR_XIR_I64);
                    CHECK(copy->types->nodes[0].nominal.fields[1] == XR_XIR_STRING);
                }
            } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
            xr_xir_compile_source_snapshot_free(snapshot); CHECK(!source_fixture_compile_live);
                if(attempt)allocation_point(attempt-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
        source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false;
        printf("Nominal query snapshot (%u): %zu allocation failure sites\n", declaration_only, sites);
    }
}
static void snapshot_interface_allocations(void) {
    size_t sites = allocation_case_begin(10);
    for(size_t site=allocation_first();allocation_more(site,sites);site=allocation_next(site)) {
        char name[] = "Measure", member[] = "get", module[] = "alpha";
        XrXirConstraint constraint = {0};
        XrXirType argument = XR_XIR_I64;
        XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_CALLABLE;
        node.result = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; node.parameter_span = 1;
        XrXirInterfaceMethod method = {{member,3},(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,0,0,NULL};
        XrXirInterfaceApplication parent = {0,&argument,1};
        XrXirInterfaceDeclaration declarations[] = {
            {{module,5},{name,7},1,&constraint,1,NULL,0,&method,1},
            {{module,5},{"Concrete",8},1,NULL,0,&parent,1,NULL,0}
        };
        XrXirInterfaceTable table = {declarations,2};
        XrXirTypes types = {&node,1,NULL,&table};
        XrXirSourceView view = {0}; view.types = &types;
        XrCompileResourceLimits limits = allocation_limits();
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = site ? site - 1 : SIZE_MAX; source_fixture_compile_injected=false;
        XrXirSourceSnapshot *snapshot = NULL;
        XrXirStatus status = allocation_snapshot_copy(&view,&limits,&snapshot);
        if (!site) {
            CHECK(status == XR_XIR_OK && snapshot); sites = source_fixture_compile_attempts;
            allocation_snapshot_boundaries(&view,allocation_last_stats);
            memset(name,0xcc,sizeof(name)); memset(member,0xcc,sizeof(member));
            memset(module,0xcc,sizeof(module)); memset(declarations,0xcc,sizeof(declarations));
            memset(&node,0xcc,sizeof(node)); memset(&parent,0xcc,sizeof(parent));
            constraint.markers = UINT32_MAX; argument = XR_XIR_UNIT;
            const XrXirTypes *copy = xr_xir_compile_source_snapshot_view(snapshot)->types;
            CHECK(copy != &types && copy->interfaces != &table);
            const XrXirInterfaceDeclaration *decls = copy->interfaces->declarations;
            CHECK(copy->interfaces->count == 2 && !memcmp(decls[0].name.bytes,"Measure",7));
            CHECK(!memcmp(decls[0].module.bytes,"alpha",5));
            CHECK(!memcmp(decls[0].methods[0].name.bytes,"get",3));
            CHECK(!decls[0].constraints[0].markers && decls[1].parents[0].arguments[0] == XR_XIR_I64);
            CHECK(xr_xir_compile_types_structure_verify(&snapshot->context,copy) == XR_XIR_OK);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
        xr_xir_compile_source_snapshot_free(snapshot); CHECK(!source_fixture_compile_live);
            if(site)allocation_point(site-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false;
    printf("Interface query snapshot: %zu allocation failure sites\n",sites);
}
static void snapshot_enum_allocations(void) {
    for (unsigned payload = 0; payload < 2; ++payload) {
        size_t sites = allocation_case_begin(11+payload);
        for(size_t site=allocation_first();allocation_more(site,sites);site=allocation_next(site)) {
            char names[][5] = {"None", "Some"};
            XrXirNominalVariant variants[] = {{{names[0], 4}, 0, 0}, {{names[1], 4}, 0, payload}};
            XrXirNominalField field = {{"value", 5}, XR_XIR_I64, 0};
            XrXirNominalDeclaration declaration = {{"alpha", 5}, {"Choice", 6}, 1,
                NULL, 0, payload ? &field : NULL, payload, XR_XIR_NOMINAL_ENUM, variants, 2, 0, {0}};
            XrXirNominalTable table = {&declaration, 1, NULL};
            XrXirTypes types = {NULL, 0, &table, NULL};
            XrXirSourceView view = {0}; view.types = &types;
            XrCompileResourceLimits limits = allocation_limits();
            source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = site ? site - 1 : SIZE_MAX; source_fixture_compile_injected=false;
            XrXirSourceSnapshot *snapshot = NULL;
            XrXirStatus status = allocation_snapshot_copy(&view,&limits,&snapshot);
            if (!site) {
                CHECK(status == XR_XIR_OK && snapshot); sites = source_fixture_compile_attempts;
                const XrXirTypes *copy = xr_xir_compile_source_snapshot_view(snapshot)->types;
                const XrXirNominalDeclaration *owned_decl = copy->nominals->declarations;
                CHECK(owned_decl->variants != variants);
                CHECK(owned_decl->variants[0].name.bytes != names[0]);
                CHECK(owned_decl->variants[1].name.bytes != names[1]);
                allocation_snapshot_boundaries(&view,allocation_last_stats);
                memset(names, 0xcc, sizeof(names)); memset(variants, 0xcc, sizeof(variants));
                memset(&declaration, 0xcc, sizeof(declaration)); memset(&field, 0xcc, sizeof(field));
                CHECK(owned_decl->variant_count == 2 && owned_decl->kind == XR_XIR_NOMINAL_ENUM);
                CHECK(owned_decl->variants[0].field_count == 0 && owned_decl->variants[1].field_count == payload);
                CHECK(!memcmp(owned_decl->variants[0].name.bytes, "None", 4));
                CHECK(!memcmp(owned_decl->variants[1].name.bytes, "Some", 4));
                CHECK(xr_xir_compile_types_structure_verify(&snapshot->context,copy) == XR_XIR_OK);
            } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
            xr_xir_compile_source_snapshot_free(snapshot); CHECK(!source_fixture_compile_live);
                if(site)allocation_point(site-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
        source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false;
        printf("Enum query snapshot (%u): %zu allocation failure sites\n", payload, sites);
    }
}
static void snapshot_type_allocations(void) {
    const XrXirCallableParameter parameter = {XR_XIR_I64, 0};
    const XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, &parameter, 1, XR_XIR_STRING, 0, 0, {0}},
        {XR_XIR_TYPE_ARRAY, XR_XIR_STRING, NULL, 0, XR_XIR_UNIT, 0, 0, {0}},
        {XR_XIR_TYPE_CELL, (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE, NULL, 0, XR_XIR_UNIT, 0, 0, {0}}
    };
    const XrXirTypes types = {nodes, 3, NULL, NULL};
    XrXirSourceView view = {0}; view.types = &types;
    view.diagnostic.status = XR_XIR_BAD_TYPE;
    XrCompileResourceLimits limits = allocation_limits();
    if(allocation_mode!=2){XrXirCompileContext verify = allocation_context(limits);
        CHECK(xr_xir_compile_types_structure_verify(&verify,&types) == XR_XIR_OK);allocation_context_close(&verify);}
    size_t sites = allocation_case_begin(16);
    for(size_t attempt=allocation_first();allocation_more(attempt,sites);attempt=allocation_next(attempt)) {
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = attempt ? attempt - 1 : SIZE_MAX; source_fixture_compile_injected=false;
        limits = allocation_limits();
        XrXirSourceSnapshot *snapshot = NULL;
        XrXirStatus status = allocation_snapshot_copy(&view,&limits,&snapshot);
        if (!attempt) {
            CHECK(status == XR_XIR_OK && snapshot); sites = source_fixture_compile_attempts;
            const XrXirSourceView *copy = xr_xir_compile_source_snapshot_view(snapshot);
            CHECK(!copy->complete && copy->diagnostic.status == XR_XIR_BAD_TYPE);
            CHECK(copy->types && copy->types != &types && copy->types->nodes != nodes);
            CHECK(copy->types->count == 3 && copy->types->nodes[0].parameters != &parameter);
            CHECK(copy->types->nodes[0].parameters[0].type == XR_XIR_I64);
            CHECK(copy->types->nodes[1].kind == XR_XIR_TYPE_ARRAY && copy->types->nodes[1].element == XR_XIR_STRING);
            CHECK(copy->types->nodes[2].kind == XR_XIR_TYPE_CELL && copy->types->nodes[2].element == XR_XIR_CONSTRUCTED_TYPE_BASE);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
        xr_xir_compile_source_snapshot_free(snapshot); CHECK(!source_fixture_compile_live);
            if(attempt)allocation_point(attempt-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false; source_fixture_compile_attempts = 0;
    printf("Constructed query snapshot: %zu allocation failure sites; no partial snapshot\n", sites);
}
static void array_source_allocations(void) {
    char directory[XR_TEST_PATH_MAX] = "xir-array-allocation-XXXXXX", absolute[XR_TEST_PATH_MAX], path[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    CHECK(snprintf(path, sizeof(path), "%s/root.xr", absolute) > 0);
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fputs("fn first<T>(a:Array<T>)->T{return a[0]}\nvar a=[\"x\"]\na.push(a[0])\na.set(0,\"y\")\n"
        "print(len(a),first<string>(a),a.get(1))\n", file) >= 0 && fclose(file) == 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {NULL, path, &authority, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    size_t sites = allocation_case_begin(18);
    for(size_t site=allocation_first();allocation_more(site,sites);site=allocation_next(site)) {
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = site ? site - 1 : SIZE_MAX; source_fixture_compile_injected=false;
        XrXirSourceResult result = {0};
        XrXirStatus status = allocation_source_check(&request, &result, NULL);
        if (!site) { CHECK(status == XR_XIR_OK && result.checked && result.snapshot); sites = source_fixture_compile_attempts; }
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && !result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result); CHECK(!source_fixture_compile_live);
            if(site)allocation_point(site-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false;
    if(allocation_mode!=2)for (unsigned kind = 0; kind < 2; ++kind) {
        XrCompileResourceLimits limits = allocation_limits();
        if (kind) limits.work = 100; else limits.allocated_bytes = 128;
        XrXirSourceResult result = {0};
        CHECK(allocation_source_check_limits(&request,&limits,&result,NULL) == XR_XIR_BUDGET);
        CHECK(!result.checked && !result.snapshot && !source_fixture_compile_live);
    }
    CHECK(xr_test_unlink(path) == 0 && xr_test_rmdir(directory) == 0);
    printf("Array source and owned native facts: %zu OOM sites; no partial publication\n", sites);
}
static bool source_nominal_substitution_case(SourceContext *ctx) {
    XrXirType args[] = {(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + 1), (XrXirType)XR_XIR_TYPE_PARAMETER_BASE};
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
    node.nominal = (XrXirNominalType) {0, args, 2, NULL, 0};
    XrXirType pair, nested, closed, repeated;
    if (!source_intern_type(ctx, node, &pair)) return false;
    CHECK(xr_xir_type_span(&ctx->types, pair) == 2);
    XrXirType nested_args[] = {pair, args[1]}; node.nominal.arguments = nested_args;
    if (!source_intern_type(ctx, node, &nested)) return false;
    CHECK(nested != pair);
    XrXirType actual[] = {XR_XIR_STRING, XR_XIR_I64};
    SourceSubstitution sub = {actual, 2};
    if (!source_substitute(ctx, &sub, nested, 0, &closed) ||
        !source_substitute(ctx, &sub, nested, 0, &repeated)) return false;
    CHECK(closed == repeated && !xr_xir_type_span(&ctx->types, closed));
    const XrXirTypeNode *outer = xr_xir_type_node(&ctx->types, closed);
    CHECK(outer->nominal.arguments[1] == XR_XIR_STRING && !outer->nominal.field_count);
    const XrXirTypeNode *inner = xr_xir_type_node(&ctx->types, outer->nominal.arguments[0]);
    CHECK(inner->nominal.arguments[0] == XR_XIR_I64 && inner->nominal.arguments[1] == XR_XIR_STRING);
    XrXirNominalField fields[] = {{{"first",5}, args[1], XR_XIR_FIELD_MUTABLE},
        {{"second",6}, args[0], 0}, {{"nested",6}, pair, 0}};
    XrXirNominalDeclaration declaration = {0}; declaration.fields = fields; declaration.field_count = 3;
    declaration.parameter_count = 2;
    ctx->nominals = (XrXirNominalTable) {&declaration, 1, NULL};
    ctx->types.nominals = &ctx->nominals;
    uint32_t members[] = {1,2,3}, *member_tables[] = {members}; ctx->nominal_members = member_tables;
    XrXirFunctionIdentity identity = {0}; ctx->identities = &identity;
    uint32_t field_index; XrXirType field_type;
    if (!source_struct_field(ctx, NULL, outer->nominal.arguments[0], "first", true, &field_index, &field_type)) return false;
    CHECK(field_index == 0 && field_type == XR_XIR_I64);
    if (!source_struct_field(ctx, NULL, outer->nominal.arguments[0], "second", false, &field_index, &field_type)) return false;
    CHECK(field_index == 1 && field_type == XR_XIR_STRING);
    if (!source_struct_field(ctx, NULL, outer->nominal.arguments[0], "nested", false, &field_index, &field_type)) return false;
    const XrXirTypeNode *field_node = xr_xir_type_node(&ctx->types, field_type);
    CHECK(field_node && field_node->nominal.arguments[0] == XR_XIR_STRING && field_node->nominal.arguments[1] == XR_XIR_I64);
    CHECK(ctx->query.reference_count == 3 && ctx->query.references[0].target == 1);
    XrXirType reversed[] = {XR_XIR_STRING, XR_XIR_I64}; node.nominal.arguments = reversed;
    if (!source_intern_type(ctx, node, &repeated)) return false;
    CHECK(repeated != outer->nominal.arguments[0]);
    node.nominal.declaration = 1;
    if (!source_intern_type(ctx, node, &closed)) return false;
    CHECK(closed != repeated);
    uint32_t references = ctx->query.reference_count;
    fields[0].flags |= XR_XIR_FIELD_PRIVATE;
    CHECK(!source_struct_field(ctx, NULL, outer->nominal.arguments[0], "first", false, &field_index, &field_type));
    CHECK(ctx->diagnostic.status == XR_XIR_BAD_TYPE && ctx->query.reference_count == references);
    ctx->diagnostic = (XrXirSourceDiagnostic) {0}; identity.nominal_owner = 1;
    identity.method_kind = XR_XIR_MEMBER_HELPER;
    if (!source_struct_field(ctx, NULL, outer->nominal.arguments[0], "first", true, &field_index, &field_type)) return false;
    CHECK(field_type == XR_XIR_I64);
    CHECK(!source_struct_field(ctx, NULL, outer->nominal.arguments[0], "second", true, &field_index, &field_type));
    CHECK(ctx->diagnostic.status == XR_XIR_BAD_TYPE && ctx->query.reference_count == references + 1);
    ctx->diagnostic = (XrXirSourceDiagnostic) {0};
    return true;
}
static void source_nominal_substitution_failures(void) {
    size_t sites = allocation_case_begin(13);
    for(size_t attempt=allocation_first();allocation_more(attempt,sites);attempt=allocation_next(attempt)) {
        SourceContext ctx = {0};
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = attempt ? attempt - 1 : SIZE_MAX; source_fixture_compile_injected=false;
        bool ok = allocation_private_context(&ctx,allocation_limits()) && source_nominal_substitution_case(&ctx);
        if (!attempt) { CHECK(ok); sites = source_fixture_compile_attempts; }
        else CHECK(!ok && ctx.diagnostic.status == XR_XIR_OUT_OF_MEMORY);
        allocation_private_close(&ctx);
        CHECK(!source_fixture_compile_live);
            if(attempt)allocation_point(attempt-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false;
    printf("Source nominal identity and substitution: %zu OOM sites\n", sites);
}
static void method_promise_allocations(void) {
    char directory[] = "xir-method-promises-XXXXXX", absolute[4096], path[8192];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    CHECK(snprintf(path, sizeof(path), "%s/root.xr", absolute) > 0);
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fputs("struct Box<T>{value:T;get()->T where T:Sendable{return this.value};"
        "call(f:fn()->i64=fn()->i64{return 31})->i64{return f()}}\n"
        "fn invoke(f:fn()->i64)->i64{return f()}\n"
        "print(invoke(Box<i64>{value:23}.get),Box<i64>{value:29}.call())\n", file) >= 0);
    CHECK(fclose(file) == 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {NULL, path, &authority, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    char manifest_path[8192];
    CHECK(snprintf(manifest_path, sizeof(manifest_path), "%s/xray.toml", absolute) > 0);
    file = fopen(manifest_path, "wb"); CHECK(file);
    CHECK(fputs("[declarations]\nversion=1\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nowner=\"Box\"\nname=\"get\"\nno_suspend=true\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nowner=\"Box\"\nname=\"call\"\nno_suspend=true\n"
        "no_suspend_parameters=[\"f\"]\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nname=\"invoke\"\nno_suspend_parameters=[\"f\"]\n", file) >= 0);
    CHECK(fclose(file) == 0);
    size_t sites = allocation_case_begin(19);
    for(size_t site=allocation_first();allocation_more(site,sites);site=allocation_next(site)) {
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = site ? site - 1 : SIZE_MAX; source_fixture_compile_injected=false;
        XrXirSourceResult result = {0};
        XrXirStatus status = allocation_source_check(&request, &result, NULL);
        if (!site) { CHECK(status == XR_XIR_OK && result.checked && result.snapshot); sites = source_fixture_compile_attempts; }
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && !result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result); CHECK(!source_fixture_compile_live);
            if(site)allocation_point(site-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false;
    CHECK(xr_test_unlink(manifest_path) == 0 && xr_test_unlink(path) == 0 && xr_test_rmdir(directory) == 0);
    printf("Method promise source ownership: %zu OOM sites; no partial publication\n", sites);
}
#include "xir_constraint_query_allocations.h"
#include "xir_implementation_query_allocations.h"
#include "xir_generic_method_query_allocations.h"
#include "xir_source_requirement_value_allocations.h"
#include "xir_source_enum_identity_allocations.h"
#include "xir_source_class_allocations.h"
#include "xir_source_iteration_allocations.h"
#include "xir_source_inference_allocations.h"
#include "xir_source_plan_budget_cases.h"
#include "xir_source_class_array_allocations.h"
#include "xir_source_region_output_cases.h"
#include "xir_query_coalloc_cases.h"
#include "xir_call_storage_cases.h"
static void source_root_allocations(void) {
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_SOURCE_FIXTURES};
    XrXirSourceRequest request = {NULL, XR_SOURCE_FIXTURES "/root.xr", &authority, NULL, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *artifact = NULL;
    size_t count=allocation_case_begin(17);
    if(allocation_mode!=2){
    source_fixture_compile_attempts=0;
    XrXirSourceResult query_result_1 = {0};
    XrXirStatus query_status_1 = allocation_source_check(&request, &query_result_1, NULL);
    CHECK(query_result_1.checked && query_result_1.snapshot && source_fixture_compile_live);
    artifact = query_result_1.checked; query_result_1.checked = NULL;
    xr_xir_compile_source_result_free(&query_result_1);
    CHECK(query_status_1 == XR_XIR_OK && artifact && source_fixture_compile_live);
    xr_xir_compile_artifact_free(artifact); CHECK(!source_fixture_compile_live);
    count = source_fixture_compile_attempts;
    }
    printf("Root complete compiler pipeline: %zu actual failure sites\n",count);
    for(size_t i=allocation_mode==2?allocation_index:0;allocation_mode!=1 && i<count;i+=allocation_mode==2?ALLOCATION_WORKERS:1) {
        source_fixture_compile_fail_at = i; source_fixture_compile_injected=false; source_fixture_compile_attempts = 0; artifact = NULL;
        XrXirSourceResult query_result_2 = {0};
        XrXirStatus query_status_2 = allocation_source_check(&request, &query_result_2, NULL);
        CHECK(!query_result_2.checked && !query_result_2.snapshot);
        artifact = query_result_2.checked; query_result_2.checked = NULL;
        xr_xir_compile_source_result_free(&query_result_2);
        CHECK(query_status_2 == XR_XIR_OUT_OF_MEMORY && !artifact && !source_fixture_compile_live);
        allocation_point(i,query_status_2,!artifact);
    }
    allocation_case_end(count);
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false;
    if(allocation_mode!=2){allocation_case_begin(32);
    for (resolver_fault = 1; resolver_fault <= 6; ++resolver_fault) {
        XrXirSourceResult result = {0};
        CHECK(allocation_source_check(&request, &result, NULL) == XR_XIR_BAD_STRUCTURE);
        CHECK(!result.checked && !result.snapshot && !source_fixture_compile_live);
    }
    resolver_fault = 0;allocation_case_end(0);}
    printf("Root complete compiler failures and resolver6: PASS; physical=0/0\n");
}
int main(int argc,char **argv) {
    allocation_dispatch(argc,argv);(void)source_fixture_lower;
    CHECK(setvbuf(stdout,NULL,_IONBF,0)==0);
    if(allocation_mode!=2){allocation_case_begin(0);query_coalloc_boundaries();allocation_case_end(0);}
    if(allocation_mode!=2){allocation_case_begin(1);query_coalloc_early_boundaries();allocation_case_end(0);}
    if(allocation_mode!=2){allocation_case_begin(2);source_call_storage_cases();allocation_case_end(0);}
    if(allocation_mode!=2){allocation_case_begin(3);source_region_output_cases();allocation_case_end(0);}
    if(allocation_mode!=2){allocation_case_begin(4);source_plan_context_cases();allocation_case_end(0);}
    if(allocation_mode!=2){allocation_case_begin(5);source_plan_conversion_budget_cases();allocation_case_end(0);}
    generic_method_query_allocations();
    implementation_query_allocations();
    constraint_query_allocations();
    snapshot_interface_allocations();
    snapshot_enum_allocations();
    source_nominal_substitution_failures();
    snapshot_nominal_allocations();
    snapshot_type_allocations();
    source_root_allocations();
    array_source_allocations();
    method_promise_allocations();
    source_requirement_value_allocations();
    source_enum_identity_allocations();
    source_class_allocations();
    source_class_array_allocations();
    source_iteration_allocations();
    inference_source_allocations();
    inference_source_allocation_fixture(31,"Unit result context producer",
        "var calls=0\nfn nop(){calls+=1}\nfn forwarded(){return nop()}\nexport fn answer()->i64{forwarded();return calls+40}\n");
    allocation_complete();
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    return 0;
}
