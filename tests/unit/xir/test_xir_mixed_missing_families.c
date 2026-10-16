/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_missing_families.c - Reproduce Source admission gaps before mixed execution
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Missing family %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"

typedef struct MissingFamilyCase {
    const char *name;
    int64_t expected;
} MissingFamilyCase;

/* Admission is only a prerequisite. No runtime, scheduler or view is emulated. */
static bool missing_family_source(const MissingFamilyCase *test) {
    const XrXirCompileContext *context = effects_source_owner(67108864, 128000000);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s.xr", XR_MIXED_MISSING_FIXTURES, test->name);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_MIXED_MISSING_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure_path = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure_path);
    bool admitted = status == XR_XIR_OK;
    CHECK(admitted ? result.checked != NULL : result.checked == NULL && result.snapshot == NULL);
    if (!strcmp(test->name, "control")) CHECK(admitted);
    printf("P20 family=%s source_status=%u checked=%u expected_i64=%lld runtime=NOT_REACHED at=%d:%d message=%s\n",
        test->name, status, (unsigned)(result.checked != NULL), (long long)test->expected,
        diagnostic.line, diagnostic.column, diagnostic.message);
    if (!admitted) printf("P20 rejection family=%s path=%s\n", test->name, failure_path ? failure_path : path);
    fflush(stdout);
    xr_compile_resources_free(failure_path);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    return admitted;
}

int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2);
    const MissingFamilyCase cases[] = {
        {"control",42}, {"generator",42}, {"iterator",42},
        {"channel",42}, {"slice",42}, {"mutable_slice",50}
    };
    unsigned selected = 0, rejected = 0;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        if (argc == 2 && strcmp(argv[1], cases[i].name)) continue;
        ++selected; if (!missing_family_source(&cases[i])) ++rejected;
    }
    CHECK(selected);
    effects_source_owners_free();
    printf("P20 Source reproductions selected=%u rejected=%u compilerPhysical0/0 allFourBackendsAndLifetime=NOT_REACHED\n",
        selected, rejected);
    /* A rejected positive fixture remains a failure; it is never a passing CTest. */
    return rejected ? 1 : 0;
}
