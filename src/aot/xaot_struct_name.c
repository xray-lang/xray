/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_struct_name.c - shared AOT native struct C type naming
 */

#include "xaot_struct_name.h"
#include "xaot_layout_gen.h"
#include "../ir/xi_module.h"
#include "../runtime/value/xtype.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool struct_c_identifier_is_valid(const char *s) {
    if (!s || !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_'))
        return false;
    for (const char *p = s + 1; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_'))
            return false;
    }
    return true;
}

static bool struct_c_identifier_is_reserved(const char *s) {
    static const char *const reserved[] = {
        "alignas",       "alignof",  "auto",           "bool",          "break",    "case",
        "char",          "const",    "continue",       "default",       "do",       "double",
        "else",          "enum",     "extern",         "false",         "float",    "for",
        "goto",          "if",       "inline",         "int",           "long",     "register",
        "restrict",      "return",   "short",          "signed",        "sizeof",   "static",
        "static_assert", "struct",   "switch",         "thread_local",  "true",     "typedef",
        "union",         "unsigned", "void",           "volatile",      "while",    "_Alignas",
        "_Alignof",      "_Atomic",  "_Bool",          "_Complex",      "_Generic", "_Imaginary",
        "_Noreturn",     "_Pragma",  "_Static_assert", "_Thread_local",
    };
    if (!s)
        return true;
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(s, reserved[i]) == 0)
            return true;
    }
    return false;
}

static bool struct_source_field_name_is_usable(const XrAggregateLayout *sl, int64_t idx) {
    if (!sl || idx < 0 || idx >= sl->field_count || !sl->field_names)
        return false;
    const char *name = sl->field_names[idx];
    if (!struct_c_identifier_is_valid(name) || struct_c_identifier_is_reserved(name))
        return false;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        if ((int64_t) i == idx)
            continue;
        const char *other = sl->field_names[i];
        if (other && struct_c_identifier_is_valid(other) &&
            !struct_c_identifier_is_reserved(other) && strcmp(name, other) == 0)
            return false;
    }
    return true;
}

static void struct_field_c_name(const XrAggregateLayout *sl, int64_t idx, char *buf,
                                size_t buflen) {
    if (!buf || buflen == 0)
        return;
    if (struct_source_field_name_is_usable(sl, idx)) {
        const char *name = sl->field_names[idx];
        if (strlen(name) < buflen) {
            snprintf(buf, buflen, "%s", name);
            return;
        }
    }
    snprintf(buf, buflen, "f%d", (int) idx);
}

static uint64_t struct_hash_string(uint64_t h, const char *s) {
    if (!s)
        return h;
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    h ^= UINT64_C(0xff);
    h *= UINT64_C(1099511628211);
    return h;
}

static void c_ident_part(char *buf, size_t buflen, const char *raw) {
    size_t j = 0;
    if (!buf || buflen == 0)
        return;
    if (!raw || !raw[0]) {
        snprintf(buf, buflen, "_");
        return;
    }
    char first = raw[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_') &&
        j + 1 < buflen)
        buf[j++] = '_';
    for (const char *p = raw; *p && j + 1 < buflen; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            buf[j++] = c;
        else
            buf[j++] = '_';
    }
    buf[j] = '\0';
}

