/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_config.c - Owned configuration transactions
 *
 * KEY CONCEPT:
 *   Parsing cannot mutate live settings. A complete owned candidate and its
 *   captured base survive producers and are published without partial changes.
 */
#include "xlsp_source_config.h"
#include "../../base/xfileio.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../os/os_fs.h"
#include <limits.h>
#include <string.h>

#define CONFIG_INTS(X) \
    X(diagnostic_debounce_ms, 0) X(completion_max_items, 0) \
    X(format_tab_size, 1) X(format_max_line_length, 0)
#define CONFIG_BOOLS(X) \
    X(diagnostics_enabled) X(format_insert_spaces) X(format_align_branch_arrows) \
    X(format_align_enum_values) X(format_align_struct_fields) \
    X(format_align_trailing_comments) X(format_wrap_long_lines) \
    X(format_multiline_trailing_comma) X(inlay_hints_type_annotations) \
    X(inlay_hints_parameter_names) X(log_to_stderr)

struct XlspSourceConfig {
    XrCompileResources *resources;
    XlspConfig base, next;
};

static XrXirStatus config_resource(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus config_io(XrOsIoStatus status) {
    return status == XR_OS_IO_OK ? XR_XIR_OK : status == XR_OS_IO_OVERFLOW ? XR_XIR_BUDGET :
        status == XR_OS_IO_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status == XR_OS_IO_NOT_FOUND ? XR_XIR_UNRESOLVED :
        status == XR_OS_IO_UNSUPPORTED ? XR_XIR_UNSUPPORTED :
        status == XR_OS_IO_BAD_ARGUMENT ? XR_XIR_BAD_STRUCTURE : XR_XIR_IO;
}
static XrXirStatus config_toml(XrTomlParseStatus status) {
    return status == XR_TOML_PARSE_OK ? XR_XIR_OK :
        status == XR_TOML_PARSE_OVERFLOW || status == XR_TOML_PARSE_LIMIT ? XR_XIR_BUDGET :
        status == XR_TOML_PARSE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status == XR_TOML_PARSE_IO ? XR_XIR_IO : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus config_work(XrCompileResources *resources, uint64_t bytes) {
    return config_resource(xr_compile_resources_work(resources, bytes));
}
static XrXirStatus config_length(XrCompileResources *resources, const char *text, size_t *length) {
    for (size_t i = 0;; ++i) {
        XrXirStatus status = config_work(resources, 1);
        if (status != XR_XIR_OK) return status;
        if (!text[i]) { *length = i; return XR_XIR_OK; }
        if (i == SIZE_MAX - 1) return XR_XIR_BUDGET;
    }
}
static void config_release(void *pointer, bool application) {
    if (application) xr_free(pointer); else xr_compile_resources_free(pointer);
}
static void config_clear(XlspConfig *config, bool application) {
    for (int i = 0; i < config->ignore_pattern_count; ++i)
        config_release(config->ignore_patterns[i].pattern, application);
    config_release(config->ignore_patterns, application);
    config_release(config->log_path, application);
    config->ignore_patterns = NULL;
    config->ignore_pattern_count = config->ignore_pattern_capacity = 0;
    config->log_path = NULL;
}
static XrXirStatus config_allocate(XrCompileResources *resources, size_t bytes,
    bool application, void **output) {
    if (!application) return config_resource(xr_compile_resources_alloc(resources, bytes, output));
    XrXirStatus status = config_work(resources, 1);
    if (status != XR_XIR_OK) return status;
    void *block = xr_malloc(bytes);
    if (!block) return XR_XIR_OUT_OF_MEMORY;
    *output = block;
    return XR_XIR_OK;
}
static XrXirStatus config_string(XrCompileResources *resources, const char *text,
    size_t length, bool application, char **output) {
    if (length == SIZE_MAX) return XR_XIR_BUDGET;
    char *copy = NULL;
    XrXirStatus status = config_allocate(resources, length + 1, application, (void **)&copy);
    if (status != XR_XIR_OK) return status;
    status = config_work(resources, length + 1);
    if (status != XR_XIR_OK) { config_release(copy, application); return status; }
    memcpy(copy, text, length);
    copy[length] = 0;
    *output = copy;
    return XR_XIR_OK;
}
static bool config_valid(const XlspConfig *config) {
    return config && config->ignore_pattern_count >= 0 &&
        config->ignore_pattern_capacity >= config->ignore_pattern_count &&
        (!config->ignore_pattern_count || config->ignore_patterns);
}
static XrXirStatus config_clone(XrCompileResources *resources, const XlspConfig *source,
    bool application, XlspConfig *output) {
    XrXirStatus status = config_work(resources, sizeof(*source));
    if (status != XR_XIR_OK) return status;
    XlspConfig copy = *source;
    copy.ignore_patterns = NULL;
    copy.ignore_pattern_count = copy.ignore_pattern_capacity = 0;
    copy.log_path = NULL;
    size_t length = 0;
    if (source->log_path) {
        status = config_length(resources, source->log_path, &length);
        if (status == XR_XIR_OK)
            status = config_string(resources, source->log_path, length, application, &copy.log_path);
        if (status != XR_XIR_OK) goto done;
    }
    if ((size_t)source->ignore_pattern_count > SIZE_MAX / sizeof(*copy.ignore_patterns)) {
        status = XR_XIR_BUDGET; goto done;
    }
    size_t bytes = (size_t)source->ignore_pattern_count * sizeof(*copy.ignore_patterns);
    if (bytes) {
        status = config_allocate(resources, bytes, application, (void **)&copy.ignore_patterns);
        if (status != XR_XIR_OK) goto done;
        copy.ignore_pattern_capacity = source->ignore_pattern_count;
    }
    XR_DCHECK(!source->ignore_pattern_count || copy.ignore_patterns, "complete pattern storage");
    for (int i = 0; i < source->ignore_pattern_count; ++i) {
        const XlspIgnorePattern *pattern = &source->ignore_patterns[i];
        if (!pattern->pattern) { status = XR_XIR_BAD_STRUCTURE; goto done; }
        char *text = NULL;
        status = config_length(resources, pattern->pattern, &length);
        if (status == XR_XIR_OK)
            status = config_string(resources, pattern->pattern, length, application, &text);
        if (status != XR_XIR_OK) goto done;
        status = config_work(resources, sizeof(*pattern));
        if (status != XR_XIR_OK) { config_release(text, application); goto done; }
        copy.ignore_patterns[i] = (XlspIgnorePattern){text, pattern->is_glob, pattern->is_dir_only};
        ++copy.ignore_pattern_count;
    }
    *output = copy;
    return XR_XIR_OK;
done:
    config_clear(&copy, application);
    return status;
}
static XrXirStatus config_dom_string(XrCompileResources *resources, XrTomlValue *value) {
    if (!xtoml_is_string(value)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = config_work(resources, value->string_length);
    if (status != XR_XIR_OK) return status;
    return memchr(value->as.string, 0, value->string_length) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
static XrXirStatus config_ignore(XlspSourceConfig *owner, XrTomlValue *array) {
    if (!xtoml_is_array(array)) return XR_XIR_BAD_STRUCTURE;
    XlspConfig *config = &owner->next;
    for (int i = 0; i < xtoml_array_len(array); ++i) {
        XrTomlValue *value = xtoml_array_get(array, i);
        XrXirStatus status = config_dom_string(owner->resources, value);
        if (status != XR_XIR_OK) return status;
        size_t length = value->string_length;
        if (!length) continue;
        if (length > UINT64_MAX / 3) return XR_XIR_BUDGET;
        status = config_work(owner->resources, length * UINT64_C(3));
        if (status != XR_XIR_OK) return status;
        const char *text = value->as.string;
        bool glob = memchr(text, '*', length) || memchr(text, '?', length);
        bool directory = (!memchr(text, '.', length) && !memchr(text, '*', length)) || text[length - 1] == '/';
        if (directory && text[length - 1] == '/') --length;
        if (config->ignore_pattern_count == INT_MAX ||
            (size_t)config->ignore_pattern_count + 1 > SIZE_MAX / sizeof(*config->ignore_patterns))
            return XR_XIR_BUDGET;
        char *copy = NULL;
        status = config_string(owner->resources, text, length, false, &copy);
        if (status != XR_XIR_OK) return status;
        void *patterns = config->ignore_patterns;
        status = config_resource(xr_compile_resources_resize(owner->resources, &patterns,
            ((size_t)config->ignore_pattern_count + 1) * sizeof(*config->ignore_patterns)));
        if (status != XR_XIR_OK) { xr_compile_resources_free(copy); return status; }
        config->ignore_patterns = patterns;
        status = config_work(owner->resources, sizeof(XlspIgnorePattern));
        if (status != XR_XIR_OK) { xr_compile_resources_free(copy); return status; }
        config->ignore_patterns[config->ignore_pattern_count++] = (XlspIgnorePattern){copy, glob, directory};
        config->ignore_pattern_capacity = config->ignore_pattern_count;
    }
    return XR_XIR_OK;
}
static XrXirStatus config_parse(XlspSourceConfig *owner, XrTomlValue *table) {
    if (!xtoml_is_table(table)) return XR_XIR_BAD_STRUCTURE;
    XrTomlValue *value = NULL;
    XrXirStatus status;
#define READ_INT(field, minimum) \
    status = config_toml(xtoml_owned_get(table, #field, &value)); \
    if (status != XR_XIR_OK) return status; \
    if (value) { \
        if (!xtoml_is_integer(value) || value->as.integer < minimum || value->as.integer > INT_MAX) \
            return XR_XIR_BAD_STRUCTURE; \
        owner->next.field = (int)value->as.integer; \
    }
    CONFIG_INTS(READ_INT)
#undef READ_INT
#define READ_BOOL(field) \
    status = config_toml(xtoml_owned_get(table, #field, &value)); \
    if (status != XR_XIR_OK) return status; \
    if (value) { if (!xtoml_is_bool(value)) return XR_XIR_BAD_STRUCTURE; owner->next.field = value->as.boolean; }
    CONFIG_BOOLS(READ_BOOL)
#undef READ_BOOL
    status = config_toml(xtoml_owned_get(table, "log_path", &value));
    if (status != XR_XIR_OK) return status;
    if (value) {
        status = config_dom_string(owner->resources, value);
        if (status != XR_XIR_OK) return status;
        char *copy = NULL;
        status = config_string(owner->resources, value->as.string, value->string_length, false, &copy);
        if (status != XR_XIR_OK) return status;
        xr_compile_resources_free(owner->next.log_path);
        owner->next.log_path = copy;
    }
    status = config_toml(xtoml_owned_get(table, "ignore", &value));
    return status != XR_XIR_OK || !value ? status : config_ignore(owner, value);
}
XR_FUNC void xlsp_source_config_free(XlspSourceConfig *owner) {
    if (!owner) return;
    config_clear(&owner->base, false);
    config_clear(&owner->next, false);
    xr_compile_resources_free(owner);
}
XR_FUNC const XlspConfig *xlsp_source_config_view(const XlspSourceConfig *owner) {
    return owner ? &owner->next : NULL;
}
XR_FUNC XrXirStatus xlsp_source_config_read(XrCompileResources *resources,
    const char *root_path, const XlspConfig *current,
    const XrTomlParseLimits *limits, XlspSourceConfig **output) {
    if (!resources || !root_path || !root_path[0] || !config_valid(current) || !limits || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    XlspSourceConfig *owner = NULL;
    char *path = NULL;
    uint8_t *bytes = NULL;
    size_t length = 0;
    XrTomlValue *root = NULL, *table = NULL;
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    XrXirStatus status = config_resource(xr_compile_resources_calloc(resources, 1, sizeof(*owner), (void **)&owner));
    if (status != XR_XIR_OK) goto done;
    owner->resources = resources;
    status = config_clone(resources, current, false, &owner->base);
    if (status != XR_XIR_OK) goto done;
    status = config_clone(resources, current, false, &owner->next);
    if (status != XR_XIR_OK) goto done;
    status = config_io(xr_path_join_owned(&policy, root_path, "xray.toml", &path));
    if (status != XR_XIR_OK) goto done;
    status = config_io(xr_os_io_read_regular_file(&policy, path, limits->input_bytes, &bytes, &length));
    if (status != XR_XIR_OK) goto done;
    status = config_toml(xtoml_parse_owned(&policy, (const char *)bytes, length, limits, &root));
    if (status != XR_XIR_OK) goto done;
    status = config_toml(xtoml_owned_get(root, "lsp", &table));
    if (status != XR_XIR_OK) goto done;
    if (!table) { status = XR_XIR_UNRESOLVED; goto done; }
    status = config_parse(owner, table);
    XR_DCHECK(status != XR_XIR_OK || config_valid(&owner->next), "validated owned configuration");
    if (status == XR_XIR_OK) { *output = owner; owner = NULL; }
done:
    xtoml_owned_free(root);
    xr_compile_resources_free(bytes);
    xr_compile_resources_free(path);
    xlsp_source_config_free(owner);
    return status;
}
static XrXirStatus config_same_string(XrCompileResources *resources, const char *left,
    const char *right, bool *same) {
    *same = left == NULL && right == NULL;
    if (!left || !right) return XR_XIR_OK;
    for (size_t i = 0;; ++i) {
        XrXirStatus status = config_work(resources, 2);
        if (status != XR_XIR_OK) return status;
        if (left[i] != right[i]) return XR_XIR_OK;
        if (!left[i]) { *same = true; return XR_XIR_OK; }
        if (i == SIZE_MAX - 1) return XR_XIR_BUDGET;
    }
}
static XrXirStatus config_matches(const XlspSourceConfig *owner, const XlspConfig *target) {
    const XlspConfig *base = &owner->base;
    XrXirStatus status = config_work(owner->resources, sizeof(*target));
    if (status != XR_XIR_OK) return status;
#define SAME_INT(field, minimum) if (base->field != target->field) return XR_XIR_UNRESOLVED;
    CONFIG_INTS(SAME_INT)
#undef SAME_INT
#define SAME_BOOL(field) if (base->field != target->field) return XR_XIR_UNRESOLVED;
    CONFIG_BOOLS(SAME_BOOL)
#undef SAME_BOOL
    if (base->ignore_pattern_count != target->ignore_pattern_count) return XR_XIR_UNRESOLVED;
    bool same = false;
    status = config_same_string(owner->resources, base->log_path, target->log_path, &same);
    if (status != XR_XIR_OK || !same) return status != XR_XIR_OK ? status : XR_XIR_UNRESOLVED;
    for (int i = 0; i < base->ignore_pattern_count; ++i) {
        status = config_work(owner->resources, sizeof(XlspIgnorePattern));
        if (status != XR_XIR_OK) return status;
        const XlspIgnorePattern *a = &base->ignore_patterns[i], *b = &target->ignore_patterns[i];
        if (!b->pattern || a->is_glob != b->is_glob || a->is_dir_only != b->is_dir_only)
            return XR_XIR_UNRESOLVED;
        status = config_same_string(owner->resources, a->pattern, b->pattern, &same);
        if (status != XR_XIR_OK || !same) return status != XR_XIR_OK ? status : XR_XIR_UNRESOLVED;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_config_apply(const XlspSourceConfig *owner, XlspConfig *target) {
    if (!owner || !config_valid(target)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = config_matches(owner, target);
    if (status != XR_XIR_OK) return status;
    XlspConfig candidate = {0};
    status = config_clone(owner->resources, &owner->next, true, &candidate);
    if (status != XR_XIR_OK) return status;
    status = config_work(owner->resources, sizeof(candidate));
    if (status != XR_XIR_OK) { config_clear(&candidate, true); return status; }
    XR_DCHECK(candidate.ignore_pattern_count == owner->next.ignore_pattern_count, "complete configuration candidate");
    XlspConfig old = *target;
    *target = candidate;
    config_clear(&old, true);
    return XR_XIR_OK;
}
