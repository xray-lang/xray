/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_declaration_manifest.c - Owned selector admission and allocation failures
 */
#include "module/xdeclaration_manifest.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *memory; size_t size; } Allocation;
static Allocation blocks[4096];
static size_t attempts, fail_at, live, allocated, peak;
static void *probe_malloc(size_t bytes) {
    if (++attempts == fail_at) return NULL;
    void *memory = xr_malloc(bytes); REQUIRE(memory);
    size_t i = 0; while (i < 4096 && blocks[i].memory) ++i;
    REQUIRE(i < 4096); blocks[i] = (Allocation){memory, bytes};
    live += bytes; allocated += bytes; if (live > peak) peak = live; return memory;
}
static void probe_free(void *memory) {
    if (!memory) return;
    size_t i = 0; while (i < 4096 && blocks[i].memory != memory) ++i;
    REQUIRE(i < 4096); live -= blocks[i].size; blocks[i] = (Allocation){0}; xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_free probe_free
#define xr_compile_resources_work actual_work
#include "base/xcompile_resources.c"
#undef xr_compile_resources_work

static bool record_work;
static uint64_t work_points[32768];
static size_t work_count;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *owner, uint64_t units) {
    if (record_work) {
        XrCompileResourceStats s;
        REQUIRE(xr_compile_resources_stats(owner, &s) == XR_COMPILE_RESOURCE_OK);
        REQUIRE(work_count < 32768); work_points[work_count++] = s.work + units;
    }
    return actual_work(owner, units);
}
static const char valid[] =
    "[project]\nname='fixture'\n[declarations]\nversion=1\n"
    "[[declarations.function]]\nmodule='src/main.xr'\nname='apply'\n"
    "no_suspend=true\nno_suspend_parameters=['callback']\n"
    "[[declarations.function]]\nmodule='src/box.xr'\nowner='Box'\nname='get'\nno_suspend=true\n";


