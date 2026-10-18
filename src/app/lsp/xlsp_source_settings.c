/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_settings.c - Atomic scalar configuration changes
 *
 * KEY CONCEPT:
 *   Reject malformed settings before touching live configuration or diagnostics.
 *   Only scalar values cross the JSON lifetime boundary.
 */
#include "xlsp_source_settings.h"
#include "../../shared/xr_utf8_core.h"
#include "../../base/xchecks.h"
#include <limits.h>
#include <string.h>

typedef enum SettingsSection {SET_DIAGNOSTICS, SET_COMPLETION, SET_FORMAT, SET_INLAY, SET_SECTION_COUNT} SettingsSection;
struct XlspSourceSettings {
    XrCompileResources *resources;
    XlspConfig values;
    uint32_t present;
};
#define SETTINGS_INTS(X) \
    X(SET_DIAGNOSTICS, "debounceMs", diagnostic_debounce_ms, 0) \
    X(SET_COMPLETION, "maxItems", completion_max_items, 0) \
    X(SET_FORMAT, "tabSize", format_tab_size, 1) \
    X(SET_FORMAT, "maxLineLength", format_max_line_length, 0)
#define SETTINGS_BOOLS(X) \
    X(SET_DIAGNOSTICS, "enabled", diagnostics_enabled) \
    X(SET_FORMAT, "insertSpaces", format_insert_spaces) \
    X(SET_FORMAT, "alignBranchArrows", format_align_branch_arrows) \
    X(SET_FORMAT, "alignEnumValues", format_align_enum_values) \
    X(SET_FORMAT, "alignStructFields", format_align_struct_fields) \
    X(SET_FORMAT, "alignTrailingComments", format_align_trailing_comments) \
    X(SET_FORMAT, "wrapLongLines", format_wrap_long_lines) \
    X(SET_FORMAT, "multilineTrailingComma", format_multiline_trailing_comma) \
    X(SET_INLAY, "typeAnnotations", inlay_hints_type_annotations) \
    X(SET_INLAY, "parameterNames", inlay_hints_parameter_names)
