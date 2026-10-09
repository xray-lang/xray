/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"

static const char entry_text[] = "export fn unsaved(value:i64)->i64 { return value }\n";
static XrCompileResourceStats text_stats;
static XrCompileResourceLimits text_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024,
        UINT64_C(8)*1024*1024,UINT64_C(128000000)};
}
/* This wraps the real resource/session/source calls. No result is fabricated. */
static XrXirStatus text_check(const XrXirSourceRequest *input, const char *logical,
    const char *text, size_t length, XrCompileResourceLimits limits, XrXirSourceResult *output) {
    XrXirCompileContext context = {0}; XrCompilerSession *session = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(&limits, &context.resources);
    if (created != XR_COMPILE_RESOURCE_OK)
        return created == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSessionStatus made = xr_compile_session_new(context.resources, &session);
    XrXirStatus status = made == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        made == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    if (status == XR_XIR_OK) {
        XrXirSourceRequest request = *input; request.context = &context; request.session = session;
        XrXirSourceDiagnostic diagnostic = {0};
        status = xr_xir_compile_source_check_text(&request, &(XrXirSourceText){logical, text, length}, output, &diagnostic, NULL);
        CHECK(diagnostic.status == status);
    }
    xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(context.resources, &text_stats) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);
    return status;
}
static void text_physical_zero(void) {
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void text_facts(const XrXirSourceResult *result, const char *path) {
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(view && view->complete && view->diagnostic.status == XR_XIR_OK);
    const XrXirSourceDeclaration *function = NULL, *parameter = NULL;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *decl = &view->declarations[i];
        if (!strcmp(decl->name, "unsaved")) { CHECK(!function); function = decl; }
    }
    CHECK(function);
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *decl = &view->declarations[i];
        if (decl->parent == function->id && !strcmp(decl->name, "value")) { CHECK(!parameter); parameter = decl; }
    }
    CHECK(parameter && function->kind == XR_XIR_SOURCE_FUNCTION);
    CHECK(function->exported && !function->parent && function->type.known && function->type.type == XR_XIR_I64);
    CHECK(function->parameter_count == 1 && function->parameters[0].known && function->parameters[0].type == XR_XIR_I64);
    CHECK(parameter->parent == function->id && parameter->kind == XR_XIR_SOURCE_PARAMETER);
    CHECK(parameter->type.known && parameter->type.type == XR_XIR_I64);
    CHECK(parameter->range.line == 1 && parameter->range.column == 19 && parameter->range.end_column == 24);
    CHECK(parameter->range.module == function->range.module && function->range.module < view->module_count);
    const XrXirSourceQueryModule *module = &view->modules[function->range.module];
    CHECK(module->identity && module->identity[0]);
    CHECK(path ? module->path && !strcmp(module->path, path) : !module->path);
    unsigned references = 0;
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        const XrXirSourceReference *reference = &view->references[i];
        if (reference->declaration != parameter->id) continue;
        CHECK(reference->target == parameter->id && reference->access == XR_XIR_SOURCE_READ);
        CHECK(reference->range.line == 1 && reference->range.column == 44 && reference->range.end_column == 49);
        ++references;
    }
    CHECK(references == 1);
}
static void text_rejections(const XrXirSourceRequest *request) {
    const char embedded[] = {'x', '\0', 'x'};
    for (unsigned mode = 0; mode < 8; ++mode) {
        XrXirSourceRequest changed = *request; XrModuleIdentityAuthority authority = *request->authority;
        changed.authority = &authority;
        const char *logical = "root.xr", *text = entry_text; size_t length = sizeof(entry_text)-1;
        if (mode == 0) text = NULL;
        if (mode == 1) { text = embedded; length = sizeof(embedded); }
        if (mode == 2) logical = "other.xr";
        if (mode == 3) logical = "../root.xr";
        if (mode == 4) changed.entry_path = NULL;
        if (mode == 5) changed.authority = NULL;
        if (mode == 6) logical = NULL;
        if (mode == 7) length = SIZE_MAX;
        XrXirSourceResult result = {0};
        CHECK(text_check(&changed, logical, text, length, text_limits(), &result) ==
            (mode == 7 ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE));
        CHECK(!result.checked && !result.snapshot); text_physical_zero();
    }
}
static void text_allocations(const XrXirSourceRequest *request) {
    size_t sites = 0; XrCompileResourceStats required = {0};
    for (size_t probe = 0; probe <= sites; ++probe) {
        source_fixture_compile_attempts = 0;
        source_fixture_compile_fail_at = probe ? probe-1 : SIZE_MAX;
        source_fixture_compile_injected = false;
        XrXirSourceResult result = {0};
        XrXirStatus status = text_check(request, "root.xr", entry_text, sizeof(entry_text)-1, text_limits(), &result);
        if (!probe) {
            CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
            sites = source_fixture_compile_attempts; required = text_stats; CHECK(sites);
            text_facts(&result, request->entry_path);
        } else {
            CHECK(status == XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected);
            CHECK(source_fixture_compile_attempts > source_fixture_compile_fail_at);
            CHECK(!result.checked && !result.snapshot);
        }
        xr_xir_compile_source_result_free(&result); text_physical_zero();
    }
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected = false;
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = {required.allocated_bytes, required.peak_bytes, required.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound && *bound < UINT64_MAX); *bound = (uint64_t)((int64_t)*bound + delta);
        XrXirSourceResult result = {0};
        CHECK(text_check(request, "root.xr", entry_text, sizeof(entry_text)-1, limits, &result) ==
            (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK));
        if (delta < 0) CHECK(!result.checked && !result.snapshot); else text_facts(&result, request->entry_path);
        xr_xir_compile_source_result_free(&result); text_physical_zero();
    }
    printf("Entry text fresh allocation sites=%zu; three axes exact/minus1/plus1; physical=0/0\n", sites);
}
static void text_null_descriptor(const XrXirSourceRequest *prototype) {
    XrCompileResourceLimits limits=text_limits();XrXirCompileContext context={0};XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest request=*prototype;request.context=&context;request.session=session;
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    size_t attempts=source_fixture_compile_attempts,live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    CHECK(xr_xir_compile_source_check_text(&request,NULL,&result,&diagnostic,&failure)==XR_XIR_BAD_STRUCTURE);
    CHECK(diagnostic.status==XR_XIR_BAD_STRUCTURE&&!result.checked&&!result.snapshot&&!failure);
    CHECK(source_fixture_compile_attempts==attempts&&source_fixture_compile_live==live&&source_fixture_compile_bytes==bytes);
    xr_compile_session_free(session);xr_compile_resources_release(context.resources);text_physical_zero();
}
int main(void) {
    XrXirSourceResult absent = {0}; XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_check_text(NULL, NULL, &absent, &diagnostic, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(diagnostic.status == XR_XIR_BAD_STRUCTURE && !absent.checked && !absent.snapshot); text_physical_zero();
    char directory[XR_TEST_PATH_MAX] = "xir-entry-text-XXXXXX", absolute[XR_TEST_PATH_MAX], path[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    CHECK(snprintf(path, sizeof(path), "%s/root.xr", absolute) > 0);
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fputs("invalid disk source\n", file) >= 0 && fclose(file) == 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {NULL, path, &authority, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    char *buffer = malloc(sizeof(entry_text)-1); CHECK(buffer);
    memcpy(buffer, entry_text, sizeof(entry_text)-1);
    XrXirSourceResult result = {0};
    char producer_path[XR_TEST_PATH_MAX], producer_root[XR_TEST_PATH_MAX], logical[] = "root.xr";
    strcpy(producer_path, path); strcpy(producer_root, absolute);
    XrModuleIdentityAuthority producer_authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, producer_root};
    XrXirSourceRequest producer = request; producer.entry_path = producer_path; producer.authority = &producer_authority;
    CHECK(text_check(&producer, logical, buffer, sizeof(entry_text)-1, text_limits(), &result) == XR_XIR_OK);
    memset(buffer, '?', sizeof(entry_text)-1); free(buffer);
    memset(producer_path, '?', sizeof(producer_path)); memset(producer_root, '?', sizeof(producer_root));
    memset(logical, '?', sizeof(logical)); producer_authority = (XrModuleIdentityAuthority){0};
    CHECK(xr_test_unlink(path) == 0);
    text_facts(&result, path);
    CHECK(xr_xir_compile_artifact_verify(result.checked, NULL) == XR_XIR_OK);
    XrXirArtifact *checked = result.checked; XrXirSourceSnapshot *snapshot = result.snapshot;
    CHECK(text_check(&request, "root.xr", entry_text, sizeof(entry_text)-1, text_limits(), &result) == XR_XIR_BAD_STRUCTURE);
    CHECK(result.checked == checked && result.snapshot == snapshot);
    xr_xir_compile_artifact_free(result.checked); result.checked = NULL;
    text_facts(&result, path);
    xr_xir_compile_source_result_free(&result); text_physical_zero();
    /* The physical entry no longer exists: supplied bytes still have the same identity. */
    text_allocations(&request); text_rejections(&request); text_null_descriptor(&request);
    XrModuleIdentityAuthority memory = {XR_MODULE_IDENTITY_MEMORY, "entry-query", NULL};
    XrXirSourceRequest memory_request = {NULL, NULL, &memory, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    CHECK(text_check(&memory_request, NULL, entry_text, sizeof(entry_text)-1, text_limits(), &result) == XR_XIR_OK);
    text_facts(&result, NULL); xr_xir_compile_source_result_free(&result); text_physical_zero();
    CHECK(text_check(&request, "root.xr", "", 0, text_limits(), &result) == XR_XIR_OK);
    CHECK(result.checked && result.snapshot); xr_xir_compile_source_result_free(&result); text_physical_zero();
    memory_request.entry_path = path;
    CHECK(text_check(&memory_request, NULL, entry_text, sizeof(entry_text)-1, text_limits(), &result) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot); text_physical_zero();
    memory_request.entry_path = NULL;
    CHECK(text_check(&memory_request, "root.xr", entry_text, sizeof(entry_text)-1, text_limits(), &result) == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot); text_physical_zero();
    CHECK(xr_test_rmdir(directory) == 0);
    puts("Entry bytes: fixed bindings/ranges, disk shadow, absent entry, producer death and failures passed");
    return 0;
}
