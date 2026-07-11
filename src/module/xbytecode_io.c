/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbytecode_io.c - Bytecode serialization/deserialization implementation
 *
 * KEY CONCEPT:
 *   Serializes XrProto to portable bytecode format (.xrc) and loads it back.
 *   Handles symbol table remapping for cross-compilation compatibility.
 */

#include "xbytecode_io.h"
#include "../base/xmalloc.h"
#include "../base/xfileio.h"
#include "../base/xlog.h"
#include "xray_vm.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xffi_sig.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/class/xenum.h"
#include "../runtime/class/xinstance.h"
#include "../base/xdynarray.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/symbol/xsymbol_table.h"

/* ========== Writer Helper ========== */

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t capacity;
    XrVMRuntime *X;
    int flags;
} BcWriter;

static void bc_writer_init(BcWriter *w, XrVMRuntime *X, int flags) {
    XR_DCHECK(w != NULL, "bc_writer_init: NULL writer");
    w->buf = NULL;
    w->size = 0;
    w->capacity = 0;
    w->X = X;
    w->flags = flags;
}

static bool bc_writer_ensure(BcWriter *w, size_t need) {
    if (w->size + need <= w->capacity)
        return true;

    size_t new_cap = w->capacity ? w->capacity * 2 : 4096;
    while (new_cap < w->size + need)
        new_cap *= 2;

    uint8_t *new_buf = xr_realloc(w->buf, new_cap);
    if (!new_buf)
        return false;

    w->buf = new_buf;
    w->capacity = new_cap;
    XR_DCHECK(w->size + need <= w->capacity, "bc_writer_ensure: capacity still insufficient");
    return true;
}

static bool bc_put_u8(BcWriter *w, uint8_t v) {
    if (!bc_writer_ensure(w, 1))
        return false;
    w->buf[w->size++] = v;
    return true;
}

static bool bc_put_u16(BcWriter *w, uint16_t v) {
    if (!bc_writer_ensure(w, 2))
        return false;
    w->buf[w->size++] = v & 0xFF;
    w->buf[w->size++] = (v >> 8) & 0xFF;
    return true;
}

static bool bc_put_u32(BcWriter *w, uint32_t v) {
    if (!bc_writer_ensure(w, 4))
        return false;
    w->buf[w->size++] = v & 0xFF;
    w->buf[w->size++] = (v >> 8) & 0xFF;
    w->buf[w->size++] = (v >> 16) & 0xFF;
    w->buf[w->size++] = (v >> 24) & 0xFF;
    return true;
}

static bool bc_put_u64(BcWriter *w, uint64_t v) {
    if (!bc_writer_ensure(w, 8))
        return false;
    for (int i = 0; i < 8; i++) {
        w->buf[w->size++] = (v >> (i * 8)) & 0xFF;
    }
    return true;
}

static bool bc_put_i64(BcWriter *w, int64_t v) {
    return bc_put_u64(w, (uint64_t) v);
}

static bool bc_put_f64(BcWriter *w, double v) {
    union {
        double d;
        uint64_t u;
    } u;
    u.d = v;
    return bc_put_u64(w, u.u);
}

static bool bc_put_bytes(BcWriter *w, const void *data, size_t len) {
    if (!bc_writer_ensure(w, len))
        return false;
    memcpy(w->buf + w->size, data, len);
    w->size += len;
    return true;
}

static bool bc_put_string(BcWriter *w, const char *str) {
    uint32_t len = str ? (uint32_t) strlen(str) : 0;
    if (!bc_put_u32(w, len))
        return false;
    if (len > 0 && !bc_put_bytes(w, str, len))
        return false;
    return true;
}

/* ========== Reader Helper ========== */

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
    XrVMRuntime *X;
    XrBcError error;
} BcReader;

static void bc_reader_init(BcReader *r, XrVMRuntime *X, const uint8_t *buf, size_t size) {
    XR_DCHECK(r != NULL, "bc_reader_init: NULL reader");
    XR_DCHECK(buf != NULL, "bc_reader_init: NULL buf");
    XR_DCHECK(size > 0, "bc_reader_init: zero size");
    r->buf = buf;
    r->size = size;
    r->pos = 0;
    r->X = X;
    r->error = XR_BC_OK;
}

static bool bc_has_bytes(BcReader *r, size_t n) {
    return r->pos + n <= r->size;
}

static uint8_t bc_get_u8(BcReader *r) {
    if (!bc_has_bytes(r, 1)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return 0;
    }
    return r->buf[r->pos++];
}

