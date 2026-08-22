/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_builtins.c - Built-in type method definitions for LSP
 *
 * KEY CONCEPT:
 *   This module provides a thin wrapper over xanalyzer_builtins
 *   for LSP-specific output (JSON completions, hover text).
 */

#include "xlsp_builtins.h"
#include "../../frontend/analyzer/xanalyzer_builtins.h"
#include "../../frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../../runtime/value/xtype.h"
#include <string.h>
#include <stdio.h>

typedef struct XlspEnumMetadataPropertySpec {
    XrEnumMetadataKind owner_kind;
    const char *name;
    const char *result_type;
    const char *summary;
    const char *profile;
    const char *effect;
    const char *allocation;
} XlspEnumMetadataPropertySpec;

#define XR_ENUM_METADATA_TYPE(kind, source_name, summary, profile, effect, allocation)
#define XR_ENUM_STATIC_PROPERTY(source_name, result_type, summary, profile, effect, allocation)
#define XR_ENUM_METADATA_PROPERTY(kind, source_name, result_type, summary, profile, effect,        \
                                  allocation)                                                      \
    {XR_ENUM_METADATA_##kind, #source_name, result_type, summary, profile, effect, allocation},
static const XlspEnumMetadataPropertySpec xlsp_enum_metadata_properties[] = {
#include "../../runtime/value/xenum_metadata_api.def"
};
#undef XR_ENUM_METADATA_PROPERTY
#undef XR_ENUM_STATIC_PROPERTY
#undef XR_ENUM_METADATA_TYPE

static const XlspEnumMetadataPropertySpec *xlsp_enum_metadata_property(XrType *type,
                                                                       const char *name) {
    if (!xr_type_is_enum_metadata(type) || !name)
        return NULL;
    XrEnumMetadataKind kind = xr_type_enum_metadata_kind(type);
    size_t count = sizeof(xlsp_enum_metadata_properties) / sizeof(xlsp_enum_metadata_properties[0]);
    for (size_t i = 0; i < count; i++) {
        const XlspEnumMetadataPropertySpec *spec = &xlsp_enum_metadata_properties[i];
        if (spec->owner_kind == kind && strcmp(spec->name, name) == 0)
            return spec;
    }
    return NULL;
}

static void xlsp_enum_metadata_result_label(XrType *type, const XlspEnumMetadataPropertySpec *spec,
                                            char *buf, size_t buf_size) {
    if (!type || !spec || !buf || buf_size == 0)
        return;
    if (strcmp(spec->result_type, "EnumPayloads<E>") != 0) {
        snprintf(buf, buf_size, "%s", spec->result_type);
        return;
    }
    XrType *owner = xr_type_enum_metadata_owner(type);
    snprintf(buf, buf_size, "EnumPayloads<%s>", owner ? xr_type_to_string(owner) : "E");
}

static int xlsp_append_enum_metadata_completions(XrJsonValue *items, XrType *type) {
    if (!items || !xr_type_is_enum_metadata(type))
        return 0;
    int added = 0;
    char receiver[192];
    snprintf(receiver, sizeof(receiver), "%s", xr_type_to_string(type));
    size_t count = sizeof(xlsp_enum_metadata_properties) / sizeof(xlsp_enum_metadata_properties[0]);
    for (size_t i = 0; i < count; i++) {
        const XlspEnumMetadataPropertySpec *spec = &xlsp_enum_metadata_properties[i];
        if (spec->owner_kind != xr_type_enum_metadata_kind(type))
            continue;
        char result[192];
        char detail[512];
        char documentation[768];
        xlsp_enum_metadata_result_label(type, spec, result, sizeof(result));
        snprintf(detail, sizeof(detail), "%s.%s: %s", receiver, spec->name, result);
        snprintf(documentation, sizeof(documentation),
                 "%s\n\nProfile: %s\nEffect: %s\nAllocation: %s", spec->summary, spec->profile,
                 spec->effect, spec->allocation);
        XrJsonValue *item = xjson_new_object();
        xjson_object_set(item, "label", xjson_new_string(spec->name));
        xjson_object_set(item, "kind", xjson_new_number(XLSP_KIND_PROPERTY));
        xjson_object_set(item, "detail", xjson_new_string(detail));
        xjson_object_set(item, "documentation", xjson_new_string(documentation));
        xjson_array_push(items, item);
        added++;
    }
    return added;
}

// ============================================================================
// XrType creation helpers (for type conversion)
// ============================================================================

static XrType *create_type_for_builtin(XlspBuiltinType type) {
    XrType *placeholder = xr_type_new_error(NULL);
    switch (type) {
        case XLSP_TYPE_I64:
            return xr_type_new_int(NULL);
        case XLSP_TYPE_F64:
            return xr_type_new_float(NULL);
        case XLSP_TYPE_STRING:
            return xr_type_new_string(NULL);
        case XLSP_TYPE_BOOL:
            return xr_type_new_bool(NULL);
        case XLSP_TYPE_ARRAY:
            return xr_type_new_array(NULL, placeholder);
        case XLSP_TYPE_MAP:
            return xr_type_new_map(NULL, placeholder, placeholder);
        case XLSP_TYPE_SET:
            return xr_type_new_set(NULL, placeholder);
        case XLSP_TYPE_JSON:
            return xr_type_new_json(NULL);
        case XLSP_TYPE_CHANNEL:
            return xr_type_new_channel(NULL, placeholder);
        case XLSP_TYPE_REGEX:
            return xr_type_new_regex(NULL);
        case XLSP_TYPE_BIGINT:
            return xr_type_new_bigint(NULL);
        case XLSP_TYPE_STRINGBUILDER:
            return xr_type_new_stringbuilder(NULL);
        case XLSP_TYPE_PANIC_INFO:
            return xr_type_new_named_instance(NULL, "PanicInfo");
        case XLSP_TYPE_COROUTINE:
            return xr_type_new_task(NULL, NULL);
        default:
            return NULL;
    }
}

typedef XaBuiltinReceiverKind XlspReceiverKind;
typedef XaBuiltinMethodTypeKind XlspMethodTypeKind;
typedef XaBuiltinReceiverMethodSpec XlspReceiverMethodSpec;

static bool xlsp_type_is_pod_slice_elem(XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static bool xlsp_receiver_matches(XrType *type, XlspReceiverKind receiver) {
    switch (receiver) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return type && type->kind == XR_KIND_INT && !type->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return type && type->kind == XR_KIND_INT && !type->is_nullable &&
                   (type->scalar_rep == XR_NATIVE_U8 || type->scalar_rep == XR_NATIVE_U16 ||
                    type->scalar_rep == XR_NATIVE_U32 || type->scalar_rep == XR_NATIVE_U64 ||
                    type->scalar_rep == XR_NATIVE_USIZE);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(type);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return type && XR_TYPE_IS_ARRAY(type);
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(type);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return type && XR_TYPE_IS_SLICE(type) && type->container.element_type &&
                   xlsp_type_is_pod_slice_elem(type->container.element_type);
    }
    return false;
}

static const XlspReceiverMethodSpec *xlsp_find_receiver_method(XrType *type,
                                                               const char *method_name) {
    if (!type || !method_name)
        return NULL;
    size_t n = xa_builtin_receiver_method_count();
    for (size_t i = 0; i < n; i++) {
        const XlspReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (strcmp(spec->source_name, method_name) == 0 &&
            xlsp_receiver_matches(type, spec->receiver))
            return spec;
    }
    return NULL;
}

static void xlsp_type_label(XrType *type, char *buf, size_t buf_size) {
    const char *s = type ? xr_type_to_string(type) : NULL;
    snprintf(buf, buf_size, "%s", s ? s : "<error>");
}

static void xlsp_receiver_label(XrType *type, const XlspReceiverMethodSpec *spec, char *buf,
                                size_t buf_size) {
    if (!spec) {
        xlsp_type_label(type, buf, buf_size);
        return;
    }
    switch (spec->receiver) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            xlsp_type_label(type, buf, buf_size);
            return;
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            snprintf(buf, buf_size, "Array<u8>");
            return;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            snprintf(buf, buf_size, "Slice<u8>");
            return;
        case XA_BUILTIN_RECEIVER_ARRAY:
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            xlsp_type_label(type, buf, buf_size);
            return;
    }
    xlsp_type_label(type, buf, buf_size);
}

