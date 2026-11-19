/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_ops.c - Checked in-place edits and complete pending replacements
 *
 * KEY CONCEPT:
 *   All failing work precedes publication. Pending String owners occupy real
 *   tail slots so rollback uses the same iterative capacity walk as final drop.
 */
#include "xnative_array_ops.h"
#include "xnative_string_backing.h"
#include "base/xchecks.h"
#include <string.h>

static bool array_item_valid(const XrtArrayItem *item) {
    return item && xrt_backing_array_stride(item->kind) &&
        (item->kind != XRT_BACKING_ARRAY_STRING || item->value.string);
}
static XrtManagedStatus array_update_check(const XrtArrayBacking *array,
    const XrtArrayUpdate *update, size_t *length) {
    if (!array || !update || array->length > array->capacity || !array_item_valid(&update->item))
        return XRT_MANAGED_ARGUMENT;
    if (update->operation == XRT_ARRAY_UPDATE_SET) {
        if (update->index < 0 || (uint64_t)update->index >= array->length) return XRT_MANAGED_BOUNDS;
        *length = array->length;
    } else if (update->operation == XRT_ARRAY_UPDATE_PUSH) {
        size_t stride = xrt_backing_array_stride(update->item.kind);
        size_t maximum = (SIZE_MAX - sizeof(XrtArrayBacking)) / stride;
        if (array->length >= maximum || array->length >= INT64_MAX) return XRT_MANAGED_OVERFLOW;
        *length = array->length + 1;
    } else return XRT_MANAGED_ARGUMENT;
    return XRT_MANAGED_OK;
}
static XrtManagedHeader *array_owned_child(const XrtArrayBacking *array, size_t index) {
    XrtManagedHeader *child = NULL;
    memcpy(&child, array->elements + index * sizeof(child), sizeof(child));
    return child;
}
static void array_store_scalar(unsigned char *destination, const XrtArrayItem *item) {
    switch (item->kind) {
    case XRT_BACKING_ARRAY_I64: memcpy(destination, &item->value.i64, sizeof(item->value.i64)); break;
    case XRT_BACKING_ARRAY_U8: memcpy(destination, &item->value.u8, sizeof(item->value.u8)); break;
    case XRT_BACKING_ARRAY_U64: memcpy(destination, &item->value.u64, sizeof(item->value.u64)); break;
    default: XR_CHECK(false, "only an admitted scalar item reaches scalar storage"); break;
    }
}
static XrtManagedStatus array_capacity(size_t current, size_t required, size_t stride, size_t *output) {
    size_t maximum = (SIZE_MAX - sizeof(XrtArrayBacking)) / stride;
    if (maximum > INT64_MAX) maximum = INT64_MAX;
    if (required > maximum) return XRT_MANAGED_OVERFLOW;
    size_t capacity = current;
    if (capacity < required) {
        capacity = required ? 4 : 0;
        while (capacity < required) {
            if (capacity > maximum / 2) { capacity = required; break; }
            capacity *= 2;
        }
    }
    if (capacity > maximum) return XRT_MANAGED_OVERFLOW;
    *output = capacity;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus array_finish(XrtManagedFrame *frame, size_t slot, XrtManagedStatus status) {
    if (status == XRT_MANAGED_OK) status = xrt_managed_slot_commit(frame, slot);
    if (status != XRT_MANAGED_OK)
        XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK, "only this factory's prepared candidate is aborted");
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_try_update(XrtArrayBacking *array,
    const XrtArrayUpdate *update, bool *applied) {
    if (!applied) return XRT_MANAGED_ARGUMENT;
    *applied = false;
    size_t length = 0;
    XrtManagedStatus status = array_update_check(array, update, &length);
    if (status != XRT_MANAGED_OK) return status;
    int32_t references = xr_signed_rc_read(&array->managed.references);
    if ((references != 1 && references != -1) || length > array->capacity) return XRT_MANAGED_OK;
    XrtManagedHeader *child = update->item.kind == XRT_BACKING_ARRAY_STRING ? &update->item.value.string->managed : NULL;
    if (child && !xrt_managed_retain(child)) return XRT_MANAGED_REFCOUNT;
    if (!xr_signed_rc_unique(&array->managed.references)) { xrt_managed_drop(child); return XRT_MANAGED_OK; }
    size_t index = update->operation == XRT_ARRAY_UPDATE_PUSH ? array->length : (size_t)update->index;
    size_t stride = xrt_backing_array_stride(update->item.kind);
    XrtManagedHeader *old = child ? array_owned_child(array, index) : NULL;
    if (child) memcpy(array->elements + index * stride, &child, sizeof(child));
    else array_store_scalar(array->elements + index * stride, &update->item);
    array->length = length;
    *applied = true;
    xrt_managed_drop(old);
    return XRT_MANAGED_OK;
}
static XrtManagedStatus array_update_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtArrayBacking *source, const XrtArrayUpdate *update,
    const XrtManagedSlotReservation *reservation) {
    size_t length = 0, capacity = 0;
    XrtManagedStatus status = array_update_check(source, update, &length);
    if (status != XRT_MANAGED_OK) return status;
    size_t stride = xrt_backing_array_stride(update->item.kind);
    if (!types || !types->arrays[update->item.kind]) return XRT_MANAGED_ARGUMENT;
    status = array_capacity(source->capacity, length, stride, &capacity);
    if (status != XRT_MANAGED_OK) return status;
    XrtManagedHeader *payload = NULL;
    size_t bytes=sizeof(XrtArrayBacking)+capacity*stride;
    XrtManagedOps *ops = types->arrays[update->item.kind];
    bool scalar = update->item.kind != XRT_BACKING_ARRAY_STRING;
    if (!reservation) status = xrt_managed_slot_prepare(frame, slot, ops, bytes, &payload);
    else if (scalar) status = xrt_managed_slot_reserved_prepare_scalar_tail(reservation, ops, bytes, &payload);
    else status = xrt_managed_slot_reserved_prepare(reservation, ops, bytes, &payload);
    if (status != XRT_MANAGED_OK) return status;
    XrtArrayBacking *array = (XrtArrayBacking *)payload;
    size_t changed = update->operation == XRT_ARRAY_UPDATE_PUSH ? source->length : (size_t)update->index;
    if (!scalar) {
        for (size_t i = 0; i < length; ++i) {
            XrtManagedHeader *child = i == changed ? &update->item.value.string->managed : array_owned_child(source, i);
            if (!xrt_managed_retain(child)) { status = XRT_MANAGED_REFCOUNT; break; }
            memcpy(array->elements + i * stride, &child, sizeof(child));
        }
    } else {
        /* Both extents were bounded above. Only initialized scalar elements
         * cross to the still-private replacement. */
        memcpy(array->elements, source->elements, source->length * stride);
        array_store_scalar(array->elements + changed * stride, &update->item);
    }
    if (status == XRT_MANAGED_OK) array->length = length;
    if (!reservation) return array_finish(frame,slot,status);
    if (status==XRT_MANAGED_OK) status=xrt_managed_slot_reserved_commit(reservation);
    if (status!=XRT_MANAGED_OK)
        XR_CHECK(xrt_managed_slot_reserved_abort(reservation)==XRT_MANAGED_OK,
            "the registered factory aborts only its own prepared candidate");
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_update_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtArrayBacking *source, const XrtArrayUpdate *update) {
    return array_update_create(frame,slot,types,source,update,NULL);
}
typedef struct ArrayBindingUpdate {
    const XrtArrayUpdate *update;
    XrtArrayBacking *source;
    bool applied;
} ArrayBindingUpdate;
static XrtManagedStatus array_binding_try(XrtManagedHeader *value, void *context) {
    ArrayBindingUpdate *binding=context;
    binding->source=(XrtArrayBacking *)value;
    return xrt_array_backing_try_update(binding->source,binding->update,&binding->applied);
}
static XrtManagedStatus array_grow_local_i64(const XrtManagedSlotReservation *reservation,
    const XrtBackingTypes *types, ArrayBindingUpdate *edit) {
    size_t length = edit->source->length, capacity = 0;
    XrtManagedStatus status = array_capacity(edit->source->capacity, length + 1, sizeof(int64_t), &capacity);
    if (status != XRT_MANAGED_OK) return status;
    XrtManagedHeader *grown = NULL;
    status = xrt_managed_slot_reserved_grow_scalar_tail(reservation, types->arrays[XRT_BACKING_ARRAY_I64],
        sizeof(XrtArrayBacking) + capacity * sizeof(int64_t), &grown);
    if (status != XRT_MANAGED_OK || !grown) return status;
    XrtArrayBacking *array = (XrtArrayBacking *)grown;
    memcpy(array->elements + length * sizeof(int64_t), &edit->update->item.value.i64, sizeof(int64_t));
    array->length = length + 1;
    edit->source = NULL;
    edit->applied = true;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus array_update_place(XrtPlace binding, XrtBackingArrayKind kind,
    XrtManagedFrame *frame, size_t temporary, const XrtBackingTypes *types, const XrtArrayUpdate *update,
    bool allow_growth) {
    if (!xrt_backing_array_stride(kind) || !update || kind!=update->item.kind ||
        !types || !types->arrays[kind]) return XRT_MANAGED_ARGUMENT;
    XrtPlaceReplacement replacement={0};
    XrtManagedStatus status=xrt_place_replacement_begin(binding,frame,temporary,&replacement);
    if (status!=XRT_MANAGED_OK) return status;
    ArrayBindingUpdate edit={.update=update};
    status=xrt_managed_slot_reserved_apply(&replacement,array_binding_try,&edit);
    if (status == XRT_MANAGED_OK && !edit.applied && allow_growth &&
        edit.source->length == edit.source->capacity)
        status = array_grow_local_i64(&replacement, types, &edit);
    if (status==XRT_MANAGED_OK && !edit.applied) {
        status=array_update_create(frame,temporary,types,edit.source,update,&replacement);
        if (status==XRT_MANAGED_OK) status=xrt_place_replacement_publish(&replacement);
    }
    XR_CHECK(xrt_place_replacement_end(&replacement)==XRT_MANAGED_OK,
        "every registered update releases its actual domains after cleanup");
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_array_update_place(XrtPlace binding, XrtBackingArrayKind kind,
    XrtManagedFrame *frame, size_t temporary, const XrtBackingTypes *types, const XrtArrayUpdate *update) {
    return array_update_place(binding, kind, frame, temporary, types, update, false);
}
XR_FUNCDEF XrtManagedStatus xrt_array_push_local_i64(XrtPlace binding, size_t temporary,
    const XrtBackingTypes *types, int64_t item) {
    if (!binding.frame || binding.external) return XRT_MANAGED_ARGUMENT;
    const XrtArrayUpdate update = {XRT_ARRAY_UPDATE_PUSH, 0, {XRT_BACKING_ARRAY_I64, {.i64=item}}};
    return array_update_place(binding, XRT_BACKING_ARRAY_I64, binding.frame, temporary, types, &update, true);
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_get(const XrtArrayBacking *array,
    XrtBackingArrayKind kind, int64_t index, XrtArrayItem *output) {
    size_t stride = xrt_backing_array_stride(kind);
    if (!array || !stride || !output || array->length > array->capacity) return XRT_MANAGED_ARGUMENT;
    if (index < 0 || (uint64_t)index >= array->length) return XRT_MANAGED_BOUNDS;
    XrtArrayItem item = {.kind=kind};
    const unsigned char *element = array->elements + (size_t)index * stride;
    switch (kind) {
    case XRT_BACKING_ARRAY_I64: memcpy(&item.value.i64, element, sizeof(item.value.i64)); break;
    case XRT_BACKING_ARRAY_U8: memcpy(&item.value.u8, element, sizeof(item.value.u8)); break;
    case XRT_BACKING_ARRAY_U64: memcpy(&item.value.u64, element, sizeof(item.value.u64)); break;
    case XRT_BACKING_ARRAY_STRING:
        item.value.string = (XrtStringBacking *)array_owned_child(array, (size_t)index);
        if (!item.value.string) return XRT_MANAGED_ARGUMENT;
        break;
    default: XR_CHECK(false, "an admitted Array kind has one read representation"); break;
    }
    *output = item;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_repeat_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtArrayItem *fill, int64_t count) {
    if (!types || !array_item_valid(fill) || !types->arrays[fill->kind] || count < 0) return XRT_MANAGED_ARGUMENT;
    size_t stride = xrt_backing_array_stride(fill->kind), capacity = 0;
    XrtManagedStatus status = array_capacity(0, (size_t)count, stride, &capacity);
    if (status != XRT_MANAGED_OK) return status;
    XrtManagedHeader *payload = NULL;
    status = xrt_managed_slot_prepare(frame, slot, types->arrays[fill->kind],
        sizeof(XrtArrayBacking) + capacity * stride, &payload);
    if (status != XRT_MANAGED_OK) return status;
    XrtArrayBacking *array = (XrtArrayBacking *)payload;
    for (size_t i = 0; i < (size_t)count; ++i) {
        if (fill->kind == XRT_BACKING_ARRAY_STRING) {
            XrtManagedHeader *child = &fill->value.string->managed;
            if (!xrt_managed_retain(child)) { status = XRT_MANAGED_REFCOUNT; break; }
            memcpy(array->elements + i * stride, &child, sizeof(child));
        } else array_store_scalar(array->elements + i * stride, fill);
    }
    if (status == XRT_MANAGED_OK) array->length = (size_t)count;
    return array_finish(frame, slot, status);
}

typedef struct ArrayFormat {
    const XrtArrayBacking *array;
    XrtBackingArrayKind kind;
} ArrayFormat;
static int array_format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    const ArrayFormat *format = context;
    if (node.type == XRT_BACKING_ARRAY_COUNT) {
        view->kind = XR_VALUE_FORMAT_ARRAY;
        view->unsigned_value = format->array->length;
    } else if (node.type == XRT_BACKING_ARRAY_STRING) {
        const XrtStringBacking *string = NULL;
        memcpy(&string, node.value, sizeof(string));
        if (!string) return 0;
        view->kind = XR_VALUE_FORMAT_BYTES;
        view->bytes = (const unsigned char *)string->bytes;
        view->size = string->length;
        view->quote = '"';
    } else if (node.type == XRT_BACKING_ARRAY_I64) {
        view->kind = XR_VALUE_FORMAT_SIGNED;
        memcpy(&view->signed_value, node.value, sizeof(view->signed_value));
    } else {
        view->kind = XR_VALUE_FORMAT_UNSIGNED;
        if (node.type == XRT_BACKING_ARRAY_U8) view->unsigned_value = *(const uint8_t *)node.value;
        else memcpy(&view->unsigned_value, node.value, sizeof(view->unsigned_value));
    }
    return 1;
}
static int array_format_child(const void *context, XrValueFormatNode node, uint32_t index, XrValueFormatNode *child) {
    const ArrayFormat *format = context;
    if (node.type != XRT_BACKING_ARRAY_COUNT || index >= format->array->length) return 0;
    *child = (XrValueFormatNode){format->array->elements + (size_t)index * xrt_backing_array_stride(format->kind),
        (uint16_t)format->kind};
    return 1;
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_display_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtArrayBacking *array, XrtBackingArrayKind kind) {
    if (!types || !types->string || !array || !xrt_backing_array_stride(kind) || array->length > array->capacity)
        return XRT_MANAGED_ARGUMENT;
    if (array->length > INT64_MAX) return XRT_MANAGED_OVERFLOW;
    ArrayFormat format = {array, kind};
    XrValueFormatReader reader = {&format, array_format_read, array_format_child};
    return xrt_string_backing_format_create(frame, slot, types, reader,
        (XrValueFormatNode){array, XRT_BACKING_ARRAY_COUNT});
}
