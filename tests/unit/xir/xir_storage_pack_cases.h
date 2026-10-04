/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_storage_pack_cases.h - Inline ownership preparation and exact rollback
 */
#ifndef XIR_STORAGE_PACK_CASES_H
#define XIR_STORAGE_PACK_CASES_H
#include "xir_storage_array_cases.h"
static XrXirValueStatus storage_pack(const XrXirValue *value, unsigned char *bytes,
    XrXirValueAdmission *admission) {
    StoragePack pack = {0};
    XrXirValueStatus status = storage_pack_begin(admission, (XrXirType)value->type, &pack);
    if (status != XR_XIR_VALUE_OK) return status;
    status = storage_pack_value(&pack, value, bytes); storage_pack_end(&pack);
    return status;
}
static void storage_pack_drop(XrXirTypeArena *arena, unsigned char *bytes) {
    StorageFrame frames[2]; StorageCursor cursor = {0}; XirObject *pending = NULL;
    CHECK(storage_cursor_init(arena, (StorageSpan){(XrXirType)257, bytes}, frames, 2, true, &cursor) == XR_XIR_VALUE_OK);
    storage_queue_release(&cursor, UINT64_MAX, &pending); release_pending(pending);
}
static void storage_unpack_cases(XrXirValueAdmission *admission, const unsigned char *bytes) {
    StorageSpan span = {(XrXirType)257, bytes}; XrXirValue value = {0};
    uint64_t baseline = admission->domain->stats.live_bytes; size_t blocks = live, begin = calls;
    admission->work = 100000;
    CHECK(storage_unpack(span, admission, &value) == XR_XIR_VALUE_OK && xr_xir_value_valid(&value));
    uint64_t work = 100000 - admission->work; size_t sites = calls - begin;
    CHECK(sites == 3 && work > 0 && admission->scratch_bytes == 65536);
    XirNominalValue *box = (XirNominalValue *)object_pointer(&value);
    XirNominalValue *pair = (XirNominalValue *)object_pointer(&box->fields[0]);
    XirNominalValue *empty = (XirNominalValue *)object_pointer(&box->fields[1]);
    const char *text = NULL; size_t length = 0;
    CHECK(box->count == 2 && pair->variant == 1 && pair->count == 2 && !empty->variant && !empty->count);
    CHECK(xr_xir_string_view(&pair->fields[0], &text, &length) && length == 4 && !memcmp(text, "left", 4));
    CHECK(xr_xir_string_view(&pair->fields[1], &text, &length) && length == 5 && !memcmp(text, "right", 5));
    xr_xir_value_drop(&value);
    for (size_t i = 0; i < sites; ++i) {
        admission->work = 100000; fail_at = calls + i;
        CHECK(storage_unpack(span, admission, &value) == XR_XIR_VALUE_OOM && unit_value(&value));
        fail_at = SIZE_MAX;
        CHECK(admission->domain->stats.live_bytes == baseline && live == blocks && admission->scratch_bytes == 65536);
    }
    for (uint64_t i = 0; i < work; ++i) {
        admission->work = i;
        CHECK(storage_unpack(span, admission, &value) == XR_XIR_VALUE_LIMIT && unit_value(&value));
        CHECK(admission->domain->stats.live_bytes == baseline && live == blocks && admission->scratch_bytes == 65536);
    }
    admission->work = 100000; admission->scratch_bytes = 2 * sizeof(StorageUnpackFrame) - 1; begin = calls;
    CHECK(storage_unpack(span, admission, &value) == XR_XIR_VALUE_LIMIT && calls == begin && unit_value(&value));
    admission->scratch_bytes = 65536;
    XrXirValue leaf = {XR_XIR_STRING, 0, 0}; memcpy(&leaf.payload, bytes + 16, 8);
    uint32_t refs = atomic_load(&object_pointer(&leaf)->references);
    atomic_store(&object_pointer(&leaf)->references, UINT32_MAX);
    CHECK(storage_unpack(span, admission, &value) == XR_XIR_VALUE_REFCOUNT_LIMIT && unit_value(&value));
    atomic_store(&object_pointer(&leaf)->references, refs);
    CHECK(admission->domain->stats.live_bytes == baseline && live == blocks && admission->scratch_bytes == 65536);
    unsigned char invalid[48]; memcpy(invalid, bytes, sizeof(invalid)); invalid[24] = 255;
    CHECK(storage_unpack((StorageSpan){(XrXirType)257, invalid}, admission, &value) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(unit_value(&value) && admission->domain->stats.live_bytes == baseline && live == blocks);
    admission->work = work;
    CHECK(storage_unpack(span, admission, &value) == XR_XIR_VALUE_OK && !admission->work);
    xr_xir_value_drop(&value);
    admission->work = 100000;
    CHECK(storage_unpack((StorageSpan){(XrXirType)258, NULL}, admission, &value) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_valid(&value) && !((XirNominalValue *)object_pointer(&value))->count);
    xr_xir_value_drop(&value);
    CHECK(admission->domain->stats.live_bytes == baseline && live == blocks);
}
static void storage_copy_cases(StoragePack *pack, unsigned char *bytes) {
    unsigned char copied[48]; XrXirValueAdmission *admission = pack->admission;
    uint64_t baseline = admission->domain->stats.live_bytes; size_t allocations = calls;
    XrXirValue left = {XR_XIR_STRING, 0, 0}, right = left;
    memcpy(&left.payload, bytes + 8, 8); memcpy(&right.payload, bytes + 16, 8);
    uint32_t left_refs = atomic_load(&object_pointer(&left)->references);
    uint32_t right_refs = atomic_load(&object_pointer(&right)->references);
    admission->work = 100000;
    CHECK(storage_pack_copy(pack, bytes, copied) == XR_XIR_VALUE_OK && !memcmp(bytes, copied, sizeof(copied)));
    uint64_t work = 100000 - admission->work;
    CHECK(work > 0 && atomic_load(&object_pointer(&left)->references) == left_refs + 1);
    storage_pack_release(pack, copied, UINT64_MAX);
    for (uint64_t i = 0; i < work; ++i) {
        admission->work = i;
        CHECK(storage_pack_copy(pack, bytes, copied) == XR_XIR_VALUE_LIMIT);
        CHECK(atomic_load(&object_pointer(&left)->references) == left_refs);
        CHECK(atomic_load(&object_pointer(&right)->references) == right_refs);
    }
    admission->work = 100000; atomic_store(&object_pointer(&right)->references, UINT32_MAX);
    CHECK(storage_pack_copy(pack, bytes, copied) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(atomic_load(&object_pointer(&left)->references) == left_refs);
    CHECK(atomic_load(&object_pointer(&right)->references) == UINT32_MAX);
    atomic_store(&object_pointer(&right)->references, right_refs);
    admission->work = work;
    CHECK(storage_pack_copy(pack, bytes, copied) == XR_XIR_VALUE_OK && !admission->work);
    storage_pack_release(pack, copied, UINT64_MAX);
    CHECK(calls == allocations && admission->domain->stats.live_bytes == baseline);
    CHECK(atomic_load(&object_pointer(&left)->references) == left_refs);
    CHECK(atomic_load(&object_pointer(&right)->references) == right_refs);
}
static void storage_pack_cases(void) {
    XrXirDomain *domain = NULL;
    CHECK(!live && xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = storage_cursor_arena(domain);
    uint64_t empty_bytes = domain->stats.live_bytes;
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 100000, 65536};
    XrXirValue strings[2] = {{0}}, choices[2] = {{0}}, box = {0};
    CHECK(xr_xir_string_new(domain, "left", 4, &strings[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "right", 5, &strings[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new((XrXirType)256, 1, strings, 2, &admission, &choices[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new((XrXirType)256, 0, NULL, 0, &admission, &choices[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new((XrXirType)257, choices, 2, &admission, &box) == XR_XIR_VALUE_OK);
    unsigned char bytes[48]; memset(bytes, 0xFF, sizeof(bytes));
    uint64_t baseline = domain->stats.live_bytes; size_t blocks = live;
    admission.work = 100000;
    CHECK(storage_pack(&box, bytes, &admission) == XR_XIR_VALUE_OK);
    uint64_t used_work = 100000 - admission.work;
    CHECK(used_work > 0 && used_work < 100 && admission.scratch_bytes == 65536);
    CHECK(bytes[0] == 1 && bytes[24] == 0);
    for (unsigned i = 1; i < 8; ++i) CHECK(bytes[i] == 0);
    for (unsigned i = 25; i < 48; ++i) CHECK(bytes[i] == 0);
    XrXirValue first = {XR_XIR_STRING, 0, 0}, second = {XR_XIR_STRING, 0, 0};
    memcpy(&first.payload, bytes + 8, 8); memcpy(&second.payload, bytes + 16, 8);
    const char *text = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&first, &text, &length) && length == 4 && !memcmp(text, "left", 4));
    CHECK(xr_xir_string_view(&second, &text, &length) && length == 5 && !memcmp(text, "right", 5));
    storage_pack_drop(arena, bytes);
    uint32_t first_refs = atomic_load(&object_pointer(&strings[0])->references);
    uint32_t second_refs = atomic_load(&object_pointer(&strings[1])->references);
    for (uint64_t work = 0; work < used_work; ++work) {
        admission.work = work; memset(bytes, 0xFF, sizeof(bytes));
        CHECK(storage_pack(&box, bytes, &admission) == XR_XIR_VALUE_LIMIT);
        CHECK(domain->stats.live_bytes == baseline && live == blocks && admission.scratch_bytes == 65536);
        CHECK(atomic_load(&object_pointer(&strings[0])->references) == first_refs);
        CHECK(atomic_load(&object_pointer(&strings[1])->references) == second_refs);
    }
    admission.work = 100000; fail_at = calls;
    CHECK(storage_pack(&box, bytes, &admission) == XR_XIR_VALUE_OOM);
    CHECK(domain->stats.live_bytes == baseline && live == blocks && admission.scratch_bytes == 65536);
    fail_at = SIZE_MAX;
    admission.scratch_bytes = 2 * sizeof(StoragePackFrame) - 1; size_t allocations = calls;
    CHECK(storage_pack(&box, bytes, &admission) == XR_XIR_VALUE_LIMIT && calls == allocations);
    admission.scratch_bytes = 65536;
    atomic_store(&object_pointer(&strings[1])->references, UINT32_MAX);
    CHECK(storage_pack(&box, bytes, &admission) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(atomic_load(&object_pointer(&strings[0])->references) == first_refs);
    CHECK(atomic_load(&object_pointer(&strings[1])->references) == UINT32_MAX);
    CHECK(domain->stats.live_bytes == baseline && live == blocks && admission.scratch_bytes == 65536);
    atomic_store(&object_pointer(&strings[1])->references, second_refs);
    StoragePack bulk = {0}; admission.work = 100000;
    CHECK(storage_pack_begin(&admission, (XrXirType)257, &bulk) == XR_XIR_VALUE_OK);
    allocations = calls;
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(storage_pack_value(&bulk, &box, bytes) == XR_XIR_VALUE_OK && calls == allocations);
        storage_pack_drop(arena, bytes);
    }
    admission.work = 2;
    CHECK(storage_pack_value(&bulk, &box, bytes) == XR_XIR_VALUE_LIMIT && calls == allocations);
    admission.work = 100000;
    CHECK(storage_pack_value(&bulk, &box, bytes) == XR_XIR_VALUE_OK && calls == allocations);
    storage_copy_cases(&bulk, bytes);
    storage_pack_drop(arena, bytes); storage_pack_end(&bulk);
    CHECK(domain->stats.live_bytes == baseline && live == blocks && admission.scratch_bytes == 65536);
    storage_array_cases(&admission, &box);
    admission.work = used_work;
    CHECK(storage_pack(&box, bytes, &admission) == XR_XIR_VALUE_OK && !admission.work);
    xr_xir_value_drop(&box); xr_xir_value_drop(&choices[0]); xr_xir_value_drop(&choices[1]);
    xr_xir_value_drop(&strings[0]); xr_xir_value_drop(&strings[1]);
    CHECK(xr_xir_string_view(&first, &text, &length) && length == 4);
    storage_unpack_cases(&admission, bytes);
    allocations = calls; fail_at = calls;
    storage_pack_drop(arena, bytes);
    CHECK(domain->stats.live_bytes == empty_bytes && calls == allocations);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    CHECK(!live && calls == allocations); fail_at = SIZE_MAX;
}
#endif // XIR_STORAGE_PACK_CASES_H
