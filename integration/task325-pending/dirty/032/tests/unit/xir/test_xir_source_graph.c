/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_graph.c - Exact caller syntax, ownership and admission
 */
#include "base/xmalloc.h"
#include "../test_win_compat.h"
#include "module/xmodule_graph.h"
#include "frontend/parser/xast.h"
#include "program/xr_xir_source_product.h"
#include "api/xsource_compiler.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_stdlib_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct GraphAllocation { void *pointer; size_t bytes; } GraphAllocation;
static GraphAllocation allocations[65536];
static size_t attempts, fail_at, live, live_bytes;
static size_t allocation_slot(const void *pointer) {
    uintptr_t value = (uintptr_t)pointer >> 4;
    return (size_t)((value ^ (value >> 17)) & 65535);
}
static void *graph_malloc(size_t bytes) {
    if (++attempts == fail_at) return NULL;
    void *pointer = xr_malloc(bytes);
    if (!pointer) return NULL;
    size_t slot = allocation_slot(pointer);
    while (allocations[slot].pointer) slot = (slot + 1) & 65535;
    allocations[slot] = (GraphAllocation){pointer, bytes};
    ++live; live_bytes += bytes;
    return pointer;
}
static void graph_free(void *pointer) {
    if (!pointer) return;
    size_t hole = allocation_slot(pointer);
    while (allocations[hole].pointer != pointer) {
        CHECK(allocations[hole].pointer); hole = (hole + 1) & 65535;
    }
    CHECK(live && live_bytes >= allocations[hole].bytes);
    --live; live_bytes -= allocations[hole].bytes;
    for (size_t next = (hole + 1) & 65535; allocations[next].pointer; next = (next + 1) & 65535) {
        size_t home = allocation_slot(allocations[next].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { allocations[hole] = allocations[next]; hole = next; }
    }
    allocations[hole] = (GraphAllocation){0}; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) graph_malloc(bytes)
#define xr_free(pointer) graph_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

typedef struct GraphOwner {
    XrXirCompileContext context;
    XrCompilerSession *session;
    XrModuleResolver *resolver;
    XrModuleGraph *graph;
    const XrXirLibraryCatalog *catalog;
    XrXirLibraryCatalog *owned_catalog;
    XrSourceCompiler *compiler;
    XrModuleIdentityAuthority authority;
    uint64_t ledger_live;
} GraphOwner;
static const char *stdlib_root;
static XrCompileResourceLimits generous(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
}
static void open_graph(GraphOwner *owner, XrCompileResourceLimits limits, unsigned mode) {
    bool library = mode != 0;
    memset(owner, 0, sizeof(*owner));
    owner->context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits, &owner->context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial = {0};
    CHECK(xr_compile_resources_stats(owner->context.resources, &initial) == XR_COMPILE_RESOURCE_OK);
    owner->ledger_live = initial.live_bytes;
    if (mode == 2) {
        XrSourceCompilerConfig config = {owner->context, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
            stdlib_root, xr_source_compile_default_manifest_limits()};
        CHECK(xr_source_compiler_new(&config, &owner->compiler, NULL) == XR_SOURCE_COMPILE_OK);
        owner->session = xr_source_compiler_session(owner->compiler);
        owner->catalog = xr_source_compiler_libraries(owner->compiler);
    } else {
        CHECK(xr_compile_session_new(owner->context.resources, &owner->session) == XR_COMPILER_SESSION_OK);
        if (library) CHECK(xir_stdlib_library_catalog_new(&owner->context, stdlib_root, &owner->owned_catalog) == XR_XIR_OK);
        owner->catalog = owner->owned_catalog;
    }
    XrModuleResolverConfig config = {library ? stdlib_root : NULL, NULL, owner->catalog};
    CHECK(xr_compile_module_resolver_new(owner->context.resources, &config, &owner->resolver) == XR_MODULE_OK);
    CHECK(xr_compile_module_graph_new(owner->context.resources, owner->session, owner->resolver, &owner->graph) == XR_MODULE_OK);
    owner->authority = (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_MEMORY, "caller-ast", NULL};
    const char *source = library ?
        "import \"std/io/output\" as output\nfn identity<T>(value:T)->T { return value }\n"
        "export fn answer()->i64 { return identity(41) }\n" :
        "export fn answer()->i64 { return 41 }\n";
    char *error = NULL;
    XrModuleStatus built = xr_compile_module_graph_build_source(owner->graph, &owner->authority, source, &error);
    if (built != XR_MODULE_OK) fprintf(stderr, "graph input Catalog%u status%u %s\n", library, built, error ? error : "");
    xr_compile_resources_free(error); CHECK(built == XR_MODULE_OK);
    AstNode *program = owner->graph->specs[owner->graph->entry_index].ast;
    CHECK(program && program->type == AST_PROGRAM && program->as.program.count);
    AstNode *function = program->as.program.statements[program->as.program.count - 1];
    CHECK(function->type == AST_FUNCTION_DECL);
    AstNode *body = function->as.function_decl.body;
    CHECK(body->type == AST_BLOCK && body->as.block.count == 1);
    AstNode *statement = body->as.block.statements[0];
    CHECK(statement->type == AST_RETURN_STMT && statement->as.return_stmt.value_count == 1);
    AstNode **slot = &statement->as.return_stmt.values[0];
    if (library) {
        CHECK((*slot)->type == AST_CALL_EXPR && (*slot)->as.call_expr.arg_count == 1);
        slot = &(*slot)->as.call_expr.arguments[0];
    }
    AstNode *literal = *slot;
    CHECK(literal->type == AST_LITERAL_INT && literal->as.literal.int_bits == 41);
    XrCompilerSessionScope scope = {0};
    CHECK(xr_compile_session_push_arena(owner->session, program->as.program.arena, &scope) == XR_COMPILE_RESOURCE_OK);
    AstNode *replacement = xr_ast_literal_int(owner->session, 73, literal->line);
    CHECK(replacement);
    *slot = replacement;
    CHECK(xr_compile_session_pop_arena(&scope) == XR_COMPILE_RESOURCE_OK);
}
static void close_inputs(GraphOwner *owner) {
    xr_compile_module_graph_free(owner->graph); owner->graph = NULL;
    xr_compile_module_resolver_free(owner->resolver); owner->resolver = NULL;
    if (owner->compiler) xr_source_compiler_free(owner->compiler);
    else {
        xr_compile_session_free(owner->session);
        xr_xir_compile_library_catalog_free(owner->owned_catalog);
    }
    owner->session = NULL; owner->catalog = NULL; owner->owned_catalog = NULL; owner->compiler = NULL;
}
static XrXirSourceProductRequest product_request(GraphOwner *owner) {
    return (XrXirSourceProductRequest){
        {owner->session, NULL, &owner->authority, &owner->context,
            owner->catalog ? stdlib_root : NULL, NULL, XR_XIR_PROGRAM, owner->catalog},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
}
static XrCompileResourceStats graph_stats(const GraphOwner *owner) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(owner->context.resources, &result) == XR_COMPILE_RESOURCE_OK);
    return result;
}
static void execute_after_inputs_die(GraphOwner *owner, XrXirSourceProduct *product) {
    close_inputs(owner);
    XrXirSourceProductPacketView packet = {0}; XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner->context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    uint32_t answer = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].name_length == 6 && !memcmp(module->functions[f].name, "answer", 6)) answer = f;
    CHECK(answer != UINT32_MAX);
    xr_xir_compile_artifact_free(checked);
    uint32_t entry = xr_xir_compile_source_product_facts(product)->entry;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product, &program) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    uint32_t functions[] = {entry, answer};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instance, functions[i], NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, 65536).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == (i ? 73u : 0u));
        xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
}
typedef struct GraphProbe { int status; XrCompileResourceStats stats; size_t sites; } GraphProbe;
static GraphProbe probe(XrCompileResourceLimits limits, size_t fault, unsigned mode, bool execute) {
    GraphOwner owner; open_graph(&owner, limits, mode);
    XrXirSourceProductRequest request = product_request(&owner);
    AstNode *ast = owner.graph->specs[owner.graph->entry_index].ast;
    int count = owner.graph->spec_count;
    size_t before = attempts; fail_at = fault ? before + fault : 0;
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    XrSourceCompileDiagnostic owner_diagnostic = {0};
    int status = owner.compiler ?
        (int)xr_source_compiler_build_graph(owner.compiler, owner.graph, &product, &owner_diagnostic) :
        (int)xr_xir_compile_source_product_build_graph(&request, owner.graph, &product, &diagnostic);
    int ok = mode == 2 ? (int)XR_SOURCE_COMPILE_OK : (int)XR_XIR_OK;
    int budget = mode == 2 ? (int)XR_SOURCE_COMPILE_BUDGET : (int)XR_XIR_BUDGET;
    int oom = mode == 2 ? (int)XR_SOURCE_COMPILE_OUT_OF_MEMORY : (int)XR_XIR_OUT_OF_MEMORY;
    GraphProbe result = {status, graph_stats(&owner), attempts - before}; fail_at = 0;
    CHECK(owner.graph->spec_count == count && owner.graph->specs[owner.graph->entry_index].ast == ast);
    CHECK(!owner.graph->specs[owner.graph->entry_index].ast_borrowed);
    if (status != ok) CHECK(!product);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_source_compile_diagnostic_free(&owner_diagnostic);
    if (status == budget || (mode == 2 && status == oom)) {
        XrCompileResourceStats previous = graph_stats(&owner); size_t sites = attempts;
        int retried = owner.compiler ?
            (int)xr_source_compiler_build_graph(owner.compiler, owner.graph, &product, NULL) :
            (int)xr_xir_compile_source_product_build_graph(&request, owner.graph, &product, NULL);
        CHECK(retried == status);
        XrCompileResourceStats after = graph_stats(&owner);
        CHECK(!product && attempts == sites && !memcmp(&previous, &after, sizeof(after)));
    }
    if (execute && status == ok) execute_after_inputs_die(&owner, product);
    else { xr_xir_compile_source_product_free(product); close_inputs(&owner); }
    /* The still-retained ledger allocation is itself charged until release. */
    CHECK(graph_stats(&owner).live_bytes == owner.ledger_live);
    xr_compile_resources_release(owner.context.resources);
    CHECK(!live && !live_bytes);
    return result;
}
static void rejections(void) {
    GraphOwner owner; open_graph(&owner, generous(), false);
    XrXirSourceProductRequest request = product_request(&owner);
    XrXirSourceResult occupied = {.checked = (XrXirArtifact *)(uintptr_t)17};
    XrCompileResourceStats before = graph_stats(&owner); size_t sites = attempts;
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &occupied, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied.checked == (XrXirArtifact *)(uintptr_t)17);
    CHECK(xr_xir_compile_source_check_graph(&request.source, NULL, &occupied, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after = graph_stats(&owner);
    CHECK(attempts == sites && !memcmp(&before, &after, sizeof(after)));
    XrXirSourceProduct *empty_product = NULL;
    XrXirSourceProductDiagnostic busy = {.snapshot = (XrXirSourceSnapshot *)(uintptr_t)17};
    XrXirSourceProductDiagnostic saved_busy = busy;
    CHECK(xr_xir_compile_source_product_build_graph(&request, NULL, &empty_product, &busy) == XR_XIR_BAD_STRUCTURE);
    CHECK(!empty_product && !memcmp(&busy, &saved_busy, sizeof(busy)));
    XrXirSourceResult result = {0};
    struct XrLockfile *lockfile = owner.resolver->config.lockfile;
    owner.resolver->config.lockfile = (struct XrLockfile *)(uintptr_t)17;
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    owner.resolver->config.lockfile = lockfile;
    after = graph_stats(&owner); CHECK(attempts == sites && !memcmp(&before, &after, sizeof(after)));
    const char *stdlib = owner.resolver->config.stdlib_path;
    owner.resolver->config.stdlib_path = "foreign";
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot); owner.resolver->config.stdlib_path = stdlib;
    XrModuleIdentityAuthority foreign = owner.authority; foreign.namespace_id = "foreign";
    request.source.authority = &foreign;
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot); request.source.authority = &owner.authority;
    XrModuleSpec *entry = &owner.graph->specs[owner.graph->entry_index];
    char *canonical = entry->canonical; entry->canonical = "forged";
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot); entry->canonical = canonical;
    char *source_path = entry->source_path; entry->source_path = "C:/foreign.xr";
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot); entry->source_path = source_path;
    GraphOwner other; open_graph(&other, generous(), false);
    before = graph_stats(&owner); sites = attempts;
    CHECK(xr_xir_compile_source_check_graph(&request.source, other.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    after = graph_stats(&owner); CHECK(attempts == sites && !memcmp(&before, &after, sizeof(after)));
    AstNode *syntax = entry->ast; entry->ast = other.graph->specs[other.graph->entry_index].ast;
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot); entry->ast = syntax;
    close_inputs(&other); xr_compile_resources_release(other.context.resources);
    int syntax_count = entry->ast->as.program.count;
    entry->ast->as.program.count = entry->ast->as.program.capacity + 1;
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot); entry->ast->as.program.count = syntax_count;
    int *saved = entry->dep_indices, saved_count = entry->dep_count;
    int saved_capacity = entry->dep_capacity, cycle = owner.graph->entry_index;
    entry->dep_indices = &cycle; entry->dep_count = entry->dep_capacity = 1;
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot);
    entry->dep_indices = saved; entry->dep_count = saved_count; entry->dep_capacity = saved_capacity;
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    close_inputs(&owner); xr_compile_resources_release(owner.context.resources); CHECK(!live && !live_bytes);
}
static void catalog_rejection(void) {
    GraphOwner owner; open_graph(&owner, generous(), true);
    XrModuleSpec *library = NULL;
    for (int i = 0; i < owner.graph->spec_count; ++i)
        if (owner.graph->specs[i].representation == XR_MODULE_CHECKED_LIBRARY) library = &owner.graph->specs[i];
    CHECK(library && library->resource);
    const XrModuleResourceBinding *original = library->resource;
    XrModuleResourceBinding forged = *original; library->resource = &forged;
    XrXirSourceProductRequest request = product_request(&owner); XrXirSourceResult result = {0};
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot); library->resource = original;
    CHECK(xr_xir_compile_source_check_graph(&request.source, owner.graph, &result, NULL, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    close_inputs(&owner); xr_compile_resources_release(owner.context.resources); CHECK(!live && !live_bytes);
}
static void resource_cases(bool all_faults) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        int ok = mode == 2 ? (int)XR_SOURCE_COMPILE_OK : (int)XR_XIR_OK;
        int budget = mode == 2 ? (int)XR_SOURCE_COMPILE_BUDGET : (int)XR_XIR_BUDGET;
        int oom = mode == 2 ? (int)XR_SOURCE_COMPILE_OUT_OF_MEMORY : (int)XR_XIR_OUT_OF_MEMORY;
        GraphProbe measured = probe(generous(), 0, mode, false);
        CHECK(measured.status == ok && measured.sites);
        for (size_t i = 1; all_faults && i <= measured.sites; ++i) {
            GraphProbe failed = probe(generous(), i, mode, false);
            CHECK(failed.status == oom);
        }
        XrCompileResourceLimits exact = {measured.stats.allocated_bytes, measured.stats.peak_bytes, measured.stats.work};
        CHECK(probe(exact, 0, mode, false).status == ok);
        for (unsigned axis = 0; axis < 3; ++axis) {
            XrCompileResourceLimits minus = exact;
            if (!axis) --minus.allocated_bytes;
            else if (axis == 1) --minus.live_bytes;
            else --minus.work;
            CHECK(probe(minus, 0, mode, false).status == budget);
        }
        printf("caller AST ownerMode%u allocation sites%zu all_faults%s; three exact/minus-one axes; compiler physical0\n",
            mode, measured.sites, all_faults ? "true" : "false");
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2 || (argc == 3 && (!strcmp(argv[2], "--resources") || !strcmp(argv[2], "--bounds"))));
    char resolved[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(argv[1], resolved, sizeof(resolved))); stdlib_root = resolved;
    if (argc == 3) resource_cases(!strcmp(argv[2], "--resources"));
    else {
        rejections(); catalog_rejection();
        CHECK(probe(generous(), 0, false, true).status == XR_XIR_OK);
        CHECK(probe(generous(), 0, true, true).status == XR_XIR_OK);
        CHECK(probe(generous(), 0, 2, true).status == XR_SOURCE_COMPILE_OK);
    }
    CHECK(!live && !live_bytes); puts("exact caller AST SourceProduct PASS"); return 0;
}
