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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at = SIZE_MAX, live;
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
#undef xr_calloc
#undef xr_free
#define xr_calloc(count, size) source_counted_calloc(count, size)
#define xr_free(pointer) source_counted_free(pointer)
#include "xir/xxir_source.c"
int main(void) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session, XR_SOURCE_FIXTURES "/root.xr", &authority, NULL, XR_SOURCE_STDLIB};
    XrXirArtifact *artifact = NULL;
    CHECK(xr_xir_source_check(&request, &artifact, NULL) == XR_XIR_OK && artifact && !live);
    xr_xir_artifact_free(artifact);
    size_t count = attempts;
    for (size_t i = 0; i < count; ++i) {
        fail_at = i; attempts = 0; artifact = NULL;
        CHECK(xr_xir_source_check(&request, &artifact, NULL) == XR_XIR_OUT_OF_MEMORY && !artifact && !live);
    }
    xr_compiler_session_delete(session);
    printf("Source-owner allocation failures: %zu; no partial artifact or live metadata\n", count);
    return 0;
}
