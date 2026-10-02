/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_stdlib_source_owner.c - Source metadata without runtime binders
 */
#include "module/xstdlib_embedded.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)

typedef struct Work { uint64_t used, limit; } Work;
static XrOsIoStatus charge(void *pointer, uint64_t units) {
    Work *work = pointer;
    if (units > work->limit - work->used) return XR_OS_IO_BUDGET;
    work->used += units; return XR_OS_IO_OK;
}
static void query_boundaries(const char *name, uint64_t exact, bool exists) {
    for (uint64_t limit = 0; limit <= exact; ++limit) {
        Work work = {0, limit};
        const XrStdlibSourceDescriptor *sentinel = (const void *)(uintptr_t)17, *result = sentinel;
        XrOsIoStatus status = xr_stdlib_source_descriptor_work(&work, charge, name, &result);
        if (limit < exact) CHECK(status == XR_OS_IO_BUDGET && result == sentinel);
        else CHECK(status == XR_OS_IO_OK && work.used == exact && (result != NULL) == exists);
    }
}
static void source_identity(const char *name, uint64_t expected_work) {
    for (uint64_t limit = 0; limit <= expected_work; ++limit) {
        Work bounded = {0, limit};
        const char *sentinel = (const void *)(uintptr_t)23, *result = sentinel;
        XrOsIoStatus status = xr_get_embedded_stdlib_work(&bounded, charge, name, &result);
        if (limit < expected_work) CHECK(status == XR_OS_IO_BUDGET && result == sentinel);
        else CHECK(status == XR_OS_IO_OK && result && bounded.used == expected_work);
    }
    Work work = {0, UINT64_MAX}; const char *source = NULL;
    CHECK(xr_get_embedded_stdlib_work(&work, charge, name, &source) == XR_OS_IO_OK);
    CHECK(source && work.used == expected_work && source == xr_get_embedded_stdlib(name));
    uint8_t digest[32]; size_t length = strlen(source);
    xr_sha256((const uint8_t *)source, length, digest);
    printf("SOURCE %s %zu ", name, length);
    for (unsigned i = 0; i < sizeof(digest); ++i) printf("%02x", (unsigned)digest[i]);
    putchar('\n');
}
int main(void) {
    /* Two generated rows, one entry visit and two reads per compared byte. */
    query_boundaries("?", 6, false);
    query_boundaries("io", 10, true);
    query_boundaries("path", 14, true);
    source_identity("io", 11);
    source_identity("path", 15);
    const XrStdlibSourceDescriptor *io = xr_stdlib_source_descriptor("io");
    CHECK(io && io->has_native_entries && !xr_stdlib_source_descriptor("?"));
    Work work = {0, UINT64_MAX};
    const XrStdlibSourceDescriptor *output = (const void *)(uintptr_t)19;
    CHECK(xr_stdlib_source_descriptor_work(NULL, charge, "io", &output) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_stdlib_source_descriptor_work(&work, NULL, "io", &output) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_stdlib_source_descriptor_work(&work, charge, NULL, &output) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(output == (const void *)(uintptr_t)19 && !work.used);
    puts("source metadata: independent work boundaries, preserved outputs, no runtime binder linkage passed");
    return 0;
}