static uint16_t bc_get_u16(BcReader *r) {
    if (!bc_has_bytes(r, 2)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return 0;
    }
    uint16_t v = r->buf[r->pos] | (r->buf[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

static uint32_t bc_get_u32(BcReader *r) {
    if (!bc_has_bytes(r, 4)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return 0;
    }
    uint32_t v = r->buf[r->pos] | (r->buf[r->pos + 1] << 8) | (r->buf[r->pos + 2] << 16) |
                 (r->buf[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

static uint64_t bc_get_u64(BcReader *r) {
    if (!bc_has_bytes(r, 8)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t) r->buf[r->pos++]) << (i * 8);
    }
    return v;
}

static int64_t bc_get_i64(BcReader *r) {
    return (int64_t) bc_get_u64(r);
}

static double bc_get_f64(BcReader *r) {
    union {
        double d;
        uint64_t u;
    } u;
    u.u = bc_get_u64(r);
    return u.d;
}

static char *bc_get_string(BcReader *r) {
    uint32_t len = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        return NULL;
    if (len == 0)
        return NULL;

    if (!bc_has_bytes(r, len)) {
        r->error = XR_BC_ERR_TRUNCATED;
        return NULL;
    }

    char *str = xr_malloc(len + 1);
    if (!str) {
        r->error = XR_BC_ERR_ALLOC;
        return NULL;
    }

    memcpy(str, r->buf + r->pos, len);
    str[len] = '\0';
    r->pos += len;
    return str;
}

/* ========== Value Serialization ========== */

// Value type tags
#define BC_VAL_NULL 0
#define BC_VAL_BOOL 1
#define BC_VAL_INT 2
#define BC_VAL_FLOAT 3
#define BC_VAL_STRING 4
#define BC_VAL_DYNAMIC_SHAPE 5
#define BC_VAL_CLASS_DESCRIPTOR 6
#define BC_VAL_ENUM_TYPE 7

#define BC_SHAPE_JSON 1
#define BC_SHAPE_RECORD 2

static bool bc_write_dynamic_shape(BcWriter *w, XrValue val) {
    if (!XR_IS_INT(val))
        return false;

    XrClass *cls = (XrClass *) (intptr_t) XR_TO_INT(val);
    if (!cls || !(cls->flags & XR_CLASS_DYNAMIC_LAYOUT))
        return false;

    uint8_t kind = 0;
    if (cls->builtin_kind == XR_BK_JSON) {
        kind = BC_SHAPE_JSON;
    } else if (cls->builtin_kind == XR_BK_RECORD) {
        kind = BC_SHAPE_RECORD;
    } else {
        return false;
    }

    if (!bc_put_u8(w, BC_VAL_DYNAMIC_SHAPE))
        return false;
    if (!bc_put_u8(w, kind))
        return false;
    if (!bc_put_u8(w, (cls->flags & XR_CLASS_DYNAMIC_SEALED) ? 1 : 0))
        return false;
    if (!bc_put_u32(w, cls->field_count))
        return false;
    for (uint16_t i = 0; i < cls->field_count; i++) {
        const char *name = (cls->fields && cls->fields[i].name) ? cls->fields[i].name : "";
        if (!bc_put_string(w, name))
            return false;
    }
    return true;
}

static bool bc_write_value(BcWriter *w, XrValue val, bool as_dynamic_shape,
                           bool as_class_descriptor);
static XrValue bc_read_value(BcReader *r);

static bool bc_put_optional_string(BcWriter *w, const char *str) {
    return bc_put_string(w, str ? str : "");
}

static bool bc_write_field_descriptor(BcWriter *w, const XrFieldDescriptorEntry *field) {
    if (!field)
        return false;
    if (!bc_put_optional_string(w, field->name))
        return false;
    if (!bc_put_optional_string(w, field->type_name))
        return false;
    if (!bc_write_value(w, field->default_value, false, false))
        return false;
    return bc_put_u16(w, field->flags);
}

static bool bc_write_method_descriptor(BcWriter *w, const XrMethodDescriptorEntry *method) {
    if (!method)
        return false;
    if (!bc_put_optional_string(w, method->name))
        return false;
    if (!bc_put_u32(w, method->closure_index))
        return false;
    if (!bc_put_optional_string(w, method->return_type_name))
        return false;
    if (!bc_put_u8(w, method->param_count))
        return false;
    for (uint8_t i = 0; i < method->param_count; i++) {
        const char *param_type = method->param_type_names ? method->param_type_names[i] : NULL;
        if (!bc_put_optional_string(w, param_type))
            return false;
    }
    if (!bc_put_u16(w, method->flags))
        return false;
    if (!bc_put_u8(w, method->op_type))
        return false;
    return bc_put_u8(w, method->is_operator ? 1 : 0);
}

static bool bc_write_class_descriptor(BcWriter *w, XrValue val) {
    if (!XR_IS_PTR(val) || !XR_TO_PTR(val))
        return false;

    const XrClassDescriptor *desc = (const XrClassDescriptor *) XR_TO_PTR(val);
    if (desc->struct_layout) {
        // Struct/native body layouts are not portable bytecode metadata yet.
        return false;
    }

    if (!bc_put_u8(w, BC_VAL_CLASS_DESCRIPTOR))
        return false;
    if (!bc_put_optional_string(w, desc->class_name))
        return false;
    if (!bc_put_optional_string(w, desc->super_name))
        return false;
    if (!bc_put_optional_string(w, desc->generic_origin_name))
        return false;
    if (!bc_put_optional_string(w, desc->display_name))
        return false;
    if (desc->mono_type_arg_count < 0 || desc->mono_type_arg_count > UINT16_MAX)
        return false;
    if (!bc_put_u32(w, (uint32_t) desc->mono_type_arg_count))
        return false;
    for (int i = 0; i < desc->mono_type_arg_count; i++) {
        const char *name = desc->mono_type_arg_names ? desc->mono_type_arg_names[i] : NULL;
        if (!bc_put_optional_string(w, name))
            return false;
    }
    if (!bc_put_u32(w, (uint32_t) desc->super_global_index))
        return false;
    if (!bc_put_u32(w, desc->flags))
        return false;
    if (!bc_put_u8(w, desc->is_monomorphized ? 1 : 0))
        return false;

    if (!bc_put_u32(w, desc->instance_field_count))
        return false;
    for (uint32_t i = 0; i < desc->instance_field_count; i++) {
        if (!bc_write_field_descriptor(w, &desc->instance_fields[i]))
            return false;
    }

    if (!bc_put_u32(w, desc->static_field_count))
        return false;
    for (uint32_t i = 0; i < desc->static_field_count; i++) {
        if (!bc_write_field_descriptor(w, &desc->static_fields[i]))
            return false;
    }

    if (!bc_put_u32(w, desc->instance_method_count))
        return false;
    for (uint32_t i = 0; i < desc->instance_method_count; i++) {
        if (!bc_write_method_descriptor(w, &desc->instance_methods[i]))
            return false;
    }

    if (!bc_put_u32(w, desc->static_method_count))
        return false;
    for (uint32_t i = 0; i < desc->static_method_count; i++) {
        if (!bc_write_method_descriptor(w, &desc->static_methods[i]))
            return false;
    }

    if (!bc_put_u8(w, desc->interface_count))
        return false;
    for (uint8_t i = 0; i < desc->interface_count; i++) {
        const char *name = desc->interfaces ? desc->interfaces[i].interface_name : NULL;
        if (!bc_put_optional_string(w, name))
            return false;
    }

    if (!bc_put_u32(w, (uint32_t) desc->clinit_proto_index))
        return false;
    if (!bc_put_u32(w, desc->descriptor_version))
        return false;
    return bc_put_u32(w, desc->checksum);
}

static bool bc_write_enum_type(BcWriter *w, XrValue val) {
    if (!XR_IS_ENUM_TYPE(val))
        return false;

    XrEnumType *enum_type = XR_TO_ENUM_TYPE(val);
    if (!enum_type || !enum_type->name || enum_type->member_count > UINT16_MAX)
        return false;

    if (!bc_put_u8(w, BC_VAL_ENUM_TYPE))
        return false;
    if (!bc_put_optional_string(w, enum_type->name))
        return false;
    if (!bc_put_u32(w, enum_type->member_count))
        return false;
    if (!bc_put_u32(w, enum_type->derive_flags))
        return false;

    for (uint32_t i = 0; i < enum_type->member_count; i++) {
        const char *name = xr_enum_type_member_name(enum_type, i);
        if (!bc_put_optional_string(w, name))
            return false;
        int payload_count = xr_enum_type_payload_count(enum_type, i);
        if (payload_count < 0 || payload_count > UINT16_MAX)
            return false;
        if (!bc_put_u16(w, (uint16_t) payload_count))
            return false;
    }
    return true;
}

static bool bc_write_value(BcWriter *w, XrValue val, bool as_dynamic_shape,
                           bool as_class_descriptor) {
    if (as_class_descriptor)
        return bc_write_class_descriptor(w, val);
    if (as_dynamic_shape)
        return bc_write_dynamic_shape(w, val);
    if (XR_IS_ENUM_TYPE(val))
        return bc_write_enum_type(w, val);

    if (XR_IS_NULL(val)) {
        return bc_put_u8(w, BC_VAL_NULL);
    } else if (XR_IS_BOOL(val)) {
        if (!bc_put_u8(w, BC_VAL_BOOL))
            return false;
        return bc_put_u8(w, XR_TO_BOOL(val) ? 1 : 0);
    } else if (XR_IS_INT(val)) {
        if (!bc_put_u8(w, BC_VAL_INT))
            return false;
        return bc_put_i64(w, XR_TO_INT(val));
    } else if (XR_IS_FLOAT(val)) {
        if (!bc_put_u8(w, BC_VAL_FLOAT))
            return false;
        return bc_put_f64(w, XR_TO_FLOAT(val));
    } else if (XR_IS_STRING(val)) {
        if (!bc_put_u8(w, BC_VAL_STRING))
            return false;
        XrString *s = XR_TO_STRING(val);
        return bc_put_string(w, s->data);
    }
    // Other types not supported yet
    return bc_put_u8(w, BC_VAL_NULL);
}

static char *bc_read_string_or_empty(BcReader *r) {
    char *str = bc_get_string(r);
    if (r->error != XR_BC_OK)
        return NULL;
    if (str)
        return str;
    str = xr_strdup("");
    if (!str)
        r->error = XR_BC_ERR_ALLOC;
    return str;
}

static char *bc_read_optional_string(BcReader *r) {
    char *str = bc_get_string(r);
    if (r->error != XR_BC_OK)
        return NULL;
    return str;
}

static bool bc_read_field_descriptor(BcReader *r, XrFieldDescriptorEntry *field) {
    if (!field)
        return false;
    field->name = bc_read_string_or_empty(r);
    field->type_name = bc_read_optional_string(r);
    field->default_value = bc_read_value(r);
    field->flags = bc_get_u16(r);
    return r->error == XR_BC_OK;
}

static bool bc_read_method_descriptor(BcReader *r, XrMethodDescriptorEntry *method) {
    if (!method)
        return false;
    method->name = bc_read_string_or_empty(r);
    method->closure_index = bc_get_u32(r);
    method->return_type_name = bc_read_optional_string(r);
    method->param_count = bc_get_u8(r);
    if (r->error != XR_BC_OK)
        return false;
    if (method->param_count > 0) {
        const char **params = xr_calloc(method->param_count, sizeof(char *));
        if (!params) {
            r->error = XR_BC_ERR_ALLOC;
            return false;
        }
        for (uint8_t i = 0; i < method->param_count; i++) {
            params[i] = bc_read_optional_string(r);
            if (r->error != XR_BC_OK)
                return false;
        }
        method->param_type_names = params;
    }
    method->flags = bc_get_u16(r);
    method->op_type = bc_get_u8(r);
    method->is_operator = bc_get_u8(r) != 0;
    return r->error == XR_BC_OK;
}

static XrValue bc_read_class_descriptor(BcReader *r) {
    XrClassDescriptor *desc = xr_calloc(1, sizeof(XrClassDescriptor));
    if (!desc) {
        r->error = XR_BC_ERR_ALLOC;
        return xr_null();
    }

    desc->class_name = bc_read_string_or_empty(r);
    desc->super_name = bc_read_optional_string(r);
    desc->generic_origin_name = bc_read_optional_string(r);
    desc->display_name = bc_read_optional_string(r);
    uint32_t mono_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        return xr_null();
    if (mono_count > UINT16_MAX) {
        r->error = XR_BC_ERR_CORRUPT;
        return xr_null();
    }
    desc->mono_type_arg_count = (int) mono_count;
    if (mono_count > 0) {
        const char **names = xr_calloc(mono_count, sizeof(char *));
        if (!names) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < mono_count; i++) {
            names[i] = bc_read_optional_string(r);
            if (r->error != XR_BC_OK)
                return xr_null();
        }
        desc->mono_type_arg_names = names;
    }
    desc->super_global_index = (int32_t) bc_get_u32(r);
    desc->flags = bc_get_u32(r);
    desc->is_monomorphized = bc_get_u8(r) != 0;

    desc->instance_field_count = bc_get_u32(r);
    if (desc->instance_field_count > 0) {
        desc->instance_fields =
            xr_calloc(desc->instance_field_count, sizeof(XrFieldDescriptorEntry));
        if (!desc->instance_fields) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < desc->instance_field_count; i++) {
            if (!bc_read_field_descriptor(r, &desc->instance_fields[i]))
                return xr_null();
        }
    }

    desc->static_field_count = bc_get_u32(r);
    if (desc->static_field_count > 0) {
        desc->static_fields = xr_calloc(desc->static_field_count, sizeof(XrFieldDescriptorEntry));
        if (!desc->static_fields) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < desc->static_field_count; i++) {
            if (!bc_read_field_descriptor(r, &desc->static_fields[i]))
                return xr_null();
        }
    }

    desc->instance_method_count = bc_get_u32(r);
    if (desc->instance_method_count > 0) {
        desc->instance_methods =
            xr_calloc(desc->instance_method_count, sizeof(XrMethodDescriptorEntry));
        if (!desc->instance_methods) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < desc->instance_method_count; i++) {
            if (!bc_read_method_descriptor(r, &desc->instance_methods[i]))
                return xr_null();
        }
    }

    desc->static_method_count = bc_get_u32(r);
    if (desc->static_method_count > 0) {
        desc->static_methods =
            xr_calloc(desc->static_method_count, sizeof(XrMethodDescriptorEntry));
        if (!desc->static_methods) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint32_t i = 0; i < desc->static_method_count; i++) {
            if (!bc_read_method_descriptor(r, &desc->static_methods[i]))
                return xr_null();
        }
    }

    desc->interface_count = bc_get_u8(r);
    if (desc->interface_count > 0) {
        desc->interfaces = xr_calloc(desc->interface_count, sizeof(XrInterfaceDescriptorEntry));
        if (!desc->interfaces) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        for (uint8_t i = 0; i < desc->interface_count; i++) {
            desc->interfaces[i].interface_name = bc_read_optional_string(r);
            desc->interfaces[i].interface_ptr = NULL;
            if (r->error != XR_BC_OK)
                return xr_null();
        }
    }

    desc->clinit_proto_index = (int32_t) bc_get_u32(r);
    desc->descriptor_version = bc_get_u32(r);
    desc->checksum = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        return xr_null();

    XrValue val = {0};
    val.tag = XR_TAG_PTR;
    val.ptr = desc;
    val.heap_type = 0;
    return val;
}

