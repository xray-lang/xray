/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_pack.inc.c - Budgeted boxed-to-inline ownership preparation
 *
 * KEY CONCEPT:
 *   Only acquired handle leaves belong to a failed construction's rollback.
 */
typedef struct StoragePackFrame {
    const XirNominalValue *record;
    const XrXirTypeNode *node;
    const XrXirStorageLayout *layout;
    unsigned char *bytes;
    uint32_t begin, next, end;
} StoragePackFrame;
typedef struct StoragePack {
    XrXirValueAdmission *admission;
    StoragePackFrame *frames;
    uint32_t depth, capacity;
    uint64_t owners;
    XrXirType type;
} StoragePack;

static XrXirValueStatus storage_pack_leaf(StoragePack *pack,
    const XrXirValue *value, unsigned char *bytes) {
    XrXirLayout layout = {0};
    if (!xr_xir_type_arena_layout(pack->admission->arena, (XrXirType)value->type, &layout) ||
        layout.size > sizeof(value->payload) || (layout.size && !bytes)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue owned = {0};
    XrXirValueStatus status = xr_xir_value_copy(value, &owned);
    if (status != XR_XIR_VALUE_OK) return status;
    if (layout.size) memcpy(bytes, &owned.payload, layout.size);
    if (owned_carrier_type((XrXirType)value->type)) ++pack->owners;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus storage_pack_enter(StoragePack *pack,
    const XrXirValue *value, unsigned char *bytes) {
    XrXirType type = (XrXirType) value->type;
    if (!xr_xir_value_argument(value, pack->admission->arena, type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types = xr_xir_type_arena_types(pack->admission->arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    const XrXirStorageLayout *layout = xr_xir_type_arena_storage(pack->admission->arena, type);
    const XrXirNominalIdentity *identity = &types->nominals->identities[node->nominal.declaration];
    const XirNominalValue *record = (const XirNominalValue *) object_pointer(value);
    uint32_t begin = identity->kind == XR_XIR_NOMINAL_ENUM ? identity->variants[record->variant].field_begin : 0;
    if (layout->tag_bytes) memcpy(bytes, &record->variant, layout->tag_bytes);
    if (record->count) {
        if (pack->depth >= pack->capacity) return XR_XIR_VALUE_LIMIT;
        pack->frames[pack->depth++] = (StoragePackFrame) {record, node, layout, bytes,
            begin, begin, begin + record->count};
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus storage_pack_walk(StoragePack *pack,
    const XrXirValue *value, unsigned char *bytes) {
    const XrXirTypes *types = xr_xir_type_arena_types(pack->admission->arena);
    bool pending = true;
    while (pending || pack->depth) {
        if (!pack->admission->work) return XR_XIR_VALUE_LIMIT;
        --pack->admission->work;
        if (pending) pending = false;
        else {
            StoragePackFrame *frame = &pack->frames[pack->depth - 1];
            if (frame->next == frame->end) { --pack->depth; continue; }
            uint32_t field = frame->next++;
            value = &frame->record->fields[field - frame->begin];
            bytes = frame->bytes ? frame->bytes + frame->layout->field_offsets[field] : NULL;
            if (value->type != (uint32_t)frame->node->nominal.fields[field]) return XR_XIR_VALUE_BAD_ARGUMENT;
        }
        XrXirValueStatus status = inline_nominal_type(types, (XrXirType)value->type) ?
            storage_pack_enter(pack, value, bytes) : storage_pack_leaf(pack, value, bytes);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus storage_pack_begin(XrXirValueAdmission *admission,
    XrXirType type, StoragePack *pack) {
    if (!admission || !admission->domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    *pack = (StoragePack){admission, NULL, 0, 0, 0, type};
    const XrXirTypes *types = xr_xir_type_arena_types(admission->arena);
    if (!inline_nominal_type(types, type)) return XR_XIR_VALUE_OK;
    const XrXirStorageLayout *layout = xr_xir_type_arena_storage(admission->arena, type);
    if (!layout) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t scratch = (uint64_t)layout->depth * sizeof(StoragePackFrame);
    if (scratch > SIZE_MAX || scratch > admission->scratch_bytes) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    pack->frames = xr_xir_domain_allocate(admission->domain, (size_t)scratch, &status);
    if (!pack->frames) return status;
    admission->scratch_bytes -= scratch; pack->capacity = layout->depth;
    return XR_XIR_VALUE_OK;
}
static void storage_pack_end(StoragePack *pack) {
    if (pack->frames) {
        size_t scratch = (size_t)pack->capacity * sizeof(StoragePackFrame);
        xr_xir_domain_deallocate(pack->admission->domain, pack->frames, scratch);
        pack->admission->scratch_bytes += scratch;
    }
    *pack = (StoragePack){0};
}
/* The destination is unpublished storage of the exact sealed size. Admission
 * of callbacks and domains belongs to the caller before ownership preparation.
 * One scratch reservation serves every element of a bulk operation. */
static XrXirValueStatus storage_pack_value(StoragePack *pack,
    const XrXirValue *value, unsigned char *bytes) {
    if (!value || value->type != (uint32_t)pack->type) return XR_XIR_VALUE_BAD_ARGUMENT;
    pack->depth = 0; pack->owners = 0;
    if (!pack->frames) return storage_pack_leaf(pack, value, bytes);
    XrXirValueAdmission *admission = pack->admission;
    const XrXirStorageLayout *layout = xr_xir_type_arena_storage(admission->arena, pack->type);
    if (layout->value.size && !bytes) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (layout->value.size) memset(bytes, 0, layout->value.size);
    XrXirValueStatus status = storage_pack_walk(pack, value, bytes);
    if (status != XR_XIR_VALUE_OK && pack->owners) {
        _Static_assert(sizeof(StoragePackFrame) >= sizeof(StorageFrame), "packing scratch also holds release frames");
        StorageCursor cursor = {0};
        StorageSpan root = {(XrXirType)value->type, bytes}; XirObject *pending = NULL;
        XR_CHECK(storage_cursor_init(admission->arena, root, (StorageFrame *)pack->frames,
            pack->capacity, true, &cursor) == XR_XIR_VALUE_OK, "packing rollback requires reserved traversal frames");
        storage_queue_release(&cursor, pack->owners, &pending); release_pending(pending);
    }
    return status;
}
static void storage_pack_release(StoragePack *pack, unsigned char *bytes, uint64_t owners) {
    StorageCursor cursor = {0}; XirObject *pending = NULL;
    XR_CHECK(storage_cursor_init(pack->admission->arena, (StorageSpan){pack->type, bytes},
        (StorageFrame *)pack->frames, pack->capacity, true, &cursor) == XR_XIR_VALUE_OK,
        "prepared storage retains its reserved release workspace");
    storage_queue_release(&cursor, owners, &pending); release_pending(pending);
}
/* Both spans use the exact admitted type. The private destination acquires only
 * leaf owners; copying an Array backing never materializes per-element boxes. */
static XrXirValueStatus storage_pack_copy(StoragePack *pack,
    const unsigned char *source, unsigned char *destination) {
    XrXirLayout layout = {0};
    if (!xr_xir_type_arena_layout(pack->admission->arena, pack->type, &layout) ||
        (layout.size && (!source || !destination))) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (layout.size) memcpy(destination, source, layout.size);
    StorageCursor cursor = {0};
    XrXirValueStatus status = storage_cursor_init(pack->admission->arena, (StorageSpan){pack->type, source},
        (StorageFrame *)pack->frames, pack->capacity, true, &cursor);
    uint64_t owners = 0;
    while (status == XR_XIR_VALUE_OK) {
        StorageSpan leaf = {0}; bool found = false;
        status = storage_cursor_next(&cursor, pack->frames ? &pack->admission->work : NULL, &leaf, &found);
        if (status != XR_XIR_VALUE_OK || !found) break;
        XrXirValue value = storage_leaf_value(leaf.type, leaf.bytes, sizeof(uint64_t)), owned = {0};
        status = xr_xir_value_copy(&value, &owned);
        if (status == XR_XIR_VALUE_OK) ++owners;
    }
    if (status != XR_XIR_VALUE_OK && owners) storage_pack_release(pack, destination, owners);
    return status;
}
