/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_vm_format.inc.c - Borrowed VM layout adapter for value formatting
 */

static int vm_format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    const XrVmCode *code = context;
    const XrVmValue *value = node.value;
    if (!code || !value)
        return 0;
    view->kind = XR_VALUE_FORMAT_SIGNED;
    switch (value->kind) {
        case XR_VM_VALUE_I8: view->signed_value = value->as.i8; return 1;
        case XR_VM_VALUE_I16: view->signed_value = value->as.i16; return 1;
        case XR_VM_VALUE_I32: view->signed_value = value->as.i32; return 1;
        case XR_VM_VALUE_I64: view->signed_value = value->as.i64; return 1;
        case XR_VM_VALUE_U8: view->unsigned_value = value->as.u8; break;
        case XR_VM_VALUE_U16: view->unsigned_value = value->as.u16; break;
        case XR_VM_VALUE_U32: view->unsigned_value = value->as.u32; break;
        case XR_VM_VALUE_U64: view->unsigned_value = value->as.u64; break;
        case XR_VM_VALUE_BOOL:
            view->kind = XR_VALUE_FORMAT_BOOL;
            view->unsigned_value = value->as.boolean;
            return 1;
        case XR_VM_VALUE_STRING: {
            XrVmStringView string;
            if (!xr_vm_value_string_view(value, &string))
                return 0;
            view->kind = XR_VALUE_FORMAT_BYTES;
            view->bytes = string.bytes;
            view->size = string.size;
            view->quote = '"';
            return 1;
        }
        case XR_VM_VALUE_RUNE:
            view->kind = XR_VALUE_FORMAT_BYTES;
            view->size = xr_text_encode_rune(value->as.rune, view->scalar_bytes);
            view->bytes = view->scalar_bytes;
            view->quote = '\'';
            return view->size != 0u;
        case XR_VM_VALUE_AGGREGATE: {
            XrVmAggregateView aggregate;
            if (!xr_vm_value_aggregate_view(value, &aggregate))
                return 0;
            const XrValidatedType *type = xr_validated_program_type(code->program, aggregate.type_id);
            if (!type || type->kind != XR_CORE_IR_TYPE_VARIANT ||
                aggregate.variant_ordinal >= type->variant_count)
                return 0;
            view->kind = XR_VALUE_FORMAT_ENUM;
            view->name = type->display_name;
            view->member = type->variants[aggregate.variant_ordinal].display_name;
            view->children = aggregate.field_count;
            return 1;
        }
        default:
            return 0;
    }
    view->kind = XR_VALUE_FORMAT_UNSIGNED;
    return 1;
}

static int vm_format_child(const void *context, XrValueFormatNode node, uint32_t index,
                           XrValueFormatNode *child) {
    (void) context;
    XrVmAggregateView aggregate;
    if (!xr_vm_value_aggregate_view(node.value, &aggregate) || index >= aggregate.field_count)
        return 0;
    child->value = &aggregate.fields[index];
    child->type = 0u;
    return 1;
}

XrValueFormatReader xr_vm_value_format_reader(const XrVmCode *code) {
    return (XrValueFormatReader) {code, vm_format_read, vm_format_child};
}
