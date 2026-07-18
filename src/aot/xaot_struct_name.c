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
#include <stdio.h>
#include <string.h>

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

XR_FUNC uint64_t xaot_struct_layout_hash(const XrAggregateLayout *sl) {
    return xr_aggregate_layout_stable_key(sl);
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
    h ^= type->ptr_is_c_view ? UINT64_C(0x400) : UINT64_C(0);
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
        case XR_KIND_ERROR:
            h ^= UINT64_C(0x4572726f72547970);
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
