/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_declaration_owner.c - Independent schema and compiler-resource admission checks
 */
#include "module/xdeclaration_manifest.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *memory; size_t size; } Allocation;
static Allocation blocks[64];
static size_t attempts, fail_at, live, allocated, peak;
static void *probe_malloc(size_t bytes) {
    if (++attempts == fail_at) return NULL;
    void *memory = xr_malloc(bytes); REQUIRE(memory);
    size_t i = 0; while (i < 64 && blocks[i].memory) ++i;
    REQUIRE(i < 64); blocks[i] = (Allocation){memory, bytes};
    live += bytes; allocated += bytes; if (live > peak) peak = live; return memory;
}
static void probe_free(void *memory) {
    if (!memory) return;
    size_t i = 0; while (i < 64 && blocks[i].memory != memory) ++i;
    REQUIRE(i < 64); live -= blocks[i].size; blocks[i] = (Allocation){0}; xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_free probe_free
#include "base/xcompile_resources.c"

typedef struct Fixture {
    XrTomlValue root, section, version, records, record[2], text[8], promise, parameters;
    XrTomlMember root_fields[1], section_fields[2], fields[2][5];
    XrTomlValue *record_items[2], *parameter_items[2];
    char module[16];
} Fixture;
static XrTomlValue text_value(char *text) {
    XrTomlValue v = {0}; v.type = XR_TOML_STRING; v.as.string = text; v.string_length = strlen(text); return v;
}
static XrTomlValue table(XrTomlMember *members, int count) {
    XrTomlValue v = {0}; v.type = XR_TOML_TABLE; v.as.table.members = members; v.as.table.count = count; return v;
}
static XrTomlMember member(char *name, XrTomlValue *v) {
    return (XrTomlMember){name, strlen(name), v};
}
static void fixture_init(Fixture *f) {
    memset(f, 0, sizeof(*f)); strcpy(f->module, "lib/main.xr");
    f->text[0] = text_value(f->module); f->text[1] = text_value("apply");
    f->text[2] = text_value("run"); f->text[3] = text_value("Widget");
    f->text[4] = text_value("callback"); f->text[5] = text_value("fallback");
    f->promise.type = XR_TOML_BOOL; f->promise.as.boolean = true;
    f->parameter_items[0] = &f->text[4]; f->parameter_items[1] = &f->text[5];
    f->parameters.type = XR_TOML_ARRAY; f->parameters.as.array.count = 2;
    f->parameters.as.array.items = f->parameter_items;
    for (unsigned i = 0; i < 2; ++i) {
        f->fields[i][0] = member("module", &f->text[0]);
        f->fields[i][1] = member("name", &f->text[i + 1]);
        f->fields[i][2] = member("no_suspend", &f->promise);
        f->record[i] = table(f->fields[i], i ? 4 : 5); f->record_items[i] = &f->record[i];
    }
    f->fields[0][3] = member("owner", &f->text[3]);
    f->fields[0][4] = member("no_suspend_parameters", &f->parameters);
    f->fields[1][3] = member("owner", &f->text[3]);
    f->records.type = XR_TOML_ARRAY; f->records.as.array.count = 2; f->records.as.array.items = f->record_items;
    f->version.type = XR_TOML_INTEGER; f->version.as.integer = 1;
    f->section_fields[0] = member("version", &f->version);
    f->section_fields[1] = member("function", &f->records);
    f->section = table(f->section_fields, 2);
    f->root_fields[0] = member("declarations", &f->section);
    f->root = table(f->root_fields, 1);
}
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static const XrDeclarationLimits shape = {2, 2};
static XrCompileResourceStats run(Fixture *f, XrCompileResourceLimits limits,
    XrDeclarationLimits declarations, XrDeclarationStatus expected, bool twice) {
    REQUIRE(!live); allocated = peak = 0;
    XrCompileResources *resources = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(&limits, &resources);
    if (created != XR_COMPILE_RESOURCE_OK) {
        REQUIRE(created == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && expected == XR_DECLARATION_OUT_OF_MEMORY);
        REQUIRE(!live); return (XrCompileResourceStats){0};
    }
    XrDeclarationManifest *result = (XrDeclarationManifest *)1;
    XrDeclarationStatus status = xr_compile_declaration_manifest_read(resources, &f->root, declarations, &result);
    if (status != expected) fprintf(stderr, "status %d expected %d\n", status, expected);
    REQUIRE(status == expected);
    if (status == XR_DECLARATION_OK) {
        REQUIRE(result && result != (XrDeclarationManifest *)1 && result->count == 2);
        REQUIRE(!strcmp(result->records[0].module, "lib/main.xr"));
        REQUIRE(!strcmp(result->records[0].name, "apply") && !strcmp(result->records[1].name, "run"));
        REQUIRE(result->records[0].no_suspend && result->records[0].parameter_count == 2);
        REQUIRE(!strcmp(result->records[0].parameters[0], "callback"));
        REQUIRE(!strcmp(result->records[0].parameters[1], "fallback"));
        REQUIRE(result->records[0].module != f->module);
    } else REQUIRE(result == (XrDeclarationManifest *)1);
    XrCompileResourceStats s;
    REQUIRE(xr_compile_resources_stats(resources, &s) == XR_COMPILE_RESOURCE_OK);
    REQUIRE(s.live_bytes == live && s.allocated_bytes == allocated && s.peak_bytes == peak);
    if (twice) {
        XrDeclarationManifest *second = (XrDeclarationManifest *)2;
        REQUIRE(xr_compile_declaration_manifest_read(resources, &f->root, declarations, &second) == XR_DECLARATION_LIMIT);
        REQUIRE(second == (XrDeclarationManifest *)2);
    }
    xr_compile_resources_release(resources);
    if (status == XR_DECLARATION_OK) {
        f->module[0] = 'X'; REQUIRE(!strcmp(result->records[0].module, "lib/main.xr")); f->module[0] = 'l';
        xr_compile_declaration_manifest_free(result);
    }
    REQUIRE(!live); return s;
}
static void rejected(void) {
    Fixture f;
    fixture_init(&f); f.records.as.array.items = NULL;
    run(&f, unlimited, shape, XR_DECLARATION_INVALID, false);
    fixture_init(&f); f.root.as.table.count = -1;
    run(&f, unlimited, shape, XR_DECLARATION_INVALID, false);
    fixture_init(&f); f.section.as.table.members = NULL;
    run(&f, unlimited, shape, XR_DECLARATION_INVALID, false);
    fixture_init(&f); f.parameter_items[1] = f.parameter_items[0];
    run(&f, unlimited, shape, XR_DECLARATION_INVALID, false);
    fixture_init(&f); f.text[2] = f.text[1];
    run(&f, unlimited, shape, XR_DECLARATION_INVALID, false);
    fixture_init(&f); f.fields[0][3].key = "unknown"; f.fields[0][3].key_length = 7;
    run(&f, unlimited, shape, XR_DECLARATION_INVALID, false);
    const char *bad[] = {"../a.xr", "a/../b.xr", "a//b.xr", "/a.xr", "a/./b.xr", "a.xr/", "a.x", "a\\b.xr", "a:b.xr"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        fixture_init(&f); f.text[0] = text_value((char *)bad[i]);
        run(&f, unlimited, shape, XR_DECLARATION_INVALID, false);
    }
    fixture_init(&f); f.text[1] = text_value("a"); f.text[1].string_length = 2;
    run(&f, unlimited, shape, XR_DECLARATION_INVALID, false);
    fixture_init(&f);
    run(&f, unlimited, (XrDeclarationLimits){1,2}, XR_DECLARATION_LIMIT, false);
    run(&f, unlimited, (XrDeclarationLimits){2,1}, XR_DECLARATION_LIMIT, false);
    f.root = table(NULL, 0);
    XrCompileResourceStats empty = run(&f, unlimited, shape, XR_DECLARATION_ABSENT, false);
    REQUIRE(empty.work == 15 && empty.allocation_count == 1);
}
int main(void) {
    Fixture f; fixture_init(&f);
    XrCompileResourceStats baseline = run(&f, unlimited, shape, XR_DECLARATION_OK, false);
    size_t count = attempts;
    for (size_t i = 1; i <= count; ++i) {
        attempts = 0; fail_at = i; run(&f, unlimited, shape, XR_DECLARATION_OUT_OF_MEMORY, false);
    }
    fail_at = 0;
    for (uint64_t w = 1; w < baseline.work; ++w) {
        XrCompileResourceLimits limits = unlimited; limits.work = w;
        run(&f, limits, shape, XR_DECLARATION_LIMIT, false);
    }
    XrCompileResourceLimits exact = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    run(&f, exact, shape, XR_DECLARATION_OK, true);
    for (unsigned axis = 0; axis < 2; ++axis) {
        XrCompileResourceLimits limits = unlimited;
        if (!axis) limits.allocated_bytes = baseline.allocated_bytes - 1;
        else limits.live_bytes = baseline.peak_bytes - 1;
        run(&f, limits, shape, XR_DECLARATION_LIMIT, false);
    }
    rejected();
    XrDeclarationManifest *canary = (XrDeclarationManifest *)1;
    REQUIRE(xr_compile_declaration_manifest_read(NULL, (XrTomlValue *)1, shape, &canary) == XR_DECLARATION_BAD_ARGUMENT);
    XrCompileResources *owner = NULL; XrCompileResourceLimits limits = unlimited; limits.work = 1;
    REQUIRE(xr_compile_resources_new(&limits, &owner) == XR_COMPILE_RESOURCE_OK);
    REQUIRE(xr_compile_declaration_manifest_read(owner, (XrTomlValue *)1, shape, &canary) == XR_DECLARATION_LIMIT);
    REQUIRE(canary == (XrDeclarationManifest *)1);
    xr_compile_resources_release(owner); REQUIRE(!live);
    printf("Declaration schema: %zu real OOM points, %llu work cutoffs, independent absent work=15, zero physical residuals\n",
        count, (unsigned long long)baseline.work - 1);
    return 0;
}
