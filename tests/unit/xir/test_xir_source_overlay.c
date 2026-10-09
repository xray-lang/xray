/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "module/xmodule_graph.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"
static XrCompileResourceStats overlay_source_stats;
static XrCompileResourceLimits overlay_source_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
}
static void overlay_source_zero(void) { CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes); }
static XrXirStatus overlay_source_check(const char *root, unsigned mode,
    XrCompileResourceLimits limits, XrXirSourceResult *output) {
    XrXirCompileContext context = {0}; XrCompilerSession *session = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_new(&limits, &context.resources);
    if (allocated != XR_COMPILE_RESOURCE_OK)
        return allocated == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSessionStatus created = xr_compile_session_new(context.resources, &session);
    XrXirStatus status = created == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    char names[3][16] = {"root.xr", "lib.xr", "leaf.xr"}, paths[3][XR_TEST_PATH_MAX];
    char texts[3][192] = {
        "import { value } from \"./lib\"\nexport fn result()->i64 { return value() }\n",
        "import { leaf } from \"./sub/../leaf\"\nexport fn value()->i64 { return leaf() }\n",
        "export fn leaf()->i64 { return 41 }\n"};
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrModuleOverlayInput inputs[3];
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(snprintf(paths[i], sizeof(paths[i]), "%s/%s", root, names[i]) > 0);
        inputs[i] = (XrModuleOverlayInput){authority, names[i], paths[i], texts[i], strlen(texts[i])};
    }
    size_t count = 3;
    if (mode == 1) { strcpy(texts[1], "fn value()->i64 { return 1 }\n"); inputs[1].length = strlen(texts[1]); }
    if (mode == 2) { strcpy(texts[2], "export fn leaf()->i64 { return \"bad\" }\n"); inputs[2].length = strlen(texts[2]); }
    if (mode == 3) count = 2;
    if (mode == 4) inputs[2] = inputs[1];
    if (mode == 5) {
        strcpy(texts[2], "import { value } from \"./lib\"\nexport fn leaf()->i64 { return value() }\n");
        inputs[2].length = strlen(texts[2]);
    }
    if (mode == 6) {
        strcpy(texts[0], "import { leaf } from \"./../../leaf\"\nexport fn result()->i64 { return leaf() }\n");
        inputs[0].length = strlen(texts[0]);
    }
    if (status == XR_XIR_OK) {
        XrXirSourceRequest request = {session, paths[0], &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
        XrXirSourceDiagnostic diagnostic = {0};
        status = xr_xir_compile_source_check_overlay(&request, inputs, count, output, &diagnostic, NULL);
        CHECK(diagnostic.status == status);
    }
    memset(texts, '?', sizeof(texts)); memset(paths, '?', sizeof(paths)); memset(names, '?', sizeof(names));
    memset(inputs, 0, sizeof(inputs));
    xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(context.resources, &overlay_source_stats) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources); return status;
}
static const XrXirSourceDeclaration *overlay_function(const XrXirSourceView *view, const char *name) {
    const XrXirSourceDeclaration *found = NULL;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *declaration = &view->declarations[i];
        if (declaration->kind == XR_XIR_SOURCE_FUNCTION && !strcmp(declaration->name, name)) {
            CHECK(!found); found = declaration;
        }
    }
    CHECK(found && found->exported && !found->parent && found->type.known && found->type.type == XR_XIR_I64);
    return found;
}
static void overlay_source_facts(const XrXirSourceResult *result) {
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(view && view->complete && view->diagnostic.status == XR_XIR_OK);
    const XrXirSourceDeclaration *entry = overlay_function(view, "result");
    const XrXirSourceDeclaration *value = overlay_function(view, "value"), *leaf = overlay_function(view, "leaf");
    CHECK(entry->range.module != value->range.module && value->range.module != leaf->range.module);
    unsigned calls = 0;
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        const XrXirSourceReference *reference = &view->references[i];
        if (reference->access != XR_XIR_SOURCE_CALL || (reference->target != value->id && reference->target != leaf->id)) continue;
        CHECK(reference->declaration && reference->declaration <= view->declaration_count);
        const XrXirSourceDeclaration *binding = &view->declarations[reference->declaration-1];
        CHECK(binding->kind == XR_XIR_SOURCE_IMPORT && binding->target == reference->target);
        CHECK(reference->range.line == 2);
        CHECK(reference->range.module == (reference->target == value->id ? entry->range.module : value->range.module));
        ++calls;
    }
    CHECK(calls == 2);
}
static void overlay_source_failures(const char *root) {
    size_t sites = 0; XrCompileResourceStats required = {0};
    for (size_t probe = 0; probe <= sites; ++probe) {
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = probe ? probe-1 : SIZE_MAX;
        source_fixture_compile_injected = false;
        XrXirSourceResult result = {0}; XrXirStatus status = overlay_source_check(root, 0, overlay_source_limits(), &result);
        if (!probe) {
            CHECK(status == XR_XIR_OK && result.checked && result.snapshot); sites = source_fixture_compile_attempts; CHECK(sites);
            required = overlay_source_stats; overlay_source_facts(&result);
        } else {
            CHECK(status == XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected);
            CHECK(!result.checked && !result.snapshot && source_fixture_compile_attempts > source_fixture_compile_fail_at);
        }
        xr_xir_compile_source_result_free(&result); overlay_source_zero();
    }
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected = false;
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = {required.allocated_bytes, required.peak_bytes, required.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound && *bound < UINT64_MAX); *bound = (uint64_t)((int64_t)*bound + delta);
        XrXirSourceResult result = {0};
        CHECK(overlay_source_check(root, 0, limits, &result) == (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK));
        if (delta < 0) CHECK(!result.checked && !result.snapshot); else overlay_source_facts(&result);
        xr_xir_compile_source_result_free(&result); overlay_source_zero();
    }
    printf("Three-document Source overlay fresh sites=%zu; three axes; physical=0/0\n", sites);
}
static void overlay_priority(const char *root) {
    char file[XR_TEST_PATH_MAX], index[XR_TEST_PATH_MAX], importer[XR_TEST_PATH_MAX];
    CHECK(snprintf(file, sizeof(file), "%s/pkg.xr", root) > 0);
    CHECK(snprintf(index, sizeof(index), "%s/pkg/index.xr", root) > 0);
    CHECK(snprintf(importer, sizeof(importer), "%s/root.xr", root) > 0);
    FILE *stream = fopen(file, "wb"); CHECK(stream); CHECK(fputs("disk\n", stream) >= 0 && fclose(stream) == 0);
    for (unsigned mode = 0; mode < 2; ++mode) {
        XrCompileResourceLimits limits = overlay_source_limits(); XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrModuleResolverConfig config = {0}; XrModuleResolver *resolver = NULL;
        CHECK(xr_compile_module_resolver_new(resources, &config, &resolver) == XR_MODULE_OK);
        XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
        XrModuleOverlayInput input = {authority, "pkg/index.xr", index, "", 0};
        CHECK(xr_compile_module_resolver_set_overlay(resolver, &input, 1) == XR_MODULE_OK);
        XrModuleId id = {0};
        CHECK(xr_compile_module_resolver_resolve(resolver, "./pkg", importer, &authority, &id, NULL) == XR_MODULE_OK);
        CHECK(!strcmp(id.logical_path, mode ? "pkg/index.xr" : "pkg.xr"));
        CHECK(xr_compile_module_resolver_set_overlay(resolver, &input, 1) == XR_MODULE_INVALID);
        xr_compile_module_id_cleanup(&id); xr_compile_module_resolver_free(resolver);
        xr_compile_resources_release(resources); overlay_source_zero();
        if (!mode) CHECK(xr_test_unlink(file) == 0);
    }
}
static void overlay_catalog_collision(const char *root) {
    XrCompileResourceLimits limits = overlay_source_limits(); XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits(); XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    char path[XR_TEST_PATH_MAX]; CHECK(snprintf(path, sizeof(path), "%s/lib.xr", root) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceRequest request = {session, path, &authority, &context, NULL, NULL, XR_XIR_LIBRARY, NULL};
    const char source[] = "export fn value()->i64 { return 41 }\n"; XrXirSourceResult result = {0};
    CHECK(xr_xir_compile_source_check_text(&request, &(XrXirSourceText){"lib.xr", source, sizeof(source)-1}, &result, NULL, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0}; CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    XrXirLibraryInput library = {0}; library.authority = authority; library.logical_path = "lib.xr";
    library.packet = packet.bytes; library.length = packet.length; xr_sha256(packet.bytes, packet.length, library.sha256);
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_compile_library_catalog_new(&context, &library, 1, &catalog) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrModuleResolverConfig config = {NULL, NULL, catalog}; XrModuleResolver *resolver = NULL;
    CHECK(xr_compile_module_resolver_new(context.resources, &config, &resolver) == XR_MODULE_OK);
    XrModuleOverlayInput input = {authority, "lib.xr", path, source, sizeof(source)-1};
    CHECK(xr_compile_module_resolver_set_overlay(resolver, &input, 1) == XR_MODULE_INVALID && !resolver->overlay);
    input.authority=(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_PROJECT,"editor-view",root};
    CHECK(xr_compile_module_resolver_set_overlay(resolver,&input,1)==XR_MODULE_INVALID&&!resolver->overlay);
    /* Renaming the editor's authority cannot bypass immutable Catalog text.
     * Both failed transactions leave the resolver empty and usable. */
    CHECK(xr_compile_module_resolver_set_overlay(resolver, NULL, 0) == XR_MODULE_OK);
    xr_compile_module_resolver_free(resolver); xr_xir_compile_library_catalog_free(catalog);
    xr_compile_resources_release(context.resources); overlay_source_zero();
}
static void overlay_memory_graph(void) {
    XrCompileResourceLimits limits = overlay_source_limits(); XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL; CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleResolverConfig config = {0}; XrModuleResolver *resolver = NULL;
    CHECK(xr_compile_module_resolver_new(resources, &config, &resolver) == XR_MODULE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "overlay", NULL};
    char text[] = "fn local()->i64 { return 1 }\n";
    XrModuleOverlayInput input = {authority, "", NULL, text, sizeof(text)-1};
    CHECK(xr_compile_module_resolver_set_overlay(resolver, &input, 1) == XR_MODULE_OK);
    memset(text, '?', sizeof(text));
    for (unsigned mode = 0; mode < 2; ++mode) {
        XrModuleGraph *graph = NULL; char *error = NULL;
        CHECK(xr_compile_module_graph_new(resources, session, resolver, &graph) == XR_MODULE_OK);
        const char *supplied = mode ? "fn local()->i64 { return 2 }\n" : "fn local()->i64 { return 1 }\n";
        CHECK(xr_compile_module_graph_build_source(graph, &authority, supplied, &error) ==
            (mode ? XR_MODULE_INVALID : XR_MODULE_OK));
        xr_compile_resources_free(error); xr_compile_module_graph_free(graph);
    }
    xr_compile_module_resolver_free(resolver); xr_compile_session_free(session);
    xr_compile_resources_release(resources); overlay_source_zero();
}
#include "xir_overlay_authority_cases.h"
int main(void) {
    char directory[XR_TEST_PATH_MAX] = "xir-overlay-query-XXXXXX", root[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory, root, sizeof(root)));
    char library_path[XR_TEST_PATH_MAX]; CHECK(snprintf(library_path, sizeof(library_path), "%s/lib.xr", root) > 0);
    FILE *library_file = fopen(library_path, "wb"); CHECK(library_file);
    CHECK(fputs("invalid disk source\n", library_file) >= 0 && fclose(library_file) == 0);
    XrXirSourceResult result = {0};
    CHECK(overlay_source_check(root, 0, overlay_source_limits(), &result) == XR_XIR_OK);
    CHECK(xr_test_unlink(library_path) == 0);
    overlay_source_facts(&result); CHECK(xr_xir_compile_artifact_verify(result.checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(result.checked); result.checked = NULL; overlay_source_facts(&result);
    xr_xir_compile_source_result_free(&result); overlay_source_zero();
    overlay_source_failures(root);
    const XrXirStatus failures[] = {XR_XIR_BAD_STRUCTURE, XR_XIR_BAD_TYPE, XR_XIR_UNRESOLVED,
        XR_XIR_BAD_STRUCTURE, XR_XIR_BAD_STRUCTURE, XR_XIR_BAD_STRUCTURE};
    for (unsigned mode = 1; mode <= sizeof(failures)/sizeof(failures[0]); ++mode) {
        CHECK(overlay_source_check(root, mode, overlay_source_limits(), &result) == failures[mode-1]);
        CHECK(!result.checked && !result.snapshot); overlay_source_zero();
    }
    overlay_priority(root); overlay_catalog_collision(root); overlay_memory_graph();
    overlay_authority_cases(root);
    CHECK(xr_test_rmdir(directory) == 0);
    puts("Overlay query: actual import bindings, absent dependencies, priority, private/cycle/type/catalog rejection passed");
    return 0;
}