static XrValue bc_read_enum_type(BcReader *r) {
    char *enum_name = bc_read_string_or_empty(r);
    uint32_t member_count = bc_get_u32(r);
    uint32_t derive_flags = bc_get_u32(r);
    if (r->error != XR_BC_OK) {
        xr_free(enum_name);
        return xr_null();
    }
    if (!enum_name || enum_name[0] == '\0' || member_count == 0 || member_count > UINT16_MAX) {
        xr_free(enum_name);
        r->error = XR_BC_ERR_CORRUPT;
        return xr_null();
    }

    char **member_names = xr_calloc(member_count, sizeof(char *));
    int *payload_counts = xr_calloc(member_count, sizeof(int));
    if (!member_names || !payload_counts) {
        xr_free(enum_name);
        xr_free(member_names);
        xr_free(payload_counts);
        r->error = XR_BC_ERR_ALLOC;
        return xr_null();
    }

    bool has_payloads = false;
    for (uint32_t i = 0; i < member_count; i++) {
        member_names[i] = bc_read_string_or_empty(r);
        uint16_t payload_count = bc_get_u16(r);
        if (r->error != XR_BC_OK)
            break;
        if (!member_names[i] || member_names[i][0] == '\0') {
            r->error = XR_BC_ERR_CORRUPT;
            break;
        }
        payload_counts[i] = (int) payload_count;
        if (payload_count > 0)
            has_payloads = true;
    }

    XrEnumType *enum_type = NULL;
    if (r->error == XR_BC_OK) {
        enum_type = xr_enum_type_new(r->X, enum_name, member_names, (int) member_count);
        if (!enum_type) {
            r->error = XR_BC_ERR_ALLOC;
        } else {
            enum_type->derive_flags = derive_flags;
            if (has_payloads &&
                !xr_enum_type_set_adt_payloads(enum_type, payload_counts, (int) member_count)) {
                r->error = XR_BC_ERR_CORRUPT;
                enum_type = NULL;
            }
        }
    }

    for (uint32_t i = 0; i < member_count; i++)
        xr_free(member_names[i]);
    xr_free(member_names);
    xr_free(payload_counts);
    xr_free(enum_name);

    return (r->error == XR_BC_OK && enum_type) ? XR_FROM_PTR(enum_type) : xr_null();
}

