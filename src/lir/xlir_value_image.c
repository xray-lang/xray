/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_image.c - Linear raw serialization with one cold dependency index
 *
 * KEY CONCEPT:
 *   Receiving plans own every source byte. Serialization copies their facts,
 *   then the image's independent billing lease owns all published raw bytes.
 */
#include "xlir_value_image.h"
#include "base/xchecks.h"
#include "runtime/native/xnative_value_image.h"
#include <stdalign.h>
#include <string.h>

struct XrLirValueImage { XrtBillingOwner *billing; size_t bytes; XrtValueImage view; };
typedef struct ImageRow { const XrtValuePlan *plan; XrtValuePlanSpec spec; } ImageRow;
typedef struct ImageIndex { const XrtValuePlan *plan; uint32_t id; } ImageIndex;
typedef struct ImageCounts { uint32_t owned, slots, parameters; size_t names; } ImageCounts;
typedef struct ImageInventory {
    const ImageRow *rows; uint32_t count;
    const ImageIndex *index; size_t capacity; ImageCounts counts;
} ImageInventory;
typedef struct ImageBuilder {
    XrtValueImageType *types; XrtValueImageSlot *slots; XrtValueImageParameter *parameters;
    size_t *owned; char *names;
    const ImageIndex *index; size_t capacity;
    ImageCounts used;
} ImageBuilder;
_Static_assert(sizeof(XrLirValueImage) % alignof(XrtValueImageType) == 0 &&
    sizeof(XrtValueImageType) % alignof(XrtValueImageSlot) == 0 &&
    sizeof(XrtValueImageSlot) % alignof(XrtValueImageParameter) == 0 &&
    sizeof(XrtValueImageParameter) % alignof(size_t) == 0 && sizeof(ImageRow) % alignof(ImageIndex) == 0,
    "one exact raw block and one temporary index preserve native alignments");

