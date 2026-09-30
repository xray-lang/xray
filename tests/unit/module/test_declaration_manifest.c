/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_declaration_manifest.c - Owned selector admission and allocation failures
 */
#include "base/xmalloc.h"
#include <stdlib.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "failed at %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at, live_count, live_bytes;
static struct { void *pointer; size_t size; } slots[1024];

static void *track(void *pointer, size_t size) {
    REQUIRE(pointer);
    for (size_t i = 0; i < XR_COUNTOF(slots); ++i) {
        if (slots[i].pointer) continue;
        slots[i].pointer = pointer;
        slots[i].size = size;
        ++live_count;
        live_bytes += size;
        return pointer;
    }
    exit(1);
}
static void *probe_malloc(size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_malloc(size), size);
}
static void *probe_calloc(size_t count, size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_calloc(count, size), count * size);
}
static void probe_free(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0; i < XR_COUNTOF(slots); ++i) {
        if (slots[i].pointer != pointer) continue;
        slots[i].pointer = NULL;
        --live_count;
        live_bytes -= slots[i].size;
        xr_free(pointer);
        return;
    }
    exit(1);
}
static void *probe_realloc(void *pointer, size_t size) {
    if (++attempts == fail_at) return NULL;
    if (!pointer) return track(xr_realloc(NULL, size), size);
    for (size_t i = 0; i < XR_COUNTOF(slots); ++i) {
        if (slots[i].pointer != pointer) continue;
        void *grown = xr_realloc(pointer, size);
        REQUIRE(grown);
        live_bytes = live_bytes - slots[i].size + size;
        slots[i].pointer = grown;
        slots[i].size = size;
        return grown;
    }
    exit(1);
}
static char *probe_strdup(const char *source) {
    size_t size = strlen(source) + 1;
    char *copy = probe_malloc(size);
    if (copy) memcpy(copy, source, size);
    return copy;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_calloc probe_calloc
#define xr_realloc probe_realloc
#define xr_free probe_free
#define xr_strdup probe_strdup

#include "../../../src/base/xtoml.c"
#include "../../../src/module/xdeclaration_manifest.c"

static const char valid[] =
    "[project]\nname='fixture'\n[declarations]\nversion=1\n"
    "[[declarations.function]]\nmodule='src/main.xr'\nname='apply'\n"
    "no_suspend=true\nno_suspend_parameters=['callback']\n"
    "[[declarations.function]]\nmodule='src/box.xr'\nowner='Box'\nname='get'\nno_suspend=true\n";

static size_t read_valid(size_t failure, XrDeclarationBudget budget, XrDeclarationStatus expected) {
    attempts = 0; fail_at = failure;
    XrTomlValue *dom = xtoml_parse(valid, strlen(valid));
    XrDeclarationManifest *manifest = NULL;
    XrDeclarationStatus status = dom ? xr_declaration_manifest_read(dom, budget, &manifest, NULL) :
        XR_DECLARATION_OUT_OF_MEMORY;
    size_t count = attempts;
    xtoml_free(dom);
    if (failure) REQUIRE(status != XR_DECLARATION_OK && !manifest);
    else REQUIRE(status == expected);
    if (manifest) {
        REQUIRE(manifest->count == 2);
        REQUIRE(!strcmp(manifest->records[0].module, "src/main.xr"));
        REQUIRE(!manifest->records[0].owner && manifest->records[0].no_suspend);
        REQUIRE(manifest->records[0].parameter_count == 1);
        REQUIRE(!strcmp(manifest->records[0].parameters[0], "callback"));
        REQUIRE(!strcmp(manifest->records[1].owner, "Box"));
    }
    xr_declaration_manifest_free(manifest);
    REQUIRE(!live_count && !live_bytes);
    return count;
}
static void admission(const char *source, XrDeclarationStatus expected) {
    fail_at = 0;
    XrTomlValue *dom = xtoml_parse(source, strlen(source)); REQUIRE(dom);
    XrDeclarationManifest *manifest = NULL;
    REQUIRE(xr_declaration_manifest_read(dom, (XrDeclarationBudget){65536, 32, 32, 10000},
        &manifest, NULL) == expected);
    if (expected != XR_DECLARATION_OK) REQUIRE(!manifest);
    xtoml_free(dom); xr_declaration_manifest_free(manifest);
    REQUIRE(!live_count && !live_bytes);
}
static void invalid_version_text(void) {
    const char *const versions[] = {"1+2", "01", "1_", "+0x1", "0b_1", "1 junk"};
    for (size_t i = 0; i < XR_COUNTOF(versions); ++i) {
        char source[256];
        int length = snprintf(source, sizeof(source),
            "[declarations]\nversion=%s\n[[declarations.function]]\n"
            "module='a.xr'\nname='f'\nno_suspend=true\n", versions[i]);
        REQUIRE(length > 0 && (size_t) length < sizeof(source));
        fail_at = 0;
        XrTomlValue *dom = xtoml_parse(source, (size_t) length);
        REQUIRE(!dom && !live_count && !live_bytes);
    }
}

int main(void) {
    invalid_version_text();
    XrDeclarationBudget budget = {65536, 32, 32, 10000};
    size_t count = read_valid(0, budget, XR_DECLARATION_OK);
    for (size_t failure = 1; failure <= count; ++failure)
        read_valid(failure, budget, XR_DECLARATION_OK);
    read_valid(0, (XrDeclarationBudget){1, 32, 32, 10000}, XR_DECLARATION_LIMIT);
    read_valid(0, (XrDeclarationBudget){65536, 1, 32, 10000}, XR_DECLARATION_LIMIT);
    read_valid(0, (XrDeclarationBudget){65536, 32, 0, 10000}, XR_DECLARATION_LIMIT);
    bool admitted = false;
    for (uint32_t work_limit = 0; work_limit < 1000 && !admitted; ++work_limit) {
        fail_at = 0; XrTomlValue *dom = xtoml_parse(valid, strlen(valid)); REQUIRE(dom);
        XrDeclarationManifest *manifest = NULL; budget.work = work_limit;
        XrDeclarationStatus status = xr_declaration_manifest_read(dom, budget, &manifest, NULL);
        admitted = status == XR_DECLARATION_OK;
        REQUIRE(admitted || status == XR_DECLARATION_LIMIT);
        xtoml_free(dom); xr_declaration_manifest_free(manifest);
        REQUIRE(!live_count && !live_bytes);
    }
    REQUIRE(admitted);
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