static XrValue bc_read_dynamic_shape(BcReader *r) {
    uint8_t kind = bc_get_u8(r);
    uint8_t sealed_raw = bc_get_u8(r);
    uint32_t count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        return xr_null();
    if (count > UINT16_MAX || (kind != BC_SHAPE_JSON && kind != BC_SHAPE_RECORD)) {
        r->error = XR_BC_ERR_CORRUPT;
        return xr_null();
    }

    char **names = NULL;
    if (count > 0) {
        names = xr_malloc(sizeof(char *) * (size_t) count);
        if (!names) {
            r->error = XR_BC_ERR_ALLOC;
            return xr_null();
        }
        memset(names, 0, sizeof(char *) * (size_t) count);
    }

    for (uint32_t i = 0; i < count; i++) {
        names[i] = bc_get_string(r);
        if (r->error != XR_BC_OK) {
            for (uint32_t j = 0; j <= i; j++)
                xr_free(names[j]);
            xr_free(names);
            return xr_null();
        }
        if (!names[i]) {
            names[i] = xr_malloc(1);
            if (!names[i]) {
                r->error = XR_BC_ERR_ALLOC;
                for (uint32_t j = 0; j < i; j++)
                    xr_free(names[j]);
                xr_free(names);
                return xr_null();
            }
            names[i][0] = '\0';
        }
    }

    XrClass *cls = NULL;
    bool sealed = sealed_raw != 0;
    if (kind == BC_SHAPE_JSON) {
        cls = xr_class_build_json_chain(r->X, (const char *const *) names, (int) count, sealed);
    } else {
        cls = xr_class_build_record_chain(r->X, (const char *const *) names, (int) count, sealed);
    }

    for (uint32_t i = 0; i < count; i++)
        xr_free(names[i]);
    xr_free(names);

    if (!cls) {
        r->error = XR_BC_ERR_ALLOC;
        return xr_null();
    }
    return xr_int((int64_t) (intptr_t) cls);
}

static XrValue bc_read_value(BcReader *r) {
    uint8_t type = bc_get_u8(r);
    if (r->error != XR_BC_OK)
        return xr_null();

    switch (type) {
        case BC_VAL_NULL:
            return xr_null();
        case BC_VAL_BOOL:
            return xr_bool(bc_get_u8(r) != 0);
        case BC_VAL_INT:
            return xr_int(bc_get_i64(r));
        case BC_VAL_FLOAT:
            return xr_float(bc_get_f64(r));
        case BC_VAL_STRING: {
            char *str = bc_get_string(r);
            if (!str)
                return xr_null();
            XrString *s = xr_string_intern(r->X, str, strlen(str), 0);
            xr_free(str);
            return s ? xr_string_value(s) : xr_null();
        }
        case BC_VAL_DYNAMIC_SHAPE:
            return bc_read_dynamic_shape(r);
        case BC_VAL_CLASS_DESCRIPTOR:
            return bc_read_class_descriptor(r);
        case BC_VAL_ENUM_TYPE:
            return bc_read_enum_type(r);
        default:
            r->error = XR_BC_ERR_CORRUPT;
            return xr_null();
    }
}