static uint64_t struct_layout_hash_depth(const XrAggregateLayout *sl, int depth) {
    uint64_t h = UINT64_C(1469598103934665603);
    if (!sl)
        return h;
    if (depth > 8)
        return h ^ UINT64_C(0x9e3779b97f4a7c15);
    h ^= sl->field_count;
    h *= UINT64_C(1099511628211);
    h ^= sl->kind;
    h *= UINT64_C(1099511628211);
    h ^= sl->explicit_align;
    h *= UINT64_C(1099511628211);
    for (uint16_t i = 0; i < sl->field_count; i++) {
        char fname[128];
        struct_field_c_name(sl, i, fname, sizeof(fname));
        h = struct_hash_string(h, fname);
        h ^= sl->fields[i].native_type;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].elem_native_type;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].elem_count;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].size;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].sub_layout_id;
        h *= UINT64_C(1099511628211);
        if (sl->fields[i].native_type == XR_NATIVE_NESTED_AGGREGATE) {
            h ^= struct_layout_hash_depth(sl->fields[i].sub_layout, depth + 1);
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

XR_FUNC uint64_t xaot_struct_layout_hash(const XrAggregateLayout *sl) {
    return struct_layout_hash_depth(sl, 0);
}

XR_FUNC void xaot_struct_c_type_name(char *buf, size_t buflen, const char *prefix,
                                     const XrAggregateLayout *sl) {
    if (!buf || buflen == 0)
        return;
    snprintf(buf, buflen, "xrt_struct_%s_%016" PRIx64, prefix ? prefix : "mod",
             xaot_struct_layout_hash(sl));
}

XR_FUNC uint64_t xaot_enum_data_hash(const XiEnumData *ed) {
    uint64_t h = UINT64_C(1469598103934665603);
    if (!ed)
        return h;
    h = struct_hash_string(h, ed->name);
    h ^= ed->member_count;
    h *= UINT64_C(1099511628211);
    h ^= (uint64_t) (ed->max_payload < 0 ? 0 : ed->max_payload);
    h *= UINT64_C(1099511628211);
    h ^= ed->is_adt ? UINT64_C(0xad7) : UINT64_C(0xe);
    h *= UINT64_C(1099511628211);
    for (uint32_t i = 0; i < ed->member_count; i++) {
        const XiEnumMemberData *member = ed->members ? &ed->members[i] : NULL;
        h = struct_hash_string(h, member ? member->name : NULL);
        h ^= member ? member->ordinal : i;
        h *= UINT64_C(1099511628211);
        h ^= (uint64_t) (member && member->payload_count > 0 ? member->payload_count : 0);
        h *= UINT64_C(1099511628211);
    }
    return h;
}

XR_FUNC void xaot_enum_c_type_name(char *buf, size_t buflen, const char *prefix,
                                   const XiEnumData *ed) {
    char pbuf[96];
    char ebuf[96];
    if (!buf || buflen == 0)
        return;
    c_ident_part(pbuf, sizeof(pbuf), prefix ? prefix : "mod");
    c_ident_part(ebuf, sizeof(ebuf), ed && ed->name ? ed->name : "Enum");
    snprintf(buf, buflen, "xrt_enum_%s_%s_%016" PRIx64, pbuf, ebuf, xaot_enum_data_hash(ed));
}

static uint64_t type_hash_depth(const XrType *type, int depth) {
    uint64_t h = UINT64_C(1469598103934665603);
    if (!type)
        return h;
    if (depth > 8)
        return h ^ UINT64_C(0x517cc1b727220a95);
    h ^= (uint64_t) type->kind;
    h *= UINT64_C(1099511628211);
    h ^= type->is_nullable ? UINT64_C(0x100) : UINT64_C(0);
    h *= UINT64_C(1099511628211);
    h ^= type->native_width;
    h *= UINT64_C(1099511628211);
    h ^= type->ptr_is_mut ? UINT64_C(0x200) : UINT64_C(0);
    h *= UINT64_C(1099511628211);
    switch ((XrTypeKind) type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_SPAN:
        case XR_KIND_VIEW:
        case XR_KIND_POINTER:
            h ^= type_hash_depth(type->container.element_type, depth + 1);
            h *= UINT64_C(1099511628211);
            break;
        case XR_KIND_MAP:
            h ^= type_hash_depth(type->map.key_type, depth + 1);
            h *= UINT64_C(1099511628211);
            h ^= type_hash_depth(type->map.value_type, depth + 1);
            h *= UINT64_C(1099511628211);
            break;
        case XR_KIND_ENUM:
            h = struct_hash_string(h, type->enum_type.enum_name);
            h ^= type->enum_type.layout_id;
            h *= UINT64_C(1099511628211);
            break;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
        case XR_KIND_INTERFACE:
            h = struct_hash_string(h, type->instance.class_name);
            h ^= (uint64_t) (type->instance.type_arg_count < 0 ? 0 : type->instance.type_arg_count);
            h *= UINT64_C(1099511628211);
            for (int i = 0; i < type->instance.type_arg_count; i++) {
                h ^= type_hash_depth(type->instance.type_args ? type->instance.type_args[i] : NULL,
                                     depth + 1);
                h *= UINT64_C(1099511628211);
            }
            break;
        case XR_KIND_TYPE_PARAM:
            h = struct_hash_string(h, type->type_param.name);
            h ^= (uint64_t) type->type_param.id;
            h *= UINT64_C(1099511628211);
            break;
        case XR_KIND_UNION:
            h ^= type->union_type.member_count;
            h *= UINT64_C(1099511628211);
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                h ^= type_hash_depth(type->union_type.members ? type->union_type.members[i] : NULL,
                                     depth + 1);
                h *= UINT64_C(1099511628211);
            }
            break;
        case XR_KIND_TUPLE:
            h ^= (uint64_t) (type->tuple.element_count < 0 ? 0 : type->tuple.element_count);
            h *= UINT64_C(1099511628211);
            for (int i = 0; i < type->tuple.element_count; i++) {
                h ^= type_hash_depth(
                    type->tuple.element_types ? type->tuple.element_types[i] : NULL, depth + 1);
                h *= UINT64_C(1099511628211);
            }
            break;
        case XR_KIND_FIXED_ARRAY:
            h ^= (uint64_t) type->fixed_array.length;
            h *= UINT64_C(1099511628211);
            h ^= type_hash_depth(type->fixed_array.element_type, depth + 1);
            h *= UINT64_C(1099511628211);
            break;
        case XR_KIND_RECORD:
        case XR_KIND_JSON:
            h = struct_hash_string(h, type->object.type_name);
            h ^= (uint64_t) (type->object.field_count < 0 ? 0 : type->object.field_count);
            h *= UINT64_C(1099511628211);
            for (int i = 0; i < type->object.field_count; i++) {
                h = struct_hash_string(h, type->object.field_names ? type->object.field_names[i]
                                                                   : NULL);
                h ^= type_hash_depth(type->object.field_types ? type->object.field_types[i] : NULL,
                                     depth + 1);
                h *= UINT64_C(1099511628211);
            }
            break;
        case XR_KIND_FUNCTION:
            h ^= (uint64_t) (type->function.param_count < 0 ? 0 : type->function.param_count);
            h *= UINT64_C(1099511628211);
            for (int i = 0; i < type->function.param_count; i++) {
                XrParamMode mode = xr_type_function_param_mode(type, i);
                h ^= (uint64_t) mode;
                h *= UINT64_C(1099511628211);
                h ^= type_hash_depth(xr_type_function_param_type(type, i), depth + 1);
                h *= UINT64_C(1099511628211);
            }
            h ^= type_hash_depth(type->function.return_type, depth + 1);
            h *= UINT64_C(1099511628211);
            break;
        default:
            break;
    }
    return h;
}

XR_FUNC uint64_t xaot_type_fingerprint(const XrType *type) {
    return type_hash_depth(type, 0);
}

XR_FUNC void xaot_enum_c_type_name_for_type(char *buf, size_t buflen, const char *prefix,
                                            const XiEnumData *ed, const XrType *type) {
    char pbuf[96];
    char ebuf[96];
    uint64_t h;
    if (!buf || buflen == 0)
        return;
    if (!type || !((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
                   type->instance.type_arg_count > 0)) {
        xaot_enum_c_type_name(buf, buflen, prefix, ed);
        return;
    }
    c_ident_part(pbuf, sizeof(pbuf), prefix ? prefix : "mod");
    c_ident_part(ebuf, sizeof(ebuf), ed && ed->name ? ed->name : "Enum");
    h = xaot_enum_data_hash(ed);
    h ^= xaot_type_fingerprint(type);
    h *= UINT64_C(1099511628211);
    snprintf(buf, buflen, "xrt_enum_%s_%s_%016" PRIx64, pbuf, ebuf, h);
}
