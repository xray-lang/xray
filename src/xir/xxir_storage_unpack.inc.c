/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_unpack.inc.c - Iterative owned snapshots of admitted inline storage
 */
typedef struct StorageUnpackFrame {
    XirNominalValue *record;
    const XrXirTypeNode *node;
    const XrXirStorageLayout *layout;
    const unsigned char *bytes;
    uint32_t begin, next;
} StorageUnpackFrame;
typedef struct StorageUnpack {
    XrXirValueAdmission *admission;
    StorageUnpackFrame *frames;
    uint32_t depth, capacity;
} StorageUnpack;

static XrXirValueStatus storage_unpack_leaf(StorageSpan span,
    XrXirValueAdmission *admission, XrXirValue *output) {
    XrXirLayout layout = {0};
    if (!xr_xir_type_arena_layout(admission->arena, span.type, &layout) ||
        !layout.size || layout.size > sizeof(uint64_t) || !span.bytes) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue value = storage_leaf_value(span.type, span.bytes, layout.size);
    if (!xr_xir_value_argument(&value, admission->arena, span.type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    return xr_xir_value_copy(&value, output);
}
static XrXirValueStatus storage_unpack_enter(StorageUnpack *unpack,
    StorageSpan span, XrXirValue *ready) {
    const XrXirTypes *types = xr_xir_type_arena_types(unpack->admission->arena);
    if (!xr_xir_type_is_nominal(types, span.type)) return storage_unpack_leaf(span, unpack->admission, ready);
    const XrXirTypeNode *node = xr_xir_type_node(types, span.type);
    const XrXirStorageLayout *layout = xr_xir_type_arena_storage(unpack->admission->arena, span.type);
    const XrXirNominalIdentity *identity = &types->nominals->identities[node->nominal.declaration];
    uint32_t variant = 0, begin = 0, count = node->nominal.field_count;
    if (!layout || (layout->value.size && !span.bytes)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (layout->tag_bytes) memcpy(&variant, span.bytes, layout->tag_bytes);
    if (identity->kind == XR_XIR_NOMINAL_ENUM) {
        if (variant >= identity->variant_count) return XR_XIR_VALUE_BAD_ARGUMENT;
        begin = identity->variants[variant].field_begin; count = identity->variants[variant].field_count;
        if (!count) {
            const XirNominalValue *record = xr_xir_type_arena_empty_variant(unpack->admission->arena, span.type, variant);
            if (!record) return XR_XIR_VALUE_BAD_ARGUMENT;
            if (!xr_xir_type_arena_retain((XrXirTypeArena *)unpack->admission->arena)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
            *ready = (XrXirValue){(uint32_t)span.type, 0, 0};
            memcpy(&ready->payload, &record, sizeof(record)); return XR_XIR_VALUE_OK;
        }
    }
    uint64_t bytes = sizeof(XirNominalValue) + (uint64_t)count * sizeof(XrXirValue);
    if (bytes > SIZE_MAX || unpack->depth >= unpack->capacity) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirNominalValue *record = (XirNominalValue *)constructed_allocate(unpack->admission->domain,
        (XrXirTypeArena *)unpack->admission->arena, span.type, (size_t)bytes, &status);
    if (!record) return status;
    record->variant = variant; record->count = count; record->fields = (XrXirValue *)(record + 1);
    unpack->frames[unpack->depth++] = (StorageUnpackFrame){record, node, layout, span.bytes, begin, 0};
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus storage_unpack_walk(StorageUnpack *unpack,
    StorageSpan span, XrXirValue *output) {
    XrXirValue ready = {0}; bool first = true;
    while (first || unpack->depth) {
        if (!unpack->admission->work) return XR_XIR_VALUE_LIMIT;
        --unpack->admission->work;
        XrXirValueStatus status = XR_XIR_VALUE_OK;
        if (first) { first = false; status = storage_unpack_enter(unpack, span, &ready); }
        else {
            StorageUnpackFrame *frame = &unpack->frames[unpack->depth - 1];
            if (frame->next == frame->record->count) {
                ready = (XrXirValue){(uint32_t)frame->record->object.type, 0, 0};
                memcpy(&ready.payload, &frame->record, sizeof(frame->record)); --unpack->depth;
            } else {
                uint32_t field = frame->begin + frame->next;
                span = (StorageSpan){frame->node->nominal.fields[field],
                    frame->bytes ? frame->bytes + frame->layout->field_offsets[field] : NULL};
                status = storage_unpack_enter(unpack, span, &ready);
            }
        }
        if (status != XR_XIR_VALUE_OK) return status;
        if (ready.type) {
            if (!unpack->depth) { *output = ready; return XR_XIR_VALUE_OK; }
            StorageUnpackFrame *parent = &unpack->frames[unpack->depth - 1];
            parent->record->fields[parent->next++] = ready; ready = (XrXirValue){0};
        }
    }
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
/* Only completed children have owners. An incomplete parent is never passed
 * to the public value drop path, which requires a fully valid value. */
static void storage_unpack_discard(StorageUnpack *unpack) {
    while (unpack->depth) {
        StorageUnpackFrame *frame = &unpack->frames[--unpack->depth];
        while (frame->next) xr_xir_value_drop(&frame->record->fields[--frame->next]);
        size_t bytes = sizeof(XirNominalValue) + (size_t)frame->record->count * sizeof(XrXirValue);
        constructed_discard(&frame->record->object, bytes);
    }
}
/* Input belongs to a previously admitted owner and remains borrowed for this
 * synchronous operation. Materialization confers no new construction authority. */
static XrXirValueStatus storage_unpack(StorageSpan span,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !unit_value(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_type_is_nominal(xr_xir_type_arena_types(admission->arena), span.type))
        return storage_unpack_leaf(span, admission, output);
    if (!admission->domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirStorageLayout *layout = xr_xir_type_arena_storage(admission->arena, span.type);
    uint64_t scratch = (uint64_t)layout->depth * sizeof(StorageUnpackFrame);
    if (scratch > SIZE_MAX || scratch > admission->scratch_bytes) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    StorageUnpack unpack = {admission, NULL, 0, layout->depth};
    unpack.frames = xr_xir_domain_allocate(admission->domain, (size_t)scratch, &status);
    if (!unpack.frames) return status;
    admission->scratch_bytes -= scratch;
    status = storage_unpack_walk(&unpack, span, output);
    if (status != XR_XIR_VALUE_OK) storage_unpack_discard(&unpack);
    xr_xir_domain_deallocate(admission->domain, unpack.frames, (size_t)scratch);
    admission->scratch_bytes += scratch;
    return status;
}