/* ========== Symbol Table Serialization ========== */

// Collect global symbol IDs from per-function symbol table, returns max_symbol_id + 1
static int collect_symbols_from_proto(XrProto *proto, int max_symbol) {
    if (!proto)
        return max_symbol;

    // Scan per-function symbol table (no need to scan instructions)
    for (int i = 0; i < proto->symbol_count; i++) {
        int32_t sym = proto->symbols[i];
        if (sym >= max_symbol) {
            max_symbol = sym + 1;
        }
    }

    // Recursively process nested Protos
    uint32_t sub_count = (uint32_t) PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        max_symbol = collect_symbols_from_proto(sub, max_symbol);
    }

    return max_symbol;
}

// Remap global symbol IDs in per-function symbol table
static void remap_symbols_in_proto(XrProto *proto, int *id_map, int map_size) {
    if (!proto)
        return;

    // Remap per-function symbol table entries (instructions are untouched)
    for (int i = 0; i < proto->symbol_count; i++) {
        int32_t old_id = proto->symbols[i];
        if (old_id >= 0 && old_id < map_size && id_map[old_id] >= 0) {
            proto->symbols[i] = id_map[old_id];
        }
    }

    // Recursively process nested Protos
    uint32_t sub_count = (uint32_t) PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        remap_symbols_in_proto(sub, id_map, map_size);
    }
}

/* ========== Shared Variable Index Remapping ========== */

// Collect max shared index used in Proto, returns max_shared_index + 1
static int collect_shared_from_proto(XrProto *proto, int max_shared) {
    if (!proto)
        return max_shared;

    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        OpCode op = GET_OPCODE(inst);

        // Check opcodes that use shared index
        if (op == OP_GETSHARED || op == OP_SETSHARED) {
            int shared_idx = GETARG_Bx(inst);
            if (shared_idx >= max_shared) {
                max_shared = shared_idx + 1;
            }
        }
    }

    // Recursively process nested Protos
    uint32_t sub_count = (uint32_t) PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        max_shared = collect_shared_from_proto(sub, max_shared);
    }

    return max_shared;
}

/* ========== Proto Serialization ========== */

static bool bc_write_proto(BcWriter *w, XrProto *proto);
static XrProto *bc_read_proto_depth(BcReader *r, int depth);

#define BC_MAX_NESTING_DEPTH 64

static bool *bc_collect_dynamic_shape_constants(XrProto *proto, uint32_t const_count) {
    if (const_count == 0)
        return NULL;

    bool *shape_consts = xr_calloc(const_count, sizeof(bool));
    if (!shape_consts)
        return NULL;

    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        OpCode op = GET_OPCODE(inst);
        int kidx = -1;
        if (op == OP_NEWJSON) {
            kidx = GETARG_B(inst);
        } else if (op == OP_JSON_DECODE) {
            kidx = GETARG_C(inst);
        }
        if (kidx >= 0 && (uint32_t) kidx < const_count) {
            shape_consts[kidx] = true;
        }
    }
    return shape_consts;
}

static bool *bc_collect_class_descriptor_constants(XrProto *proto, uint32_t const_count) {
    if (const_count == 0)
        return NULL;

    bool *class_consts = xr_calloc(const_count, sizeof(bool));
    if (!class_consts)
        return NULL;

    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (GET_OPCODE(inst) != OP_CLASS_CREATE_FROM_DESCRIPTOR)
            continue;
        int kidx = GETARG_Bx(inst);
        if (kidx >= 0 && (uint32_t) kidx < const_count)
            class_consts[kidx] = true;
    }
    return class_consts;
}

