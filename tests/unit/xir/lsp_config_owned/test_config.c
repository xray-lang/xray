/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_config.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1);}} while (0)
#include "xir_sdk_resource_test.h"

static void *config_native_realloc(void *pointer, size_t bytes) { return xr_realloc(pointer, bytes); }
static char *config_native_strdup(const char *text) { return xr_strdup(text); }
static SdkAllocation app_blocks[256];
static size_t app_attempts, app_fail_at = SIZE_MAX, app_live, app_bytes;
static void app_record(void *pointer, size_t bytes) {
    CHECK(pointer);
    for (size_t i = 0; i < 256; ++i) if (!app_blocks[i].pointer) {
        app_blocks[i] = (SdkAllocation){pointer, bytes}; ++app_live; app_bytes += bytes; return;
    }
    CHECK(false);
}
static void *config_app_malloc(size_t bytes) {
    if (app_attempts++ == app_fail_at) return NULL;
    void *pointer = sdk_fixture_malloc(bytes);
    if (pointer) app_record(pointer, bytes);
    return pointer;
}
static void config_app_free(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0; i < 256; ++i) if (app_blocks[i].pointer == pointer) {
        --app_live; app_bytes -= app_blocks[i].bytes; app_blocks[i] = (SdkAllocation){0};
        sdk_fixture_free(pointer); return;
    }
    CHECK(false);
}
static void *config_app_realloc(void *pointer, size_t bytes) {
    if (app_attempts++ == app_fail_at) return NULL;
    size_t slot = 256;
    if (pointer) for (size_t i = 0; i < 256; ++i) if (app_blocks[i].pointer == pointer) { slot = i; break; }
    CHECK(!pointer || slot != 256);
    void *resized = config_native_realloc(pointer, bytes);
    if (!resized) return NULL;
    if (pointer) {
        app_bytes -= app_blocks[slot].bytes;
        app_blocks[slot] = (SdkAllocation){resized, bytes}; app_bytes += bytes;
    } else app_record(resized, bytes);
    return resized;
}
static char *config_app_strdup(const char *text) {
    if (app_attempts++ == app_fail_at) return NULL;
    char *copy = config_native_strdup(text);
    if (copy) app_record(copy, strlen(text) + 1);
    return copy;
}
#undef xr_malloc
#undef xr_free
#undef xr_realloc
#undef xr_strdup
#define xr_malloc config_app_malloc
#define xr_free config_app_free
#define xr_realloc config_app_realloc
#define xr_strdup config_app_strdup
/* Include the real adapters so every receiving-heap allocation is faultable. */
#include "app/lsp/xlsp_source_config.c"
#include "app/lsp/xlsp_config.c"

static const XrCompileResourceLimits config_limits = {
    UINT64_C(64) * 1024 * 1024, UINT64_C(8) * 1024 * 1024, UINT64_C(128000000)};
