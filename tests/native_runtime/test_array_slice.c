/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_array_slice.c - Real hidden keepers without an owning Slice value
 */
#include "runtime/native/xnative_array_slice.h"
#include "xray_native_state.h"
#include "base/xmalloc.h"
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
    XrtBillingStats initial = xrt_billing_owner_stats(billing);
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
    CHECK(xrt_array_slice_range_bind(&primary, kind, (const XrtArrayBacking *)header, -1, 2) == XRT_MANAGED_BOUNDS);
    CHECK(xrt_array_slice_range_reborrow(&child, kind, &storage->value.value, 1, 0, false) == XRT_MANAGED_BOUNDS);
    int32_t references = atomic_load(&header->references);
    atomic_store(&header->references, -INT32_MAX);
    CHECK(xrt_array_slice_bind(&primary, kind, (const XrtArrayBacking *)header, 0, 1) == XRT_MANAGED_REFCOUNT);
    CHECK(!memcmp(&saved, &storage->value, sizeof(saved)) && !storage->temporary.hold);
    atomic_store(&header->references, references);
    OK(xrt_array_slice_reborrow(&primary, kind, &storage->value.value, 0, 2));
    CHECK(!memcmp(&saved, &storage->value, sizeof(saved)) && !storage->temporary.hold);
    CHECK(atomic_load(&header->references) == references);
    OK(xrt_array_slice_range_reborrow(&child, kind, &storage->value.value, 1, 0, true));
    CHECK(xrt_array_slice_reborrow(&child, kind, &storage->value.value, 2, 1) == XRT_MANAGED_BOUNDS);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(before.allocations == after.allocations && before.bytes == after.bytes && before.blocks == after.blocks);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    OK(xrt_managed_slot_drop(frame, 4));
    CHECK(xrt_array_slice_clear(NULL,primary.value,primary.temporary,primary.first_owner,
        primary.temporary_first_owner)==XRT_MANAGED_ARGUMENT);
    CHECK(xrt_array_slice_clear(primary.frame,primary.value,primary.temporary,primary.temporary_first_owner,
        primary.first_owner)==XRT_MANAGED_ARGUMENT);
    CHECK(!memcmp(&saved,&storage->value,sizeof(saved)) && !storage->temporary.hold);
    XrtBillingStats clear_before=xrt_billing_owner_stats(billing);
    xrt_billing_owner_fail_after(billing,0);
    OK(xrt_array_slice_clear(primary.frame,primary.value,primary.temporary,primary.first_owner,
        primary.temporary_first_owner)); CHECK(!storage->value.hold && !storage->value.value.backing);
    OK(xrt_array_slice_clear(primary.frame,primary.value,primary.temporary,primary.first_owner,
        primary.temporary_first_owner));
    XrtBillingStats clear_after=xrt_billing_owner_stats(billing);
    CHECK(clear_after.allocations==clear_before.allocations && clear_after.bytes==clear_before.bytes &&
        clear_after.blocks==clear_before.blocks);
    xrt_billing_owner_fail_after(billing,SIZE_MAX);
    OK(xrt_array_slice_read_bits(&storage->child.value, kind, 0, &bits)); CHECK(bits == 33);
    OK(xrt_array_slice_copy_create(frame, 5, &types, kind, &storage->child.value));
    XrtManagedHeader *copy = NULL; OK(xrt_managed_slot_borrow(frame, 5, &copy)); CHECK(copy && copy != header);
    uint64_t copied = 0; OK(xrt_array_backing_read_bits((const XrtArrayBacking *)copy, kind, 0, &copied)); CHECK(copied == 33);
    xrt_backing_types_drop(&types);
    xrt_billing_owner_close(billing);
    OK(xrt_array_slice_read_bits(&storage->child.value, kind, 0, &bits)); CHECK(bits == 33);
    OK(xrt_array_slice_reborrow(&primary, kind, &storage->child.value, 0, 1));
    OK(xrt_array_slice_clear(child.frame,child.value,child.temporary,child.first_owner,
        child.temporary_first_owner));
    CHECK(xrt_array_slice_length(&storage->child.value, &length) == XRT_MANAGED_ARGUMENT);
    CHECK(storage->value.hold == header);
    OK(xrt_managed_root_destroy(&root));
    XrtBillingStats final = xrt_billing_owner_stats(billing);
    CHECK(final.bytes == initial.bytes && final.blocks == initial.blocks);
    xrt_billing_owner_drop(billing);
}
typedef struct SliceFrameProbe {
    XrtModuleState *state;
    size_t leases, calls, bytes, blocks, cleared;
    bool armed;
} SliceFrameProbe;
typedef struct SliceReceiving { XrtArraySliceBundle value, temporary; } SliceReceiving;
static bool slice_provider_retain(void *context) { ++((SliceFrameProbe *)context)->leases; return true; }
static void slice_provider_drop(void *context) {
    SliceFrameProbe *probe = context; CHECK(probe->leases); --probe->leases;
}
static void *slice_provider_allocate(void *context, size_t bytes) {
    SliceFrameProbe *probe = context; ++probe->calls;
    if (probe->armed) {
        CHECK(probe->state); probe->armed = false;
        xrt_module_state_clear(probe->state); ++probe->cleared;
    }
    void *memory = xr_malloc(bytes);
    if (memory) { probe->bytes += bytes; ++probe->blocks; }
    return memory;
}
static void slice_provider_free(void *context, void *memory, size_t bytes) {
    SliceFrameProbe *probe = context;
    CHECK(probe->blocks && probe->bytes >= bytes); --probe->blocks; probe->bytes -= bytes;
    xr_free(memory);
}
static XrtModuleLayout *slice_state_layout(XrtBillingOwner *billing, XrtBackingArrayKind kind) {
    size_t owned = 0;
    XrtValueImageType type = {.kind=kind == XRT_BACKING_ARRAY_I64 ? XRT_VALUE_ARRAY_I64 : XRT_VALUE_ARRAY_U8,
        .size=sizeof(XrtManagedHeader *),.alignment=_Alignof(XrtManagedHeader *),.owned_count=1};
    XrtValueImage image = {.types=&type,.type_count=1,.owned_offsets=&owned,.owned_count=1};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.type=1,.flags=XRT_MODULE_SLOT_MUTABLE,.size=type.size,.alignment=type.alignment};
    XrtModuleLayoutSpec spec = {.values=&image,.modules=&module,.module_count=1,.slots=&slot,.slot_count=1,
        .payload_size=type.size,.payload_alignment=type.alignment};
    XrtModuleLayout *layout = NULL; OK(xrt_module_layout_new(billing, &spec, (XrtManagedLease){0}, &layout));
    return layout;
}
static XrtModuleRef *slice_ready_state(XrtModuleState *state, XrtManagedRoot *root,
    const XrtBackingTypes *types, XrtBackingArrayKind kind) {
    XrtManagedFrame *seed = NULL; OK(xrt_managed_frame_enter(root, 1, &seed));
    int64_t integers[] = {41,42,43}; uint8_t octets[] = {41,42,43};
    if (kind == XRT_BACKING_ARRAY_I64) OK(xrt_array_backing_i64_create(seed, 0, types, integers, 3, 3));
    else OK(xrt_array_backing_u8_create(seed, 0, types, octets, 3, 3));
    XrtManagedHeader *owner = NULL; OK(xrt_managed_slot_borrow(seed, 0, &owner)); CHECK(owner);
    XrtModuleRef *reference = NULL; OK(xrt_module_begin(state, 0));
    OK(xrt_module_construct(state, 0, &owner, &reference)); OK(xrt_module_publish(reference));
    OK(xrt_managed_slot_drop(seed, 0)); OK(xrt_module_ready(state, 0));
    return reference;
}
static void slice_provider_state_probe(XrtBackingArrayKind kind, bool published_hold) {
    SliceFrameProbe probe = {.leases=1}; XrtBillingOwner *frames = NULL, *data = NULL;
    XrtManagedAllocator allocator = {&probe,slice_provider_allocate,slice_provider_free,
        {&probe,slice_provider_retain,slice_provider_drop}};
    OK(xrt_billing_owner_new(&allocator, SIZE_MAX, &frames)); OK(xrt_billing_owner_new(NULL, SIZE_MAX, &data));
    XrtBillingStats initial_frames = xrt_billing_owner_stats(frames), initial_data = xrt_billing_owner_stats(data);
    XrtManagedRoot *root = NULL, *seed_root = NULL;
    OK(xrt_managed_root_new(frames, &root)); OK(xrt_managed_root_new(data, &seed_root));
    XrtBackingTypes types = {0}; OK(xrt_backing_types_init(data, &types));
    XrtModuleLayout *layout = slice_state_layout(data, kind); OK(xrt_module_state_new(layout, data, &probe.state));
    XrtModuleRef *reference = slice_ready_state(probe.state, seed_root, &types, kind);
    xrt_module_layout_drop(&layout);
    const size_t offsets[] = {offsetof(SliceReceiving,value.hold),offsetof(SliceReceiving,temporary.hold)};
    XrtManagedFrameSpec spec = {2,sizeof(SliceReceiving),_Alignof(SliceReceiving),offsets};
    XrtManagedFrame *frame = NULL, *later = NULL;
    probe.armed = !published_hold; size_t calls = probe.calls;
    OK(xrt_managed_frame_enter_layout(root, &spec, &frame)); CHECK(probe.calls == calls + 1);
    void *payload = NULL; size_t bytes = 0; OK(xrt_managed_frame_payload(frame, &payload, &bytes));
    CHECK(bytes == sizeof(SliceReceiving)); SliceReceiving *storage = payload;
    XrtInlineValueHome receiving = {frame,&storage->value,&storage->temporary,0,1};
    if (published_hold) {
        CHECK(!probe.cleared); const void *value = NULL;
        XrtBillingStats before_frames = xrt_billing_owner_stats(frames), before_data = xrt_billing_owner_stats(data);
        xrt_billing_owner_fail_after(frames, 0); xrt_billing_owner_fail_after(data, 0);
        OK(xrt_module_borrow(reference, &value));
        XrtManagedHeader *backing = *(XrtManagedHeader *const *)value;
        OK(xrt_array_slice_bind(&receiving, kind, (const XrtArrayBacking *)backing, 1, 2));
        CHECK(storage->value.hold == backing && !storage->temporary.hold);
        CHECK(xrt_billing_owner_stats(frames).allocations == before_frames.allocations &&
            xrt_billing_owner_stats(data).allocations == before_data.allocations);
        xrt_billing_owner_fail_after(frames, SIZE_MAX); xrt_billing_owner_fail_after(data, SIZE_MAX);
        probe.armed = true; calls = probe.calls;
        OK(xrt_managed_frame_enter_layout(root, &spec, &later)); CHECK(probe.calls == calls + 1);
    } else CHECK(!storage->value.hold && !storage->temporary.hold);
    CHECK(probe.cleared == 1 && !probe.armed);
    const void *unchanged = &probe;
    CHECK(xrt_module_borrow(reference, &unchanged) == XRT_MANAGED_ARGUMENT && unchanged == &probe);
    xrt_module_state_drop(&probe.state); CHECK(!probe.state);
    xrt_backing_types_drop(&types); OK(xrt_managed_root_destroy(&seed_root)); xrt_billing_owner_close(data);
    if (published_hold) {
        uint64_t bits = 0; OK(xrt_array_slice_read_bits(&storage->value.value, kind, 0, &bits)); CHECK(bits == 42);
        OK(xrt_array_slice_read_bits(&storage->value.value, kind, 1, &bits)); CHECK(bits == 43);
    }
    OK(xrt_managed_root_destroy(&root));
    XrtBillingStats final_data = xrt_billing_owner_stats(data), final_frames = xrt_billing_owner_stats(frames);
    CHECK(final_data.bytes == initial_data.bytes && final_data.blocks == initial_data.blocks &&
        final_frames.bytes == initial_frames.bytes && final_frames.blocks == initial_frames.blocks);
    xrt_billing_owner_drop(data); xrt_billing_owner_drop(frames);
    CHECK(!probe.blocks && !probe.bytes && probe.leases == 1);
}
int main(void) {
    XrtInlineValueSpec spec = xrt_array_slice_bundle_spec();
    CHECK(spec.size == sizeof(XrtArraySliceBundle) && spec.owned_count == 1 &&
        spec.owned_offsets[0] == offsetof(XrtArraySliceBundle, hold));
    kernel(XRT_BACKING_ARRAY_I64); kernel(XRT_BACKING_ARRAY_U8);
    slice_provider_state_probe(XRT_BACKING_ARRAY_I64, false); slice_provider_state_probe(XRT_BACKING_ARRAY_I64, true);
    slice_provider_state_probe(XRT_BACKING_ARRAY_U8, false); slice_provider_state_probe(XRT_BACKING_ARRAY_U8, true);
    puts("readonly natural Slice bytes and real hidden keeper transactions passed");
    return 0;
}