static bool bc_write_proto(BcWriter *w, XrProto *proto) {
    if (!proto)
        return false;

    // 1. Function name
    const char *name = proto->name ? proto->name->data : "";
    if (!bc_put_string(w, name))
        return false;

    // 2. Source file (optional)
    if (w->flags & XR_BC_STRIP_SOURCE) {
        if (!bc_put_string(w, ""))
            return false;
    } else {
        if (!bc_put_string(w, proto->source_file))
            return false;
    }

    // 3. Function attributes
    if (!bc_put_u32(w, proto->numparams))
        return false;
    if (!bc_put_u32(w, proto->maxstacksize))
        return false;
    if (!bc_put_u32(w, proto->num_globals))
        return false;
    if (!bc_put_u32(w, proto->struct_area_size))
        return false;
    if (!bc_put_u8(w, proto->is_vararg ? 1 : 0))
        return false;
    if (!bc_put_u8(w, proto->is_coro_safe ? 1 : 0))
        return false;

    // 3b. FFI @extern signature (self-contained; the Xi IR is not serialized,
    // so the embedded-bytecode VM resolves the C symbol from here). The flag
    // doubles as "a signature follows" so a malformed extern proto without a
    // sig stays byte-aligned.
    {
        XrFFISig *sig = (proto->is_extern && proto->ffi_sig) ? proto->ffi_sig : NULL;
        if (!bc_put_u8(w, sig ? 1 : 0))
            return false;
        if (sig) {
            if (!bc_put_string(w, sig->symbol))
                return false;
            if (!bc_put_string(w, sig->dylib))
                return false;
            if (!bc_put_u8(w, sig->nparams))
                return false;
            for (uint8_t i = 0; i < sig->nparams; i++) {
                if (!bc_put_u8(w, sig->params[i]))
                    return false;
                XrFFICallbackSig *cb = sig->param_cbacks ? sig->param_cbacks[i] : NULL;
                if (!bc_put_u8(w, cb ? 1 : 0))
                    return false;
                if (cb) {
                    if (!bc_put_u8(w, cb->nparams))
                        return false;
                    for (uint8_t ci = 0; ci < cb->nparams; ci++) {
                        if (!bc_put_u8(w, cb->params[ci]))
                            return false;
                    }
                    if (!bc_put_u8(w, cb->ret))
                        return false;
                }
            }
            if (!bc_put_u8(w, sig->ret))
                return false;
        }
    }

    // 4. Bytecode
    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    if (!bc_put_u32(w, code_count))
        return false;
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (!bc_put_u64(w, inst))
            return false;
    }

    // 5. Constants
    uint32_t const_count = (uint32_t) PROTO_CONST_COUNT(proto);
    if (!bc_put_u32(w, const_count))
        return false;
    bool *shape_consts = bc_collect_dynamic_shape_constants(proto, const_count);
    bool *class_consts = bc_collect_class_descriptor_constants(proto, const_count);
    if (const_count > 0 && (!shape_consts || !class_consts)) {
        xr_free(shape_consts);
        xr_free(class_consts);
        return false;
    }
    for (uint32_t i = 0; i < const_count; i++) {
        XrValue val = PROTO_CONSTANT(proto, i);
        if (!bc_write_value(w, val, shape_consts[i], class_consts[i])) {
            xr_free(shape_consts);
            xr_free(class_consts);
            return false;
        }
    }
    xr_free(shape_consts);
    xr_free(class_consts);

    // 6. Line info (optional)
    if (w->flags & XR_BC_STRIP_DEBUG) {
        if (!bc_put_u32(w, 0))
            return false;
    } else {
        uint32_t line_count = (uint32_t) PROTO_LINE_COUNT(proto);
        if (!bc_put_u32(w, line_count))
            return false;
        for (uint32_t i = 0; i < line_count; i++) {
            if (!bc_put_u32(w, PROTO_LINE(proto, i)))
                return false;
        }
    }

    // 7. Upvalue info
    uint32_t upval_count = (uint32_t) PROTO_UPVAL_COUNT(proto);
    if (!bc_put_u32(w, upval_count))
        return false;
    for (uint32_t i = 0; i < upval_count; i++) {
        UpvalInfo info = PROTO_UPVALUE(proto, i);
        if (!bc_put_u16(w, info.index))
            return false;
        if (!bc_put_u8(w, info.source))
            return false;
        if (!bc_put_u8(w, info.storage_mode))
            return false;
        if (!bc_put_u8(w, info.is_const))
            return false;
        if (!bc_put_u8(w, info.slot_type))
            return false;
    }

    // 8. Nested Protos
    uint32_t sub_count = (uint32_t) PROTO_PROTO_COUNT(proto);
    if (!bc_put_u32(w, sub_count))
        return false;
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = PROTO_PROTO(proto, i);
        if (!bc_write_proto(w, sub))
            return false;
    }

    // 9. Per-function symbol table
    if (!bc_put_u32(w, (uint32_t) proto->symbol_count))
        return false;
    for (int i = 0; i < proto->symbol_count; i++) {
        if (!bc_put_u32(w, (uint32_t) proto->symbols[i]))
            return false;
    }

    return true;
}

static XrProto *bc_read_proto_depth(BcReader *r, int depth) {
    if (depth > BC_MAX_NESTING_DEPTH) {
        r->error = XR_BC_ERR_CORRUPT;
        return NULL;
    }
    // Allocate Proto through the canonical constructor so runtime metadata
    // such as proto_id stays unique for inline-cache indexing.
    XrProto *proto = xr_vm_proto_new();
    if (!proto) {
        r->error = XR_BC_ERR_ALLOC;
        return NULL;
    }

    // 1. Function name
    char *name = bc_get_string(r);
    if (r->error != XR_BC_OK)
        goto fail;
    if (name && name[0]) {
        proto->name = xr_string_intern(r->X, name, strlen(name), 0);
        xr_free(name);
    }

    // 2. Source file
    char *source = bc_get_string(r);
    if (r->error != XR_BC_OK)
        goto fail;
    if (source && source[0]) {
        proto->source_file = source;
    } else {
        xr_free(source);
    }

    // 3. Function attributes
    proto->numparams = bc_get_u32(r);
    proto->maxstacksize = bc_get_u32(r);
    proto->num_globals = bc_get_u32(r);
    uint32_t struct_area_size = bc_get_u32(r);
    if (struct_area_size > UINT16_MAX) {
        r->error = XR_BC_ERR_CORRUPT;
        goto fail;
    }
    proto->struct_area_size = (uint16_t) struct_area_size;
    proto->is_vararg = bc_get_u8(r) != 0;
    proto->is_coro_safe = bc_get_u8(r) != 0;
    if (r->error != XR_BC_OK)
        goto fail;

    // 3b. FFI @extern signature
    {
        uint8_t has_ffi = bc_get_u8(r);
        if (r->error != XR_BC_OK)
            goto fail;
        if (has_ffi) {
            char *sym = bc_get_string(r);
            char *dylib = bc_get_string(r);
            uint8_t np = bc_get_u8(r);
            if (r->error != XR_BC_OK) {
                xr_free(sym);
                xr_free(dylib);
                goto fail;
            }
            XrFFISig *sig = xr_ffi_sig_new(sym ? sym : "", dylib, np);
            xr_free(sym);
            xr_free(dylib);
            if (!sig) {
                r->error = XR_BC_ERR_ALLOC;
                goto fail;
            }
            for (uint8_t i = 0; i < np; i++) {
                sig->params[i] = bc_get_u8(r);
                uint8_t has_cb = bc_get_u8(r);
                if (r->error != XR_BC_OK) {
                    xr_ffi_sig_free(sig);
                    goto fail;
                }
                if (has_cb) {
                    uint8_t cb_np = bc_get_u8(r);
                    uint8_t cb_stack[16];
                    uint8_t *cb_params = cb_np <= 16 ? cb_stack : xr_malloc(cb_np);
                    if (cb_np > 0 && !cb_params) {
                        xr_ffi_sig_free(sig);
                        r->error = XR_BC_ERR_ALLOC;
                        goto fail;
                    }
                    for (uint8_t ci = 0; ci < cb_np; ci++)
                        cb_params[ci] = bc_get_u8(r);
                    uint8_t cb_ret = bc_get_u8(r);
                    if (r->error != XR_BC_OK) {
                        if (cb_params != cb_stack)
                            xr_free(cb_params);
                        xr_ffi_sig_free(sig);
                        goto fail;
                    }
                    bool ok = xr_ffi_sig_set_param_callback_codes(sig, i, cb_params, cb_np, cb_ret);
                    if (cb_params != cb_stack)
                        xr_free(cb_params);
                    if (!ok) {
                        xr_ffi_sig_free(sig);
                        r->error = XR_BC_ERR_ALLOC;
                        goto fail;
                    }
                }
            }
            sig->ret = bc_get_u8(r);
            if (r->error != XR_BC_OK) {
                xr_ffi_sig_free(sig);
                goto fail;
            }
            proto->ffi_sig = sig;
            proto->is_extern = true;
        }
    }

    // 4. Bytecode
    uint32_t code_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = bc_get_u64(r);
        if (r->error != XR_BC_OK)
            goto fail;
        DYNARRAY_ADD(&proto->code, inst, XrInstruction);
    }

    // 5. Constants
    uint32_t const_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < const_count; i++) {
        XrValue val = bc_read_value(r);
        if (r->error != XR_BC_OK)
            goto fail;
        DYNARRAY_ADD(&proto->constants, val, XrValue);
    }

    // 6. Line info
    uint32_t line_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < line_count; i++) {
        int line = (int) bc_get_u32(r);
        if (r->error != XR_BC_OK)
            goto fail;
        DYNARRAY_ADD(&proto->lineinfo, line, int);
    }

    // 7. Upvalue info
    uint32_t upval_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < upval_count; i++) {
        UpvalInfo info = {0};
        info.index = bc_get_u16(r);
        info.source = bc_get_u8(r);
        info.storage_mode = bc_get_u8(r);
        info.is_const = bc_get_u8(r);
        info.slot_type = bc_get_u8(r);
        if (r->error != XR_BC_OK)
            goto fail;
        DYNARRAY_ADD(&proto->upvalues, info, UpvalInfo);
    }

    // 8. Nested Protos
    uint32_t sub_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    for (uint32_t i = 0; i < sub_count; i++) {
        XrProto *sub = bc_read_proto_depth(r, depth + 1);
        if (!sub)
            goto fail;
        xr_vm_proto_add_proto(proto, sub);
    }

    // 9. Per-function symbol table
    uint32_t sym_count = bc_get_u32(r);
    if (r->error != XR_BC_OK)
        goto fail;
    if (sym_count > 0) {
        proto->symbols = xr_malloc(sym_count * sizeof(int32_t));
        if (!proto->symbols) {
            r->error = XR_BC_ERR_ALLOC;
            goto fail;
        }
        proto->symbol_count = (int) sym_count;
        proto->symbol_capacity = (int) sym_count;
        for (uint32_t i = 0; i < sym_count; i++) {
            proto->symbols[i] = (int32_t) bc_get_u32(r);
            if (r->error != XR_BC_OK)
                goto fail;
        }
    }

    return proto;