static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static const XrTomlParseLimits parsing = {65536,128};
static XrCompileResources *open_owner(void) {
    XrCompileResources *resources = NULL;
    REQUIRE(xr_compile_resources_new(&unlimited, &resources) == XR_COMPILE_RESOURCE_OK);
    return resources;
}
static size_t read_valid(size_t failure, XrDeclarationLimits limits, XrDeclarationStatus expected) {
    REQUIRE(!live); attempts = allocated = peak = 0; fail_at = failure;
    XrCompileResources *owner = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(&unlimited, &owner);
    if (created != XR_COMPILE_RESOURCE_OK) {
        REQUIRE(failure && created == XR_COMPILE_RESOURCE_OUT_OF_MEMORY); return attempts;
    }
    XrOsIoPolicy policy = xr_compile_io_policy(owner);
    XrTomlValue *dom = NULL;
    XrTomlParseStatus parsed = xtoml_parse_owned(&policy, valid, strlen(valid), &parsing, &dom);
    XrDeclarationManifest *manifest = (XrDeclarationManifest *)1;
    XrDeclarationStatus status = parsed == XR_TOML_PARSE_OK ?
        xr_compile_declaration_manifest_read(owner, dom, limits, &manifest) : XR_DECLARATION_OUT_OF_MEMORY;
    if (failure) REQUIRE(status == XR_DECLARATION_OUT_OF_MEMORY);
    else REQUIRE(status == expected);
    xtoml_owned_free(dom);
    size_t count = attempts;
    xr_compile_resources_release(owner);
    if (status == XR_DECLARATION_OK) {
        REQUIRE(manifest->count == 2);
        REQUIRE(!strcmp(manifest->records[0].module, "src/main.xr"));
        REQUIRE(!manifest->records[0].owner && manifest->records[0].no_suspend);
        REQUIRE(manifest->records[0].parameter_count == 1);
        REQUIRE(!strcmp(manifest->records[0].parameters[0], "callback"));
        REQUIRE(!strcmp(manifest->records[1].owner, "Box"));
        xr_compile_declaration_manifest_free(manifest);
    } else REQUIRE(manifest == (XrDeclarationManifest *)1);
    REQUIRE(!live); return count;
}
static void admission(const char *source, XrDeclarationStatus expected) {
    fail_at = 0; XrCompileResources *owner = open_owner(); XrOsIoPolicy policy = xr_compile_io_policy(owner);
    XrTomlValue *dom = NULL;
    REQUIRE(xtoml_parse_owned(&policy, source, strlen(source), &parsing, &dom) == XR_TOML_PARSE_OK);
    XrDeclarationManifest *manifest = (XrDeclarationManifest *)1;
    REQUIRE(xr_compile_declaration_manifest_read(owner, dom, (XrDeclarationLimits){32,32}, &manifest) == expected);
    xtoml_owned_free(dom); xr_compile_resources_release(owner);
    if (expected == XR_DECLARATION_OK) xr_compile_declaration_manifest_free(manifest);
    else REQUIRE(manifest == (XrDeclarationManifest *)1);
    REQUIRE(!live);
}
static void invalid_version_text(void) {
    const char *const versions[] = {"1+2", "01", "1_", "+0x1", "0b_1", "1 junk"};
    for (size_t i = 0; i < XR_COUNTOF(versions); ++i) {
        char source[256];
        int length = snprintf(source, sizeof(source),
            "[declarations]\nversion=%s\n[[declarations.function]]\n"
            "module='a.xr'\nname='f'\nno_suspend=true\n", versions[i]);
        REQUIRE(length > 0 && (size_t)length < sizeof(source));
        fail_at = 0; XrCompileResources *owner = open_owner(); XrOsIoPolicy policy = xr_compile_io_policy(owner);
        XrTomlValue *dom = (XrTomlValue *)1;
        REQUIRE(xtoml_parse_owned(&policy, source, (size_t)length, &parsing, &dom) == XR_TOML_PARSE_INVALID);
        REQUIRE(dom == (XrTomlValue *)1);
        xr_compile_resources_release(owner); REQUIRE(!live);
    }
}
int main(void) {
    invalid_version_text();
    XrDeclarationLimits limits = {32,32};
    size_t count = read_valid(0, limits, XR_DECLARATION_OK);
    for (size_t failure = 1; failure <= count; ++failure) read_valid(failure, limits, XR_DECLARATION_OK);
    read_valid(0, (XrDeclarationLimits){1,32}, XR_DECLARATION_LIMIT);
    read_valid(0, (XrDeclarationLimits){32,0}, XR_DECLARATION_LIMIT);
    admission("[project]\nname='ordinary'\n", XR_DECLARATION_ABSENT);
    static const char *const invalid_sections[] = {
        "declarations=1\n", "[declarations]\nversion=2\nfunction=[]\n",
        "[declarations]\nversion=true\nfunction=[]\n", "[declarations]\nfunction=[]\n",
        "[declarations]\nversion=1\nfunction=[]\n", "[declarations]\nversion=1\nextra=1\n"
    };
    for (size_t i = 0; i < XR_COUNTOF(invalid_sections); ++i)
        admission(invalid_sections[i], XR_DECLARATION_INVALID);
    static const char *const invalid_records[] = {
        "module='a.xr'\nname='f'", "module='a.xr'\nname='f'\nno_suspend=false",
        "module='a.xr'\nname='f'\nno_suspend=1", "module='a.xr'\nname='f'\nowner=''\nno_suspend=true",
        "module='a.xr'\nname='f'\nno_suspend_parameters=[]",
        "module='a.xr'\nname='f'\nno_suspend_parameters=['x','x']",
        "module='a.xr'\nname='f'\nno_suspend_parameters=[1]",
        "module='a.xr'\nname='f'\nno_suspend=true\nnamespace='another'",
        "module='a.xr'\nname=\"f\\u0000suffix\"\nno_suspend=true",
        "module='a.xr'\nname='f'\n\"no_suspend\\u0000suffix\"=true",
        "module='/a.xr'\nname='f'\nno_suspend=true", "module='../a.xr'\nname='f'\nno_suspend=true",
        "module='a/./b.xr'\nname='f'\nno_suspend=true", "module='a//b.xr'\nname='f'\nno_suspend=true",
        "module='C:/a.xr'\nname='f'\nno_suspend=true", "module='a\\b.xr'\nname='f'\nno_suspend=true",
        "module='a'\nname='f'\nno_suspend=true", "module='a.xr'\nname=''\nno_suspend=true",
        "name='f'\nno_suspend=true", "module='a.xr'\nno_suspend=true"
    };
    char source[1024];
    for (size_t i = 0; i < XR_COUNTOF(invalid_records); ++i) {
        int length = snprintf(source, sizeof(source), "[declarations]\nversion=1\n[[declarations.function]]\n%s\n", invalid_records[i]);
        REQUIRE(length > 0 && (size_t)length < sizeof(source));
        admission(source, XR_DECLARATION_INVALID);
    }
    admission("[declarations]\nversion=1\n[[declarations.function]]\nmodule='a.xr'\nname='f'\nno_suspend=true\n"
        "[[declarations.function]]\nmodule='a.xr'\nname='f'\nno_suspend_parameters=['cb']\n", XR_DECLARATION_INVALID);
    admission("[declarations]\nversion=1\n[[declarations.function]]\nmodule='a.xr'\nname='f'\nno_suspend_parameters=['cb']\n", XR_DECLARATION_OK);
    printf("Declaration schema: 27 rejections, work/space/count limits, owned selectors and %zu allocation failures passed\n", count);
    return 0;
}
