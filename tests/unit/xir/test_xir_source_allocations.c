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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at = SIZE_MAX, live;
static unsigned resolver_fault;
static void *owned[4096];
static void *source_counted_calloc(size_t count, size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *pointer = xr_calloc(count, size);
    if (pointer) { CHECK(live < 4096); owned[live++] = pointer; }
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
int main(void) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_SOURCE_FIXTURES "/root.xr", &authority, NULL, XR_SOURCE_STDLIB};
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
    xr_compiler_session_delete(session);
    printf("Source-owner allocation failures: %zu; no partial artifact or live metadata\n", count);
    return 0;
}