fail:
    xr_vm_proto_free(proto);
    return NULL;
}

/* ========== Public API ========== */

uint8_t *xr_bytecode_write(XrVMRuntime *X, XrProto *proto, int flags, size_t *out_size) {
    if (!X || !proto || !out_size)
        return NULL;

    BcWriter w;
    bc_writer_init(&w, X, flags);

    // Collect symbols (symbol ID starts from 1, returns max ID + 1)
    int max_symbol_id = collect_symbols_from_proto(proto, 0);

    int used_shared_count = collect_shared_from_proto(proto, 0);
    int shared_count =
        proto->shared_count > used_shared_count ? proto->shared_count : used_shared_count;

    // Write header
    if (!bc_put_u32(&w, XR_BC_MAGIC))
        goto fail;
    if (!bc_put_u16(&w, XR_BC_VERSION))
        goto fail;
    if (!bc_put_u16(&w, (uint16_t) flags))
        goto fail;
    if (!bc_put_u32(&w, 1))
        goto fail;  // proto count
    if (!bc_put_u32(&w, (uint32_t) max_symbol_id))
        goto fail;  // max symbol id
    if (!bc_put_u32(&w, (uint32_t) shared_count))
        goto fail;  // shared count

    // Write symbol table (symbol ID starts from 1)
    for (int i = 1; i <= max_symbol_id; i++) {
        const char *name = xr_symbol_get_name_by_id(X, i);
        if (!bc_put_string(&w, name ? name : ""))
            goto fail;
    }

    // Write Proto
    if (!bc_write_proto(&w, proto))
        goto fail;

    *out_size = w.size;
    return w.buf;

fail:
    xr_free(w.buf);
    *out_size = 0;
    return NULL;
}

XrProto *xr_bytecode_read(XrVMRuntime *X, const uint8_t *data, size_t size, XrBcError *error) {
    if (!X || !data || size == 0) {
        if (error)
            *error = XR_BC_ERR_TRUNCATED;
        return NULL;
    }

    BcReader r;
    bc_reader_init(&r, X, data, size);

    // Read header
    uint32_t magic = bc_get_u32(&r);
    if (magic != XR_BC_MAGIC) {
        if (error)
            *error = XR_BC_ERR_MAGIC;
        return NULL;
    }

    uint16_t version = bc_get_u16(&r);
    if (version != XR_BC_VERSION) {
        if (error)
            *error = XR_BC_ERR_VERSION;
        return NULL;
    }

    bc_get_u16(&r);                           // flags
    bc_get_u32(&r);                           // proto count
    uint32_t max_symbol_id = bc_get_u32(&r);  // max symbol id
    uint32_t shared_count = bc_get_u32(&r);   // shared count

    if (r.error != XR_BC_OK) {
        if (error)
            *error = r.error;
        return NULL;
    }

    if (shared_count > (uint32_t) INT_MAX) {
        if (error)
            *error = XR_BC_ERR_CORRUPT;
        return NULL;
    }

    // Read symbol table and build mapping (symbol ID starts from 1)
    int *id_map = NULL;
    int map_size = (int) max_symbol_id + 1;
    if (max_symbol_id > 0) {
        id_map = xr_malloc(map_size * sizeof(int));
        if (!id_map) {
            if (error)
                *error = XR_BC_ERR_ALLOC;
            return NULL;
        }
        memset(id_map, -1, map_size * sizeof(int));

        for (uint32_t i = 1; i <= max_symbol_id; i++) {
            char *name = bc_get_string(&r);
            if (r.error != XR_BC_OK) {
                xr_free(id_map);
                if (error)
                    *error = r.error;
                return NULL;
            }
            if (name && name[0]) {
                id_map[i] = xr_symbol_register_in_table(xr_isolate_get_symbol_table(X), name);
                xr_free(name);
            } else {
                id_map[i] = -1;
                xr_free(name);
            }
        }
    }

    // Read Proto
    XrProto *proto = bc_read_proto_depth(&r, 0);

    // Remap symbol IDs
    if (proto && id_map) {
        remap_symbols_in_proto(proto, id_map, map_size);
    }
    if (proto)
        proto->shared_count = (int) shared_count;

    xr_free(id_map);
    if (error)
        *error = r.error;
    return proto;
}

