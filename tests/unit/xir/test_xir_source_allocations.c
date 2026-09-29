/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_allocations.c - Fail each source-owner metadata allocation
 *
 * KEY CONCEPT:
 *   Parsed graph allocation is separate from the counted XIR producer boundary.
 */
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include "module/xmodule_resolver.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at = SIZE_MAX, live;
static unsigned resolver_fault;
static void **owned;
static size_t owned_capacity;
static void *source_counted_calloc(size_t count, size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *pointer = xr_calloc(count, size);
    if (pointer) {
        if (live == owned_capacity) {
            CHECK(owned_capacity <= SIZE_MAX / 2 / sizeof(*owned));
            size_t capacity = owned_capacity ? owned_capacity * 2 : 256;
            void **grown = xr_realloc(owned, capacity * sizeof(*owned));
            CHECK(grown); owned = grown; owned_capacity = capacity;
        }
        owned[live++] = pointer;
    }
    return pointer;
}
static void source_counted_free(void *pointer) {
    for (size_t i = 0; i < live; ++i) if (owned[i] == pointer) {
        owned[i] = owned[--live]; break;
    }
    xr_free(pointer);
}
static int source_fault_resolve(XrModuleResolver *resolver, const char *specifier,
    const char *importer, const XrModuleIdentityAuthority *authority, XrModuleId *id, char **error) {
    if (resolver_fault == 1) return 0;
    if (resolver_fault == 2) return -1;
    int status = xr_module_resolver_resolve(resolver, specifier, importer, authority, id, error);
    char **field = NULL;
    if (resolver_fault == 3) field = &id->canonical;
    if (resolver_fault == 4) field = &id->logical_path;
    if (resolver_fault == 5) {
        CHECK(status == 0 && id->authority.physical_root);
        xr_free((char *)id->authority.physical_root);
        id->authority.physical_root = NULL;
    }
    if (resolver_fault == 6) field = &id->source_path;
    if (field) { CHECK(status == 0 && *field); xr_free(*field); *field = NULL; }
    return status;
}
#undef xr_calloc
#undef xr_free
#define xr_calloc(count, size) source_counted_calloc(count, size)
#define xr_free(pointer) source_counted_free(pointer)
#include "xir/xxir_source_query.c"
#define xr_module_resolver_resolve source_fault_resolve
#include "xir/xxir_source.c"
#undef xr_module_resolver_resolve
#include "xir_nominal_fixture.h"
static void snapshot_nominal_allocations(void) {
    for (unsigned declaration_only = 0; declaration_only < 2; ++declaration_only) {
        size_t sites = 0;
        for (size_t attempt = 0; attempt <= sites; ++attempt) {
            NominalFixture fixture; nominal_fixture(&fixture);
            XrXirType argument = XR_XIR_I64, fields[] = {XR_XIR_I64, XR_XIR_STRING};
            XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
            node.nominal = (XrXirNominalType) {0, &argument, 1, fields, 2};
            XrXirTypes types = {declaration_only ? NULL : &node, declaration_only ? 0 : 1, &fixture.table};
            XrXirSourceView view = {0}; view.types = &types; view.diagnostic.status = XR_XIR_BAD_TYPE;
            XrXirBudget budget = xr_xir_default_budget();
            attempts = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
            XrXirSourceSnapshot *snapshot = NULL;
            XrXirStatus status = xr_xir_source_snapshot_copy(&view, &budget, &snapshot);
            if (!attempt) {
                CHECK(status == XR_XIR_OK && snapshot); sites = attempts;
                XrXirBudget required = xr_xir_default_budget();
                required.work -= budget.work; required.metadata_bytes -= budget.metadata_bytes;
                for (unsigned kind = 0; kind < 3; ++kind) {
                    XrXirBudget limit = required;
                    if (kind == 0) --limit.work;
                    if (kind == 1) --limit.metadata_bytes;
                    XrXirSourceSnapshot *bounded = NULL;
                    XrXirStatus bounded_status = xr_xir_source_snapshot_copy(&view, &limit, &bounded);
                    CHECK(bounded_status == (kind == 2 ? XR_XIR_OK : XR_XIR_BUDGET));
                    CHECK((bounded != NULL) == (kind == 2));
                    xr_xir_source_snapshot_free(bounded);
                }
                memset(&fixture, 0xcc, sizeof(fixture)); memset(&node, 0xcc, sizeof(node));
                argument = XR_XIR_UNIT; fields[0] = fields[1] = XR_XIR_UNIT;
                const XrXirSourceView *copy = xr_xir_source_snapshot_view(snapshot);
                CHECK(!copy->complete && copy->diagnostic.status == XR_XIR_BAD_TYPE);
                CHECK(copy->types->count == (declaration_only ? 0u : 1u));
                const XrXirNominalTable *table = copy->types->nominals;
                CHECK(table && table->count == 2 && !table->identities);
                const XrXirNominalDeclaration *decl = table->declarations;
                CHECK(decl[0].module.length == 5 && !memcmp(decl[0].module.bytes, "alpha", 5));
                CHECK(decl[0].name.length == 4 && !memcmp(decl[0].name.bytes, "Pair", 4));
                CHECK(decl[0].constraints[0] == XR_XIR_CONSTRAINT_SENDABLE);
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
            xr_xir_source_snapshot_free(snapshot); CHECK(!live);
        }
        fail_at = SIZE_MAX;
        printf("Nominal query snapshot (%u): %zu allocation failure sites\n", declaration_only, sites);
    }
}
static void snapshot_type_allocations(void) {
    const XrXirCallableParameter parameter = {XR_XIR_I64, 0};
    const XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, &parameter, 1, XR_XIR_STRING, 0, 0, {0}},
        {XR_XIR_TYPE_ARRAY, XR_XIR_STRING, NULL, 0, XR_XIR_UNIT, 0, 0, {0}},
        {XR_XIR_TYPE_CELL, (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE, NULL, 0, XR_XIR_UNIT, 0, 0, {0}}
    };
    const XrXirTypes types = {nodes, 3, NULL};
    XrXirSourceView view = {0}; view.types = &types;
    view.diagnostic.status = XR_XIR_BAD_TYPE;
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_types_verify(&types, &budget) == XR_XIR_OK);
    size_t sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        attempts = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
        budget = xr_xir_default_budget();
        XrXirSourceSnapshot *snapshot = NULL;
        XrXirStatus status = xr_xir_source_snapshot_copy(&view, &budget, &snapshot);
        if (!attempt) {
            CHECK(status == XR_XIR_OK && snapshot); sites = attempts;
            const XrXirSourceView *copy = xr_xir_source_snapshot_view(snapshot);
            CHECK(!copy->complete && copy->diagnostic.status == XR_XIR_BAD_TYPE);
            CHECK(copy->types && copy->types != &types && copy->types->nodes != nodes);
            CHECK(copy->types->count == 3 && copy->types->nodes[0].parameters != &parameter);
            CHECK(copy->types->nodes[0].parameters[0].type == XR_XIR_I64);
            CHECK(copy->types->nodes[1].kind == XR_XIR_TYPE_ARRAY && copy->types->nodes[1].element == XR_XIR_STRING);
            CHECK(copy->types->nodes[2].kind == XR_XIR_TYPE_CELL && copy->types->nodes[2].element == XR_XIR_CONSTRUCTED_TYPE_BASE);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
        xr_xir_source_snapshot_free(snapshot); CHECK(!live);
    }
    fail_at = SIZE_MAX; attempts = 0;
    printf("Constructed query snapshot: %zu allocation failure sites; no partial snapshot\n", sites);
}
static void array_source_allocations(XrCompilerSession *session) {
    char directory[XR_TEST_PATH_MAX] = "xir-array-allocation-XXXXXX", absolute[XR_TEST_PATH_MAX], path[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    CHECK(snprintf(path, sizeof(path), "%s/root.xr", absolute) > 0);
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fputs("fn first<T>(a:Array<T>)->T{return a[0]}\nvar a=[\"x\"]\na.push(a[0])\na.set(0,\"y\")\n"
        "print(len(a),first<string>(a),a.get(1))\n", file) >= 0 && fclose(file) == 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {session, path, &authority, NULL, NULL, NULL};
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        attempts = 0; fail_at = site ? site - 1 : SIZE_MAX;
        XrXirSourceResult result = {0};
        XrXirStatus status = xr_xir_source_check(&request, &result, NULL);
        if (!site) { CHECK(status == XR_XIR_OK && result.checked && result.snapshot); sites = attempts; }
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && !result.checked && !result.snapshot);
        xr_xir_source_result_free(&result); CHECK(!live);
    }
    fail_at = SIZE_MAX;
    for (unsigned kind = 0; kind < 2; ++kind) {
        XrXirBudget budget = xr_xir_default_budget();
        if (kind) budget.work = 100; else budget.metadata_bytes = 128;
        request.budget = &budget;
        XrXirSourceResult result = {0};
        CHECK(xr_xir_source_check(&request, &result, NULL) == XR_XIR_BUDGET);
        CHECK(!result.checked && !result.snapshot && !live);
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
    if (!source_struct_field(ctx, NULL, outer->nominal.arguments[0], "first", true, &field_index, &field_type)) return false;
    CHECK(field_type == XR_XIR_I64);
    CHECK(!source_struct_field(ctx, NULL, outer->nominal.arguments[0], "second", true, &field_index, &field_type));
    CHECK(ctx->diagnostic.status == XR_XIR_BAD_TYPE && ctx->query.reference_count == references + 1);
    ctx->diagnostic = (XrXirSourceDiagnostic) {0};
    return true;
}
static void source_nominal_substitution_failures(void) {
    size_t sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        SourceContext ctx = {0}; ctx.budget = xr_xir_default_budget();
        attempts = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
        bool ok = source_nominal_substitution_case(&ctx);
        if (!attempt) { CHECK(ok); sites = attempts; }
        else CHECK(!ok && ctx.diagnostic.status == XR_XIR_OUT_OF_MEMORY);
        while (ctx.memory) { SourceMemory *next = ctx.memory->next; xr_free(ctx.memory); ctx.memory = next; }
        CHECK(!live);
    }
    fail_at = SIZE_MAX;
    printf("Source nominal identity and substitution: %zu OOM sites\n", sites);
}
int main(void) {
    source_nominal_substitution_failures();
    snapshot_nominal_allocations();
    snapshot_type_allocations();
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_SOURCE_FIXTURES "/root.xr", &authority, NULL, XR_SOURCE_STDLIB, NULL};
    XrXirArtifact *artifact = NULL;
    XrXirSourceResult query_result_1 = {0};
    XrXirStatus query_status_1 = xr_xir_source_check(&request, &query_result_1, NULL);
    CHECK(query_result_1.checked && query_result_1.snapshot && live);
    artifact = query_result_1.checked; query_result_1.checked = NULL;
    xr_xir_source_result_free(&query_result_1);
    CHECK(query_status_1 == XR_XIR_OK && artifact && !live);
    xr_xir_artifact_free(artifact);
    size_t count = attempts;
    for (size_t i = 0; i < count; ++i) {
        fail_at = i; attempts = 0; artifact = NULL;
        XrXirSourceResult query_result_2 = {0};
        XrXirStatus query_status_2 = xr_xir_source_check(&request, &query_result_2, NULL);
        CHECK(!query_result_2.checked && !query_result_2.snapshot);
        artifact = query_result_2.checked; query_result_2.checked = NULL;
        xr_xir_source_result_free(&query_result_2);
        CHECK(query_status_2 == XR_XIR_OUT_OF_MEMORY && !artifact && !live);
    }
    fail_at = SIZE_MAX;
    for (resolver_fault = 1; resolver_fault <= 6; ++resolver_fault) {
        XrXirSourceResult result = {0};
        CHECK(xr_xir_source_check(&request, &result, NULL) == XR_XIR_BAD_STRUCTURE);
        CHECK(!result.checked && !result.snapshot && !live);
    }
    resolver_fault = 0;
    array_source_allocations(session);
    xr_compiler_session_delete(session);
    printf("Source-owner allocation failures: %zu; no partial artifact or live metadata\n", count);
    CHECK(!live); xr_free(owned); owned = NULL; owned_capacity = 0;
    return 0;
}