static void xlsp_receiver_elem_label(XrType *receiver, char *buf, size_t buf_size) {
    XrType *elem = receiver ? receiver->container.element_type : NULL;
    xlsp_type_label(elem, buf, buf_size);
}

static const char *xlsp_type_param_label(const XlspReceiverMethodSpec *spec) {
    return spec && spec->type_params == XA_BUILTIN_TYPE_PARAMS_U ? "U" : "T";
}

static void xlsp_component_label(XrType *receiver, const XlspReceiverMethodSpec *spec,
                                 XlspMethodTypeKind kind, char *buf, size_t buf_size) {
    char elem[128];
    const char *tp = xlsp_type_param_label(spec);
    switch (kind) {
        case XA_BUILTIN_TYPE_NONE:
            snprintf(buf, buf_size, "()");
            break;
        case XA_BUILTIN_TYPE_BOOL:
            snprintf(buf, buf_size, "bool");
            break;
        case XA_BUILTIN_TYPE_INT:
            snprintf(buf, buf_size, "i64");
            break;
        case XA_BUILTIN_TYPE_STRING:
            snprintf(buf, buf_size, "string");
            break;
        case XA_BUILTIN_TYPE_U8:
            snprintf(buf, buf_size, "u8");
            break;
        case XA_BUILTIN_TYPE_U8_ARRAY:
            snprintf(buf, buf_size, "Array<u8>");
            break;
        case XA_BUILTIN_TYPE_U8_SLICE:
            snprintf(buf, buf_size, "Slice<u8>");
            break;
        case XA_BUILTIN_TYPE_UNIT:
            snprintf(buf, buf_size, "()");
            break;
        case XA_BUILTIN_TYPE_ENDIAN:
            snprintf(buf, buf_size, "Endian");
            break;
        case XA_BUILTIN_TYPE_PARAM_0:
            snprintf(buf, buf_size, "%s", tp);
            break;
        case XA_BUILTIN_TYPE_ARRAY_OF_PARAM_0:
            snprintf(buf, buf_size, "Array<%s>", tp);
            break;
        case XA_BUILTIN_TYPE_RECEIVER:
            xlsp_type_label(receiver, buf, buf_size);
            break;
        case XA_BUILTIN_TYPE_RECEIVER_ELEM:
            xlsp_receiver_elem_label(receiver, buf, buf_size);
            break;
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_NULLABLE:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "%s?", elem);
            break;
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_TO_BOOL_FN:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "(%s) -> bool", elem);
            break;
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_BOOL_FN:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "(%s, i64) -> bool", elem);
            break;
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_UNIT_FN:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "(%s, i64) -> ()", elem);
            break;
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "(%s, i64) -> %s", elem, tp);
            break;
        case XA_BUILTIN_TYPE_PARAM_0_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "(%s, %s, i64) -> %s", tp, elem, tp);
            break;
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_COMPARE_FN:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "(%s, %s) -> i64", elem, elem);
            break;
        case XA_BUILTIN_TYPE_ITERATOR_OF_RECEIVER_ELEM:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "Iterator<%s>", elem);
            break;
        case XA_BUILTIN_TYPE_ITERATOR_OF_INDEX_RECEIVER_ELEM_TUPLE:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "Iterator<(i64, %s)>", elem);
            break;
        case XA_BUILTIN_TYPE_ARRAY_OF_INDEX_RECEIVER_ELEM_TUPLE:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "Array<(i64, %s)>", elem);
            break;
        case XA_BUILTIN_TYPE_SLICE_OF_PARAM_0:
            snprintf(buf, buf_size, "Slice<%s>", tp);
            break;
        case XA_BUILTIN_TYPE_SLICE_OF_RECEIVER_ELEM:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "Slice<%s>", elem);
            break;
        case XA_BUILTIN_TYPE_PTR_OF_RECEIVER_ELEM:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "Ptr<%s>", elem);
            break;
        case XA_BUILTIN_TYPE_MUT_PTR_OF_RECEIVER_ELEM:
            xlsp_receiver_elem_label(receiver, elem, sizeof(elem));
            snprintf(buf, buf_size, "MutPtr<%s>", elem);
            break;
    }
}

