/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_bindings.h"
#include "app/lsp/xlsp_source_workspace.h"
#include "xir/xxir_types.h"
#include "base/xmalloc.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1);}} while (0)
#include "xir_sdk_resource_test.h"
#include "original_inlay_inputs.h"

static void observations(const XlspSourceBindings *owner) {
    const XrXirSourceView *view = xlsp_source_bindings_view(owner, 0);
    const XrXirSourceSyntaxView *syntax = xlsp_source_bindings_syntax(owner, 0);
    CHECK(view && view->complete && syntax && syntax->declaration_count == view->declaration_count);
    printf("Complete Source declarations=%u references=%u expressions=%u modules=%u\n",
        view->declaration_count, view->reference_count, view->expression_count, view->module_count);
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *d = &view->declarations[i];
        const XrXirTypeNode *callable = d->type.known ? xr_xir_callable_signature(view->types, d->type.type) : NULL;
        XrXirSourceRange name = syntax->declarations[i].name;
        printf("Declaration id=%u parent=%u kind=%u name=%s known=%u type=%u parameters=%u callable=%u flags=%u signature=%s nameSpan=%d:%d..%d:%d\n",
            d->id, d->parent, (unsigned)d->kind, d->name ? d->name : "NULL", d->type.known ? 1u : 0u,
            (unsigned)d->type.type, d->parameter_count, callable ? 1u : 0u, callable ? callable->flags : 0u,
            d->signature ? d->signature : "NULL", name.line, name.column, name.end_line, name.end_column);
        if (callable) for (uint32_t p = 0; p < callable->parameter_count; ++p)
            printf("Callable parameter=%u type=%u physicalMode=%u\n", p,
                (unsigned)callable->parameters[p].type, callable->parameters[p].mode);
    }
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        const XrXirSourceReference *r = &view->references[i];
        printf("Reference binding=%u target=%u access=%u span=%d:%d..%d:%d\n", r->declaration, r->target,
            (unsigned)r->access, r->range.line, r->range.column, r->range.end_line, r->range.end_column);
    }
}
static int run(int argc, char **argv) {
    (void)sdk_unlimited;
    CHECK(argc == 3);
    unsigned mode = (unsigned)strtoul(argv[2], NULL, 10); CHECK(mode < 2);
    const XrCompileResourceLimits limits = {UINT64_C(64) * 1024 * 1024, UINT64_C(8) * 1024 * 1024, UINT64_C(128000000)};
    XrCompileResources *resources = sdk_ledger(&limits);
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    size_t length = strlen(original_inlay_inputs[mode]), uri_length = strlen(argv[1]);
    char *text = sdk_fixture_malloc(length + 1), *uri = sdk_fixture_malloc(uri_length + 1);
    CHECK(text && uri); memcpy(text, original_inlay_inputs[mode], length + 1); memcpy(uri, argv[1], uri_length + 1);
    XlspSourceDocument document = {uri, text, uri_length, length, 1}; XlspSourceBindings *owner = NULL;
    XrXirStatus status = xlsp_source_bindings_build(&context, &document, 1, &owner);
    if (status != XR_XIR_OK) {
        XlspSourceWorkspace *control = NULL; XlspSourceWorkspaceFailure failure = {0}; unsigned stage = 0;
        XrXirStatus observed = xlsp_source_workspace_build(&context, &document, 1, &control, &stage, &failure);
        printf("Independent same-input Source workspace status=%u stage=%u diagnostic=%s\n",
            (unsigned)observed, stage, failure.diagnostic.message);
        CHECK(observed == status && !control);
        xlsp_source_workspace_failure_free(&failure); xlsp_source_workspace_free(control);
    }
    XrCompileResourceStats stats = sdk_stats(resources);
    memset(text, '?', length); memset(uri, '?', uri_length); sdk_fixture_free(text); sdk_fixture_free(uri);
    xr_compile_resources_release(resources);
    printf("Original inlay Source admission case=%s status=%u output=%s SourceCalls=%zu allocated=%llu peak=%llu work=%llu\n",
        original_inlay_names[mode], (unsigned)status, owner ? "owned" : "NULL", runtime_attempts,
        (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.peak_bytes, (unsigned long long)stats.work);
    if (status == XR_XIR_OK) observations(owner); else CHECK(!owner);
    xlsp_source_bindings_free(owner); CHECK(!runtime_live && !runtime_bytes);
    printf("Original inlay canonical Source producers dead; physical0/0. Public inlay labels/effects NOT_TESTED\n");
    return status == XR_XIR_OK ? 0 : 1;
}
#if XR_OS_WINDOWS
int wmain(int argc, wchar_t **wide) {
    XrWinPathStatus status; char **argv = xr_win_utf16_arguments(argc, wide, &status); CHECK(argv);
    int result = run(argc, argv); xr_win_utf8_arguments_free(argc, argv); return result;
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
