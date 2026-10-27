/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_source_fixture.h - Authentic source and specialization test inputs
 */
#ifndef LIR_SOURCE_FIXTURE_H
#define LIR_SOURCE_FIXTURE_H
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%d: %s\n", __LINE__, #c);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

typedef struct SourceFixture {
    XrXirSourceResult source;
    XrXirArtifact *closed;
} SourceFixture;

static SourceFixture source_fixture(const char *text) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "lir-lowering", NULL};
    XrXirSourceRequest request = {.session = session,
                                  .authority = &authority,
                                  .context = &context,
                                  .linkage_kind = XR_XIR_PROGRAM};
    SourceFixture fixture = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    XrXirSourceText input = {NULL, text, strlen(text)};
    XrXirStatus status =
        xr_xir_compile_source_check_text(&request, &input, &fixture.source, &diagnostic, NULL);
    if (status != XR_XIR_OK)
        fprintf(stderr, "source %u:%d:%d: %s\n%s\n", (unsigned) status, diagnostic.line,
                diagnostic.column, diagnostic.message, text);
    CHECK(status == XR_XIR_OK && fixture.source.checked && fixture.source.snapshot);
    CHECK(xr_xir_compile_specialize(fixture.source.checked, &fixture.closed, NULL) == XR_XIR_OK);
    xr_compile_session_free(session);
    xr_compile_resources_release(context.resources);
    return fixture;
}
static void source_fixture_free(SourceFixture *fixture) {
    xr_xir_compile_artifact_free(fixture->closed);
    xr_xir_compile_source_result_free(&fixture->source);
    *fixture = (SourceFixture) {0};
}
#endif  // LIR_SOURCE_FIXTURE_H
