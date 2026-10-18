/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_params.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d:%s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"

static void same(const XrJsonValue *a, const XrJsonValue *b) {
    CHECK(a && b && a != b && a->type == b->type && a->is_integer == b->is_integer && a->string_len == b->string_len);
    switch (a->type) {
    case XR_JSON_NULL: break;
    case XR_JSON_BOOL: CHECK(a->as.boolean == b->as.boolean); break;
    case XR_JSON_NUMBER:
        if (a->is_integer) CHECK(a->as.integer == b->as.integer);
        else CHECK(a->as.number == b->as.number);
        break;
    case XR_JSON_STRING:
        CHECK(a->as.string != b->as.string && !memcmp(a->as.string, b->as.string, a->string_len + 1)); break;
    case XR_JSON_ARRAY:
        CHECK(a->as.array.count == b->as.array.count);
        for (int i = 0; i < a->as.array.count; ++i) same(a->as.array.items[i], b->as.array.items[i]);
        break;
    case XR_JSON_OBJECT:
        CHECK(a->as.object.count == b->as.object.count);
        for (int i = 0; i < a->as.object.count; ++i) {
            const XrJsonMember *x = &a->as.object.members[i], *y = &b->as.object.members[i];
            CHECK(x->key != y->key && x->key_len == y->key_len && !memcmp(x->key, y->key, x->key_len + 1));
            same(x->value, y->value);
        }
        break;
    }
}
static XrJsonValue *input(unsigned index) {
    const char *texts[] = {"null", "9223372036854775807",
        "{\"x\":1,\"x\":1.0,\"a\\u0000b\":\"λ\\u0000z\",\"list\":[true,false,null,\"\"]}", "{}", "[]", "\"\""};
    if (index < 6) { XrJsonValue *value = xjson_parse(texts[index], strlen(texts[index])); CHECK(value); return value; }
    char text[2 * XJSON_MAX_DEPTH];
    memset(text, '[', XJSON_MAX_DEPTH - 1); text[XJSON_MAX_DEPTH - 1] = '0';
    memset(text + XJSON_MAX_DEPTH, ']', XJSON_MAX_DEPTH - 1);
    XrJsonValue *value = xjson_parse(text, sizeof(text) - 1); CHECK(value); return value;
}
static XrCompileResourceStats normal(unsigned index, size_t *sites) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XrJsonValue *value = input(index); XlspSourceParams *params = NULL;
    size_t begin = runtime_attempts;
    CHECK(xlsp_source_params_new(resources, value, &params) == XR_COMPILE_RESOURCE_OK);
    *sites = runtime_attempts - begin; CHECK(*sites);
    const XrJsonValue *view = xlsp_source_params_view(params); same(value, view);
    size_t length = 0; char *before = xjson_stringify(value, &length); CHECK(before);
    XrCompileResourceStats stats = sdk_stats(resources);
    CHECK(stats.live_bytes && stats.work && runtime_live > 1);
    xjson_free(value); xr_compile_resources_release(resources);
    CHECK(runtime_live > 1);
    size_t actual = 0; char *after = xjson_stringify((XrJsonValue *)view, &actual);
    CHECK(after && actual == length && !memcmp(before, after, length));
    lsp_json_free(before); lsp_json_free(after); xlsp_source_params_free(params);
    CHECK(!runtime_live && !runtime_bytes && !lsp_json_live && !lsp_json_bytes);
    return stats;
}
static void faults(unsigned index, size_t sites, XrCompileResourceStats stats) {
    XrJsonValue *value = input(index);
    for (size_t i = 0; i < sites; ++i) {
        XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XlspSourceParams *params = NULL;
        size_t blocks = runtime_live, bytes = runtime_bytes; XrCompileResourceStats before = sdk_stats(resources);
        runtime_fail_at = runtime_attempts + i;
        CHECK(xlsp_source_params_new(resources, value, &params) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        CHECK(runtime_attempts == runtime_fail_at + 1); runtime_fail_at = SIZE_MAX;
        XrCompileResourceStats after = sdk_stats(resources);
        CHECK(!params && runtime_live == blocks && runtime_bytes == bytes && after.live_bytes == before.live_bytes);
        CHECK(after.work > before.work && after.allocated_bytes >= before.allocated_bytes);
        xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits limits = sdk_unlimited;
        if (axis == 0) limits.allocated_bytes = stats.allocated_bytes - 1;
        if (axis == 1) limits.live_bytes = stats.peak_bytes - 1;
        if (axis == 2) limits.work = stats.work - 1;
        XrCompileResources *resources = NULL; XlspSourceParams *params = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(xlsp_source_params_new(resources, value, &params) == XR_COMPILE_RESOURCE_BUDGET);
        CHECK(!params); xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
    }
    xjson_free(value); CHECK(!lsp_json_live && !lsp_json_bytes);
}
static bool cancelled(const void *context) { return *(const bool *)context; }
static void malformed(void) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XlspSourceParams *params = NULL;
    CHECK(xlsp_source_params_new(NULL, NULL, &params) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrJsonValue cycle = {.type = XR_JSON_ARRAY}; XrJsonValue *self = &cycle;
    cycle.as.array.items = &self; cycle.as.array.count = cycle.as.array.capacity = 1;
    size_t blocks = runtime_live, bytes = runtime_bytes;
    CHECK(xlsp_source_params_new(resources, &cycle, &params) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(!params && runtime_live == blocks && runtime_bytes == bytes);
    cycle.as.array.count = -1;
    CHECK(xlsp_source_params_new(resources, &cycle, &params) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrJsonValue bad = {.type = XR_JSON_STRING, .string_len = SIZE_MAX}; bad.as.string = "x";
    CHECK(xlsp_source_params_new(resources, &bad, &params) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(!params && runtime_live == blocks && runtime_bytes == bytes);
    bool cancel = true;
    CHECK(xr_compile_resources_begin_cancel(resources, cancelled, &cancel) == XR_COMPILE_RESOURCE_OK);
    CHECK(xlsp_source_params_new(resources, &bad, &params) == XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(!params && runtime_live == blocks && runtime_bytes == bytes);
    CHECK(xr_compile_resources_end_cancel(resources, cancelled, &cancel) == XR_COMPILE_RESOURCE_CANCELLED);
    xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--full")));
    (void)sdk_fixture_malloc; (void)sdk_fixture_free;
    size_t total = 0;
    for (unsigned i = 0; i < 7; ++i) {
        size_t sites = 0; XrCompileResourceStats stats = normal(i, &sites); total += sites;
        if (argc == 2) faults(i, sites, stats);
        printf("PARAMS case=%u sites=%zu allocated=%llu peak=%llu work=%llu\n", i, sites,
            (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.peak_bytes, (unsigned long long)stats.work);
    }
    malformed();
    printf("Ledger-owned params producer death physical0/0 JSON0/0 cases=7 prefixes=%zu full=%u\n", total, argc == 2 ? 1u : 0u);
    return 0;
}
