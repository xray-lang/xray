/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_state_admission.inc.c - Bounded raw State descriptions and capability uses
 *
 * KEY CONCEPT:
 *   Compilation reads raw immutable descriptions without binding runtime plans.
 *   Execution preparation performs complete receiving layout validation.
 */
static bool rbc_state_register_kind(uint32_t kind, XrRbcRegisterKind *payload) {
    if (!payload) return false;
    switch (kind) {
    case XRT_VALUE_UNIT: case XRT_VALUE_BOOL: case XRT_VALUE_RUNE:
    case XRT_VALUE_I8: case XRT_VALUE_I16: case XRT_VALUE_I32: case XRT_VALUE_I64:
    case XRT_VALUE_U8: case XRT_VALUE_U16: case XRT_VALUE_U32: case XRT_VALUE_U64:
    case XRT_VALUE_F32: case XRT_VALUE_F64: *payload = XR_RBC_BITS; return true;
    case XRT_VALUE_STRING: *payload = XR_RBC_STRING; return true;
    default: return false;
    }
}
static bool xr_rbc_state_payload_kind(uint32_t kind) {
    XrRbcRegisterKind payload;
    return xrt_value_kind_has_fields(kind) || kind==XRT_VALUE_NULLABLE || kind==XRT_VALUE_FUNCTION ||
        kind==XRT_VALUE_MAP_STRING_I64 || kind==XRT_VALUE_SET_I64 || kind==XRT_VALUE_CLASS ||
        xrt_value_kind_is_array(kind) ||
        rbc_state_register_kind(kind,&payload);
}
static bool rbc_state_span(size_t first, size_t count, size_t total) {
    return (!count ? !first : first <= total && count <= total - first);
}
static bool rbc_state_vector(const void *data, size_t count, size_t stride) {
    return (!!data == !!count) && (!stride || count <= SIZE_MAX / stride);
}
static bool rbc_state_alignment(size_t alignment) {
    return alignment && !(alignment & (alignment - 1));
}
static bool rbc_state_image_shape(const XrtValueImage *image) {
    if (!image || image->type_count > UINT32_MAX / 2 ||
        !rbc_state_vector(image->types,image->type_count,sizeof(*image->types)) ||
        !rbc_state_vector(image->slots,image->slot_count,sizeof(*image->slots)) ||
        !rbc_state_vector(image->parameters,image->parameter_count,sizeof(*image->parameters)) ||
        !rbc_state_vector(image->owned_offsets,image->owned_count,sizeof(*image->owned_offsets)) ||
        !rbc_state_vector(image->names,image->name_bytes,1)) return false;
    for (uint32_t i = 0; i < image->type_count; ++i) {
        const XrtValueImageType *type = &image->types[i];
        if ((uint32_t)type->kind > XRT_VALUE_CLASS || !rbc_state_alignment(type->alignment) ||
            type->size % type->alignment ||
            !rbc_state_span(type->first_owned,type->owned_count,image->owned_count) ||
            !rbc_state_span(type->first_field,type->field_count,image->slot_count) ||
            !rbc_state_span(type->first_capture,type->capture_count,image->slot_count) ||
            !rbc_state_span(type->first_parameter,type->parameter_count,image->parameter_count) ||
            type->nullable_element > i || type->result > i || type->entry_signature > i || type->element > i ||
            type->class_parent > i || (type->class_parent && (type->kind != XRT_VALUE_CLASS ||
                image->types[type->class_parent - 1].kind != XRT_VALUE_CLASS)))
            return false;
        if (type->kind == XRT_VALUE_ARRAY_INLINE &&
            (!type->element || !xrt_value_kind_has_fields(image->types[type->element - 1].kind))) return false;
        if (type->kind == XRT_VALUE_CLASS &&
            (!type->element || image->types[type->element-1].kind != XRT_VALUE_RECORD ||
                type->size != sizeof(XrtManagedHeader *) || type->alignment != _Alignof(XrtManagedHeader *) ||
                type->owned_count != 1 || image->owned_offsets[type->first_owned])) return false;
        size_t end = 0;
        for (uint32_t o = 0; o < type->owned_count; ++o) {
            size_t offset = image->owned_offsets[type->first_owned + o];
            if (offset < end || offset % _Alignof(XrtManagedHeader *) || offset > type->size ||
                sizeof(XrtManagedHeader *) > type->size - offset) return false;
            end = offset + sizeof(XrtManagedHeader *);
        }
        for (unsigned vector = 0; vector < 2; ++vector) {
            uint32_t first = vector ? type->first_capture : type->first_field;
            uint32_t count = vector ? type->capture_count : type->field_count;
            for (uint32_t f = 0; f < count; ++f) {
                const XrtValueImageSlot *field = &image->slots[first + f];
                if (!field->type || field->type > i ||
                    !rbc_state_span(field->name_offset,field->name_length,image->name_bytes)) return false;
                const XrtValueImageType *child = &image->types[field->type - 1];
                if (field->offset % child->alignment || field->offset > type->size ||
                    child->size > type->size - field->offset) return false;
            }
        }
        for (uint32_t p = 0; p < type->parameter_count; ++p) {
            const XrtValueImageParameter *parameter = &image->parameters[type->first_parameter + p];
            if (!parameter->type || parameter->type > i || parameter->mode > 2 || parameter->escaping > 1 ||
                !rbc_state_span(parameter->name_offset,parameter->name_length,image->name_bytes)) return false;
        }
        if ((type->flags & XRT_VALUE_INLINE_DISPLAY) && !xrt_value_image_inline_display_shape(image, i))
            return false;
    }
    return true;
}
static bool rbc_state_spec_shape(const XrtModuleLayoutSpec *spec) {
    if (!spec || !rbc_state_image_shape(spec->values) || !spec->module_count ||
        !rbc_state_vector(spec->modules,spec->module_count,sizeof(*spec->modules)) ||
        !rbc_state_vector(spec->dependencies,spec->dependency_count,sizeof(*spec->dependencies)) ||
        !rbc_state_vector(spec->slots,spec->slot_count,sizeof(*spec->slots)) ||
        !rbc_state_alignment(spec->payload_alignment) || spec->payload_size % spec->payload_alignment) return false;
    uint32_t first = 0;
    for (uint32_t m = 0; m < spec->module_count; ++m) {
        XrtModuleSpec module = spec->modules[m];
        if (module.first_dependency != first || module.dependency_count > spec->dependency_count - first) return false;
        for (uint32_t d = 0; d < module.dependency_count; ++d)
            if (spec->dependencies[first + d] >= spec->module_count || spec->dependencies[first + d] == m) return false;
        first += module.dependency_count;
    }
    if (first != spec->dependency_count) return false;
    size_t end = 0;
    for (uint32_t i = 0; i < spec->slot_count; ++i) {
        const XrtModuleSlotSpec *slot = &spec->slots[i];
        if (slot->module >= spec->module_count || !slot->type || slot->type > spec->values->type_count ||
            (slot->flags & ~XRT_MODULE_SLOT_MUTABLE)) return false;
        const XrtValueImageType *type = &spec->values->types[slot->type - 1];
        bool pod = !type->owned_count && (type->kind == XRT_VALUE_FIXED_U8 ||
            type->kind == XRT_VALUE_NULLABLE);
        if ((!xr_rbc_state_payload_kind(type->kind) && !pod) || slot->size != type->size || slot->alignment != type->alignment ||
            slot->alignment > spec->payload_alignment || slot->offset % slot->alignment || slot->offset < end ||
            slot->offset > spec->payload_size || slot->size > spec->payload_size - slot->offset) return false;
        end = slot->offset + slot->size;
    }
    return true;
}
static bool xr_rbc_state_layout_valid(const XrRbcModule *module) {
    const XrtModuleLayoutSpec *spec = module->state_spec;
    if (spec) {
        if (!rbc_state_spec_shape(spec) || module->state_entry >= module->function_count) return false;
        const XrRbcFunction *entry = &module->functions[module->state_entry];
        if (!entry->parameter_count || !entry->register_kinds || entry->register_kind_count != entry->register_count ||
            entry->parameter_count > entry->register_count ||
            rbc_register_kind(entry,entry->parameter_count - 1) != XR_RBC_STATE) return false;
    } else {
        if (module->state_entry) return false;
        const XrtValueImage *image = xr_rbc_module_value_image(module);
        if (image && !rbc_state_image_shape(image)) return false;
    }
    for (size_t f = 0; f < module->function_count; ++f) {
        const XrRbcFunction *function = &module->functions[f];
        if (!spec && function->register_kinds && function->register_kind_count == function->register_count)
            for (uint32_t r = 0; r < function->register_count; ++r)
                if (rbc_register_kind(function,r) == XR_RBC_STATE || rbc_register_kind(function,r) == XR_RBC_STATE_REF)
                    return false;
        if (!rbc_state_vector(function->state_accesses,function->state_access_count,sizeof(*function->state_accesses)) ||
            (function->state_access_count && !spec)) return false;
        for (size_t i = 0; i < function->state_access_count; ++i) {
            XrRbcStateAccess access = function->state_accesses[i];
            if (access.slot >= spec->slot_count || access.state >= function->register_count ||
                access.value >= function->register_count) return false;
        }
    }
    return true;
}
static bool xr_rbc_state_instruction_valid(const XrRbcModule *module, const XrRbcFunction *function, uint32_t word) {
    uint32_t op = word & 255, a = (word >> 8) & 255, b = (word >> 16) & 255, c = word >> 24, index = word >> 16;
    const XrtModuleLayoutSpec *spec = module->state_spec;
    if (!spec || !function->register_kinds || a >= function->register_count) return false;
    if (op == XR_RBC_MODULE_BEGIN || op == XR_RBC_MODULE_READY)
        return rbc_register_kind(function,a) == XR_RBC_STATE && index < spec->module_count;
    if (op == XR_RBC_STATE_PUBLISH) return !b && !c && rbc_register_kind(function,a) == XR_RBC_STATE_REF;
    XrRbcRegisterKind payload = XR_RBC_BITS;
    if (op == XR_RBC_STATE_READ || op == XR_RBC_STATE_WRITE) {
        if (b >= function->register_count || !rbc_state_register_kind(c,&payload)) return false;
        /* Owned reads require the common SSA destination home, not a register
         * address. The untyped register instruction cannot publish an owner. */
        if (op == XR_RBC_STATE_READ && c == XRT_VALUE_STRING) return false;
        return op == XR_RBC_STATE_READ ? rbc_register_kind(function,b) == XR_RBC_STATE_REF &&
            (c == XRT_VALUE_UNIT ? !a : rbc_register_kind(function,a) == payload) :
            rbc_register_kind(function,a) == XR_RBC_STATE_REF &&
            (c == XRT_VALUE_UNIT ? !b : rbc_register_kind(function,b) == payload);
    }
    if (index >= function->state_access_count) return false;
    XrRbcStateAccess access = function->state_accesses[index];
    const XrtModuleSlotSpec *slot = &spec->slots[access.slot];
    XrtValueKind kind = spec->values->types[slot->type - 1].kind;
    if (op != XR_RBC_STATE_REFERENCE && !rbc_state_register_kind(kind,&payload)) return false;
    if (op == XR_RBC_STATE_INITIALIZE)
        return !(slot->flags & XRT_MODULE_SLOT_MUTABLE) && !access.state &&
            rbc_register_kind(function,a) == XR_RBC_STATE &&
            (kind == XRT_VALUE_UNIT ? !access.value : rbc_register_kind(function,access.value) == payload);
    if (rbc_register_kind(function,a) != XR_RBC_STATE_REF ||
        rbc_register_kind(function,access.state) != XR_RBC_STATE) return false;
    if (op == XR_RBC_STATE_REFERENCE) return !access.value;
    return op == XR_RBC_STATE_CONSTRUCT && (slot->flags & XRT_MODULE_SLOT_MUTABLE) &&
        (kind == XRT_VALUE_UNIT ? !access.value : rbc_register_kind(function,access.value) == payload);
}
static uint32_t xr_rbc_state_shape(const XrRbcModule *module, uint32_t slot) {
    const XrtModuleLayoutSpec *spec = module->state_spec;
    if (!spec || slot >= spec->slot_count || !spec->slots[slot].type || spec->slots[slot].type > UINT32_MAX / 2) return 0;
    return (spec->slots[slot].type << 1) | ((spec->slots[slot].flags & XRT_MODULE_SLOT_MUTABLE) != 0);
}
static uint32_t xr_rbc_state_kind(const XrRbcModule *module, uint32_t shape) {
    const XrtModuleLayoutSpec *spec = module->state_spec;
    uint32_t id = shape >> 1;
    if (!spec || !id || id > spec->values->type_count) return UINT32_MAX;
    return (uint32_t)spec->values->types[id - 1].kind;
}
