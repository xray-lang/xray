/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_value_image.c - Checked receiving copies of index-based value metadata
 *
 * KEY CONCEPT:
 *   One cold boundary resolves a topological image into independent sealed plans.
 *   No executing value needs the image's vectors, a compiler pool or a Layout.
 */
#include "xnative_value_image.h"
#include "xnative_array_backing.h"
#include "base/xchecks.h"
#include <stdalign.h>

typedef struct ImageBacking {
    const XrtValuePlan *plan;
    XrtManagedOps *ops;
    XrtValueKind kind;
} ImageBacking;
struct XrtValueImagePlans {
    XrtBillingOwner *billing;
    size_t bytes;
    uint32_t count;
    ImageBacking *backings;
};
typedef struct ImageScratch {
    void *memory;
    size_t bytes;
    XrtValueSlotSpec *fields, *captures;
    XrtValueParameterSpec *parameters;
} ImageScratch;
_Static_assert(sizeof(XrtValueImagePlans) % alignof(XrtValuePlan *) == 0,
    "receiving plan references follow the aligned set prefix");
_Static_assert(alignof(XrtValuePlan *) >= alignof(ImageBacking) &&
    sizeof(XrtValuePlan *) % alignof(ImageBacking) == 0,
    "object bindings follow the aligned plan vector");
_Static_assert(sizeof(XrtValueSlotSpec) % alignof(XrtValueParameterSpec) == 0,
    "cold scratch preserves parameter vector alignment");