static const char *xlsp_registry_param_name(const XlspReceiverMethodSpec *spec, int index) {
    if (!spec || index < 0 || index >= spec->param_count)
        return "arg";
    XlspMethodTypeKind kind = spec->params[index];
    if (kind == XA_BUILTIN_TYPE_ENDIAN)
        return "endian";
    if (kind == XA_BUILTIN_TYPE_RECEIVER_ELEM || kind == XA_BUILTIN_TYPE_U8 ||
        kind == XA_BUILTIN_TYPE_PARAM_0)
        return "value";
    if (kind == XA_BUILTIN_TYPE_U8_SLICE || kind == XA_BUILTIN_TYPE_SLICE_OF_RECEIVER_ELEM)
        return "source";
    if (kind == XA_BUILTIN_TYPE_INT) {
        if (strcmp(spec->source_name, "rotateLeft") == 0 ||
            strcmp(spec->source_name, "rotateRight") == 0)
            return "count";
        if (strcmp(spec->source_name, "resize") == 0)
            return "length";
        if (strcmp(spec->source_name, "reserve") == 0)
            return "capacity";
        if (strcmp(spec->source_name, "repeatFrom") == 0)
            return index == 0 ? "offset" : (index == 1 ? "distance" : "count");
        return index == 0 ? "index" : (index == 1 ? "start" : "end");
    }
    if (kind == XA_BUILTIN_TYPE_RECEIVER_ELEM_COMPARE_FN ||
        kind == XA_BUILTIN_TYPE_RECEIVER_ELEM_TO_BOOL_FN ||
        kind == XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_BOOL_FN ||
        kind == XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_UNIT_FN ||
        kind == XA_BUILTIN_TYPE_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN ||
        kind == XA_BUILTIN_TYPE_PARAM_0_RECEIVER_ELEM_INDEX_TO_PARAM_0_FN)
        return "fn";
    if (kind == XA_BUILTIN_TYPE_RECEIVER)
        return "array";
    return "arg";
}

