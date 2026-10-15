/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_settings.h"
#include "base/xmalloc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1);}} while (0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
static const XrCompileResourceLimits limits = {UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
static char kept_log[] = "kept.log", kept_pattern[] = "kept";
static XlspIgnorePattern kept_ignores[] = {{kept_pattern, false, true}};
static const char *const cases[] = {
    "{\"xray\":{\"diagnostics\":{\"enabled\":false,\"debounceMs\":500},\"completion\":{\"maxItems\":50},\"format\":{\"tabSize\":2,\"maxLineLength\":120,\"insertSpaces\":false,\"alignBranchArrows\":false,\"alignEnumValues\":true,\"alignStructFields\":true,\"alignTrailingComments\":true,\"wrapLongLines\":true,\"multilineTrailingComma\":true},\"inlayHints\":{\"typeAnnotations\":false,\"parameterNames\":false}}}",
    "{\"xray\":{\"diagnostics\":{\"enabled\":true,\"debounceMs\":0},\"completion\":{\"maxItems\":0},\"format\":{\"tabSize\":8,\"maxLineLength\":0},\"inlayHints\":{\"parameterNames\":false}}}",
    "{\"future\":false,\"xray\":{\"unknown\":[1,2],\"format\":{\"future\":true}}}",
    "{}",
    "{\"xray\":{\"diagnostics\":{\"enabled\":0}}}",
    "{\"xray\":{\"diagnostics\":{\"enabled\":false},\"format\":{\"tabSize\":0}}}",
    "{\"xray\":{\"completion\":{\"maxItems\":2147483648}}}",
    "{\"xray\":{\"format\":{\"tabSize\":1.5}}}",
    "{\"xray\":{\"format\":{\"tabSize\":4.0}}}",
    "{\"xray\":{\"format\":null}}",
    "{\"xray\":{\"diagnostics\":{\"enabled\":null}}}",
    "{\"xray\":{}}",
    "{\"xray\":{\"diagnostics\":{}}}",
    "{\"xray\":{\"format\":{\"tabSize\\u0000bad\":1}}}",
    "{\"xray\":{\"format\":{\"tabSize\":1}}}",
    "{\"xray\":{\"format\":{}}}",
    "{\"xray\":{\"format\":{\"tabSize\":1}}}",
    "{\"xray\":{\"format\":{\"tabSize\":1}}}",
    "{\"xray\":{\"format\":{\"tabSize\":2147483647}}}",
    "{\"xray\":{\"completion\":{\"maxItems\":-1}}}",
    "[]",
    "{\"xray\":true}",
    "{\"xray\":{},\"xray\":{}}",
    "{\"xray\":{\"format\":{},\"format\":{}}}",
    "{\"xray\":{\"diagnostics\":{\"enabled\":false,\"enabled\":true}}}",
    "{\"xray\":{\"format\":{\"将来\":true}}}",
};
static XrXirStatus wanted(unsigned mode) {
    if (mode == 0 || mode == 1 || mode == 2 || mode == 8 || mode == 18 || mode == 25) return XR_XIR_OK;
    return mode == 3 ? XR_XIR_UNRESOLVED : XR_XIR_BAD_STRUCTURE;
}
static XrJsonValue *input(unsigned mode) {
    XrJsonValue *root = xjson_parse(cases[mode], strlen(cases[mode])); CHECK(root);
    XrJsonValue *xray = xjson_get_object(root, "xray");
    if (mode == 11) {
        xjson_object_set_new(xray, "diagnostics", xjson_new_object());
        xjson_object_set_new(xray, "diagnostics", xjson_new_object()); CHECK(xray->as.object.count == 2);
    } else if (mode == 12) {
        XrJsonValue *section = xjson_get_object(xray, "diagnostics");
        xjson_object_set_new(section, "enabled", xjson_new_bool(false));
        xjson_object_set_new(section, "enabled", xjson_new_bool(true)); CHECK(section->as.object.count == 2);
    } else if (mode == 14) memcpy(xjson_get_object(xray, "format")->as.object.members[0].key, "\x80", 1);
    else if (mode == 15) xjson_get_object(xray, "format")->as.object.count = -1;
    else if (mode == 16 || mode == 17) {
        XrJsonValue *number = xjson_get(xjson_get_object(xray, "format"), "tabSize");
        number->is_integer = false; number->as.number = mode == 16 ? NAN : INFINITY;
    }
    return root;
}
static void initial(XrLspServer *server, XrLspDocument *a, XrLspDocument *b, XrLspDocument **pending) {
    memset(server, 0, sizeof(*server)); memset(a, 0, sizeof(*a)); memset(b, 0, sizeof(*b));
    server->config.diagnostics_enabled = true; server->config.diagnostic_debounce_ms = 300;
    server->config.completion_max_items = 100; server->config.format_tab_size = 4;
    server->config.format_max_line_length = 80; server->config.format_insert_spaces = true;
    server->config.format_align_branch_arrows = true; server->config.inlay_hints_type_annotations = true;
    server->config.inlay_hints_parameter_names = true; server->config.log_to_stderr = true;
    server->config.log_path = kept_log; server->config.ignore_patterns = kept_ignores;
    server->config.ignore_pattern_count = server->config.ignore_pattern_capacity = 1;
    a->diag_pending = b->diag_pending = true; pending[0] = a; pending[1] = NULL; pending[2] = b;
    server->pending_diag = pending; server->pending_diag_count = server->pending_diag_capacity = 3;
}
static void expected(const XrLspServer *server, const XrLspDocument *a, const XrLspDocument *b, unsigned mode, bool clear) {
    CHECK(server->config.log_to_stderr && server->config.log_path == kept_log && !strcmp(server->config.log_path, "kept.log"));
    CHECK(server->config.ignore_patterns == kept_ignores && server->config.ignore_pattern_count == 1 && server->config.ignore_pattern_capacity == 1);
    CHECK(server->config.ignore_patterns[0].pattern == kept_pattern && !strcmp(kept_pattern, "kept"));
    CHECK(clear == (mode == 0));
    CHECK(server->pending_diag_count == (clear ? 0 : 3)); CHECK(a->diag_pending == !clear && b->diag_pending == !clear);
    if (mode == 0) {
        CHECK(!server->config.diagnostics_enabled && server->config.diagnostic_debounce_ms == 500);
        CHECK(server->config.completion_max_items == 50 && server->config.format_tab_size == 2);
        CHECK(server->config.format_max_line_length == 120 && !server->config.format_insert_spaces);
        CHECK(!server->config.format_align_branch_arrows && server->config.format_align_enum_values);
        CHECK(server->config.format_align_struct_fields && server->config.format_align_trailing_comments);
        CHECK(server->config.format_wrap_long_lines && server->config.format_multiline_trailing_comma);
        CHECK(!server->config.inlay_hints_type_annotations && !server->config.inlay_hints_parameter_names);
    } else if (mode == 1) {
        CHECK(server->config.diagnostics_enabled && server->config.diagnostic_debounce_ms == 0);
        CHECK(server->config.completion_max_items == 0 && server->config.format_tab_size == 8);
        CHECK(server->config.format_max_line_length == 0 && !server->config.inlay_hints_parameter_names);
        CHECK(server->config.format_insert_spaces && server->config.inlay_hints_type_annotations);
    } else if (mode == 18) CHECK(server->config.format_tab_size == INT_MAX);
    else CHECK(server->config.diagnostics_enabled && server->config.diagnostic_debounce_ms == 300 && server->config.format_tab_size == 4);
}
static XrXirStatus once(unsigned mode, const XrCompileResourceLimits *caps, size_t fail, XrCompileResourceStats *stats) {
    XrJsonValue *json = input(mode); XrLspServer server, before; XrLspDocument a, b; XrLspDocument *pending[3];
    initial(&server, &a, &b, pending); memcpy(&before, &server, sizeof(before)); bool clear = true;
    size_t json_calls = lsp_json_calls; runtime_attempts = 0; runtime_fail_at = fail;
    XrCompileResources *resources = NULL; XrCompileResourceStatus opened = xr_compile_resources_new(caps, &resources);
    XrXirStatus status = opened == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : opened == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    XlspSourceSettings *owner = NULL;
    if (status == XR_XIR_OK) status = xlsp_source_settings_read(resources, json, &owner);
    if (mode == 15) xjson_get_object(xjson_get_object(json, "xray"), "format")->as.object.count = 0;
    xjson_free(json); CHECK(!lsp_json_live && !lsp_json_bytes && lsp_json_calls == json_calls);
    if (status == XR_XIR_OK) {
        CHECK(owner);
        status = xlsp_source_settings_apply(owner, &server, &clear);
    }
    CHECK(lsp_json_calls == json_calls && !lsp_json_live && !lsp_json_bytes);
    if (stats && resources) *stats = sdk_stats(resources);
    if (status == XR_XIR_OK) expected(&server, &a, &b, mode, clear);
    else {CHECK(!memcmp(&server, &before, sizeof(server)) && a.diag_pending && b.diag_pending && clear);}
    xlsp_source_settings_free(owner); xr_compile_resources_release(resources); runtime_fail_at = SIZE_MAX;
    CHECK(!runtime_live && !runtime_bytes); return status;
}
static void lifetime(void) {
    XrJsonValue *json = input(0); XrCompileResources *resources = sdk_ledger(&limits); XlspSourceSettings *owner = NULL;
    CHECK(xlsp_source_settings_read(resources, json, &owner) == XR_XIR_OK);
    XlspSourceSettings *occupied = owner; size_t calls = runtime_attempts;
    CHECK(xlsp_source_settings_read(resources, json, &occupied) == XR_XIR_BAD_STRUCTURE && occupied == owner && runtime_attempts == calls);
    xjson_free(json);
    xr_compile_resources_release(resources);
    XrLspServer server; XrLspDocument a, b; XrLspDocument *pending[3]; initial(&server, &a, &b, pending);
    bool clear = false; server.pending_diag_capacity = 2; XrLspServer before; memcpy(&before, &server, sizeof(before));
    CHECK(xlsp_source_settings_apply(owner, &server, &clear) == XR_XIR_BAD_STRUCTURE && !clear);
    CHECK(!memcmp(&server, &before, sizeof(server)) && a.diag_pending && b.diag_pending);
    server.pending_diag_capacity = 3;
    CHECK(xlsp_source_settings_apply(owner, &server, &clear) == XR_XIR_OK); expected(&server, &a, &b, 0, clear);
    CHECK(xlsp_source_settings_apply(owner, &server, &clear) == XR_XIR_OK && !clear);
    occupied = owner; calls = runtime_attempts;
    CHECK(xlsp_source_settings_read(NULL, NULL, &occupied) == XR_XIR_BAD_STRUCTURE && occupied == owner && runtime_attempts == calls);
    xlsp_source_settings_free(owner); CHECK(!runtime_live && !runtime_bytes && !lsp_json_live && !lsp_json_bytes);
}
int main(void) {
    bool full = getenv("XR_LSP_SETTINGS_NORMAL_ONLY") == NULL; (void)sdk_unlimited;
    for (unsigned mode = 0; mode < sizeof(cases) / sizeof(*cases); ++mode) {
        XrCompileResourceStats stats = {0}; CHECK(once(mode, &limits, SIZE_MAX, &stats) == wanted(mode)); size_t sites = runtime_attempts;
        if (full) {
            for (size_t i = 0; i < sites; ++i) CHECK(once(mode, &limits, i, NULL) == XR_XIR_OUT_OF_MEMORY);
            if (wanted(mode) == XR_XIR_OK) for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
                XrCompileResourceLimits exact = limits; uint64_t amount = axis == 0 ? stats.allocated_bytes : axis == 1 ? stats.peak_bytes : stats.work;
                uint64_t value = delta < 0 ? amount - 1 : amount + (unsigned)delta;
                if (!axis) exact.allocated_bytes = value; else if (axis == 1) exact.live_bytes = value; else exact.work = value;
                CHECK(once(mode, &exact, SIZE_MAX, NULL) == (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK));
            }
        }
        printf("Settings mode=%u SourceFI=%zu JSONallocations=0 full=%u all-or-none state/queue physical0/0\n", mode, sites, full ? 1u : 0u);
    }
    lifetime(); return 0;
}