int xr_eval_bytecode(XrVMRuntime *X, const uint8_t *data, size_t size) {
    XR_DCHECK(X != NULL, "eval_bytecode: NULL isolate");
    XR_DCHECK(data != NULL, "eval_bytecode: NULL data");
    XrBcError error;
    XrProto *proto = xr_bytecode_read(X, data, size, &error);
    if (!proto) {
        xr_log_warning("bytecode", "failed to load: error=%d", error);
        return -1;
    }

    // Use xr_execute which properly initializes coroutine and runtime
    int result = xr_execute(X, proto);
    xr_vm_proto_free(proto);
    return result;
}

/* ========== AOT Bytecode Load (decomposed API) ========== */

XrProto *xr_bytecode_load(XrVMRuntime *X, const uint8_t *data, size_t size) {
    XR_DCHECK(X != NULL, "bytecode_load: NULL isolate");
    XR_DCHECK(data != NULL, "bytecode_load: NULL data");
    XrBcError error;
    XrProto *proto = xr_bytecode_read(X, data, size, &error);
    if (!proto) {
        xr_log_warning("bytecode", "failed to load: error=%d", error);
        return NULL;
    }
    return proto;
}

/* ========== AOT Registration Helpers ========== */

const char *xr_proto_name(XrProto *p) {
    if (!p || !p->name)
        return NULL;
    return XR_STRING_CHARS(p->name);
}

XrProto **xr_proto_children(XrProto *p, int *count) {
    if (!p) {
        *count = 0;
        return NULL;
    }
    *count = PROTO_PROTO_COUNT(p);
    if (*count == 0)
        return NULL;
    return (XrProto **) p->protos.data;
}

void xr_proto_set_param_types(XrProto *p, const uint8_t *ptypes, int nparams, uint8_t return_type) {
    if (!p)
        return;
    p->return_type_info = xr_slot_type_to_type(NULL, return_type);
    if (nparams > 0 && ptypes && !p->param_types) {
        p->param_types = (struct XrType **) xr_calloc(nparams, sizeof(struct XrType *));
        if (p->param_types) {
            p->param_types_count = nparams;
            for (int i = 0; i < nparams; i++) {
                if (ptypes[i] == XR_SLOT_I64)
                    p->param_types[i] = xr_type_new_int(NULL);
                else if (ptypes[i] == XR_SLOT_F64)
                    p->param_types[i] = xr_type_new_float(NULL);
                else if (ptypes[i] == XR_SLOT_BOOL)
                    p->param_types[i] = xr_type_new_bool(NULL);
            }
        }
    }
}

int xr_run_bytecode_file(XrVMRuntime *X, const char *bytecode_file) {
    XR_DCHECK(X != NULL, "run_bytecode_file: NULL isolate");
    XR_DCHECK(bytecode_file != NULL, "run_bytecode_file: NULL bytecode_file");

    /* xr_file_read_all checks ftell, allocates with xr_malloc, and reports
     * the number of bytes that fread actually delivered, closing the door
     * on the previous unchecked-ftell + unchecked-fread pattern. */
    size_t size = 0;
    char *data = xr_file_read_all(bytecode_file, "rb", &size);
    if (!data) {
        xr_log_warning("bytecode", "cannot open or read: %s", bytecode_file);
        return -1;
    }

    int result = xr_eval_bytecode(X, (uint8_t *) data, size);
    xr_free(data);
    return result;
}

/* ========== Output Format ========== */

XrOutputFormat xr_detect_output_format(const char *filename, XrOutputFormat explicit_fmt) {
    if (explicit_fmt != XR_OUTPUT_AUTO)
        return explicit_fmt;
    if (!filename)
        return XR_OUTPUT_BYTECODE;

    const char *ext = strrchr(filename, '.');
    if (!ext)
        return XR_OUTPUT_BYTECODE;

    if (strcmp(ext, ".c") == 0)
        return XR_OUTPUT_C_SOURCE;
    if (strcmp(ext, ".h") == 0)
        return XR_OUTPUT_C_HEADER;
    if (strcmp(ext, ".xrc") == 0)
        return XR_OUTPUT_BYTECODE;

    return XR_OUTPUT_BYTECODE;
}

bool xr_output_c_source(XrVMRuntime *X, XrProto *proto, const char *output_file,
                        const char *var_name, int flags) {
    // Serialize
    size_t bc_size;
    uint8_t *bc = xr_bytecode_write(X, proto, flags, &bc_size);
    if (!bc)
        return false;

    // Write C file
    FILE *f = fopen(output_file, "w");
    if (!f) {
        xr_free(bc);
        return false;
    }

    fprintf(f, "/* Auto-generated by xray compile */\n\n");
    fprintf(f, "#include <stdint.h>\n\n");
    fprintf(f, "const uint32_t %s_size = %zu;\n\n", var_name, bc_size);
    fprintf(f, "const uint8_t %s[%zu] = {\n", var_name, bc_size);

    for (size_t i = 0; i < bc_size; i++) {
        if (i % 12 == 0)
            fprintf(f, "    ");
        fprintf(f, "0x%02x", bc[i]);
        if (i < bc_size - 1)
            fprintf(f, ",");
        if ((i + 1) % 12 == 0 || i == bc_size - 1)
            fprintf(f, "\n");
        else
            fprintf(f, " ");
    }

    fprintf(f, "};\n");

    fclose(f);
    xr_free(bc);
    return true;
}
