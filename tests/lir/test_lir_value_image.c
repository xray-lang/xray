/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_value_image.c - Owned raw images and independent physical bindings
 */
#include "lir/xlir_array.h"
#include "lir/xlir_builder.h"
#include "lir/xlir_internal.h"
#include "lir/xlir_value_image.h"
#include "runtime/native/xnative_value_image.h"
#include "runtime/native/xnative_class_array.h"
#include "runtime/native/xnative_array_backing.h"
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
    XrLirRecordField fields[] = {{"active",6,XR_LIR_I1, 0, 0}, {name,5,XR_LIR_STRING, 0, 0}, {"maybe",5,optional, 0, 0}};
    LIR(xr_lir_record_type(module,
        &(XrLirRecordIdentity){.module="tests.lir.value_image", .module_length=sizeof("tests.lir.value_image")-1,
            .name="ImageRecord", .name_length=sizeof("ImageRecord")-1},
        fields, 3, &f.record)); name[0] = 'x';
    LIR(xr_lir_nullable_type(module, f.record, &f.optional_record));
    LIR(xr_lir_record_type(module,
        &(XrLirRecordIdentity){.module="tests.lir.value_image", .module_length=sizeof("tests.lir.value_image")-1,
            .name="EmptyRecord", .name_length=sizeof("EmptyRecord")-1},
        NULL, 0, &f.empty)); LIR(xr_lir_nullable_type(module, f.empty, &optional_empty));
    XrLirRecordField field = {"score",5,XR_LIR_I64, 0, 0}; LIR(xr_lir_record_type(module,
        &(XrLirRecordIdentity){.module="tests.lir.value_image", .module_length=sizeof("tests.lir.value_image")-1,
            .name="PodRecord", .name_length=sizeof("PodRecord")-1},
        &field, 1, &pod));
    LIR(xr_lir_nullable_type(module, pod, &optional_pod));
    LIR(xr_lir_fixed_array_type(module, XR_LIR_U8, 13, &fixed)); LIR(xr_lir_tuple_i64_type(module, 3, &tuple));
    LIR(xr_lir_nullable_type(module, fixed, &optional_fixed)); LIR(xr_lir_nullable_type(module, tuple, &optional_tuple));
    LIR(xr_lir_nullable_type(module, XR_LIR_UNIT, &optional_unit)); LIR(xr_lir_module_finish(module, NULL));
    XrLirValueLayoutKey roots[] = {{f.optional_record,0,0},{optional_empty,0,0},{optional_pod,0,0},{optional_fixed,0,0},{optional_tuple,0,0},
        {optional_unit,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0},{XR_LIR_ARRAY,XR_LIR_STRING,0},{XR_LIR_ARRAY,XR_LIR_U8,0},{XR_LIR_ARRAY,XR_LIR_U64,0},
        {XR_LIR_MAP_STRING_I64,0,0},{XR_LIR_I8,0,0},{XR_LIR_I16,0,0},{XR_LIR_I32,0,0},{XR_LIR_U8,0,0},{XR_LIR_U16,0,0},{XR_LIR_U32,0,0},{XR_LIR_U64,0,0},
        {XR_LIR_F32,0,0},{XR_LIR_F64,0,0}};
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
            if (a.kind == XRT_VALUE_TUPLE) {
                CHECK(!a.fields[field].name_length && !a.fields[field].name && !b.fields[field].name);
            } else {
                CHECK(a.fields[field].name_length && a.fields[field].name && b.fields[field].name);
                CHECK(!memcmp(b.fields[field].name, a.fields[field].name, a.fields[field].name_length));
            }
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
    XrLirValueLayoutKey key = {type, 0,0}; LAYOUT(xr_lir_value_layouts_create(module, &key, 1, billing, &layouts));
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
static void rejects_forged_display_closure(void) {
    XrtValueImageType types[] = {
        {.kind = XRT_VALUE_RECORD, .size = 1, .alignment = 1},
        {.kind = XRT_VALUE_TUPLE, .size = 1, .alignment = 1, .field_count = 1},
    };
    const XrtValueImageSlot fields[] = {{.type = 1}};
    const XrtValueImage image = {.types = types, .type_count = 2, .slots = fields, .slot_count = 1};
    Owner owner;
    owner_new(&owner, SIZE_MAX);
    XrtValueImagePlans *bound = NULL;
    MANAGED(xrt_value_image_bind(owner.billing, &image, (XrtManagedLease){0}, &bound));
    xrt_value_image_drop(&bound);
    XrtBillingStats before = xrt_billing_owner_stats(owner.billing);
    types[1].flags = XRT_VALUE_INLINE_DISPLAY;
    CHECK(xrt_value_image_bind(owner.billing, &image, (XrtManagedLease){0}, &bound) == XRT_MANAGED_ARGUMENT);
    CHECK(!bound && xrt_billing_owner_stats(owner.billing).allocations == before.allocations);
    owner_drop(&owner);
    CHECK(owner.audit.final == 1);
}
typedef struct ClassRow { int64_t id; XrtManagedHeader *name; } ClassRow;
static void class_values_follow_their_exact_owner(void) {
    static const char names[] = "idname";
    const size_t offsets[] = {0,offsetof(ClassRow,name)};
    const XrtValueImageSlot fields[] = {
        {1,0,offsetof(ClassRow,id),0,2}, {2,0,offsetof(ClassRow,name),2,4}
    };
    const XrtValueImageType types[] = {
        {.kind=XRT_VALUE_I64,.size=sizeof(int64_t),.alignment=_Alignof(int64_t)},
        {.kind=XRT_VALUE_STRING,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1},
        {.kind=XRT_VALUE_RECORD,.size=sizeof(ClassRow),.alignment=_Alignof(ClassRow),
            .first_owned=1,.owned_count=1,.field_count=2},
        {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=3},
        {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=3}
    };
    XrtValueImage image = {.types=types,.type_count=5,.slots=fields,.slot_count=2,
        .owned_offsets=offsets,.owned_count=2,.names=names,.name_bytes=sizeof(names)-1};
    Owner owner; owner_new(&owner,SIZE_MAX);
    XrtValueImagePlans *bound = NULL;
    MANAGED(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&bound));
    XrtBoundClass box = {0}, unrelated = {0}, rejected = {0};
    MANAGED(xrt_value_image_class(bound,4,&box)); MANAGED(xrt_value_image_class(bound,5,&unrelated));
    CHECK(box.plan != unrelated.plan && box.ops != unrelated.ops);
    CHECK(xrt_value_image_class(bound,3,&rejected) == XRT_MANAGED_ARGUMENT);
    XrtBoundInlineArray array = {0};
    CHECK(xrt_value_image_inline_array(bound,4,&array) == XRT_MANAGED_ARGUMENT);
    XrtBackingTypes backings = {0}; XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    MANAGED(xrt_backing_types_init(owner.billing,&backings));
    MANAGED(xrt_managed_root_new(owner.billing,&root));
    MANAGED(xrt_managed_frame_enter(root,2,&frame));
    MANAGED(xrt_string_backing_create(frame,0,&backings,"alive",5,0));
    XrtManagedHeader *text = NULL, *instance = NULL;
    MANAGED(xrt_managed_slot_borrow(frame,0,&text)); MANAGED(xrt_managed_slot_borrow(frame,1,&instance));
    const XrtValuePlan *field_plan = NULL; const void *field_bytes = NULL;
    CHECK(!instance && xrt_class_field_borrow(instance,box.plan,0,&field_plan,&field_bytes) == XRT_MANAGED_ARGUMENT);
    CHECK(xrt_class_create(frame,1,box.ops,NULL) == XRT_MANAGED_ARGUMENT);
    CHECK(xrt_class_create(frame,1,box.ops,&(ClassRow){42,NULL}) == XRT_MANAGED_ARGUMENT);
    ClassRow row = {42,text};
    XrtBillingStats before = xrt_billing_owner_stats(owner.billing);
    xrt_billing_owner_fail_after(owner.billing,0);
    CHECK(xrt_class_create(frame,1,box.ops,&row) == XRT_MANAGED_OOM);
    xrt_billing_owner_fail_after(owner.billing,SIZE_MAX);
    atomic_store(&text->references,INT32_MAX);
    CHECK(xrt_class_create(frame,1,box.ops,&row) == XRT_MANAGED_REFCOUNT);
    atomic_store(&text->references,1);
    XrtBillingStats after = xrt_billing_owner_stats(owner.billing);
    CHECK(before.blocks == after.blocks && before.bytes == after.bytes);
    MANAGED(xrt_managed_slot_borrow(frame,1,&instance)); CHECK(!instance);
    MANAGED(xrt_class_create(frame,1,box.ops,&row));
    MANAGED(xrt_managed_slot_borrow(frame,1,&instance)); CHECK(instance && xrt_managed_retain(instance));
    CHECK(xrt_class_field_borrow(instance,unrelated.plan,0,&field_plan,&field_bytes) == XRT_MANAGED_ARGUMENT);
    CHECK(xrt_class_field_borrow(instance,box.plan,2,&field_plan,&field_bytes) == XRT_MANAGED_BOUNDS);
    const XrtValuePlan *live_plan = box.plan; /* Retained by the owning Header's Ops. */
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&backings);
    xrt_value_image_drop(&bound); owner_drop(&owner); CHECK(!owner.audit.final);
    MANAGED(xrt_class_field_borrow(instance,live_plan,0,&field_plan,&field_bytes));
    CHECK(*(const int64_t *)field_bytes == 42);
    field_plan = NULL; field_bytes = NULL;
    MANAGED(xrt_class_field_borrow(instance,live_plan,1,&field_plan,&field_bytes));
    XrtManagedHeader *kept = NULL;
    MANAGED(xrt_value_copy_empty(field_plan,field_bytes,&kept));
    xrt_managed_drop(instance); CHECK(!owner.audit.final);
    const XrtStringBacking *live = (const XrtStringBacking *)kept;
    CHECK(live->length == 5 && !memcmp(live->bytes,"alive",5));
    xrt_managed_drop(kept); CHECK(owner.audit.final == 1);
}
static void class_arrays_keep_their_exact_backing(void) {
    const size_t offset = 0;
    const XrtValueImageSlot field = {.type=1,.name_length=5};
    const XrtValueImageType types[] = {
        {.kind=XRT_VALUE_ARRAY_I64,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1},
        {.kind=XRT_VALUE_RECORD,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.field_count=1},
        {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=2}
    };
    const XrtValueImage image = {.types=types,.type_count=3,.slots=&field,.slot_count=1,
        .owned_offsets=&offset,.owned_count=1,.names="items",.name_bytes=5};
    Owner owner; owner_new(&owner,SIZE_MAX);
    XrtValueImagePlans *bound=NULL, *different_program=NULL;
    MANAGED(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&bound));
    MANAGED(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&different_program));
    XrtBoundClass holder={0}, other={0};
    MANAGED(xrt_value_image_class(bound,3,&holder)); MANAGED(xrt_value_image_class(different_program,3,&other));
    XrtBackingTypes backings={0}; XrtManagedRoot *root=NULL; XrtManagedFrame *frame=NULL;
    MANAGED(xrt_backing_types_init(owner.billing,&backings));
    MANAGED(xrt_managed_root_new(owner.billing,&root)); MANAGED(xrt_managed_frame_enter(root,3,&frame));
    const int64_t data[2]={3,7}; const uint64_t unrelated[2]={3,7};
    MANAGED(xrt_array_backing_i64_create(frame,0,&backings,data,2,2));
    MANAGED(xrt_array_backing_u64_create(frame,1,&backings,unrelated,2,2));
    XrtManagedHeader *array=NULL, *wrong=NULL, *instance=NULL;
    MANAGED(xrt_managed_slot_borrow(frame,0,&array)); MANAGED(xrt_managed_slot_borrow(frame,1,&wrong));
    XrtBillingStats before=xrt_billing_owner_stats(owner.billing);
    CHECK(xrt_class_create(frame,2,holder.ops,&wrong)==XRT_MANAGED_ARGUMENT);
    xrt_billing_owner_fail_after(owner.billing,0);
    CHECK(xrt_class_create(frame,2,holder.ops,&array)==XRT_MANAGED_OOM);
    xrt_billing_owner_fail_after(owner.billing,SIZE_MAX);
    XrtBillingStats after=xrt_billing_owner_stats(owner.billing);
    CHECK(after.bytes==before.bytes && after.blocks==before.blocks && xr_signed_rc_read(&array->references)==1);
    MANAGED(xrt_managed_slot_borrow(frame,2,&instance)); CHECK(!instance);
    MANAGED(xrt_class_create(frame,2,holder.ops,&array));
    MANAGED(xrt_managed_slot_borrow(frame,2,&instance));
    const XrtValuePlan *plan=NULL;
    const void *payload=NULL;
    CHECK(xrt_class_field_borrow(instance,other.plan,0,&plan,&payload)==XRT_MANAGED_ARGUMENT);
    MANAGED(xrt_class_field_borrow(instance,holder.plan,0,&plan,&payload));
    XrtManagedHeader *kept=NULL;
    MANAGED(xrt_value_copy_empty(plan,payload,&kept)); CHECK(kept==array);
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&backings);
    xrt_value_image_drop(&bound); xrt_value_image_drop(&different_program); owner_drop(&owner);
    CHECK(!owner.audit.final);
    CHECK(((const XrtArrayBacking *)kept)->length==2);
    CHECK(!memcmp(((const XrtArrayBacking *)kept)->elements,data,sizeof(data)));
    xrt_managed_drop(kept); CHECK(owner.audit.final==1);
}
static void class_array_update_failure_preserves_the_field(bool byte_array) {
    const size_t offset=0;
    const XrtValueImageSlot field={.type=1,.flags=XRT_VALUE_FIELD_MUTABLE,.name_length=5};
    const XrtValueImageType types[]={
        {.kind=byte_array ? XRT_VALUE_ARRAY_U8 : XRT_VALUE_ARRAY_I64,
            .size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1},
        {.kind=XRT_VALUE_RECORD,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.field_count=1},
        {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=2}
    };
    const XrtValueImage image={.types=types,.type_count=3,.slots=&field,.slot_count=1,
        .owned_offsets=&offset,.owned_count=1,.names="items",.name_bytes=5};
    Owner owner; owner_new(&owner,SIZE_MAX);
    XrtValueImagePlans *bound=NULL;
    MANAGED(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&bound));
    XrtBoundClass holder={0}; MANAGED(xrt_value_image_class(bound,3,&holder));
    XrtBackingTypes backings={0}; XrtManagedRoot *root=NULL; XrtManagedFrame *frame=NULL;
    MANAGED(xrt_backing_types_init(owner.billing,&backings));
    MANAGED(xrt_managed_root_new(owner.billing,&root)); MANAGED(xrt_managed_frame_enter(root,6,&frame));
    const int64_t item=3;
    const uint8_t byte=3;
    MANAGED(xrt_array_backing_i64_create(frame,byte_array ? 5 : 0,&backings,&item,1,1));
    MANAGED(xrt_array_backing_u8_create(frame,byte_array ? 0 : 5,&backings,&byte,1,1));
    XrtManagedHeader *old=NULL,*instance=NULL,*read=NULL,*work=NULL,*wrong=NULL;
    MANAGED(xrt_managed_slot_borrow(frame,0,&old));
    MANAGED(xrt_managed_slot_borrow(frame,5,&wrong));
    CHECK(xrt_class_create(frame,1,holder.ops,&wrong)==XRT_MANAGED_ARGUMENT);
    MANAGED(xrt_class_create(frame,1,holder.ops,&old));
    MANAGED(xrt_managed_slot_borrow(frame,1,&instance));
    const XrtClassFieldAccess access={instance,holder.plan,0};
    MANAGED(xrt_class_array_field_copy(&access,frame,2));
    const int64_t appended = byte_array ? UINT8_MAX : -7;
    XrtBillingStats before=xrt_billing_owner_stats(owner.billing);
    CHECK(xrt_class_array_field_replace(&access,wrong)==XRT_MANAGED_ARGUMENT);
    if (byte_array) {
        CHECK(xrt_class_array_field_push(&access,frame,3,&backings,-1)==XRT_MANAGED_ARGUMENT);
        CHECK(xrt_class_array_field_push(&access,frame,3,&backings,256)==XRT_MANAGED_ARGUMENT);
    }
    xrt_billing_owner_fail_after(owner.billing,0);
    CHECK(xrt_class_array_field_push(&access,frame,3,&backings,appended)==XRT_MANAGED_OOM);
    xrt_billing_owner_fail_after(owner.billing,SIZE_MAX);
    XrtBillingStats after=xrt_billing_owner_stats(owner.billing);
    CHECK(after.bytes==before.bytes && after.blocks==before.blocks);
    MANAGED(xrt_managed_slot_borrow(frame,3,&work)); CHECK(!work);
    MANAGED(xrt_class_array_field_copy(&access,frame,4));
    MANAGED(xrt_managed_slot_borrow(frame,4,&read)); CHECK(read==old);
    MANAGED(xrt_class_array_field_push(&access,frame,3,&backings,appended));
    MANAGED(xrt_class_array_field_copy(&access,frame,4));
    read=NULL;
    MANAGED(xrt_managed_slot_borrow(frame,4,&read)); CHECK(read!=old);
    CHECK(((const XrtArrayBacking *)old)->length==1 && ((const XrtArrayBacking *)read)->length==2);
    const uint8_t expected_bytes[]={3,UINT8_MAX};
    const int64_t expected_i64[]={3,-7};
    CHECK(!(byte_array ? memcmp(((const XrtArrayBacking *)read)->elements,expected_bytes,sizeof(expected_bytes)) :
        memcmp(((const XrtArrayBacking *)read)->elements,expected_i64,sizeof(expected_i64))));
    MANAGED(xrt_class_array_field_replace(&access,old));
    MANAGED(xrt_class_array_field_copy(&access,frame,4));
    read=NULL;
    MANAGED(xrt_managed_slot_borrow(frame,4,&read)); CHECK(read==old);
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&backings);
    xrt_value_image_drop(&bound); owner_drop(&owner); CHECK(owner.audit.final==1);
}
static void derived_classes_keep_concrete_identity(void) {
    const size_t offset = 0;
    XrtValueImageType types[] = {
        {.kind=XRT_VALUE_I64,.size=8,.alignment=8},
        {.kind=XRT_VALUE_RECORD,.size=8,.alignment=8,.field_count=1},
        {.kind=XRT_VALUE_CLASS,.size=8,.alignment=8,.owned_count=1,.element=2},
        {.kind=XRT_VALUE_RECORD,.size=16,.alignment=8,.first_field=1,.field_count=2},
        {.kind=XRT_VALUE_CLASS,.size=8,.alignment=8,.owned_count=1,.element=4,.class_parent=3}
    };
    XrtValueImageSlot fields[] = {{1,0,0,0,2},{1,0,0,0,2},{1,0,8,2,3}};
    const XrtValueImage image = {.types=types,.type_count=5,.slots=fields,.slot_count=3,
        .owned_offsets=&offset,.owned_count=1,.names="idage",.name_bytes=5};
    Owner owner; owner_new(&owner,SIZE_MAX);
    XrtValueImagePlans *bound=NULL;
    XrtBillingStats before=xrt_billing_owner_stats(owner.billing);
    fields[0].name_offset=2; fields[0].name_length=3;
    CHECK(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&bound)==XRT_MANAGED_ARGUMENT && !bound);
    fields[0]=(XrtValueImageSlot){.type=1,.name_length=2};
    types[4].class_parent=2;
    CHECK(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&bound)==XRT_MANAGED_ARGUMENT && !bound);
    types[4].class_parent=3;
    XrtBillingStats after=xrt_billing_owner_stats(owner.billing);
    CHECK(before.bytes==after.bytes && before.blocks==after.blocks);
    MANAGED(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&bound));
    XrtBoundClass base={0}, derived={0};
    MANAGED(xrt_value_image_class(bound,3,&base)); MANAGED(xrt_value_image_class(bound,5,&derived));
    XrtValuePlanSpec spec;
    MANAGED(xrt_value_plan_view(derived.plan,&spec)); CHECK(spec.class_parent==base.plan);
    XrtManagedRoot *root=NULL; XrtManagedFrame *frame=NULL;
    MANAGED(xrt_managed_root_new(owner.billing,&root)); MANAGED(xrt_managed_frame_enter(root,1,&frame));
    const int64_t row[]={42,73};
    MANAGED(xrt_class_create(frame,0,derived.ops,row));
    XrtManagedHeader *instance=NULL;
    MANAGED(xrt_managed_slot_borrow(frame,0,&instance));
    const XrtValuePlan *type=NULL;
    const void *bytes=NULL;
    CHECK(xrt_class_field_borrow(instance,base.plan,0,&type,&bytes)==XRT_MANAGED_ARGUMENT);
    MANAGED(xrt_class_field_borrow(instance,derived.plan,0,&type,&bytes));
    CHECK(*(const int64_t *)bytes==42);
    MANAGED(xrt_managed_root_destroy(&root)); xrt_value_image_drop(&bound); owner_drop(&owner);
    CHECK(owner.audit.final==1);
}
static void nested_classes_keep_exact_owner_plans(void) {
    const size_t offset = 0;
    const XrtValueImageSlot field = {.type=2,.name_length=5};
    const XrtValueImageType types[] = {
        {.kind=XRT_VALUE_RECORD,.size=1,.alignment=1},
        {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=1},
        {.kind=XRT_VALUE_RECORD,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.field_count=1},
        {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=3},
    };
    const XrtValueImage image = {.types=types,.type_count=4,.slots=&field,.slot_count=1,
        .owned_offsets=&offset,.owned_count=1,.names="child",.name_bytes=5};
    Owner owner; owner_new(&owner,SIZE_MAX);
    XrtValueImagePlans *bound=NULL, *different_program=NULL;
    MANAGED(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&bound));
    MANAGED(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&different_program));
    XrtBoundClass child={0}, different={0}, parent={0};
    MANAGED(xrt_value_image_class(bound,2,&child));
    MANAGED(xrt_value_image_class(different_program,2,&different));
    MANAGED(xrt_value_image_class(bound,4,&parent));
    XrtManagedRoot *root=NULL; XrtManagedFrame *frame=NULL;
    MANAGED(xrt_managed_root_new(owner.billing,&root)); MANAGED(xrt_managed_frame_enter(root,3,&frame));
    const unsigned char empty=0;
    MANAGED(xrt_class_create(frame,0,child.ops,&empty));
    MANAGED(xrt_class_create(frame,1,different.ops,&empty));
    XrtManagedHeader *expected=NULL,*wrong=NULL,*instance=NULL;
    MANAGED(xrt_managed_slot_borrow(frame,0,&expected));
    MANAGED(xrt_managed_slot_borrow(frame,1,&wrong));
    XrtBillingStats before=xrt_billing_owner_stats(owner.billing);
    CHECK(xrt_class_create(frame,2,parent.ops,&wrong)==XRT_MANAGED_ARGUMENT);
    XrtBillingStats after=xrt_billing_owner_stats(owner.billing);
    CHECK(before.bytes==after.bytes && before.blocks==after.blocks && xr_signed_rc_read(&wrong->references)==1);
    MANAGED(xrt_managed_slot_borrow(frame,2,&instance)); CHECK(!instance);
    MANAGED(xrt_class_create(frame,2,parent.ops,&expected));
    MANAGED(xrt_managed_slot_borrow(frame,2,&instance));
    const XrtValuePlan *type=NULL;
    const void *payload=NULL;
    MANAGED(xrt_class_field_borrow(instance,parent.plan,0,&type,&payload)); CHECK(type==child.plan);
    XrtManagedHeader *kept=NULL;
    MANAGED(xrt_value_copy_empty(type,payload,&kept)); CHECK(kept==expected);
    MANAGED(xrt_managed_root_destroy(&root));
    xrt_value_image_drop(&bound); xrt_value_image_drop(&different_program); owner_drop(&owner);
    CHECK(!owner.audit.final && xrt_class_backing_matches(kept,type));
    xrt_managed_drop(kept); CHECK(owner.audit.final==1);
}
static void nested_class_owners_are_checked_before_publication(void) {
    typedef struct Pair { XrtManagedHeader *array, *text; } Pair;
    typedef struct OptionalPair { uint8_t tag; Pair payload; } OptionalPair;
    typedef struct Row { OptionalPair optional; XrtManagedHeader *required; } Row;
    const size_t offsets[] = {0,offsetof(Pair,text),
        offsetof(OptionalPair,payload)+offsetof(Pair,text),offsetof(Row,required)};
    const XrtValueImageSlot fields[] = {
        {.type=1},{.type=2,.offset=offsetof(Pair,text)},
        {.type=4},{.type=2,.offset=offsetof(Row,required)},
        {.type=5,.name_length=3}
    };
    const XrtValueImageType types[] = {
        {.kind=XRT_VALUE_ARRAY_I64,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1},
        {.kind=XRT_VALUE_STRING,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1},
        {.kind=XRT_VALUE_TUPLE,.size=sizeof(Pair),.alignment=_Alignof(Pair),.owned_count=2,.field_count=2},
        {.kind=XRT_VALUE_NULLABLE,.size=sizeof(OptionalPair),.alignment=_Alignof(OptionalPair),
            .first_owned=1,.owned_count=2,.nullable_element=3,.nullable_payload=offsetof(OptionalPair,payload)},
        {.kind=XRT_VALUE_TUPLE,.size=sizeof(Row),.alignment=_Alignof(Row),
            .first_owned=1,.owned_count=3,.first_field=2,.field_count=2},
        {.kind=XRT_VALUE_RECORD,.size=sizeof(Row),.alignment=_Alignof(Row),
            .first_owned=1,.owned_count=3,.first_field=4,.field_count=1},
        {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=6}
    };
    const XrtValueImage image = {.types=types,.type_count=7,.slots=fields,.slot_count=5,
        .owned_offsets=offsets,.owned_count=4,.names="row",.name_bytes=3};
    Owner owner; owner_new(&owner,SIZE_MAX);
    XrtValueImagePlans *bound=NULL;
    MANAGED(xrt_value_image_bind(owner.billing,&image,(XrtManagedLease){0},&bound));
    XrtBoundClass type={0};
    MANAGED(xrt_value_image_class(bound,7,&type));
    XrtBackingTypes backings={0}; XrtManagedRoot *root=NULL; XrtManagedFrame *frame=NULL;
    MANAGED(xrt_backing_types_init(owner.billing,&backings));
    MANAGED(xrt_managed_root_new(owner.billing,&root)); MANAGED(xrt_managed_frame_enter(root,4,&frame));
    const int64_t data[]={42}; const uint64_t unrelated[]={42};
    MANAGED(xrt_array_backing_i64_create(frame,0,&backings,data,1,1));
    MANAGED(xrt_array_backing_u64_create(frame,1,&backings,unrelated,1,1));
    MANAGED(xrt_string_backing_create(frame,2,&backings,"ready",5,0));
    XrtManagedHeader *array=NULL, *wrong=NULL, *text=NULL, *old=NULL;
    MANAGED(xrt_managed_slot_borrow(frame,0,&array)); MANAGED(xrt_managed_slot_borrow(frame,1,&wrong));
    MANAGED(xrt_managed_slot_borrow(frame,2,&text));
    Row row={.required=text};
    MANAGED(xrt_class_create(frame,3,type.ops,&row)); MANAGED(xrt_managed_slot_borrow(frame,3,&old));
    XrtBillingStats before=xrt_billing_owner_stats(owner.billing);
    int32_t text_refs=xr_signed_rc_read(&text->references);
    Row rejected[]={
        {{1,{wrong,text}},text}, {{1,{array,NULL}},text}, {{1,{NULL,text}},text},
        {{0,{NULL,text}},text}, {{0,{array,NULL}},text}, {{0,{NULL,NULL}},NULL}, {{2,{NULL,NULL}},text}
    };
    for (size_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i)
        CHECK(xrt_class_create(frame,3,type.ops,&rejected[i])==XRT_MANAGED_ARGUMENT);
    row.optional=(OptionalPair){1,{array,text}};
    xrt_billing_owner_fail_after(owner.billing,0);
    CHECK(xrt_class_create(frame,3,type.ops,&row)==XRT_MANAGED_OOM);
    xrt_billing_owner_fail_after(owner.billing,SIZE_MAX);
    XrtBillingStats after=xrt_billing_owner_stats(owner.billing);
    CHECK(after.blocks==before.blocks && after.bytes==before.bytes);
    CHECK(xr_signed_rc_read(&array->references)==1 && xr_signed_rc_read(&text->references)==text_refs);
    XrtManagedHeader *current=NULL;
    MANAGED(xrt_managed_slot_borrow(frame,3,&current)); CHECK(current==old);
    MANAGED(xrt_class_create(frame,3,type.ops,&row));
    current=NULL; MANAGED(xrt_managed_slot_borrow(frame,3,&current)); CHECK(current);
    const XrtValuePlan *check=NULL; const void *payload=NULL;
    MANAGED(xrt_class_field_borrow(current,type.plan,0,&check,&payload));
    const Row *published=payload;
    CHECK(published->optional.tag==1 && published->optional.payload.array==array &&
        published->optional.payload.text==text && published->required==text);
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&backings);
    xrt_value_image_drop(&bound); owner_drop(&owner); CHECK(owner.audit.final==1);
}
static void nullable_arrays_keep_their_exact_backing(void) {
    typedef struct OptionalArray { uint8_t tag; XrtManagedHeader *payload; } OptionalArray;
    typedef struct OptionalI64 { uint8_t tag; int64_t payload; } OptionalI64;
    typedef struct Row { OptionalArray bytes, items, texts; } Row;
    XrLirModule *module = NULL, *imported = NULL;
    XrLirType bytes, items, texts, scalar, again, row_type, holder_type;
    LIR(xr_lir_module_create(NULL, &module));
    CHECK(xr_lir_nullable_type(module, XR_LIR_ARRAY, &bytes) == XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(module); module = NULL;
    LIR(xr_lir_module_create(NULL, &module));
    CHECK(xr_lir_nullable_array_type(module, XR_LIR_U64, &bytes) == XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(module); module = NULL;

    LIR(xr_lir_module_create(NULL, &module));
    LIR(xr_lir_nullable_array_type(module, XR_LIR_U8, &bytes));
    LIR(xr_lir_nullable_array_type(module, XR_LIR_I64, &items));
    LIR(xr_lir_nullable_array_type(module, XR_LIR_STRING, &texts));
    /* Grow the index before finding the existing shapes again. */
    for (uint32_t count = 1; count <= 64; ++count)
        LIR(xr_lir_fixed_array_type(module, XR_LIR_U8, count, &again));
    LIR(xr_lir_nullable_array_type(module, XR_LIR_U8, &again)); CHECK(again == bytes);
    LIR(xr_lir_nullable_array_type(module, XR_LIR_I64, &again)); CHECK(again == items && items != bytes);
    LIR(xr_lir_nullable_array_type(module, XR_LIR_STRING, &again)); CHECK(again == texts && texts != bytes && texts != items);
    XrLirTypeView shape;
    LIR(xr_lir_type_view(module, bytes, &shape)); CHECK(shape.kind == XR_LIR_TYPE_NULLABLE && shape.element == XR_LIR_ARRAY);
    LIR(xr_lir_type_view(module, items, &shape)); CHECK(shape.kind == XR_LIR_TYPE_NULLABLE && shape.element == XR_LIR_ARRAY);
    CHECK(xr_lir_nullable_array_element(module, bytes) == XR_LIR_U8);
    CHECK(xr_lir_nullable_array_element(module, items) == XR_LIR_I64);
    CHECK(xr_lir_nullable_array_element(module, texts) == XR_LIR_STRING);
    LIR(xr_lir_nullable_type(module, XR_LIR_I64, &scalar));
    CHECK(xr_lir_nullable_array_element(module, scalar) == XR_LIR_TYPE_INVALID);

    const XrLirRecordField fields[] = {{"bytes",5,bytes,0,0},{"items",5,items,0,0},{"texts",5,texts,0,0}};
    const XrLirRecordIdentity record = {.module="tests.lir.value_image",.module_length=sizeof("tests.lir.value_image")-1,
        .name="ArrayPair",.name_length=sizeof("ArrayPair")-1};
    LIR(xr_lir_record_type(module, &record, fields, 3, &row_type));
    LIR(xr_lir_internal_class_type(module, row_type, XR_LIR_TYPE_INVALID, &holder_type));
    LIR(xr_lir_module_finish(module, NULL));
    LIR(xr_lir_module_create(NULL, &imported));
    LIR(xr_lir_import_type(imported, module, items, &again));
    CHECK(xr_lir_nullable_array_element(imported, again) == XR_LIR_I64);
    LIR(xr_lir_import_type(imported, module, bytes, &again));
    CHECK(xr_lir_nullable_array_element(imported, again) == XR_LIR_U8);
    LIR(xr_lir_import_type(imported, module, texts, &again));
    CHECK(xr_lir_nullable_array_element(imported, again) == XR_LIR_STRING);
    xr_lir_module_destroy(imported);

    Owner owner; owner_new(&owner, SIZE_MAX);
    XrLirValueLayouts *layouts = NULL; XrLirValueImage *image = NULL;
    const XrLirValueLayoutKey roots[] = {{holder_type,0,0},{scalar,0,0}};
    LAYOUT(xr_lir_value_layouts_create(module, roots, 2, owner.billing, &layouts));
    LAYOUT(xr_lir_value_image_create(layouts, owner.billing, &image));
    const XrtValueImage *view = xr_lir_value_image_view(image);
    const XrLirType types[] = {bytes,items,texts,scalar,holder_type};
    const XrtValueKind families[] = {XRT_VALUE_ARRAY_U8,XRT_VALUE_ARRAY_I64,XRT_VALUE_ARRAY_STRING};
    uint32_t ids[sizeof(types)/sizeof(*types)];
    for (size_t i = 0; i < sizeof(ids)/sizeof(*ids); ++i) {
        const XrtValuePlan *plan = NULL;
        LAYOUT(xr_lir_value_layouts_lookup(layouts, (XrLirValueLayoutKey){types[i],0,0}, &plan, &ids[i]));
    }
    for (size_t i = 0; i < sizeof(families)/sizeof(*families); ++i) {
        const XrtValueImageType *type = &view->types[ids[i] - 1];
        CHECK(type->kind == XRT_VALUE_NULLABLE && type->nullable_tag == 0 && type->count == 0);
        CHECK(type->size == sizeof(OptionalArray) && type->alignment == _Alignof(OptionalArray));
        CHECK(type->nullable_payload == offsetof(OptionalArray,payload) && type->owned_count == 1);
        CHECK(view->owned_offsets[type->first_owned] == type->nullable_payload);
        CHECK(type->nullable_element && type->nullable_element < ids[i] &&
            view->types[type->nullable_element - 1].kind == families[i]);
    }
    const XrtValueImageType *old = &view->types[ids[3] - 1];
    CHECK(old->kind == XRT_VALUE_NULLABLE && old->nullable_payload == offsetof(OptionalI64,payload) &&
        old->size == sizeof(OptionalI64) && !old->owned_count && old->nullable_element &&
        view->types[old->nullable_element - 1].kind == XRT_VALUE_I64);

    XrtValueImagePlans *bound = NULL;
    MANAGED(xrt_value_image_bind(owner.billing, view, (XrtManagedLease){0}, &bound));
    xr_lir_value_image_destroy(image); xr_lir_value_layouts_destroy(layouts); xr_lir_module_destroy(module);
    XrtBoundClass holder = {0}; MANAGED(xrt_value_image_class(bound, ids[4], &holder));
    XrtBackingTypes backings = {0}; XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    MANAGED(xrt_backing_types_init(owner.billing, &backings));
    MANAGED(xrt_managed_root_new(owner.billing, &root)); MANAGED(xrt_managed_frame_enter(root, 4, &frame));
    MANAGED(xrt_array_backing_u8_create(frame, 0, &backings, (const uint8_t[]){7}, 1, 1));
    MANAGED(xrt_array_backing_i64_create(frame, 1, &backings, (const int64_t[]){-4}, 1, 1));
    MANAGED(xrt_array_backing_string_create(frame, 2, &backings, NULL, 0, 0));
    XrtManagedHeader *owners[3] = {NULL,NULL,NULL};
    for (size_t i = 0; i < sizeof(owners)/sizeof(*owners); ++i)
        MANAGED(xrt_managed_slot_borrow(frame, i, &owners[i]));
    for (size_t i = 0; i < sizeof(families)/sizeof(*families); ++i) {
        const XrtValuePlan *plan = NULL; MANAGED(xrt_value_image_get(bound, ids[i], &plan));
        OptionalArray some = {1,owners[i]}, none = {0}, copy = {0};
        MANAGED(xrt_value_copy_empty(plan, &none, &copy)); CHECK(!copy.tag && !copy.payload);
        MANAGED(xrt_value_copy_empty(plan, &some, &copy)); CHECK(copy.tag && copy.payload == owners[i]);
        CHECK(xr_signed_rc_read(&owners[i]->references) == 2);
        MANAGED(xrt_value_move_empty(plan, &copy, &none)); CHECK(!copy.tag && !copy.payload && none.payload == owners[i]);
        xrt_value_drop(plan, &none); CHECK(xr_signed_rc_read(&owners[i]->references) == 1);
        OptionalArray forged[] = {{1,owners[(i+1) % 3]},{1,NULL},{0,owners[i]},{2,owners[i]}};
        for (size_t n = 0; n < sizeof(forged)/sizeof(*forged); ++n) {
            CHECK(xrt_value_copy_empty(plan, &forged[n], &copy) == XRT_MANAGED_ARGUMENT);
            CHECK(xrt_value_move_empty(plan, &forged[n], &copy) == XRT_MANAGED_ARGUMENT);
            CHECK(!copy.tag && !copy.payload);
        }
    }
    Row row = {{1,owners[0]},{1,owners[1]},{1,owners[2]}};
    MANAGED(xrt_class_create(frame, 3, holder.ops, &row));
    XrtManagedHeader *saved = NULL; MANAGED(xrt_managed_slot_borrow(frame, 3, &saved));
    row.bytes.payload = owners[1]; CHECK(xrt_class_create(frame, 3, holder.ops, &row) == XRT_MANAGED_ARGUMENT);
    row.bytes = (OptionalArray){0}; row.items.payload = owners[0];
    CHECK(xrt_class_create(frame, 3, holder.ops, &row) == XRT_MANAGED_ARGUMENT);
    XrtManagedHeader *current = NULL; MANAGED(xrt_managed_slot_borrow(frame, 3, &current)); CHECK(current == saved);
    row.items.payload = owners[1]; row.texts.payload = owners[0];
    CHECK(xrt_class_create(frame, 3, holder.ops, &row) == XRT_MANAGED_ARGUMENT);
    row.texts.payload = owners[2]; MANAGED(xrt_class_create(frame, 3, holder.ops, &row));
    CHECK(xr_signed_rc_read(&owners[0]->references) == 1 && xr_signed_rc_read(&owners[1]->references) == 2 &&
        xr_signed_rc_read(&owners[2]->references) == 2);
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&backings);
    xrt_value_image_drop(&bound); owner_drop(&owner); CHECK(owner.audit.final == 1);
}
static void class_array_elements_keep_their_exact_backing(void) {
    typedef struct OptionalArray { uint8_t tag; XrtManagedHeader *payload; } OptionalArray;
    const size_t offsets[] = {0, offsetof(OptionalArray,payload)};
    const XrtValueImageSlot fields[] = {{.type=1,.name_length=5}};
    const XrtValueImageType types[] = {
        {.kind=XRT_VALUE_I64,.size=8,.alignment=8},
        {.kind=XRT_VALUE_RECORD,.size=8,.alignment=8,.field_count=1},
        {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=2},
        {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=2},
        {.kind=XRT_VALUE_ARRAY_INLINE,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=3},
        {.kind=XRT_VALUE_ARRAY_INLINE,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=4},
        {.kind=XRT_VALUE_NULLABLE,.size=sizeof(OptionalArray),.alignment=_Alignof(OptionalArray),
            .first_owned=1,.owned_count=1,.nullable_element=5,.nullable_payload=offsetof(OptionalArray,payload)},
    };
    const XrtValueImage image = {.types=types,.type_count=sizeof(types)/sizeof(*types),.slots=fields,.slot_count=1,
        .owned_offsets=offsets,.owned_count=sizeof(offsets)/sizeof(*offsets),.names="count",.name_bytes=5};
    Owner owner; owner_new(&owner, SIZE_MAX);
    XrtValueImagePlans *bound = NULL;
    MANAGED(xrt_value_image_bind(owner.billing, &image, (XrtManagedLease){0}, &bound));
    XrtBoundClass type = {0}, other = {0};
    XrtBoundInlineArray array_type = {0}, other_array_type = {0};
    MANAGED(xrt_value_image_class(bound, 3, &type));
    MANAGED(xrt_value_image_class(bound, 4, &other));
    MANAGED(xrt_value_image_inline_array(bound, 5, &array_type));
    MANAGED(xrt_value_image_inline_array(bound, 6, &other_array_type));
    const XrtValuePlan *optional = NULL;
    MANAGED(xrt_value_image_get(bound, 7, &optional));
    CHECK(array_type.element == type.plan && other_array_type.element == other.plan && other.plan != type.plan);
    XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    MANAGED(xrt_managed_root_new(owner.billing, &root));
    MANAGED(xrt_managed_frame_enter(root, 4, &frame));
    int64_t count = 42;
    MANAGED(xrt_class_create(frame, 0, type.ops, &count));
    MANAGED(xrt_class_create(frame, 1, other.ops, &count));
    XrtManagedHeader *original = NULL, *wrong = NULL, *array = NULL;
    MANAGED(xrt_managed_slot_borrow(frame, 0, &original));
    MANAGED(xrt_managed_slot_borrow(frame, 1, &wrong));
    XrtManagedHeader *rows[] = {original, original};
    MANAGED(xrt_array_backing_inline_create(frame, 2, array_type.ops, rows, 2, 2));
    MANAGED(xrt_managed_slot_borrow(frame, 2, &array));
    MANAGED(xrt_array_backing_inline_create(frame, 3, other_array_type.ops, NULL, 0, 0));
    XrtManagedHeader *wrong_array = NULL;
    MANAGED(xrt_managed_slot_borrow(frame, 3, &wrong_array));
    OptionalArray some = {1,array}, none = {0}, copy = {0}, held = {0};
    MANAGED(xrt_value_copy_empty(optional, &none, &copy));
    MANAGED(xrt_value_copy_empty(optional, &some, &copy));
    MANAGED(xrt_value_move_empty(optional, &copy, &held));
    CHECK(held.tag == 1 && held.payload == array && !copy.tag && !copy.payload);
    OptionalArray forged[] = {{1,wrong_array},{1,NULL},{0,array},{2,array}};
    for (size_t i = 0; i < sizeof(forged)/sizeof(*forged); ++i) {
        CHECK(xrt_value_copy_empty(optional, &forged[i], &copy) == XRT_MANAGED_ARGUMENT);
        CHECK(xrt_value_move_empty(optional, &forged[i], &copy) == XRT_MANAGED_ARGUMENT);
        CHECK(!copy.tag && !copy.payload);
    }
    CHECK(xr_signed_rc_read(&array->references) == 2 && xr_signed_rc_read(&wrong_array->references) == 1);
    CHECK(xr_signed_rc_read(&original->references) == 3);
    size_t allocations = owner.audit.allocations;
    rows[1] = wrong;
    CHECK(xrt_array_backing_inline_create(frame, 2, array_type.ops, rows, 2, 2) == XRT_MANAGED_ARGUMENT);
    rows[1] = NULL;
    CHECK(xrt_array_backing_inline_create(frame, 2, array_type.ops, rows, 2, 2) == XRT_MANAGED_ARGUMENT);
    XrtManagedHeader *current = NULL;
    MANAGED(xrt_managed_slot_borrow(frame, 2, &current));
    CHECK(current == array && owner.audit.allocations == allocations &&
        xr_signed_rc_read(&original->references) == 3 && xr_signed_rc_read(&wrong->references) == 1);
    MANAGED(xrt_managed_slot_drop(frame, 0));
    const void *row = NULL;
    CHECK(xrt_array_backing_inline_at((const XrtArrayBacking *)array, other.plan, 1, &row) == XRT_MANAGED_ARGUMENT && !row);
    MANAGED(xrt_array_backing_inline_at((const XrtArrayBacking *)array, type.plan, 1, &row));
    XrtManagedHeader *kept = NULL;
    MANAGED(xrt_value_copy_empty(type.plan, row, &kept)); CHECK(kept == original);
    MANAGED(xrt_managed_slot_drop(frame, 2));
    CHECK(xr_signed_rc_read(&kept->references) == 3);
    row = NULL;
    MANAGED(xrt_array_backing_inline_at((const XrtArrayBacking *)held.payload, type.plan, 0, &row));
    CHECK(*(XrtManagedHeader *const *)row == kept);
    xrt_value_drop(optional, &held);
    CHECK(xr_signed_rc_read(&kept->references) == 1);
    const XrtValuePlan *field = NULL; const void *payload = NULL;
    MANAGED(xrt_class_field_borrow(kept, type.plan, 0, &field, &payload));
    CHECK(*(const int64_t *)payload == count);
    xrt_value_drop(type.plan, &kept);
    MANAGED(xrt_managed_root_destroy(&root));
    xrt_value_image_drop(&bound); owner_drop(&owner); CHECK(owner.audit.final == 1);
}
static void imported_nullable_array_elements_are_remapped(void) {
    const XrLirRecordField member = {"count",5,XR_LIR_I64,0,0};
    XrLirRecordIdentity identity = {.module="tests.lir.value_image",.module_length=sizeof("tests.lir.value_image")-1,
        .name="Row",.name_length=3};
    XrLirModule *source = NULL;
    XrLirType other, element, optional;
    LIR(xr_lir_module_create(NULL, &source));
    LIR(xr_lir_internal_named_class_type(source, &identity, &member, 1, XR_LIR_TYPE_INVALID, &element));
    LIR(xr_lir_require_inline_array_element(source, element));
    LIR(xr_lir_nullable_array_type(source, element, &optional));
    LIR(xr_lir_array_catalog_install(source));
    LIR(xr_lir_module_finish(source, NULL));
    for (unsigned selective = 0; selective < 2; ++selective) {
        XrLirModule *target = NULL;
        XrLirType received = optional;
        LIR(xr_lir_module_create(NULL, &target));
        if (selective) {
            identity.name = "Alt";
            LIR(xr_lir_internal_named_class_type(target, &identity, &member, 1, XR_LIR_TYPE_INVALID, &other));
            LIR(xr_lir_require_inline_array_element(target, other));
            LIR(xr_lir_import_type(target, source, optional, &received));
            CHECK(received != optional && xr_lir_nullable_array_element(target, received) != other);
        } else {
            LIR(xr_lir_copy_types(target, source));
        }
        LIR(xr_lir_array_catalog_install(target));
        LIR(xr_lir_module_finish(target, NULL));
        XrLirType received_element = xr_lir_nullable_array_element(target, received);
        XrLirType payload;
        XrLirRecordIdentity received_identity;
        CHECK(xr_lir_class_payload(target, received_element, &payload));
        CHECK(xr_lir_record_identity(target, payload, &received_identity) == XR_LIR_OK &&
            received_identity.name_length == 3 && !memcmp(received_identity.name, "Row", 3));
        CHECK(xr_lir_inline_array_ordinal(target, received_element) == selective + 1);
        xr_lir_module_destroy(target);
    }
    xr_lir_module_destroy(source);
}
static void class_optional_i64_fields_keep_their_inline_layout(void) {
    typedef struct OptionalI64 { uint8_t tag; int64_t payload; } OptionalI64;
    typedef struct Row { OptionalI64 uid; int64_t size; OptionalI64 gid; } Row;
    const XrLirRecordIdentity identity = {.module="tests.lir.value_image",.module_length=sizeof("tests.lir.value_image")-1,
        .name="Metadata",.name_length=sizeof("Metadata")-1};
    XrLirModule *module = NULL;
    XrLirType optional, class_type, derived_type;
    LIR(xr_lir_module_create(NULL, &module));
    LIR(xr_lir_nullable_type(module, XR_LIR_I64, &optional));
    const XrLirRecordField fields[] = {{"uid",3,optional,XR_LIR_RECORD_MUTABLE,0},
        {"size",4,XR_LIR_I64,0,0},{"gid",3,optional,0,0}};
    LIR(xr_lir_internal_named_class_type(module, &identity, fields, 3, XR_LIR_TYPE_INVALID, &class_type));
    XrLirRecordIdentity derived = identity;
    derived.name = "DerivedMetadata";
    derived.name_length = sizeof("DerivedMetadata") - 1;
    LIR(xr_lir_internal_named_class_type(module, &derived, fields, 3, class_type, &derived_type));
    LIR(xr_lir_module_finish(module, NULL));
    for (unsigned selective = 0; selective < 2; ++selective) {
        XrLirModule *receiving = NULL;
        XrLirType received = derived_type, row;
        LIR(xr_lir_module_create(NULL, &receiving));
        if (selective) {
            XrLirType extra;
            LIR(xr_lir_nullable_type(receiving, XR_LIR_I1, &extra));
            LIR(xr_lir_import_type(receiving, module, derived_type, &received));
            CHECK(received != derived_type);
        } else {
            LIR(xr_lir_copy_types(receiving, module));
        }
        CHECK(xr_lir_class_payload(receiving, xr_lir_class_parent(receiving, received), &row));
        CHECK(!xr_lir_internal_type_valid(receiving, row, true) && !xr_lir_inline_aggregate_type(receiving, row));
        CHECK(xr_lir_internal_types_valid(receiving));
        if (selective) {
            LIR(xr_lir_require_inline_array_element(receiving, received));
            CHECK(xr_lir_internal_array_element(receiving, received));
            CHECK(xr_lir_require_inline_array_element(receiving, row) == XR_LIR_UNSUPPORTED);
        } else {
            const XrLirRecordField tuple_member = {.type=row};
            XrLirType refused = XR_LIR_I64;
            CHECK(xr_lir_tuple_type(receiving, &tuple_member, 1, &refused) == XR_LIR_INVALID_ARGUMENT &&
                refused == XR_LIR_I64);
        }
        xr_lir_module_destroy(receiving);
    }

    Owner owner; owner_new(&owner, SIZE_MAX);
    XrLirValueLayouts *layouts = NULL;
    XrLirValueImage *image = NULL;
    const XrLirValueLayoutKey key = {class_type,0,0};
    LAYOUT(xr_lir_value_layouts_create(module, &key, 1, owner.billing, &layouts));
    LAYOUT(xr_lir_value_image_create(layouts, owner.billing, &image));
    const XrtValuePlan *class_plan = NULL;
    uint32_t id;
    LAYOUT(xr_lir_value_layouts_lookup(layouts, key, &class_plan, &id));
    const XrtValueImage *view = xr_lir_value_image_view(image);
    const XrtValueImageType *payload = &view->types[view->types[id - 1].element - 1];
    CHECK(payload->kind == XRT_VALUE_RECORD && payload->size == sizeof(Row) &&
        !payload->owned_count && payload->field_count == 3 && view->slot_count == 3);
    const XrtValueImageSlot *slots = view->slots;
    CHECK(slots[0].offset == offsetof(Row,uid) && slots[1].offset == offsetof(Row,size) &&
        slots[2].offset == offsetof(Row,gid) && slots[0].type == slots[2].type &&
        slots[0].flags == XRT_VALUE_FIELD_MUTABLE && !slots[2].flags);
    const XrtValueImageType *member = &view->types[slots[0].type - 1];
    CHECK(member->kind == XRT_VALUE_NULLABLE && member->size == sizeof(OptionalI64) &&
        member->nullable_tag == offsetof(OptionalI64,tag) && member->nullable_payload == offsetof(OptionalI64,payload) &&
        !member->owned_count && view->types[member->nullable_element - 1].kind == XRT_VALUE_I64);

    XrtValueImageSlot forged_slots[3];
    memcpy(forged_slots, slots, sizeof(forged_slots));
    forged_slots[2].offset = forged_slots[1].offset;
    XrtValueImage forged = *view;
    forged.slots = forged_slots;
    XrtValueImagePlans *bound = NULL;
    CHECK(xrt_value_image_bind(owner.billing, &forged, (XrtManagedLease){0}, &bound) == XRT_MANAGED_ARGUMENT && !bound);
    MANAGED(xrt_value_image_bind(owner.billing, view, (XrtManagedLease){0}, &bound));
    xr_lir_value_image_destroy(image); xr_lir_value_layouts_destroy(layouts); xr_lir_module_destroy(module);

    XrtBoundClass type = {0};
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    MANAGED(xrt_value_image_class(bound, id, &type));
    MANAGED(xrt_managed_root_new(owner.billing, &root));
    MANAGED(xrt_managed_frame_enter(root, 1, &frame));
    Row row = {{1,0},7,{0,0}};
    MANAGED(xrt_class_create(frame, 0, type.ops, &row));
    XrtManagedHeader *original = NULL, *current = NULL;
    MANAGED(xrt_managed_slot_borrow(frame, 0, &original));
    size_t allocations = owner.audit.allocations;
    XrtValuePlanSpec class_spec;
    MANAGED(xrt_value_plan_view(type.plan, &class_spec));
    const XrtValueSlotSpec nested = {.type=class_spec.element,.name="nested",.name_length=6};
    const XrtValuePlanSpec nested_row = {.kind=XRT_VALUE_RECORD,.size=sizeof(Row),.alignment=_Alignof(Row),
        .fields=&nested,.field_count=1};
    XrtValuePlan *rejected = NULL;
    CHECK(xrt_value_plan_new(owner.billing, &nested_row, &rejected) == XRT_MANAGED_ARGUMENT && !rejected);
    row.uid.tag = 2;
    CHECK(xrt_class_create(frame, 0, type.ops, &row) == XRT_MANAGED_ARGUMENT);
    MANAGED(xrt_managed_slot_borrow(frame, 0, &current));
    CHECK(current == original && owner.audit.allocations == allocations);

    const XrtValuePlan *uid = NULL, *gid = NULL;
    const void *uid_bytes = NULL, *gid_bytes = NULL;
    MANAGED(xrt_class_field_borrow(current, type.plan, 0, &uid, &uid_bytes));
    MANAGED(xrt_class_field_borrow(current, type.plan, 2, &gid, &gid_bytes));
    OptionalI64 some = {0}, none = {0};
    CHECK(uid == gid);
    MANAGED(xrt_value_copy_empty(uid, uid_bytes, &some));
    MANAGED(xrt_value_copy_empty(gid, gid_bytes, &none));
    OptionalI64 replacement = {1,-29};
    MANAGED(xrt_class_scalar_field_write(current, type.plan, 0, &replacement, sizeof(replacement)));
    CHECK(xrt_class_scalar_field_write(current, type.plan, 2, &replacement, sizeof(replacement)) == XRT_MANAGED_ARGUMENT);
    replacement.tag = 2;
    CHECK(xrt_class_scalar_field_write(current, type.plan, 0, &replacement, sizeof(replacement)) == XRT_MANAGED_ARGUMENT);
    CHECK(((const OptionalI64 *)uid_bytes)->tag == 1 && ((const OptionalI64 *)uid_bytes)->payload == -29);
    MANAGED(xrt_class_scalar_field_write(current, type.plan, 0, uid_bytes, sizeof(replacement)));
    CHECK(((const OptionalI64 *)uid_bytes)->tag == 1 && ((const OptionalI64 *)uid_bytes)->payload == -29);
    memset(&replacement, 0xff, sizeof(replacement));
    replacement.tag = 0;
    MANAGED(xrt_class_scalar_field_write(current, type.plan, 0, &replacement, sizeof(replacement)));
    const unsigned char zero[sizeof(OptionalI64)] = {0};
    CHECK(!memcmp(uid_bytes, zero, sizeof(zero)));
    CHECK(((const OptionalI64 *)gid_bytes)->tag == 0);
    CHECK(owner.audit.allocations == allocations);
    MANAGED(xrt_managed_root_destroy(&root));
    CHECK(some.tag == 1 && some.payload == 0 && none.tag == 0);
    xrt_value_image_drop(&bound); owner_drop(&owner); CHECK(owner.audit.final == 1);
}
int main(void) {
    class_arrays_keep_their_exact_backing();
    class_array_update_failure_preserves_the_field(false);
    class_array_update_failure_preserves_the_field(true);
    derived_classes_keep_concrete_identity();
    nested_classes_keep_exact_owner_plans();
    nested_class_owners_are_checked_before_publication();
    nullable_arrays_keep_their_exact_backing();
    class_array_elements_keep_their_exact_backing();
    imported_nullable_array_elements_are_remapped();
    class_optional_i64_fields_keep_their_inline_layout();
    copied_names_and_real_values(); failures_and_empty(); deep_single_inventory(); rejects_forged_display_closure();
    class_values_follow_their_exact_owner();
    puts("raw value images: billed ownership, copied names, exact ids, creator retirement and real inline values passed");
    return 0;
}