static void xlsp_build_registry_signature(XrType *receiver, const XlspReceiverMethodSpec *spec,
                                          char *buf, size_t buf_size) {
    int n = snprintf(buf, buf_size, "%s", spec->source_name);
    if (spec->type_params != XA_BUILTIN_TYPE_PARAMS_NONE) {
        n += snprintf(buf + n, buf_size > (size_t) n ? buf_size - (size_t) n : 0, "<%s>",
                      xlsp_type_param_label(spec));
    }
    n += snprintf(buf + n, buf_size > (size_t) n ? buf_size - (size_t) n : 0, "(");
    for (int i = 0; i < spec->param_count && i < 3; i++) {
        char type_buf[160];
        xlsp_component_label(receiver, spec, spec->params[i], type_buf, sizeof(type_buf));
        if (i > 0)
            n += snprintf(buf + n, buf_size > (size_t) n ? buf_size - (size_t) n : 0, ", ");
        const char *rest = (spec->is_variadic && i == spec->param_count - 1) ? "..." : "";
        const char *optional = i >= spec->min_params ? "?" : "";
        n += snprintf(buf + n, buf_size > (size_t) n ? buf_size - (size_t) n : 0, "%s%s%s: %s",
                      rest, xlsp_registry_param_name(spec, i), optional, type_buf);
    }
    char result_buf[160];
    xlsp_component_label(receiver, spec, spec->result, result_buf, sizeof(result_buf));
    snprintf(buf + n, buf_size > (size_t) n ? buf_size - (size_t) n : 0, "): %s", result_buf);
}

