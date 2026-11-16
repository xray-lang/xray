/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_array_slice.c - Real hidden keepers without an owning Slice value
 */
#include "runtime/native/xnative_array_slice.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)

typedef struct SliceStorage { XrtArraySliceBundle value, temporary, child, child_temporary; } SliceStorage;
static XrtInlineValueHome home(XrtManagedFrame *frame, SliceStorage *storage, bool child) {
    return (XrtInlineValueHome){frame, child ? &storage->child : &storage->value,
        child ? &storage->child_temporary : &storage->temporary, child ? 2u : 0u, child ? 3u : 1u};
}
static void kernel(XrtBackingArrayKind kind) {
    XrtBillingOwner *billing = NULL; XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    XrtBackingTypes types = {0};
    OK(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    OK(xrt_backing_types_init(billing, &types));
    OK(xrt_managed_root_new(billing, &root));
    const size_t offsets[] = {offsetof(SliceStorage, value.hold), offsetof(SliceStorage, temporary.hold),
        offsetof(SliceStorage, child.hold), offsetof(SliceStorage, child_temporary.hold), SIZE_MAX, SIZE_MAX};
    XrtManagedFrameSpec layout = {6, sizeof(SliceStorage), _Alignof(SliceStorage), offsets};
    OK(xrt_managed_frame_enter_layout(root, &layout, &frame));
    SliceStorage *storage = NULL; void *payload = NULL; size_t bytes = 0;
    OK(xrt_managed_frame_payload(frame, &payload, &bytes)); storage = payload;
    CHECK(bytes == sizeof(*storage));
    int64_t integers[] = {11, 22, 33}; uint8_t octets[] = {11, 22, 33};
    if (kind == XRT_BACKING_ARRAY_I64) OK(xrt_array_backing_i64_create(frame, 4, &types, integers, 3, 3));
    else OK(xrt_array_backing_u8_create(frame, 4, &types, octets, 3, 3));
    XrtManagedHeader *header = NULL; OK(xrt_managed_slot_borrow(frame, 4, &header)); CHECK(header);
    XrtInlineValueHome primary = home(frame, storage, false), child = home(frame, storage, true);
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    xrt_billing_owner_fail_after(billing, 0);
    OK(xrt_array_slice_bind(&primary, kind, (const XrtArrayBacking *)header, 1, 2));
    CHECK(storage->value.hold == header && storage->value.value.backing == (const XrtArrayBacking *)header);
    CHECK(!storage->temporary.hold);
    int64_t length = -1; uint64_t bits = 0;
    OK(xrt_array_slice_length(&storage->value.value, &length)); CHECK(length == 2);
    OK(xrt_array_slice_read_bits(&storage->value.value, kind, 0, &bits)); CHECK(bits == 22);
    bits = 99;
    CHECK(xrt_array_slice_read_bits(&storage->value.value, kind, -1, &bits) == XRT_MANAGED_BOUNDS && bits == 99);
    CHECK(xrt_array_slice_read_bits(&storage->value.value, kind, 2, &bits) == XRT_MANAGED_BOUNDS && bits == 99);
    XrtArraySliceBundle saved = storage->value;
    CHECK(xrt_array_slice_bind(&primary, kind, (const XrtArrayBacking *)header, 4, 0) == XRT_MANAGED_BOUNDS);
    CHECK(!memcmp(&saved, &storage->value, sizeof(saved)));
    int32_t references = atomic_load(&header->references);
    atomic_store(&header->references, -INT32_MAX);
    CHECK(xrt_array_slice_bind(&primary, kind, (const XrtArrayBacking *)header, 0, 1) == XRT_MANAGED_REFCOUNT);
    CHECK(!memcmp(&saved, &storage->value, sizeof(saved)) && !storage->temporary.hold);
    atomic_store(&header->references, references);
    OK(xrt_array_slice_reborrow(&child, kind, &storage->value.value, 1, 1));
    CHECK(xrt_array_slice_reborrow(&child, kind, &storage->value.value, 2, 1) == XRT_MANAGED_BOUNDS);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(before.allocations == after.allocations && before.bytes == after.bytes && before.blocks == after.blocks);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    OK(xrt_managed_slot_drop(frame, 4));
    OK(xrt_array_slice_clear(&primary)); CHECK(!storage->value.hold && !storage->value.value.backing);
    OK(xrt_array_slice_read_bits(&storage->child.value, kind, 0, &bits)); CHECK(bits == 33);
    OK(xrt_array_slice_copy_create(frame, 5, &types, kind, &storage->child.value));
    XrtManagedHeader *copy = NULL; OK(xrt_managed_slot_borrow(frame, 5, &copy)); CHECK(copy && copy != header);
    uint64_t copied = 0; OK(xrt_array_backing_read_bits((const XrtArrayBacking *)copy, kind, 0, &copied)); CHECK(copied == 33);
    xrt_backing_types_drop(&types);
    xrt_billing_owner_close(billing);
    OK(xrt_array_slice_read_bits(&storage->child.value, kind, 0, &bits)); CHECK(bits == 33);
    OK(xrt_array_slice_clear(&child));
    CHECK(xrt_array_slice_length(&storage->child.value, &length) == XRT_MANAGED_ARGUMENT);
    OK(xrt_managed_root_destroy(&root));
    XrtBillingStats final = xrt_billing_owner_stats(billing); CHECK(!final.bytes && !final.blocks);
    xrt_billing_owner_drop(billing);
}
int main(void) {
    XrtInlineValueSpec spec = xrt_array_slice_bundle_spec();
    CHECK(spec.size == sizeof(XrtArraySliceBundle) && spec.owned_count == 1 &&
        spec.owned_offsets[0] == offsetof(XrtArraySliceBundle, hold));
    kernel(XRT_BACKING_ARRAY_I64); kernel(XRT_BACKING_ARRAY_U8);
    puts("readonly natural Slice bytes and real hidden keeper transactions passed");
    return 0;
}
