/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_value_image.c - Owned raw images and independent physical bindings
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_value_image.h"
#include "runtime/native/xnative_value_image.h"
#include "runtime/native/xnative_string_backing.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define LIR(c) CHECK((c) == XR_LIR_OK)
#define MANAGED(c) CHECK((c) == XRT_MANAGED_OK)
#define LAYOUT(c) CHECK((c) == XR_LIR_VALUE_LAYOUT_OK)
static size_t enumerated;
static XrLirValueLayoutStatus counted_at(const XrLirValueLayouts *set, size_t ordinal,
    XrLirValueLayoutKey *key, const XrtValuePlan **plan) {
    ++enumerated; return xr_lir_value_layouts_at(set, ordinal, key, plan);
}
#define xr_lir_value_layouts_at counted_at
#include "lir/xlir_value_image.c"
#undef xr_lir_value_layouts_at

typedef struct Audit {
    size_t leases, bytes, blocks, final, allocations;
    XrLirValueImage **pending_output; void *first_metadata;
} Audit;
typedef struct Owner { Audit audit; XrtBillingOwner *billing; } Owner;
static bool provider_retain(void *context) { ++((Audit *)context)->leases; return true; }
static void provider_drop(void *context) {
    Audit *audit = context; CHECK(audit->leases);
    if (!--audit->leases) { CHECK(!audit->bytes && !audit->blocks); ++audit->final; }
}
static void *provider_allocate(void *context, size_t bytes) {
    Audit *audit = context; void *memory = xr_malloc(bytes);
    if (memory) {
        audit->bytes += bytes; ++audit->blocks; ++audit->allocations;
        if (audit->pending_output && audit->allocations == 2) audit->first_metadata = memory;
    }
    return memory;
}
static void provider_free(void *context, void *memory, size_t bytes) {
    Audit *audit = context; CHECK(audit->bytes >= bytes && audit->blocks);
    if (memory == audit->first_metadata) { CHECK(audit->pending_output && !*audit->pending_output); audit->first_metadata = NULL; }
    audit->bytes -= bytes; --audit->blocks; xr_free(memory);
}
static void owner_new(Owner *owner, size_t limit) {
    *owner = (Owner){.audit={.leases=1}};
    XrtManagedAllocator provider = {&owner->audit, provider_allocate, provider_free,
        {&owner->audit, provider_retain, provider_drop}};
    MANAGED(xrt_billing_owner_new(&provider, limit, &owner->billing));
}
static void owner_drop(Owner *owner) { xrt_billing_owner_drop(owner->billing); provider_drop(&owner->audit); }
typedef struct Fixture {
    XrtBillingOwner *billing;
    XrLirValueLayouts *layouts;
    XrLirType record, optional_record, empty;
} Fixture;
static Fixture fixture_new(void) {
    Fixture f = {0}; XrLirModule *module = NULL;
    LIR(xr_lir_module_create(NULL, &module)); MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &f.billing));
    XrLirType optional, fixed, tuple, optional_fixed, optional_tuple, optional_unit, optional_empty, pod, optional_pod;
    LIR(xr_lir_nullable_type(module, XR_LIR_STRING, &optional));
    char name[] = "label";
    XrLirRecordField fields[] = {{"active",6,XR_LIR_I1}, {name,5,XR_LIR_STRING}, {"maybe",5,optional}};
    LIR(xr_lir_record_type(module, fields, 3, &f.record)); name[0] = 'x';
    LIR(xr_lir_nullable_type(module, f.record, &f.optional_record));
    LIR(xr_lir_record_type(module, NULL, 0, &f.empty)); LIR(xr_lir_nullable_type(module, f.empty, &optional_empty));
    XrLirRecordField field = {"score",5,XR_LIR_I64}; LIR(xr_lir_record_type(module, &field, 1, &pod));
    LIR(xr_lir_nullable_type(module, pod, &optional_pod));
    LIR(xr_lir_fixed_array_type(module, XR_LIR_U8, 13, &fixed)); LIR(xr_lir_tuple_i64_type(module, 3, &tuple));
    LIR(xr_lir_nullable_type(module, fixed, &optional_fixed)); LIR(xr_lir_nullable_type(module, tuple, &optional_tuple));
    LIR(xr_lir_nullable_type(module, XR_LIR_UNIT, &optional_unit)); LIR(xr_lir_module_finish(module, NULL));
    XrLirValueLayoutKey roots[] = {{f.optional_record,0},{optional_empty,0},{optional_pod,0},{optional_fixed,0},{optional_tuple,0},
        {optional_unit,0},{XR_LIR_ARRAY,XR_LIR_I64},{XR_LIR_ARRAY,XR_LIR_STRING},{XR_LIR_ARRAY,XR_LIR_U8},{XR_LIR_ARRAY,XR_LIR_U64},
        {XR_LIR_MAP_STRING_I64,0},{XR_LIR_I8,0},{XR_LIR_I16,0},{XR_LIR_I32,0},{XR_LIR_U8,0},{XR_LIR_U16,0},{XR_LIR_U32,0},{XR_LIR_U64,0},
        {XR_LIR_F32,0},{XR_LIR_F64,0}};
    LAYOUT(xr_lir_value_layouts_create(module, roots, sizeof(roots)/sizeof(*roots), f.billing, &f.layouts));
    xr_lir_module_destroy(module); xrt_billing_owner_close(f.billing);
    return f;
}
static void fixture_drop(Fixture *f) {
    xr_lir_value_layouts_destroy(f->layouts); CHECK(xrt_billing_owner_stats(f->billing).blocks == 1);
    xrt_billing_owner_drop(f->billing);
}
typedef struct OptionalString { uint8_t tag; XrtManagedHeader *payload; } OptionalString;
typedef struct Record { bool active; XrtManagedHeader *label; OptionalString maybe; } Record;
typedef struct OptionalRecord { uint8_t tag; Record payload; } OptionalRecord;
static void copied_names_and_real_values(void) {
    Fixture f = fixture_new(); Owner raw, receiving; owner_new(&raw, SIZE_MAX); owner_new(&receiving, SIZE_MAX);
    XrLirValueImage *image = NULL; enumerated = 0; raw.audit.pending_output = &image;
    LAYOUT(xr_lir_value_image_create(f.layouts, raw.billing, &image)); CHECK(image);
    const XrtValueImage *view = xr_lir_value_image_view(image);
    CHECK(enumerated == xr_lir_value_layouts_count(f.layouts) && enumerated == view->type_count);
    CHECK(xrt_billing_owner_stats(raw.billing).blocks == 2);
    owner_drop(&raw); CHECK(!raw.audit.final);
    XrtValueImagePlans *bound = NULL;
    MANAGED(xrt_value_image_bind(receiving.billing, view, (XrtManagedLease){0}, &bound));
    CHECK(xrt_value_image_count(bound) == view->type_count);
    uint32_t record_id = 0, optional_id = 0; unsigned arrays = 0; bool map = false;
    for (uint32_t id = 1; id <= view->type_count; ++id) {
        XrLirValueLayoutKey key; const XrtValuePlan *source = NULL, *target = NULL; XrtValuePlanSpec a, b;
        LAYOUT(xr_lir_value_layouts_at(f.layouts, id - 1, &key, &source)); MANAGED(xrt_value_plan_view(source, &a));
        MANAGED(xrt_value_image_get(bound, id, &target)); MANAGED(xrt_value_plan_view(target, &b));
        const XrtValueImageType *type = &view->types[id - 1];
        CHECK(type->kind == a.kind && type->size == a.size && type->alignment == a.alignment && type->count == a.count);
        CHECK(b.kind == a.kind && b.size == a.size && b.alignment == a.alignment && b.count == a.count && b.owned_count == a.owned_count);
        for (size_t n = 0; n < a.owned_count; ++n)
            CHECK(view->owned_offsets[type->first_owned + n] == a.owned_offsets[n] && b.owned_offsets[n] == a.owned_offsets[n]);
        for (size_t field = 0; field < a.field_count; ++field) {
            CHECK(b.fields[field].offset == a.fields[field].offset && b.fields[field].name_length == a.fields[field].name_length);
            CHECK(!memcmp(b.fields[field].name, a.fields[field].name, a.fields[field].name_length));
            CHECK(view->slots[type->first_field + field].type < id);
        }
        if (key.type == f.record) record_id = id;
        if (key.type == f.optional_record) optional_id = id;
        if (key.type == XR_LIR_ARRAY) arrays |= 1u << (a.kind - XRT_VALUE_ARRAY_I64);
        map |= a.kind == XRT_VALUE_MAP_STRING_I64;
        if (key.type == f.empty) CHECK(type->size == 1 && !type->owned_count && !type->field_count);
    }
    CHECK(record_id && optional_id && arrays == 15 && map);
    const XrtValuePlan *borrowed = NULL; MANAGED(xrt_value_image_get(bound, optional_id, &borrowed));
    XrtValuePlan *plan = (XrtValuePlan *)borrowed; CHECK(xrt_value_plan_retain(plan));
    CHECK(xrt_value_image_get(bound, 0, &borrowed) == XRT_MANAGED_ARGUMENT);
    fixture_drop(&f); xr_lir_value_image_destroy(image); CHECK(raw.audit.final == 1);
    xrt_value_image_drop(&bound); CHECK(!bound);
    XrtValuePlanSpec spec, child; MANAGED(xrt_value_plan_view(plan, &spec));
    MANAGED(xrt_value_plan_view(spec.nullable.element, &child));
    CHECK(spec.size == sizeof(OptionalRecord) && child.size == sizeof(Record) && child.owned_count == 2);
    CHECK(child.fields[1].name_length == 5 && !memcmp(child.fields[1].name, "label", 5));
    XrtBackingTypes types = {0}; XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    MANAGED(xrt_backing_types_init(receiving.billing, &types)); MANAGED(xrt_managed_root_new(receiving.billing, &root));
    MANAGED(xrt_managed_frame_enter(root, 1, &frame)); MANAGED(xrt_string_backing_create(frame, 0, &types, "owned", 5, 0));
    XrtManagedHeader *header = NULL; MANAGED(xrt_managed_slot_borrow(frame, 0, &header));
    CHECK(xrt_managed_retain(header) && xrt_managed_retain(header));
    OptionalRecord source = {1, {true, header, {1, header}}}, destination;
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&types); owner_drop(&receiving);
    CHECK(!receiving.audit.final && xr_signed_rc_read(&header->references) == 2);
    MANAGED(xrt_value_zero(plan, &destination)); MANAGED(xrt_value_copy_empty(plan, &source, &destination));
    CHECK(xr_signed_rc_read(&header->references) == 4); xrt_value_drop(plan, &source);
    CHECK(destination.tag && destination.payload.active && destination.payload.maybe.tag &&
        ((const XrtStringBacking *)destination.payload.label)->length == 5);
    xrt_value_drop(plan, &destination); xrt_value_plan_drop(plan); CHECK(receiving.audit.final == 1);
}
static void failures_and_empty(void) {
    Fixture f = fixture_new(); Owner raw; owner_new(&raw, SIZE_MAX);
    XrtBillingStats before = xrt_billing_owner_stats(raw.billing); XrLirValueImage *image = NULL;
    LAYOUT(xr_lir_value_image_create(f.layouts, raw.billing, &image));
    size_t allocations = xrt_billing_owner_stats(raw.billing).allocations - before.allocations;
    CHECK(allocations == 2); XrLirValueImage *preserved = image;
    CHECK(xr_lir_value_image_create(f.layouts, raw.billing, &image) == XR_LIR_VALUE_LAYOUT_ARGUMENT && image == preserved);
    xr_lir_value_image_destroy(image); image = NULL;
    for (size_t allowed = 0; allowed < allocations; ++allowed) {
        xrt_billing_owner_fail_after(raw.billing, allowed);
        CHECK(xr_lir_value_image_create(f.layouts, raw.billing, &image) == XR_LIR_VALUE_LAYOUT_OOM && !image);
        XrtBillingStats after = xrt_billing_owner_stats(raw.billing);
        CHECK(after.bytes == before.bytes && after.blocks == before.blocks);
    }
    xrt_billing_owner_fail_after(raw.billing, SIZE_MAX); xrt_billing_owner_close(raw.billing);
    CHECK(xr_lir_value_image_create(f.layouts, raw.billing, &image) == XR_LIR_VALUE_LAYOUT_CLOSED && !image);
    owner_drop(&raw); CHECK(raw.audit.final == 1);
    owner_new(&raw, 1024);
    CHECK(xr_lir_value_image_create(f.layouts, raw.billing, &image) == XR_LIR_VALUE_LAYOUT_LIMIT && !image);
    owner_drop(&raw); CHECK(raw.audit.final == 1); fixture_drop(&f);
    XrLirModule *module = NULL; XrtBillingOwner *billing = NULL; XrLirValueLayouts *empty = NULL;
    LIR(xr_lir_module_create(NULL, &module)); LIR(xr_lir_module_finish(module, NULL));
    MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing)); LAYOUT(xr_lir_value_layouts_create(module, NULL, 0, billing, &empty));
    xr_lir_module_destroy(module); xrt_billing_owner_close(billing); before = xrt_billing_owner_stats(billing);
    LAYOUT(xr_lir_value_image_create(empty, billing, &image)); CHECK(!image);
    const XrtValueImage *view = xr_lir_value_image_view(NULL); CHECK(!view->type_count && !view->types && !view->name_bytes);
    XrtValueImagePlans *bound = NULL; MANAGED(xrt_value_image_bind(billing, view, (XrtManagedLease){0}, &bound)); CHECK(!bound);
    CHECK(!xrt_value_image_count(bound));
    CHECK(xrt_billing_owner_stats(billing).allocations == before.allocations);
    xr_lir_value_layouts_destroy(empty); xrt_billing_owner_drop(billing);
}
static void deep_single_inventory(void) {
    XrLirModule *module = NULL; XrtBillingOwner *billing = NULL; XrLirValueLayouts *layouts = NULL;
    LIR(xr_lir_module_create(NULL, &module)); XrLirType type = XR_LIR_STRING;
    for (unsigned i = 0; i < 512; ++i) LIR(xr_lir_nullable_type(module, type, &type));
    LIR(xr_lir_module_finish(module, NULL)); MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    XrLirValueLayoutKey key = {type, 0}; LAYOUT(xr_lir_value_layouts_create(module, &key, 1, billing, &layouts));
    XrLirValueImage *image = NULL; enumerated = 0; LAYOUT(xr_lir_value_image_create(layouts, billing, &image));
    const XrtValueImage *view = xr_lir_value_image_view(image); CHECK(enumerated == 513 && view->type_count == 513);
    for (uint32_t i = 1; i < view->type_count; ++i)
        CHECK(view->types[i].kind == XRT_VALUE_NULLABLE && view->types[i].nullable_element == i);
    xr_lir_module_destroy(module); xr_lir_value_layouts_destroy(layouts);
    XrtValueImagePlans *bound = NULL; MANAGED(xrt_value_image_bind(billing, view, (XrtManagedLease){0}, &bound));
    const XrtValuePlan *plan = NULL; MANAGED(xrt_value_image_get(bound, 513, &plan)); XrtValuePlanSpec spec;
    MANAGED(xrt_value_plan_view(plan, &spec)); CHECK(spec.owned_count == 1 && spec.size == 8u * 513u);
    xr_lir_value_image_destroy(image); xrt_value_image_drop(&bound);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
    puts("raw value image: 513 rows use exactly 513 enumeration reads and earlier-only dependencies");
}
int main(void) {
    copied_names_and_real_values(); failures_and_empty(); deep_single_inventory();
    puts("raw value images: billed ownership, copied names, exact ids, creator retirement and real inline values passed");
    return 0;
}