static void xlsp_build_registry_documentation(const XlspReceiverMethodSpec *spec, char *buf,
                                              size_t buf_size) {
    XaBuiltinMethodDocumentationGroup group = xa_builtin_receiver_method_documentation_group(spec);
    XaBuiltinMethodProfileAvailability profile =
        xa_builtin_receiver_method_profile_availability(spec);
    snprintf(
        buf, buf_size,
        "%s\n\nAvailability: %s\nEffect: %s\nAllocation: %s\nUnsafe: %s\nLowering: %s",
        xa_builtin_receiver_documentation_group_label(group),
        xa_builtin_receiver_profile_availability_label(profile),
        xa_builtin_receiver_effect_label(spec ? spec->effect : XA_BUILTIN_EFFECT_READS_RECEIVER),
        xa_builtin_receiver_allocation_label(spec ? spec->allocation
                                                  : XA_BUILTIN_ALLOCATION_NO_HEAP),
        xa_builtin_receiver_unsafe_requirement_label(spec ? spec->unsafe_requirement
                                                          : XA_BUILTIN_UNSAFE_NONE),
        spec && spec->lowering ? spec->lowering : "none");
}

static bool xlsp_completion_has_label(XrJsonValue *items, const char *label) {
    int count = xjson_array_len(items);
    for (int i = 0; i < count; i++) {
        XrJsonValue *item = xjson_array_get(items, i);
        const char *existing = xjson_get_string(item, "label");
        if (existing && strcmp(existing, label) == 0)
            return true;
    }
    return false;
}

static int xlsp_append_receiver_registry_completions(XrJsonValue *items, XrType *type) {
    int added = 0;
    size_t count = xa_builtin_receiver_method_count();
    for (size_t i = 0; i < count; i++) {
        const XlspReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (!xlsp_receiver_matches(type, spec->receiver) ||
            xlsp_completion_has_label(items, spec->source_name))
            continue;

        char signature[512];
        xlsp_build_registry_signature(type, spec, signature, sizeof(signature));

        XrJsonValue *item = xjson_new_object();
        xjson_object_set(item, "label", xjson_new_string(spec->source_name));
        xjson_object_set(item, "kind", xjson_new_number(XLSP_KIND_METHOD));
        xjson_object_set(item, "detail", xjson_new_string(signature));

        char doc[512];
        xlsp_build_registry_documentation(spec, doc, sizeof(doc));
        xjson_object_set(item, "documentation", xjson_new_string(doc));
        xjson_array_push(items, item);
        added++;
    }
    return added;
}

static void xlsp_append_native_completions(XrJsonValue *items, XrType *type) {
    const XaBuiltinMember *members = NULL;
    int count = xa_builtin_get_members_for_type(type, &members);
    for (int i = 0; i < count; i++) {
        const XaBuiltinMember *m = &members[i];
        if (!m->name || xlsp_completion_has_label(items, m->name))
            continue;

        XrJsonValue *item = xjson_new_object();
        xjson_object_set(item, "label", xjson_new_string(m->name));
        int kind = m->is_method ? XLSP_KIND_METHOD : XLSP_KIND_PROPERTY;
        xjson_object_set(item, "kind", xjson_new_number(kind));

        if (m->signature) {
            char detail[256];
            snprintf(detail, sizeof(detail), "%s%s", m->name, m->signature);
            xjson_object_set(item, "detail", xjson_new_string(detail));
        }
        if (m->doc)
            xjson_object_set(item, "documentation", xjson_new_string(m->doc));
        xjson_array_push(items, item);
    }
}

// ============================================================================
// Type name → XlspBuiltinType resolution
// ============================================================================