static const XrTomlParseLimits parse_limits = {1024 * 1024, 64};
static void initial(XlspConfig *config) {
    *config = (XlspConfig){0};
    config->diagnostics_enabled = true; config->diagnostic_debounce_ms = 300;
    config->completion_max_items = 100; config->format_tab_size = 4;
    config->format_insert_spaces = true; config->inlay_hints_type_annotations = true;
    config->inlay_hints_parameter_names = true; config->log_to_stderr = true;
    config->log_path = config_app_strdup("before.log"); CHECK(config->log_path);
    xlsp_config_add_ignore(config, "seed", true); CHECK(config->ignore_pattern_count == 1);
}
static void unchanged(const XlspConfig *config, const XlspConfig *before) {
    CHECK(!memcmp(config, before, sizeof(*config)));
    CHECK(!strcmp(config->log_path, "before.log"));
    CHECK(config->ignore_pattern_count == 1 && !strcmp(config->ignore_patterns[0].pattern, "seed"));
}
static void expected(const XlspConfig *config, unsigned mode) {
    CHECK(config->ignore_pattern_count == (mode == 1 ? 4 : mode == 4 ? 5 : 1));
    CHECK(config->ignore_patterns[0].is_dir_only && !strcmp(config->ignore_patterns[0].pattern, "seed"));
    if (mode == 0) {
        CHECK(!config->diagnostics_enabled && config->diagnostic_debounce_ms == 500);
        CHECK(config->completion_max_items == 50 && config->format_tab_size == 2 && !config->format_insert_spaces);
    } else if (mode == 1) {
        CHECK(!strcmp(config->ignore_patterns[1].pattern, "node_modules"));
        CHECK(!strcmp(config->ignore_patterns[2].pattern, "build"));
        CHECK(!strcmp(config->ignore_patterns[3].pattern, "*.log") && config->ignore_patterns[3].is_glob);
    } else if (mode == 2) {
        CHECK(!config->inlay_hints_type_annotations && !config->inlay_hints_parameter_names);
    } else if (mode == 3) {
        CHECK(config->format_tab_size == 8 && config->diagnostics_enabled && config->diagnostic_debounce_ms == 300);
    } else if (mode == 4) {
        CHECK(!strcmp(config->log_path, "日志/λ.log") && !config->log_to_stderr);
        CHECK(config->format_align_branch_arrows && config->format_align_enum_values && config->format_align_struct_fields);
        CHECK(config->format_align_trailing_comments && config->format_wrap_long_lines && config->format_multiline_trailing_comma);
        CHECK(config->format_max_line_length == 120);
        CHECK(!strcmp(config->ignore_patterns[1].pattern, "cache") && config->ignore_patterns[1].is_dir_only);
        CHECK(!strcmp(config->ignore_patterns[2].pattern, "?.tmp") && config->ignore_patterns[2].is_glob);
        CHECK(!strcmp(config->ignore_patterns[3].pattern, "/a/b") && config->ignore_patterns[3].is_dir_only);
        CHECK(!strcmp(config->ignore_patterns[4].pattern, "λ"));
    }
}
static XrXirStatus transaction(const char *root, unsigned mode, const XrCompileResourceLimits *limits,
    size_t physical_fail, size_t receiving_fail, XrCompileResourceStats *stats) {
    XlspConfig target; initial(&target); XlspConfig before = target;
    size_t live_before = app_live, bytes_before = app_bytes;
    runtime_attempts = 0; runtime_fail_at = physical_fail;
    XrCompileResources *resources = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(limits, &resources);
    XrXirStatus status = config_resource(opened);
    XlspSourceConfig *owner = NULL;
    if (status == XR_XIR_OK) status = xlsp_source_config_read(resources, root, &target, &parse_limits, &owner);
    if (status == XR_XIR_OK) {
        expected(xlsp_source_config_view(owner), mode);
        app_attempts = 0; app_fail_at = receiving_fail;
        status = xlsp_source_config_apply(owner, &target);
        app_fail_at = SIZE_MAX;
    }
    if (resources && stats) *stats = sdk_stats(resources);
    if (status == XR_XIR_OK) expected(&target, mode);
    else { unchanged(&target, &before); CHECK(app_live == live_before && app_bytes == bytes_before); }
    xlsp_source_config_free(owner); xr_compile_resources_release(resources);
    runtime_fail_at = SIZE_MAX;
    CHECK(runtime_live == 0 && runtime_bytes == 0);
    config_clear(&target, true);
    CHECK(app_live == 0 && app_bytes == 0);
    return status;
}
static void lifetime_and_stale(const char *root, unsigned mode) {
    XlspConfig target; initial(&target);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XlspSourceConfig *owner = NULL;
    char *input = config_app_strdup(root); CHECK(input);
    CHECK(xlsp_source_config_read(resources, input, &target, &parse_limits, &owner) == XR_XIR_OK);
    config_app_free(input); xr_compile_resources_release(resources);
    config_clear(&target, true);
    expected(xlsp_source_config_view(owner), mode);
    initial(&target);
    ++target.completion_max_items;
    XlspConfig before = target; size_t attempts = app_attempts;
    CHECK(xlsp_source_config_apply(owner, &target) == XR_XIR_UNRESOLVED);
    CHECK(!memcmp(&target, &before, sizeof(target)) && app_attempts == attempts);
    --target.completion_max_items;
    CHECK(xlsp_source_config_apply(owner, &target) == XR_XIR_OK);
    expected(&target, mode);
    xlsp_source_config_free(owner);
    config_clear(&target, true);
    CHECK(runtime_live == 0 && runtime_bytes == 0 && app_live == 0 && app_bytes == 0);
}
static void rejected_limits(const char *root) {
    XlspConfig target; initial(&target); XlspConfig before = target;
    XrCompileResources *resources = sdk_ledger(&config_limits);
    XrTomlParseLimits low[] = {{1, 64}, {1024 * 1024, 1}};
    for (unsigned i = 0; i < 2; ++i) {
        XlspSourceConfig *owner = NULL;
        CHECK(xlsp_source_config_read(resources, root, &target, &low[i], &owner) == XR_XIR_BUDGET);
        CHECK(!owner); unchanged(&target, &before);
    }
    XlspSourceConfig *sentinel = NULL;
    CHECK(xlsp_source_config_read(resources, root, &target, &parse_limits, &sentinel) == XR_XIR_OK);
    XlspSourceConfig *saved = sentinel;
    CHECK(xlsp_source_config_read(resources, root, &target, &parse_limits, &sentinel) == XR_XIR_BAD_STRUCTURE);
    CHECK(sentinel == saved); xlsp_source_config_free(sentinel);
    CHECK(xlsp_source_config_apply(NULL, &target) == XR_XIR_BAD_STRUCTURE);
    unchanged(&target, &before);
    xr_compile_resources_release(resources); config_clear(&target, true);
    CHECK(!runtime_live && !runtime_bytes && !app_live && !app_bytes);
}
static void ignore_failures(void) {
    for (size_t fail = 0; fail < 2; ++fail) {
        XlspConfig target = {0}, before = target;
        app_attempts = 0; app_fail_at = fail;
        xlsp_config_add_ignore(&target, "first", true);
        app_fail_at = SIZE_MAX;
        CHECK(!memcmp(&target, &before, sizeof(target)) && !app_live && !app_bytes);
    }
    XlspConfig target = {0};
    for (unsigned i = 0; i < 16; ++i) xlsp_config_add_ignore(&target, "old", false);
    XlspConfig before = target; size_t live = app_live, bytes = app_bytes;
    app_attempts = 0; app_fail_at = 1;
    xlsp_config_add_ignore(&target, "growth", false); app_fail_at = SIZE_MAX;
    CHECK(!memcmp(&target, &before, sizeof(target)) && live == app_live && bytes == app_bytes);
    for (int i = 0; i < 16; ++i) CHECK(!strcmp(target.ignore_patterns[i].pattern, "old"));
    xlsp_config_free_ignores(&target);
    CHECK(!app_live && !app_bytes);
    for (unsigned i = 0; i < 32; ++i) xlsp_config_add_ignore(&target, "*.log", false);
    CHECK(target.ignore_pattern_count == 32 && target.ignore_pattern_capacity >= 32);
    CHECK(xlsp_config_should_ignore(&target, "debug.log", false));
    CHECK(!xlsp_config_should_ignore(&target, "main.xr", false));
    xlsp_config_free_ignores(&target); CHECK(!app_live && !app_bytes);
    xlsp_config_add_ignore(NULL, "x", false); xlsp_config_add_ignore(&target, NULL, false);
    CHECK(!xlsp_config_load_from_toml(NULL, ".") && !xlsp_config_load_from_toml(&target, NULL));
}
static void adapter_failure(const char *root, size_t physical_fail, size_t receiving_fail) {
    XlspConfig target; initial(&target); XlspConfig before = target;
    size_t live = app_live, bytes = app_bytes;
    runtime_attempts = app_attempts = 0;
    runtime_fail_at = physical_fail; app_fail_at = receiving_fail;
    CHECK(!xlsp_config_load_from_toml(&target, root));
    runtime_fail_at = app_fail_at = SIZE_MAX;
    unchanged(&target, &before);
    CHECK(!runtime_live && !runtime_bytes && app_live == live && app_bytes == bytes);
    config_clear(&target, true); CHECK(!app_live && !app_bytes);
}
int main(int argc, char **argv) {
    CHECK(argc == 5);
    unsigned mode = (unsigned)strtoul(argv[2], NULL, 10);
    XrXirStatus wanted = (XrXirStatus)strtoul(argv[3], NULL, 10);
    bool full = !strcmp(argv[4], "full");
    ignore_failures();
    XrCompileResourceStats stats = {0};
    CHECK(transaction(argv[1], mode, &config_limits, SIZE_MAX, SIZE_MAX, &stats) == wanted);
    size_t physical_count = runtime_attempts, receiving_count = wanted == XR_XIR_OK ? app_attempts : 0;
    if (wanted == XR_XIR_OK) {
        lifetime_and_stale(argv[1], mode);
        rejected_limits(argv[1]);
        XlspConfig target; initial(&target);
        CHECK(xlsp_config_load_from_toml(&target, argv[1])); expected(&target, mode);
        config_clear(&target, true); CHECK(!app_live && !app_bytes && !runtime_live);
    } else adapter_failure(argv[1], SIZE_MAX, SIZE_MAX);
    if (full) {
        for (size_t i = 0; i < physical_count; ++i) {
            XrXirStatus status = transaction(argv[1], mode, &config_limits, i, SIZE_MAX, NULL);
            CHECK(status == XR_XIR_OUT_OF_MEMORY);
            adapter_failure(argv[1], i, SIZE_MAX);
        }
        if (wanted == XR_XIR_OK) {
            for (size_t i = 0; i < receiving_count; ++i) {
                CHECK(transaction(argv[1], mode, &config_limits, SIZE_MAX, i, NULL) == XR_XIR_OUT_OF_MEMORY);
                adapter_failure(argv[1], SIZE_MAX, i);
            }
            uint64_t used[] = {stats.allocated_bytes, stats.peak_bytes, stats.work};
            for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
                XrCompileResourceLimits limits = config_limits;
                uint64_t value = delta < 0 ? used[axis] - 1 : used[axis] + (unsigned)delta;
                if (axis == 0) limits.allocated_bytes = value;
                else if (axis == 1) limits.live_bytes = value; else limits.work = value;
                XrXirStatus status = transaction(argv[1], mode, &limits, SIZE_MAX, SIZE_MAX, NULL);
                CHECK(status == (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK));
            }
        }
    }
    CHECK(!runtime_live && !runtime_bytes && !app_live && !app_bytes);
    printf("mode=%u Source=%zu app=%zu full=%d allocated=%llu peak=%llu work=%llu\n", mode,
        physical_count, receiving_count, full, (unsigned long long)stats.allocated_bytes,
        (unsigned long long)stats.peak_bytes, (unsigned long long)stats.work);
    return 0;
}
