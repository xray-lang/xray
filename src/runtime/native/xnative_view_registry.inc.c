/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_view_registry.inc.c - Root-owned, typed and non-repeating identities
 */
static atomic_uint_fast64_t view_identity_sequence;

static uint64_t view_identity(uint32_t source) {
    uint_fast64_t value = atomic_load_explicit(&view_identity_sequence, memory_order_relaxed);
    for (;;) {
        if (value >= UINT64_MAX) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
        if (atomic_compare_exchange_weak_explicit(&view_identity_sequence, &value, value + 1,
            memory_order_relaxed, memory_order_relaxed)) return (uint64_t)value + 1;
    }
}
static void *view_allocate(XrtViewRegistry *registry, size_t bytes, uint32_t source) {
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    void *memory = xr_xir_domain_allocate(registry->root->domain, bytes, &status);
    if (!memory) xrt_value_failure(status, source);
    memset(memory, 0, bytes);
    return memory;
}
static size_t view_record_position(uint64_t identity, uint32_t capacity) {
    return xr_hash_bytes(&identity, sizeof(identity)) & (capacity - 1);
}
static XrtViewRecord *view_find(XrtViewRegistry *registry, uint64_t identity) {
    if (!registry || !identity || !registry->records) return NULL;
    for (size_t at = view_record_position(identity, registry->capacity);;
         at = (at + 1) & (registry->capacity - 1)) {
        XrtViewRecord *record = &registry->records[at];
        if (!record->identity) return NULL;
        if (record->identity == identity) return record;
    }
}
static void view_record_place(XrtViewRegistry *registry, XrtViewRecord value) {
    for (size_t at = view_record_position(value.identity, registry->capacity);;
         at = (at + 1) & (registry->capacity - 1)) {
        if (!registry->records[at].identity) { registry->records[at] = value; return; }
    }
}
static void view_reserve(XrtViewRegistry *registry, uint32_t extra, uint32_t source) {
    if (extra > UINT32_MAX - registry->count) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    uint32_t needed = registry->count + extra;
    if (needed < registry->capacity / 2) return;
    uint32_t capacity = registry->capacity ? registry->capacity : 16;
    while (needed >= capacity / 2) {
        if (capacity > UINT32_MAX / 2) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
        capacity *= 2;
    }
    if (sizeof(XrtViewRecord) > SIZE_MAX / capacity) xrt_value_failure(XR_XIR_VALUE_OVERFLOW, source);
    XrtViewRecord *records = view_allocate(registry, (size_t)capacity * sizeof(*records), source);
    XrtViewRecord *previous = registry->records;
    uint32_t old_capacity = registry->capacity;
    registry->records = records;
    registry->capacity = capacity;
    for (uint32_t n = 0; n < old_capacity; ++n)
        if (previous[n].identity) view_record_place(registry, previous[n]);
    if (previous) xr_xir_domain_deallocate(registry->root->domain, previous,
        (size_t)old_capacity * sizeof(*previous));
}
static void view_record_insert(XrtViewRegistry *registry, XrtViewRecord value) {
    XR_CHECK(value.identity && registry->count < registry->capacity / 2 &&
        !view_find(registry, value.identity), "identity publication consumes prepaid registry storage");
    view_record_place(registry, value);
    ++registry->count;
}
static void view_record_remove(XrtViewRegistry *registry, uint64_t identity) {
    XrtViewRecord *record = view_find(registry, identity);
    if (!record) return;
    size_t hole = (size_t)(record - registry->records), mask = registry->capacity - 1;
    for (size_t next = (hole + 1) & mask; registry->records[next].identity; next = (next + 1) & mask) {
        size_t home = view_record_position(registry->records[next].identity, registry->capacity);
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            registry->records[hole] = registry->records[next];
            hole = next;
        }
    }
    registry->records[hole] = (XrtViewRecord){0};
    --registry->count;
}
static XrtViewFrameBody *view_current(XrtViewFrame token, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_active();
    XrtViewRegistry *registry = root ? root->views : NULL;
    XrtViewRecord *record = view_find(registry, token.identity);
    if (!record || record->kind != XRT_VIEW_FRAME || record->frame != registry->current ||
        !record->frame->active) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return record->frame;
}
static bool view_frame_ancestor(const XrtViewFrameBody *frame, const XrtViewFrameBody *owner) {
    for (; frame; frame = frame->parent) if (frame == owner) return owner->active;
    return false;
}
static XrtViewRecord view_require(XrtViewFrameBody *frame, uint64_t identity,
    XrtViewRecordKind kind, uint32_t source) {
    XrtViewRecord *record = view_find(frame->registry, identity);
    if (!record || record->kind != kind || !view_frame_ancestor(frame, record->frame))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return *record;
}