XlspBuiltinType xlsp_builtin_type_from_name(const char *name) {
    if (!name)
        return XLSP_TYPE_UNRESOLVED;
    if (strcmp(name, TYPE_NAME_STRING) == 0)
        return XLSP_TYPE_STRING;
    if (strcmp(name, TYPE_NAME_ARRAY) == 0)
        return XLSP_TYPE_ARRAY;
    if (strcmp(name, TYPE_NAME_MAP) == 0)
        return XLSP_TYPE_MAP;
    if (strcmp(name, TYPE_NAME_SET) == 0)
        return XLSP_TYPE_SET;
    if (strcmp(name, TYPE_NAME_CHANNEL) == 0)
        return XLSP_TYPE_CHANNEL;
    if (strcmp(name, TYPE_NAME_I64) == 0)
        return XLSP_TYPE_I64;
    if (strcmp(name, TYPE_NAME_F64) == 0)
        return XLSP_TYPE_F64;
    if (strcmp(name, TYPE_NAME_BOOL) == 0)
        return XLSP_TYPE_BOOL;
    if (strcmp(name, TYPE_NAME_JSON) == 0)
        return XLSP_TYPE_JSON;
    if (strcmp(name, TYPE_NAME_BIGINT) == 0)
        return XLSP_TYPE_BIGINT;
    if (strcmp(name, TYPE_NAME_STRINGBUILDER) == 0)
        return XLSP_TYPE_STRINGBUILDER;
    if (strcmp(name, TYPE_NAME_REGEX) == 0)
        return XLSP_TYPE_REGEX;
    if (strcmp(name, TYPE_NAME_PANIC_INFO) == 0)
        return XLSP_TYPE_PANIC_INFO;
    if (strcmp(name, TYPE_NAME_COROUTINE) == 0)
        return XLSP_TYPE_COROUTINE;
    return XLSP_TYPE_UNRESOLVED;
}

// ============================================================================
// Completion and hover
// ============================================================================

XrJsonValue *xlsp_builtin_get_completions(XlspBuiltinType type) {
    XrType *xa_type = create_type_for_builtin(type);
    if (!xa_type)
        return xjson_new_array();
    return xlsp_builtin_get_completions_for_type(xa_type);
}

XrJsonValue *xlsp_builtin_get_completions_for_type(XrType *type) {
    XrJsonValue *items = xjson_new_array();
    if (!type)
        return items;

    xlsp_append_enum_metadata_completions(items, type);
    xlsp_append_receiver_registry_completions(items, type);
    xlsp_append_native_completions(items, type);
    return items;
}

const char *xlsp_builtin_get_hover(XlspBuiltinType type, const char *method_name, char *buf,
                                   size_t buf_size) {
    XrType *xa_type = create_type_for_builtin(type);
    if (!xa_type)
        return NULL;
    return xlsp_builtin_get_hover_for_type(xa_type, method_name, buf, buf_size);
}

const char *xlsp_builtin_get_signature_for_type(XrType *type, const char *method_name, char *buf,
                                                size_t buf_size) {
    if (!type || !method_name || !buf || buf_size == 0)
        return NULL;

    const XlspReceiverMethodSpec *spec = xlsp_find_receiver_method(type, method_name);
    if (spec) {
        xlsp_build_registry_signature(type, spec, buf, buf_size);
        return buf;
    }

    const char *signature = xa_builtin_get_member_signature(type, method_name);
    if (!signature)
        return NULL;

    snprintf(buf, buf_size, "%s%s", method_name, signature);
    return buf;
}