static XrtValuePlan **image_plans(XrtValueImagePlans *set) {
    return (XrtValuePlan **)(set + 1);
}
static bool image_lease_valid(XrtManagedLease code) {
    return (code.retain && code.drop) || (!code.context && !code.retain && !code.drop);
}
static bool image_span(uint32_t first, uint32_t count, uint32_t total) {
    return count ? first <= total && count <= total - first : !first;
}
static bool image_name(const XrtValueImage *image, size_t offset, size_t length) {
    return length ? offset <= image->name_bytes && length <= image->name_bytes - offset : !offset;
}
static const char *image_name_bytes(const XrtValueImage *image, size_t offset, size_t length) {
    return length ? image->names + offset : NULL;
}
static bool image_extent(size_t *bytes, size_t count, size_t stride) {
    if (count > (SIZE_MAX - *bytes) / stride) return false;
    *bytes += count * stride;
    return true;
}
static bool image_shape(const XrtValueImage *image, bool *has_backings) {
    if (!image || ((image->type_count != 0) != (image->types != NULL)) ||
        ((image->slot_count != 0) != (image->slots != NULL)) ||
        ((image->parameter_count != 0) != (image->parameters != NULL)) ||
        ((image->owned_count != 0) != (image->owned_offsets != NULL)) ||
        ((image->name_bytes != 0) != (image->names != NULL)) ||
        (!image->type_count && (image->slot_count || image->parameter_count || image->owned_count || image->name_bytes)))
        return false;
    for (uint32_t i = 0; i < image->slot_count; ++i) {
        const XrtValueImageSlot *slot = &image->slots[i];
        if (!slot->type || slot->type > image->type_count || (slot->flags & ~(uint32_t)XRT_VALUE_FIELD_MUTABLE) ||
            !image_name(image, slot->name_offset, slot->name_length)) return false;
    }
    for (uint32_t i = 0; i < image->parameter_count; ++i) {
        const XrtValueImageParameter *parameter = &image->parameters[i];
        if (!parameter->type || parameter->type > image->type_count ||
            parameter->mode > XR_PARAM_MOVE || parameter->escaping > 1 ||
            !image_name(image, parameter->name_offset, parameter->name_length)) return false;
    }
    for (uint32_t i = 0; i < image->type_count; ++i) {
        const XrtValueImageType *type = &image->types[i];
        *has_backings |= type->kind == XRT_VALUE_ARRAY_INLINE || type->kind == XRT_VALUE_CLASS;
        if ((unsigned)type->kind > XRT_VALUE_CLASS || type->throws > 1 ||
            !image_span(type->first_owned, type->owned_count, image->owned_count) ||
            !image_span(type->first_field, type->field_count, image->slot_count) ||
            !image_span(type->first_capture, type->capture_count, image->slot_count) ||
            !image_span(type->first_parameter, type->parameter_count, image->parameter_count) ||
            type->nullable_element > i || type->result > i || type->entry_signature > i || type->element > i ||
            type->class_parent > i || (type->class_parent && (type->kind != XRT_VALUE_CLASS ||
                image->types[type->class_parent - 1].kind != XRT_VALUE_CLASS)))
            return false;
        for (uint32_t n = 0; n < type->field_count; ++n)
            if (image->slots[type->first_field + n].type > i) return false;
        for (uint32_t n = 0; n < type->capture_count; ++n)
            if (image->slots[type->first_capture + n].type > i || image->slots[type->first_capture + n].flags) return false;
        for (uint32_t n = 0; n < type->parameter_count; ++n)
            if (image->parameters[type->first_parameter + n].type > i) return false;
        if ((type->flags & XRT_VALUE_INLINE_DISPLAY) && !xrt_value_image_inline_display_shape(image, i))
            return false;
    }
    return true;
}
static XrtManagedStatus image_scratch(XrtBillingOwner *billing, const XrtValueImage *image,
    ImageScratch *scratch) {
    uint32_t fields = 0, captures = 0, parameters = 0;
    for (uint32_t i = 0; i < image->type_count; ++i) {
        const XrtValueImageType *type = &image->types[i];
        if (type->field_count > fields) fields = type->field_count;
        if (type->capture_count > captures) captures = type->capture_count;
        if (type->parameter_count > parameters) parameters = type->parameter_count;
    }
    if (!image_extent(&scratch->bytes, fields, sizeof(XrtValueSlotSpec)) ||
        !image_extent(&scratch->bytes, captures, sizeof(XrtValueSlotSpec)) ||
        !image_extent(&scratch->bytes, parameters, sizeof(XrtValueParameterSpec))) return XRT_MANAGED_OVERFLOW;
    if (!scratch->bytes) return XRT_MANAGED_OK;
    XrtManagedStatus status = xrt_managed_metadata_allocate(billing, scratch->bytes, &scratch->memory);
    if (status != XRT_MANAGED_OK) return status;
    scratch->fields = scratch->memory;
    scratch->captures = scratch->fields + fields;
    scratch->parameters = (XrtValueParameterSpec *)(scratch->captures + captures);
    return XRT_MANAGED_OK;
}
static const XrtValuePlan *image_dependency(XrtValueImagePlans *set, uint32_t id) {
    return id ? image_plans(set)[id - 1] : NULL;
}
static void image_slots(XrtValueImagePlans *set, const XrtValueImage *image,
    uint32_t first, uint32_t count, XrtValueSlotSpec *output) {
    for (uint32_t i = 0; i < count; ++i) {
        const XrtValueImageSlot *slot = &image->slots[first + i];
        output[i] = (XrtValueSlotSpec){image_dependency(set, slot->type), slot->offset,
            image_name_bytes(image, slot->name_offset, slot->name_length), slot->name_length, slot->flags};
    }
}
static XrtValuePlanSpec image_plan(XrtValueImagePlans *set, const XrtValueImage *image,
    uint32_t index, ImageScratch *scratch, XrtManagedLease code) {
    const XrtValueImageType *type = &image->types[index];
    image_slots(set, image, type->first_field, type->field_count, scratch->fields);
    image_slots(set, image, type->first_capture, type->capture_count, scratch->captures);
    for (uint32_t i = 0; i < type->parameter_count; ++i) {
        const XrtValueImageParameter *parameter = &image->parameters[type->first_parameter + i];
        scratch->parameters[i] = (XrtValueParameterSpec){image_dependency(set, parameter->type),
            (XrParamMode)parameter->mode, parameter->escaping != 0,
            image_name_bytes(image, parameter->name_offset, parameter->name_length), parameter->name_length};
    }
    XrtValuePlanSpec spec = {.kind=type->kind, .size=type->size, .alignment=type->alignment, .count=type->count,
        .owned_offsets=type->owned_count ? image->owned_offsets + type->first_owned : NULL,
        .owned_count=type->owned_count, .fields=type->field_count ? scratch->fields : NULL,
        .field_count=type->field_count, .captures=type->capture_count ? scratch->captures : NULL,
        .capture_count=type->capture_count, .nullable={type->nullable_tag, type->nullable_payload,
            image_dependency(set, type->nullable_element)},
        .signature={type->parameter_count ? scratch->parameters : NULL, type->parameter_count,
            image_dependency(set, type->result), type->flags & ~(uint32_t)XRT_VALUE_INLINE_DISPLAY, type->throws != 0},
        .entry={type->entry_ordinal, image_dependency(set, type->entry_signature)},
        .element=image_dependency(set, type->element), .class_parent=image_dependency(set, type->class_parent)};
    if (type->kind == XRT_VALUE_FUNCTION || type->kind == XRT_VALUE_ENVIRONMENT) spec.code = code;
    return spec;
}
XR_FUNCDEF void xrt_value_image_drop(XrtValueImagePlans **owned) {
    if (!owned || !*owned) return;
    XrtValueImagePlans *set = *owned;
    *owned = NULL;
    XrtBillingOwner *billing = set->billing;
    size_t bytes = set->bytes;
    if (set->backings)
        for (uint32_t i = set->count; i; --i) xrt_managed_ops_drop(set->backings[i - 1].ops);
    for (uint32_t i = set->count; i; --i) xrt_value_plan_drop(image_plans(set)[i - 1]);
    xrt_managed_metadata_free(billing, set, bytes);
    xrt_billing_owner_drop(billing);
}
XR_FUNCDEF XrtManagedStatus xrt_value_image_bind(XrtBillingOwner *billing,
    const XrtValueImage *image, XrtManagedLease code, XrtValueImagePlans **output) {
    if (!billing || !image || !output || *output || !image_lease_valid(code)) return XRT_MANAGED_ARGUMENT;
    if (code.retain && !code.retain(code.context)) return XRT_MANAGED_REFCOUNT;
    bool has_backings = false;
    XrtManagedStatus status = image_shape(image, &has_backings) ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
    XrtValueImagePlans *set = NULL;
    ImageScratch scratch = {0};
    if (status == XRT_MANAGED_OK && image->type_count) {
        size_t bytes = sizeof(*set);
        if (!image_extent(&bytes, image->type_count, sizeof(XrtValuePlan *)) ||
            (has_backings && !image_extent(&bytes, image->type_count, sizeof(ImageBacking)))) status = XRT_MANAGED_OVERFLOW;
        else if (!xrt_billing_owner_retain(billing)) status = XRT_MANAGED_REFCOUNT;
        else {
            void *memory = NULL;
            status = xrt_managed_metadata_allocate(billing, bytes, &memory);
            if (status != XRT_MANAGED_OK) xrt_billing_owner_drop(billing);
            else {
                set = memory;
                *set = (XrtValueImagePlans){.billing = billing, .bytes = bytes};
                if (has_backings)
                    set->backings = (ImageBacking *)(image_plans(set) + image->type_count);
                status = image_scratch(billing, image, &scratch);
            }
        }
        for (uint32_t i = 0; status == XRT_MANAGED_OK && i < image->type_count; ++i) {
            XrtValuePlanSpec spec = image_plan(set, image, i, &scratch, code);
            XrtValuePlan *plan = NULL;
            status = xrt_value_plan_new(billing, &spec, &plan);
            if (status == XRT_MANAGED_OK) {
                image_plans(set)[set->count++] = plan;
                if (set->backings) {
                    ImageBacking *backing = &set->backings[i];
                    *backing = (ImageBacking){0};
                    if (spec.kind == XRT_VALUE_ARRAY_INLINE) {
                        status = xrt_array_backing_inline_bind(billing, spec.element, &backing->ops);
                        if (status == XRT_MANAGED_OK) *backing = (ImageBacking){spec.element,backing->ops,spec.kind};
                    } else if (spec.kind == XRT_VALUE_CLASS) {
                        status = xrt_class_backing_bind(billing, plan, &backing->ops);
                        if (status == XRT_MANAGED_OK) *backing = (ImageBacking){plan,backing->ops,spec.kind};
                    }
                }
            }
        }
    }
    if (scratch.memory) xrt_managed_metadata_free(billing, scratch.memory, scratch.bytes);
    if (status == XRT_MANAGED_OK) { *output = set; set = NULL; }
    xrt_value_image_drop(&set);
    if (code.drop) code.drop(code.context);
    return status;
}
XR_FUNCDEF uint32_t xrt_value_image_count(const XrtValueImagePlans *set) { return set ? set->count : 0; }
XR_FUNCDEF XrtManagedStatus xrt_value_image_get(const XrtValueImagePlans *set, uint32_t id,
    const XrtValuePlan **output) {
    if (!set || !id || id > set->count || !output || *output) return XRT_MANAGED_ARGUMENT;
    *output = image_plans((XrtValueImagePlans *)set)[id - 1];
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_value_image_inline_array(const XrtValueImagePlans *set, uint32_t id,
    XrtBoundInlineArray *empty) {
    if (!set || !id || id > set->count || !empty || empty->element || empty->ops ||
        !set->backings || set->backings[id - 1].kind != XRT_VALUE_ARRAY_INLINE) return XRT_MANAGED_ARGUMENT;
    ImageBacking backing = set->backings[id - 1];
    *empty = (XrtBoundInlineArray){backing.plan,backing.ops};
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_value_image_class(const XrtValueImagePlans *set, uint32_t id,
    XrtBoundClass *empty) {
    if (!set || !id || id > set->count || !empty || empty->plan || empty->ops ||
        !set->backings || set->backings[id - 1].kind != XRT_VALUE_CLASS) return XRT_MANAGED_ARGUMENT;
    ImageBacking backing = set->backings[id - 1];
    *empty = (XrtBoundClass){backing.plan,backing.ops};
    return XRT_MANAGED_OK;
}