typedef enum SettingsField {
#define INT_ID(section, key, field, minimum) SET_FIELD_##field,
    SETTINGS_INTS(INT_ID)
#undef INT_ID
#define BOOL_ID(section, key, field) SET_FIELD_##field,
    SETTINGS_BOOLS(BOOL_ID)
#undef BOOL_ID
    SET_FIELD_COUNT
} SettingsField;
_Static_assert(SET_FIELD_COUNT < 32, "settings presence fits its mask");
typedef struct SettingsDescriptor {SettingsSection section; const char *key; size_t length; SettingsField field;} SettingsDescriptor;
static const SettingsDescriptor settings_fields[] = {
#define INT_DESCRIPTOR(section, key, field, minimum) {section, key, sizeof(key) - 1, SET_FIELD_##field},
    SETTINGS_INTS(INT_DESCRIPTOR)
#undef INT_DESCRIPTOR
#define BOOL_DESCRIPTOR(section, key, field) {section, key, sizeof(key) - 1, SET_FIELD_##field},
    SETTINGS_BOOLS(BOOL_DESCRIPTOR)
#undef BOOL_DESCRIPTOR
};
static const char *const settings_sections[] = {"diagnostics", "completion", "format", "inlayHints"};
static XrXirStatus settings_work(XrCompileResources *resources, uint64_t units) {
    XrCompileResourceStatus status = xr_compile_resources_work(resources, units);
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}
static int settings_read_byte(void *context, const uint8_t *address, uint8_t *output) {
    if (settings_work(context, 1) != XR_XIR_OK) return 0;
    *output = *address; return 1;
}
static bool settings_object(const XrJsonValue *value) {
    return value && value->type == XR_JSON_OBJECT && value->as.object.count >= 0 &&
        value->as.object.capacity >= value->as.object.count &&
        (!value->as.object.count || value->as.object.members);
}
static XrXirStatus settings_key(XrCompileResources *resources, const XrJsonMember *member) {
    if (!member->key || member->key_len == SIZE_MAX || !member->value) return XR_XIR_BAD_STRUCTURE;
    for (size_t at = 0; at < member->key_len;) {
        XrUtf8Step step;
        if (!xr_utf8_core_decode_step_read((const uint8_t *)member->key + at, member->key_len - at,
                settings_read_byte, resources, &step)) return XR_XIR_BUDGET;
        if (step.error != XR_UTF8_OK || !step.scalar) return XR_XIR_BAD_STRUCTURE;
        at += step.consumed;
    }
    return XR_XIR_OK;
}
static XrXirStatus settings_equal(XrCompileResources *resources, const XrJsonMember *member,
    const char *key, size_t length, bool *equal) {
    XrXirStatus status = settings_work(resources, 1);
    if (status != XR_XIR_OK) return status;
    *equal = false;
    if (member->key_len != length) return XR_XIR_OK;
    status = settings_work(resources, length);
    if (status == XR_XIR_OK) *equal = !memcmp(member->key, key, length);
    return status;
}
static XrXirStatus settings_member(XrCompileResources *resources, const XrJsonValue *object,
    const char *key, size_t length, const XrJsonValue **output) {
    if (!settings_object(object)) return XR_XIR_BAD_STRUCTURE;
    const XrJsonValue *found = NULL;
    for (int i = 0; i < object->as.object.count; ++i) {
        XrXirStatus status = settings_work(resources, sizeof(XrJsonMember));
        if (status != XR_XIR_OK) return status;
        const XrJsonMember *member = &object->as.object.members[i];
        status = settings_key(resources, member); bool equal = false;
        if (status == XR_XIR_OK) status = settings_equal(resources, member, key, length, &equal);
        if (status != XR_XIR_OK) return status;
        if (equal) {if (found) return XR_XIR_BAD_STRUCTURE; found = member->value;}
    }
    *output = found; return XR_XIR_OK;
}
static XrXirStatus settings_number(const XrJsonValue *value, int minimum, int *output) {
    if (value->type != XR_JSON_NUMBER) return XR_XIR_BAD_STRUCTURE;
    if (value->is_integer) {
        if (value->as.integer < minimum || value->as.integer > INT_MAX) return XR_XIR_BAD_STRUCTURE;
        *output = (int)value->as.integer;
    } else {
        double number = value->as.number;
        if (!(number >= minimum && number <= INT_MAX)) return XR_XIR_BAD_STRUCTURE;
        int integer = (int)number;
        if ((double)integer != number) return XR_XIR_BAD_STRUCTURE;
        *output = integer;
    }
    return XR_XIR_OK;
}
static XrXirStatus settings_field(XlspSourceSettings *patch, SettingsField field, const XrJsonValue *value) {
    uint32_t bit = UINT32_C(1) << field;
    if (patch->present & bit) return XR_XIR_BAD_STRUCTURE;
    switch (field) {
#define INT_READ(section, key, field, minimum) \
        case SET_FIELD_##field: { \
            XrXirStatus status = settings_number(value, minimum, &patch->values.field); \
            if (status != XR_XIR_OK) return status; break; }
        SETTINGS_INTS(INT_READ)
#undef INT_READ
#define BOOL_READ(section, key, field) \
        case SET_FIELD_##field: \
            if (value->type != XR_JSON_BOOL) return XR_XIR_BAD_STRUCTURE; \
            patch->values.field = value->as.boolean; break;
        SETTINGS_BOOLS(BOOL_READ)
#undef BOOL_READ
        default: return XR_XIR_BAD_STRUCTURE;
    }
    patch->present |= bit; return XR_XIR_OK;
}
static XrXirStatus settings_section(XrCompileResources *resources, const XrJsonValue *value,
    SettingsSection section, XlspSourceSettings *patch) {
    if (!settings_object(value)) return XR_XIR_BAD_STRUCTURE;
    for (int i = 0; i < value->as.object.count; ++i) {
        XrXirStatus status = settings_work(resources, sizeof(XrJsonMember));
        if (status != XR_XIR_OK) return status;
        const XrJsonMember *member = &value->as.object.members[i];
        status = settings_key(resources, member); if (status != XR_XIR_OK) return status;
        for (size_t f = 0; f < sizeof(settings_fields) / sizeof(*settings_fields); ++f) {
            const SettingsDescriptor *descriptor = &settings_fields[f]; bool equal = false;
            status = settings_work(resources, 1); if (status != XR_XIR_OK) return status;
            if (descriptor->section != section) continue;
            status = settings_equal(resources, member, descriptor->key, descriptor->length, &equal);
            if (status != XR_XIR_OK) return status;
            if (equal) {
                status = settings_work(resources, sizeof(*member->value));
                if (status == XR_XIR_OK) status = settings_field(patch, descriptor->field, member->value);
                if (status != XR_XIR_OK) return status;
                break;
            }
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_settings_read(XrCompileResources *resources, const XrJsonValue *settings,
    XlspSourceSettings **output) {
    if (!resources || !output || *output) return XR_XIR_BAD_STRUCTURE;
    const XrJsonValue *xray = NULL;
    XrXirStatus status = settings_member(resources, settings, "xray", 4, &xray);
    if (status != XR_XIR_OK) return status;
    if (!xray) return XR_XIR_UNRESOLVED;
    if (!settings_object(xray)) return XR_XIR_BAD_STRUCTURE;
    XlspSourceSettings patch = {0}; patch.resources = resources; uint32_t sections = 0;
    for (int i = 0; i < xray->as.object.count; ++i) {
        status = settings_work(resources, sizeof(XrJsonMember)); if (status != XR_XIR_OK) return status;
        const XrJsonMember *member = &xray->as.object.members[i];
        status = settings_key(resources, member); if (status != XR_XIR_OK) return status;
        for (unsigned s = 0; s < SET_SECTION_COUNT; ++s) {
            bool equal = false;
            status = settings_equal(resources, member, settings_sections[s], strlen(settings_sections[s]), &equal);
            if (status != XR_XIR_OK) return status;
            if (equal) {
                if (sections & (UINT32_C(1) << s)) return XR_XIR_BAD_STRUCTURE;
                sections |= UINT32_C(1) << s;
                status = settings_section(resources, member->value, (SettingsSection)s, &patch);
                if (status != XR_XIR_OK) return status;
                break;
            }
        }
    }
    XlspSourceSettings *owner = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_alloc(resources, sizeof(patch), (void **)&owner);
    if (allocated != XR_COMPILE_RESOURCE_OK) return allocated == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
    status = settings_work(resources, sizeof(patch));
    if (status == XR_XIR_OK) { *owner = patch; *output = owner; }
    else xr_compile_resources_free(owner);
    return status;
}
XR_FUNC void xlsp_source_settings_free(XlspSourceSettings *owner) {
    xr_compile_resources_free(owner);
}
XR_FUNC XrXirStatus xlsp_source_settings_apply(const XlspSourceSettings *patch, XrLspServer *server, bool *clear_diagnostics) {
    const uint32_t allowed = (UINT32_C(1) << SET_FIELD_COUNT) - 1;
    if (!patch || !server || !clear_diagnostics || (patch->present & ~allowed)) return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *resources = patch->resources;
#define INT_VALIDATE(section, key, field, minimum) \
    if ((patch->present & (UINT32_C(1) << SET_FIELD_##field)) && patch->values.field < minimum) return XR_XIR_BAD_STRUCTURE;
    SETTINGS_INTS(INT_VALIDATE)
#undef INT_VALIDATE
    bool clear = (patch->present & (UINT32_C(1) << SET_FIELD_diagnostics_enabled)) &&
        server->config.diagnostics_enabled && !patch->values.diagnostics_enabled;
    if (clear && (server->pending_diag_count < 0 || server->pending_diag_capacity < server->pending_diag_count ||
        (server->pending_diag_count && !server->pending_diag))) return XR_XIR_BAD_STRUCTURE;
    uint64_t work = sizeof(XlspConfig) * UINT64_C(2) + sizeof(*patch);
    if (clear) work += (uint64_t)server->pending_diag_count * (sizeof(XrLspDocument *) + sizeof(bool));
    XrXirStatus status = settings_work(resources, work); if (status != XR_XIR_OK) return status;
    XlspConfig candidate = server->config;
#define INT_APPLY(section, key, field, minimum) \
    if (patch->present & (UINT32_C(1) << SET_FIELD_##field)) candidate.field = patch->values.field;
    SETTINGS_INTS(INT_APPLY)
#undef INT_APPLY
#define BOOL_APPLY(section, key, field) \
    if (patch->present & (UINT32_C(1) << SET_FIELD_##field)) candidate.field = patch->values.field;
    SETTINGS_BOOLS(BOOL_APPLY)
#undef BOOL_APPLY
    XR_DCHECK(candidate.ignore_patterns == server->config.ignore_patterns && candidate.log_path == server->config.log_path,
        "scalar settings preserve owned dynamic members");
    if (patch->present) server->config = candidate;
    if (clear) {
        for (int i = 0; i < server->pending_diag_count; ++i)
            if (server->pending_diag[i]) server->pending_diag[i]->diag_pending = false;
        server->pending_diag_count = 0;
    }
    *clear_diagnostics = clear; return XR_XIR_OK;
}