const char *xlsp_builtin_get_hover_for_type(XrType *type, const char *method_name, char *buf,
                                            size_t buf_size) {
    if (!type || !method_name || !buf || buf_size == 0)
        return NULL;

    if (XR_TYPE_IS_ENUM(type) && strcmp(method_name, "variants") == 0) {
        const char *owner = type->enum_type.enum_name ? type->enum_type.enum_name : "E";
        snprintf(
            buf, buf_size,
            "```xray\n%s.variants: EnumVariants<%s>\n```\n\n"
            "Read-only declaration-order variant descriptor view. Iteration yields "
            "`EnumVariant<%s>`, not enum values.\n\nProfile: all\nEffect: pure\nAllocation: none",
            owner, owner, owner);
        return buf;
    }

    const XlspEnumMetadataPropertySpec *enum_property =
        xlsp_enum_metadata_property(type, method_name);
    if (enum_property) {
        char receiver[192];
        char result[192];
        snprintf(receiver, sizeof(receiver), "%s", xr_type_to_string(type));
        xlsp_enum_metadata_result_label(type, enum_property, result, sizeof(result));
        snprintf(buf, buf_size,
                 "```xray\n%s.%s: %s\n```\n\n%s\n\nProfile: %s\nEffect: %s\nAllocation: %s",
                 receiver, enum_property->name, result, enum_property->summary,
                 enum_property->profile, enum_property->effect, enum_property->allocation);
        return buf;
    }

    char signature[512];
    const XlspReceiverMethodSpec *spec = xlsp_find_receiver_method(type, method_name);
    if (spec) {
        char receiver[160];
        xlsp_receiver_label(type, spec, receiver, sizeof(receiver));
        xlsp_build_registry_signature(type, spec, signature, sizeof(signature));
        char doc[512];
        xlsp_build_registry_documentation(spec, doc, sizeof(doc));
        snprintf(buf, buf_size, "```xray\n%s.%s\n```\n\n%s", receiver, signature, doc);
        return buf;
    }

    const char *signature_suffix = xa_builtin_get_member_signature(type, method_name);
    const char *doc = xa_builtin_get_member_doc(type, method_name);
    if (!signature_suffix)
        return NULL;

    char type_name[160];
    xlsp_type_label(type, type_name, sizeof(type_name));
    snprintf(buf, buf_size, "```xray\n%s.%s%s\n```\n\n%s", type_name, method_name, signature_suffix,
             doc ? doc : "");

    return buf;
}

XlspBuiltinType xlsp_infer_literal_type(const char *text) {
    if (!text || !*text)
        return XLSP_TYPE_UNRESOLVED;

    // String literal: "..." or '...'
    if (text[0] == '"' || text[0] == '\'') {
        return XLSP_TYPE_STRING;
    }

    // Array literal: [...]
    if (text[0] == '[') {
        return XLSP_TYPE_ARRAY;
    }

    // Object/Map literal: {...}
    if (text[0] == '{') {
        return XLSP_TYPE_JSON;
    }

    // Number literals
    int has_dot = 0;
    int is_number = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '.')
            has_dot = 1;
        else if (*p < '0' || *p > '9') {
            if (p == text && *p == '-')
                continue;  // Negative
            is_number = 0;
            break;
        }
    }
    if (is_number && *text) {
        return has_dot ? XLSP_TYPE_F64 : XLSP_TYPE_I64;
    }

    // Constructor calls
    if (strncmp(text, TYPE_NAME_ARRAY, 5) == 0)
        return XLSP_TYPE_ARRAY;
    if (strncmp(text, TYPE_NAME_MAP, 3) == 0)
        return XLSP_TYPE_MAP;
    if (strncmp(text, TYPE_NAME_SET, 3) == 0)
        return XLSP_TYPE_SET;
    if (strncmp(text, TYPE_NAME_CHANNEL, 7) == 0)
        return XLSP_TYPE_CHANNEL;
    if (strncmp(text, TYPE_NAME_BIGINT, 6) == 0)
        return XLSP_TYPE_BIGINT;
    if (strncmp(text, TYPE_NAME_STRINGBUILDER, 13) == 0)
        return XLSP_TYPE_STRINGBUILDER;
    if (strncmp(text, TYPE_NAME_REGEX, 5) == 0)
        return XLSP_TYPE_REGEX;

    return XLSP_TYPE_UNRESOLVED;
}