static XrLirValueLayoutStatus image_memory(XrtManagedStatus status) {
    switch (status) {
    case XRT_MANAGED_OK: return XR_LIR_VALUE_LAYOUT_OK;
    case XRT_MANAGED_OOM: return XR_LIR_VALUE_LAYOUT_OOM;
    case XRT_MANAGED_LIMIT: return XR_LIR_VALUE_LAYOUT_LIMIT;
    case XRT_MANAGED_CLOSED: return XR_LIR_VALUE_LAYOUT_CLOSED;
    case XRT_MANAGED_REFCOUNT: return XR_LIR_VALUE_LAYOUT_REFCOUNT;
    case XRT_MANAGED_OVERFLOW: return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    default: return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    }
}
static bool image_extent(size_t *bytes, size_t count, size_t stride) {
    if (stride && count > (SIZE_MAX - *bytes) / stride) return false;
    *bytes += count * stride; return true;
}
static bool image_count(uint32_t *count, size_t add) {
    if (add > UINT32_MAX - *count) return false;
    *count += (uint32_t)add; return true;
}
static size_t image_hash(const XrtValuePlan *plan) {
    uintptr_t key = (uintptr_t)plan;
    key ^= key >> 17; key *= (uintptr_t)UINT64_C(0x9e3779b97f4a7c15); key ^= key >> 11;
    return (size_t)key;
}
static bool image_insert(ImageIndex *index, size_t capacity, const XrtValuePlan *plan, uint32_t id) {
    size_t slot = image_hash(plan) & (capacity - 1);
    while (index[slot].plan) {
        if (index[slot].plan == plan) return false;
        slot = (slot + 1) & (capacity - 1);
    }
    index[slot] = (ImageIndex){plan, id}; return true;
}
static bool image_child(const ImageBuilder *builder, const XrtValuePlan *plan, uint32_t parent, uint32_t *id) {
    if (!plan) { *id = 0; return true; }
    size_t slot = image_hash(plan) & (builder->capacity - 1);
    while (builder->index[slot].plan && builder->index[slot].plan != plan) slot = (slot + 1) & (builder->capacity - 1);
    *id = builder->index[slot].id;
    return *id && *id < parent;
}
static XrLirValueLayoutStatus image_inventory(const XrLirValueLayouts *set, ImageRow *rows,
    ImageIndex *index, size_t capacity, ImageCounts *counts) {
    size_t count = xr_lir_value_layouts_count(set);
    for (size_t i = 0; i < count; ++i) {
        XrLirValueLayoutKey key;
        if (xr_lir_value_layouts_at(set, i, &key, &rows[i].plan) != XR_LIR_VALUE_LAYOUT_OK ||
            xrt_value_plan_view(rows[i].plan, &rows[i].spec) != XRT_MANAGED_OK ||
            !image_insert(index, capacity, rows[i].plan, (uint32_t)i + 1)) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        const XrtValuePlanSpec *spec = &rows[i].spec;
        if (!image_count(&counts->owned, spec->owned_count) || !image_count(&counts->slots, spec->field_count) ||
            !image_count(&counts->slots, spec->capture_count) || !image_count(&counts->parameters, spec->signature.parameter_count))
            return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        for (size_t field = 0; field < spec->field_count; ++field)
            if (!image_extent(&counts->names, spec->fields[field].name_length, 1)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        for (size_t capture = 0; capture < spec->capture_count; ++capture)
            if (!image_extent(&counts->names, spec->captures[capture].name_length, 1)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        for (size_t parameter = 0; parameter < spec->signature.parameter_count; ++parameter)
            if (!image_extent(&counts->names, spec->signature.parameters[parameter].name_length, 1)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    }
    return XR_LIR_VALUE_LAYOUT_OK;
}
static size_t image_name(ImageBuilder *builder, const char *name, size_t length) {
    if (!length) return 0;
    size_t offset = builder->used.names;
    memcpy(builder->names + offset, name, length); builder->used.names += length;
    return offset;
}
static bool image_slots(ImageBuilder *builder, const XrtValueSlotSpec *slots, size_t count, uint32_t parent) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t id;
        if (!image_child(builder, slots[i].type, parent, &id)) return false;
        builder->slots[builder->used.slots++] = (XrtValueImageSlot){id, slots[i].offset,
            image_name(builder, slots[i].name, slots[i].name_length), slots[i].name_length};
    }
    return true;
}
static bool image_type(ImageBuilder *builder, const XrtValuePlanSpec *spec, uint32_t id) {
    XrtValueImageType *row = &builder->types[id - 1];
    *row = (XrtValueImageType){.kind=spec->kind, .size=spec->size, .alignment=spec->alignment, .count=spec->count,
        .first_owned=spec->owned_count ? builder->used.owned : 0, .owned_count=(uint32_t)spec->owned_count,
        .first_field=spec->field_count ? builder->used.slots : 0, .field_count=(uint32_t)spec->field_count,
        .flags=spec->signature.flags, .throws=spec->signature.throws,
        .nullable_tag=spec->nullable.tag_offset, .nullable_payload=spec->nullable.payload_offset,
        .entry_ordinal=spec->entry.ordinal};
    if (spec->owned_count) memcpy(builder->owned + builder->used.owned, spec->owned_offsets, spec->owned_count * sizeof(size_t));
    builder->used.owned += row->owned_count;
    if (!image_slots(builder, spec->fields, spec->field_count, id)) return false;
    row->first_capture = spec->capture_count ? builder->used.slots : 0; row->capture_count = (uint32_t)spec->capture_count;
    if (!image_slots(builder, spec->captures, spec->capture_count, id)) return false;
    row->first_parameter = spec->signature.parameter_count ? builder->used.parameters : 0;
    row->parameter_count = (uint32_t)spec->signature.parameter_count;
    for (size_t i = 0; i < spec->signature.parameter_count; ++i) {
        XrtValueParameterSpec source = spec->signature.parameters[i]; uint32_t child;
        if (!image_child(builder, source.type, id, &child)) return false;
        builder->parameters[builder->used.parameters++] = (XrtValueImageParameter){child, (uint32_t)source.mode, source.escaping,
            image_name(builder, source.name, source.name_length), source.name_length};
    }
    bool complete = image_child(builder, spec->nullable.element, id, &row->nullable_element) &&
        image_child(builder, spec->signature.result, id, &row->result) &&
        image_child(builder, spec->entry.signature, id, &row->entry_signature) &&
        image_child(builder, spec->element, id, &row->element);
    const XrtValueImage view = {.types=builder->types, .slots=builder->slots};
    if (complete && xrt_value_image_inline_display_shape(&view, id - 1))
        row->flags |= XRT_VALUE_INLINE_DISPLAY;
    return complete;
}
static XrLirValueLayoutStatus image_build(XrtBillingOwner *billing, ImageInventory inventory, XrLirValueImage **output) {
    uint32_t count = inventory.count; ImageCounts counts = inventory.counts;
    size_t bytes = sizeof(XrLirValueImage);
    if (!image_extent(&bytes, count, sizeof(XrtValueImageType)) || !image_extent(&bytes, counts.slots, sizeof(XrtValueImageSlot)) ||
        !image_extent(&bytes, counts.parameters, sizeof(XrtValueImageParameter)) || !image_extent(&bytes, counts.owned, sizeof(size_t)) ||
        !image_extent(&bytes, counts.names, 1)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    void *memory = NULL;
    XrLirValueLayoutStatus status = image_memory(xrt_managed_metadata_allocate(billing, bytes, &memory));
    if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
    XrLirValueImage *owner = memory;
    *owner = (XrLirValueImage){.billing=billing, .bytes=bytes};
    ImageBuilder builder = {.types=(XrtValueImageType *)(owner + 1), .index=inventory.index, .capacity=inventory.capacity};
    builder.slots = (XrtValueImageSlot *)(builder.types + count);
    builder.parameters = (XrtValueImageParameter *)(builder.slots + counts.slots);
    builder.owned = (size_t *)(builder.parameters + counts.parameters); builder.names = (char *)(builder.owned + counts.owned);
    owner->view = (XrtValueImage){builder.types, count, counts.slots ? builder.slots : NULL, counts.slots,
        counts.parameters ? builder.parameters : NULL, counts.parameters, counts.owned ? builder.owned : NULL,
        counts.owned, counts.names ? builder.names : NULL, counts.names};
    for (uint32_t i = 0; i < count; ++i) {
        if (!image_type(&builder, &inventory.rows[i].spec, i + 1)) {
            xrt_managed_metadata_free(billing, owner, bytes); return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        }
    }
    XR_CHECK(builder.used.owned == counts.owned && builder.used.slots == counts.slots &&
        builder.used.parameters == counts.parameters && builder.used.names == counts.names,
        "published raw vectors exactly match their checked receiving inventory");
    *output = owner; return XR_LIR_VALUE_LAYOUT_OK;
}
XR_FUNCDEF XrLirValueLayoutStatus xr_lir_value_image_create(const XrLirValueLayouts *set,
    XrtBillingOwner *billing, XrLirValueImage **output) {
    if (!set || !billing || !output || *output) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    size_t count = xr_lir_value_layouts_count(set);
    if (!count) return XR_LIR_VALUE_LAYOUT_OK;
    if (count > UINT32_MAX || count > SIZE_MAX / 2) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    size_t capacity = 2, bytes = 0;
    while (capacity < count * 2) {
        if (capacity > SIZE_MAX / 2) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        capacity *= 2;
    }
    if (!image_extent(&bytes, count, sizeof(ImageRow)) || !image_extent(&bytes, capacity, sizeof(ImageIndex))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    if (!xrt_billing_owner_retain(billing)) return XR_LIR_VALUE_LAYOUT_REFCOUNT;
    void *memory = NULL; XrLirValueImage *pending = NULL;
    XrLirValueLayoutStatus status = image_memory(xrt_managed_metadata_allocate(billing, bytes, &memory));
    if (status == XR_LIR_VALUE_LAYOUT_OK) {
        ImageRow *rows = memory; ImageIndex *index = (ImageIndex *)(rows + count); ImageCounts counts = {0};
        memset(index, 0, capacity * sizeof(*index));
        status = image_inventory(set, rows, index, capacity, &counts);
        if (status == XR_LIR_VALUE_LAYOUT_OK)
            status = image_build(billing, (ImageInventory){rows, (uint32_t)count, index, capacity, counts}, &pending);
        xrt_managed_metadata_free(billing, memory, bytes);
    }
    if (status == XR_LIR_VALUE_LAYOUT_OK) *output = pending;
    else xrt_billing_owner_drop(billing);
    return status;
}
XR_FUNCDEF const XrtValueImage *xr_lir_value_image_view(const XrLirValueImage *image) {
    static const XrtValueImage empty = {0};
    return image ? &image->view : &empty;
}
XR_FUNCDEF void xr_lir_value_image_destroy(XrLirValueImage *image) {
    if (!image) return;
    XrtBillingOwner *billing = image->billing; size_t bytes = image->bytes;
    xrt_managed_metadata_free(billing, image, bytes); xrt_billing_owner_drop(billing);
}
