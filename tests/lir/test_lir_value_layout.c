/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_value_layout.c - Natural C layouts and independent metered receiving plans
 */
#include "lir/xlir_value_layout.h"
#include "lir/xlir_internal.h"
#include "lir/xlir_builtin_admission.h"
#include "runtime/native/xnative_string_backing.h"
#include "xray_native_function.h"
#include "xray_native_array_slice.h"
#include "base/xmalloc.h"
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define LIR(c) CHECK((c) == XR_LIR_OK)
#define MANAGED(c) CHECK((c) == XRT_MANAGED_OK)
#define LAYOUT(c) CHECK((c) == XR_LIR_VALUE_LAYOUT_OK)
typedef struct Audit { _Atomic(size_t) leases, bytes, blocks, final; } Audit;
typedef struct Owner { Audit audit; XrtBillingOwner *billing; } Owner;
static bool provider_retain(void *context) { atomic_fetch_add(&((Audit *)context)->leases, 1); return true; }
static void provider_drop(void *context) {
    Audit *audit = context;
    size_t previous = atomic_fetch_sub(&audit->leases, 1); CHECK(previous);
    if (previous == 1) { CHECK(!atomic_load(&audit->bytes) && !atomic_load(&audit->blocks)); atomic_fetch_add(&audit->final, 1); }
}
static void *provider_allocate(void *context, size_t bytes) {
    Audit *audit = context;
    void *memory = xr_malloc(bytes);
    if (memory) { atomic_fetch_add(&audit->bytes, bytes); atomic_fetch_add(&audit->blocks, 1); }
    return memory;
}
static void provider_free(void *context, void *memory, size_t bytes) {
    Audit *audit = context;
    CHECK(atomic_fetch_sub(&audit->bytes, bytes) >= bytes && atomic_fetch_sub(&audit->blocks, 1));
    xr_free(memory);
}
static void owner_new(Owner *owner, size_t limit) {
    *owner = (Owner){0}; atomic_init(&owner->audit.leases, 1);
    XrtManagedAllocator provider = {&owner->audit, provider_allocate, provider_free,
        {&owner->audit, provider_retain, provider_drop}};
    MANAGED(xrt_billing_owner_new(&provider, limit, &owner->billing));
}
static void owner_drop(Owner *owner) { xrt_billing_owner_drop(owner->billing); provider_drop(&owner->audit); }
typedef struct Types {
    XrLirModule *module;
    XrLirType inner, optional_inner, outer, optional_outer, optional_string, nested_string;
    XrLirType fixed, tuple, optional_fixed, optional_tuple, optional_i64, optional_unit;
    XrLirType function, cell, empty, optional_empty, pod, optional_pod;
} Types;
static Types types_new(void) {
    Types t = {0};
    LIR(xr_lir_module_create(NULL, &t.module));
    LIR(xr_lir_nullable_type(t.module, XR_LIR_STRING, &t.optional_string));
    LIR(xr_lir_nullable_type(t.module, t.optional_string, &t.nested_string));
    char first_name[] = "a", second_name[] = "b";
    XrLirRecordField inner[] = {{first_name, 1, XR_LIR_I1, 0}, {second_name, 1, XR_LIR_STRING, 0}};
    LIR(xr_lir_record_type(t.module,
        &(XrLirRecordIdentity){.module="tests.lir.value_layout", .module_length=sizeof("tests.lir.value_layout")-1,
            .name="Inner", .name_length=sizeof("Inner")-1},
        inner, 2, &t.inner));
    first_name[0] = second_name[0] = 'x';
    LIR(xr_lir_nullable_type(t.module, t.inner, &t.optional_inner));
    XrLirRecordField outer[] = {{"first", 5, t.inner, 0}, {"maybe", 5, t.optional_inner, 0}, {"tail", 4, XR_LIR_I64, 0}};
    LIR(xr_lir_record_type(t.module,
        &(XrLirRecordIdentity){.module="tests.lir.value_layout", .module_length=sizeof("tests.lir.value_layout")-1,
            .name="Outer", .name_length=sizeof("Outer")-1},
        outer, 3, &t.outer));
    LIR(xr_lir_nullable_type(t.module, t.outer, &t.optional_outer));
    XrLirRecordField pod[] = {{"a", 1, XR_LIR_I1, 0}, {"b", 1, XR_LIR_F64, 0}, {"c", 1, XR_LIR_I64, 0}};
    LIR(xr_lir_record_type(t.module,
        &(XrLirRecordIdentity){.module="tests.lir.value_layout", .module_length=sizeof("tests.lir.value_layout")-1,
            .name="Pod", .name_length=sizeof("Pod")-1},
        pod, 3, &t.pod));
    LIR(xr_lir_nullable_type(t.module, t.pod, &t.optional_pod));
    LIR(xr_lir_fixed_array_type(t.module, XR_LIR_U8, 17, &t.fixed));
    LIR(xr_lir_tuple_i64_type(t.module, 3, &t.tuple));
    LIR(xr_lir_nullable_type(t.module, t.fixed, &t.optional_fixed));
    LIR(xr_lir_nullable_type(t.module, t.tuple, &t.optional_tuple));
    LIR(xr_lir_nullable_type(t.module, XR_LIR_I64, &t.optional_i64));
    LIR(xr_lir_nullable_type(t.module, XR_LIR_UNIT, &t.optional_unit));
    XrLirCallableType signature = {.result=XR_LIR_UNIT};
    LIR(xr_lir_callable_type(t.module, &signature, &t.function));
    LIR(xr_lir_cell_type(t.module, (XrLirCellType){XR_LIR_I64, 0}, &t.cell));
    LIR(xr_lir_record_type(t.module,
        &(XrLirRecordIdentity){.module="tests.lir.value_layout", .module_length=sizeof("tests.lir.value_layout")-1,
            .name="Empty", .name_length=sizeof("Empty")-1},
        NULL, 0, &t.empty));
    LIR(xr_lir_nullable_type(t.module, t.empty, &t.optional_empty));
    LIR(xr_lir_module_finish(t.module, NULL));
    return t;
}
typedef struct Inner { bool a; XrtManagedHeader *b; } Inner;
typedef struct OptionalInner { uint8_t tag; Inner payload; } OptionalInner;
typedef struct Outer { Inner first; OptionalInner maybe; int64_t tail; } Outer;
typedef struct OptionalOuter { uint8_t tag; Outer payload; } OptionalOuter;
typedef struct OptionalString { uint8_t tag; XrtManagedHeader *payload; } OptionalString;
typedef struct OptionalFixed { uint8_t tag; uint8_t payload[17]; } OptionalFixed;
typedef struct OptionalTuple { uint8_t tag; int64_t payload[3]; } OptionalTuple;
typedef struct Pod { bool a; double b; int64_t c; } Pod;
typedef struct OptionalPod { uint8_t tag; Pod payload; } OptionalPod;
typedef struct Empty { uint8_t storage; } Empty;
typedef struct OptionalEmpty { uint8_t tag; Empty payload; } OptionalEmpty;
static const XrtValuePlan *get(const XrLirValueLayouts *set, XrLirType type) {
    const XrtValuePlan *plan = NULL;
    LAYOUT(xr_lir_value_layouts_get(set, (XrLirValueLayoutKey){type, 0,0}, &plan));
    return plan;
}
static XrtValuePlanSpec view(const XrtValuePlan *plan) {
    XrtValuePlanSpec spec;
    MANAGED(xrt_value_plan_view(plan, &spec));
    return spec;
}
static void natural_layouts(void) {
    Types t = types_new(); Owner owner; owner_new(&owner, SIZE_MAX);
    XrLirValueLayoutKey roots[] = {{t.optional_outer, 0,0}, {t.optional_string, 0,0}, {t.nested_string, 0,0},
        {t.optional_fixed, 0,0}, {t.optional_tuple, 0,0}, {t.optional_i64, 0,0}, {t.optional_unit, 0,0}, {t.optional_pod, 0,0}, {t.optional_empty, 0,0},
        {XR_LIR_ARRAY, XR_LIR_I64,0}, {XR_LIR_ARRAY, XR_LIR_STRING,0}, {XR_LIR_ARRAY, XR_LIR_U8,0}, {XR_LIR_ARRAY, XR_LIR_U64,0},
        {XR_LIR_MAP_STRING_I64, 0,0}, {XR_LIR_I8, 0,0}, {XR_LIR_I16, 0,0}, {XR_LIR_I32, 0,0},
        {XR_LIR_U8, 0,0}, {XR_LIR_U16, 0,0}, {XR_LIR_U32, 0,0}, {XR_LIR_U64, 0,0}, {XR_LIR_F32, 0,0}, {XR_LIR_F64, 0,0}};
    XrLirValueLayouts *set = NULL;
    LAYOUT(xr_lir_value_layouts_create(t.module, roots, sizeof(roots) / sizeof(*roots), owner.billing, &set));
    XrtValuePlanSpec inner = view(get(set, t.inner)), outer = view(get(set, t.outer));
    CHECK(inner.kind == XRT_VALUE_RECORD && inner.size == sizeof(Inner) && inner.alignment == alignof(Inner));
    CHECK(inner.fields[0].offset == offsetof(Inner, a) && inner.fields[1].offset == offsetof(Inner, b));
    CHECK(inner.fields[0].name_length == 1 && inner.fields[0].name[0] == 'a' && inner.fields[1].name[0] == 'b');
    CHECK(inner.owned_count == 1 && inner.owned_offsets[0] == offsetof(Inner, b));
    CHECK(outer.size == sizeof(Outer) && outer.alignment == alignof(Outer) && outer.field_count == 3);
    CHECK(outer.fields[1].offset == offsetof(Outer, maybe) && outer.fields[2].offset == offsetof(Outer, tail));
    CHECK(outer.owned_count == 2 && outer.owned_offsets[0] == offsetof(Outer, first) + offsetof(Inner, b));
    CHECK(outer.owned_offsets[1] == offsetof(Outer, maybe) + offsetof(OptionalInner, payload) + offsetof(Inner, b));
    XrtValuePlanSpec optional = view(get(set, t.optional_outer));
    CHECK(optional.size == sizeof(OptionalOuter) && optional.nullable.payload_offset == offsetof(OptionalOuter, payload));
    CHECK(optional.owned_offsets[1] == offsetof(OptionalOuter, payload) + outer.owned_offsets[1]);
    optional = view(get(set, t.optional_string));
    CHECK(optional.size == sizeof(OptionalString) && optional.nullable.payload_offset == offsetof(OptionalString, payload));
    CHECK(optional.owned_offsets[0] == offsetof(OptionalString, payload));
    optional = view(get(set, t.optional_fixed)); CHECK(optional.size == sizeof(OptionalFixed) && optional.nullable.payload_offset == 1);
    optional = view(get(set, t.optional_tuple)); CHECK(optional.size == sizeof(OptionalTuple) && optional.nullable.payload_offset == 8);
    optional = view(get(set, t.optional_unit)); CHECK(optional.size == 1 && optional.alignment == 1 && !optional.owned_count);
    XrtValuePlanSpec pod = view(get(set, t.pod)); CHECK(pod.size == sizeof(Pod) && pod.alignment == alignof(Pod) && !pod.owned_count);
    CHECK(pod.fields[1].offset == offsetof(Pod, b) && pod.fields[2].offset == offsetof(Pod, c));
    optional = view(get(set, t.optional_pod));
    CHECK(optional.size == sizeof(OptionalPod) && optional.nullable.payload_offset == offsetof(OptionalPod, payload) && !optional.owned_count);
    CHECK(view(get(set, t.fixed)).count == 17);
    XrtValuePlanSpec tuple = view(get(set, t.tuple));
    CHECK(tuple.kind == XRT_VALUE_TUPLE && tuple.field_count == 3 && !tuple.owned_count);
    for (size_t i = 0; i < tuple.field_count; ++i)
        CHECK(!tuple.fields[i].name && !tuple.fields[i].name_length && tuple.fields[i].offset == i * sizeof(int64_t));
    XrtValuePlanSpec empty = view(get(set, t.empty));
    CHECK(empty.size == sizeof(Empty) && empty.alignment == alignof(Empty) && !empty.field_count && !empty.owned_count);
    optional = view(get(set, t.optional_empty));
    CHECK(optional.size == sizeof(OptionalEmpty) && optional.nullable.payload_offset == offsetof(OptionalEmpty, payload) && !optional.owned_count);
    const XrtValuePlan *seen[128]; size_t count = xr_lir_value_layouts_count(set);
    CHECK(count > 20 && count < 128);
    for (size_t i = 0; i < count; ++i) {
        XrLirValueLayoutKey key; const XrtValuePlan *plan;
        LAYOUT(xr_lir_value_layouts_at(set, i, &key, &plan)); seen[i] = plan;
        XrtValuePlanSpec spec = view(plan);
        for (size_t f = 0; f < spec.field_count + (spec.nullable.element != NULL); ++f) {
            const XrtValuePlan *child = f < spec.field_count ? spec.fields[f].type : spec.nullable.element;
            size_t before = 0; while (before < i && seen[before] != child) ++before;
            CHECK(before < i);
        }
        if (key.type == XR_LIR_ARRAY) {
            XrtValueKind expected = key.array_element == XR_LIR_I64 ? XRT_VALUE_ARRAY_I64 : key.array_element == XR_LIR_STRING ?
                XRT_VALUE_ARRAY_STRING : key.array_element == XR_LIR_U8 ? XRT_VALUE_ARRAY_U8 : XRT_VALUE_ARRAY_U64;
            CHECK(spec.size == sizeof(XrtManagedHeader *) && spec.owned_count == 1 && spec.kind == expected);
        }
    }
    const XrLirOwnershipTypeFact *facts = NULL; uint32_t fact_count = 0;
    LAYOUT(xr_lir_value_layouts_ownership(set, &facts, &fact_count)); CHECK(facts && fact_count > 6);
    bool pod_fact = false, record_fact = false, tuple_fact = false;
    for (uint32_t i = 0; i < fact_count; ++i) {
        XrtValuePlanSpec spec = view(get(set, facts[i].type));
        CHECK((!i || facts[i - 1].type < facts[i].type) &&
            (xrt_value_kind_has_fields(spec.kind) || spec.kind == XRT_VALUE_NULLABLE) &&
            facts[i].has_owned == (spec.owned_count != 0));
        if (facts[i].type == t.optional_pod) pod_fact = !facts[i].has_owned;
        if (facts[i].type == t.optional_outer) record_fact = facts[i].has_owned;
        if (facts[i].type == t.tuple) tuple_fact = !facts[i].has_owned;
    }
    CHECK(pod_fact && record_fact && tuple_fact);
    XrLirTypeLayout old;
    LIR(xr_lir_type_layout(t.module, t.outer, &old)); CHECK(old.size == 8 && outer.size != old.size);
    xr_lir_module_destroy(t.module); xr_lir_value_layouts_destroy(set); owner_drop(&owner);
    CHECK(atomic_load(&owner.audit.final) == 1);
}
static void creator_retirement(void) {
    Types t = types_new(); Owner owner; owner_new(&owner, SIZE_MAX);
    XrLirValueLayoutKey key = {t.outer, 0,0}; XrLirValueLayouts *set = NULL;
    LAYOUT(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set));
    XrtValuePlan *plan = (XrtValuePlan *)get(set, t.outer); CHECK(xrt_value_plan_retain(plan));
    XrtBackingTypes types = {0}; XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    MANAGED(xrt_backing_types_init(owner.billing, &types)); MANAGED(xrt_managed_root_new(owner.billing, &root));
    MANAGED(xrt_managed_frame_enter(root, 1, &frame));
    MANAGED(xrt_string_backing_create(frame, 0, &types, "live", 4, 0));
    XrtManagedHeader *header = NULL; MANAGED(xrt_managed_slot_borrow(frame, 0, &header));
    CHECK(xrt_managed_retain(header) && xrt_managed_retain(header));
    Outer source = {.first={true, header}, .maybe={1, {false, header}}, .tail=42};
    xr_lir_module_destroy(t.module); xr_lir_value_layouts_destroy(set);
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&types); owner_drop(&owner);
    CHECK(!atomic_load(&owner.audit.final) && xr_signed_rc_read(&header->references) == 2);
    XrtValuePlanSpec spec = view(plan); CHECK(spec.fields[0].name_length == 5 && !memcmp(spec.fields[0].name, "first", 5));
    Outer destination;
    MANAGED(xrt_value_zero(plan, &destination));
    MANAGED(xrt_value_copy_empty(plan, &source, &destination)); CHECK(xr_signed_rc_read(&header->references) == 4);
    xrt_value_drop(plan, &source); CHECK(xr_signed_rc_read(&header->references) == 2);
    CHECK(destination.tail == 42 && ((const XrtStringBacking *)destination.maybe.payload.b)->length == 4 &&
        !memcmp(((const XrtStringBacking *)destination.first.b)->bytes, "live", 4));
    xrt_value_drop(plan, &destination); xrt_value_plan_drop(plan);
    CHECK(atomic_load(&owner.audit.final) == 1);
}
static void failures_and_boundaries(void) {
    Types t = types_new(); Owner owner; owner_new(&owner, SIZE_MAX);
    XrtBillingStats baseline = xrt_billing_owner_stats(owner.billing);
    XrLirValueLayoutKey key = {t.optional_outer, 0,0}; XrLirValueLayouts *set = NULL;
    LAYOUT(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set));
    size_t allocations = xrt_billing_owner_stats(owner.billing).allocations - baseline.allocations;
    xr_lir_value_layouts_destroy(set); set = NULL;
    for (size_t allowed = 0; allowed < allocations; ++allowed) {
        xrt_billing_owner_fail_after(owner.billing, allowed);
        CHECK(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_OOM && !set);
        XrtBillingStats after = xrt_billing_owner_stats(owner.billing);
        CHECK(after.bytes == baseline.bytes && after.blocks == baseline.blocks);
    }
    xrt_billing_owner_fail_after(owner.billing, SIZE_MAX);
    const XrLirType unsupported[] = {XR_LIR_STATE, XR_LIR_STATE_REF, XR_LIR_PTR, XR_LIR_FUNCTION_SCOPE};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(*unsupported); ++i) {
        key.type = unsupported[i];
        CHECK(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_UNSUPPORTED && !set);
    }
    key = (XrLirValueLayoutKey){XR_LIR_ARRAY, XR_LIR_I32,0};
    CHECK(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_UNSUPPORTED && !set);
    key = (XrLirValueLayoutKey){XR_LIR_I64, XR_LIR_I64,0};
    CHECK(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_ARGUMENT && !set);
    key = (XrLirValueLayoutKey){t.optional_inner, 0,0};
    XrLirTypeData *row = &t.module->types[t.optional_inner - XR_LIR_TYPE_COUNT];
    XrLirType saved = row->shape.element; row->shape.element = t.optional_inner;
    CHECK(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_ARGUMENT && !set);
    row->shape.element = saved;
    key = (XrLirValueLayoutKey){XR_LIR_I64, 0,0};
    LAYOUT(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set));
    const XrtValuePlan *unchanged = get(set, XR_LIR_I64); XrLirValueLayoutKey unchanged_key = key;
    const XrLirOwnershipTypeFact *facts = NULL; uint32_t fact_count = UINT32_MAX;
    LAYOUT(xr_lir_value_layouts_ownership(set, &facts, &fact_count)); CHECK(!facts && !fact_count);
    CHECK(xr_lir_value_layouts_at(set, SIZE_MAX, &key, &unchanged) == XR_LIR_VALUE_LAYOUT_ARGUMENT);
    CHECK(key.type == unchanged_key.type && unchanged == get(set, XR_LIR_I64));
    xr_lir_value_layouts_destroy(set); set = NULL;
    xrt_billing_owner_close(owner.billing);
    CHECK(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_CLOSED && !set);
    owner_drop(&owner); CHECK(atomic_load(&owner.audit.final) == 1);
    owner_new(&owner, 1024);
    CHECK(xr_lir_value_layouts_create(t.module, &key, 1, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_LIMIT && !set);
    owner_drop(&owner); CHECK(atomic_load(&owner.audit.final) == 1);
    xr_lir_module_destroy(t.module);
    printf("Value layout allocation rollback sites: %zu\n", allocations);
}
static void inline_overflow(void) {
    XrLirModule *module = NULL; LIR(xr_lir_module_create(NULL, &module));
    XrLirType type = XR_LIR_I64;
    for (unsigned i = 0; i < 64; ++i) {
        char name[32];
        int length = snprintf(name, sizeof(name), "OverflowLevel%u", i);
        CHECK(length > 0 && (size_t)length < sizeof(name));
        XrLirRecordField fields[] = {{"a", 1, type, 0}, {"b", 1, type, 0}};
        LIR(xr_lir_record_type(module,
            &(XrLirRecordIdentity){.module="tests.lir.value_layout",
                .module_length=sizeof("tests.lir.value_layout")-1, .name=name, .name_length=(uint32_t)length},
            fields, 2, &type));
    }
    LIR(xr_lir_module_finish(module, NULL));
    Owner owner; owner_new(&owner, SIZE_MAX);
    XrLirValueLayoutKey key = {type, 0,0}; XrLirValueLayouts *set = NULL;
    CHECK(xr_lir_value_layouts_create(module, &key, 1, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_OVERFLOW && !set);
    owner_drop(&owner); CHECK(atomic_load(&owner.audit.final) == 1); xr_lir_module_destroy(module);
}
static void callable_layouts(void) {
    XrLirModule *module = NULL; LIR(xr_lir_module_create(NULL, &module));
    XrLirType callback = 0, c_callback = 0, record = 0, optional = 0, outer = 0, array_result = 0, unit = 0, cell_ref = 0;
    XrLirCallableParameter input = {.type=XR_LIR_I64, .mode=XR_PARAM_READ};
    XrLirCallableType signature = {&input, 1, XR_LIR_I64, 0, XR_LIR_CALLABLE_NO_SUSPEND, false};
    LIR(xr_lir_callable_type(module, &signature, &callback));
    signature.flags |= XR_LIR_CALLABLE_C_ABI;
    LIR(xr_lir_callable_type(module, &signature, &c_callback));
    XrLirRecordField fields[] = {{"name",4,XR_LIR_STRING, 0}, {"score",5,XR_LIR_I64, 0}};
    LIR(xr_lir_record_type(module,
        &(XrLirRecordIdentity){.module="tests.lir.value_layout", .module_length=sizeof("tests.lir.value_layout")-1,
            .name="CallableRecord", .name_length=sizeof("CallableRecord")-1},
        fields, 2, &record));
    LIR(xr_lir_nullable_type(module, record, &optional));
    XrLirCallableParameter parameters[] = {
        {callback,0,XR_PARAM_READ,true}, {c_callback,0,XR_PARAM_READ,false},
        {XR_LIR_ARRAY,XR_LIR_STRING,XR_PARAM_MOVE,false}, {XR_LIR_ARRAY,XR_LIR_I64,XR_PARAM_READ,false},
        {XR_LIR_ARRAY,XR_LIR_U8,XR_PARAM_READ,false}, {XR_LIR_ARRAY,XR_LIR_U64,XR_PARAM_READ,false},
        {record,0,XR_PARAM_READ,false}, {optional,0,XR_PARAM_READ,false}};
    signature = (XrLirCallableType){parameters,8,callback,0,0,false};
    LIR(xr_lir_callable_type(module, &signature, &outer));
    signature = (XrLirCallableType){.result=XR_LIR_ARRAY,.result_element=XR_LIR_STRING};
    LIR(xr_lir_callable_type(module, &signature, &array_result));
    signature = (XrLirCallableType){.result=XR_LIR_UNIT};
    LIR(xr_lir_callable_type(module, &signature, &unit));
    input = (XrLirCallableParameter){.type=XR_LIR_I64_CELL,.mode=XR_PARAM_REF};
    signature = (XrLirCallableType){.parameters=&input,.parameter_count=1,.result=XR_LIR_UNIT};
    LIR(xr_lir_callable_type(module, &signature, &cell_ref));
    memset(parameters, 0xa5, sizeof(parameters)); memset(&input, 0xa5, sizeof(input));
    LIR(xr_lir_module_finish(module, NULL));
    Owner owner; owner_new(&owner, SIZE_MAX);
    XrLirValueLayoutKey roots[] = {{outer,0,0},{array_result,0,0},{unit,0,0}};
    XrLirValueLayouts *set = NULL;
    XrtBillingStats baseline = xrt_billing_owner_stats(owner.billing);
    LAYOUT(xr_lir_value_layouts_create(module, roots, 3, owner.billing, &set));
    size_t allocations = xrt_billing_owner_stats(owner.billing).allocations - baseline.allocations;
    const XrtValuePlan *plan = get(set, outer); XrtValuePlanSpec spec = view(plan);
    CHECK(spec.kind == XRT_VALUE_FUNCTION && spec.size == sizeof(XrtFunction) && spec.alignment == alignof(XrtFunction));
    CHECK(spec.owned_count == 1 && spec.owned_offsets[0] == offsetof(XrtFunction, owner));
    CHECK(spec.signature.parameter_count == 8 && !spec.signature.flags && !spec.signature.throws);
    CHECK(spec.signature.result == get(set, callback));
    CHECK(spec.signature.parameters[0].type == get(set, callback) && spec.signature.parameters[0].escaping);
    CHECK(spec.signature.parameters[1].type == get(set, c_callback) && !spec.signature.parameters[1].escaping);
    CHECK(spec.signature.parameters[2].mode == XR_PARAM_MOVE && spec.signature.parameters[3].mode == XR_PARAM_READ);
    for (size_t i = 0; i < spec.signature.parameter_count; ++i)
        CHECK(!spec.signature.parameters[i].name && !spec.signature.parameters[i].name_length);
    const XrtValueKind arrays[] = {XRT_VALUE_ARRAY_STRING,XRT_VALUE_ARRAY_I64,XRT_VALUE_ARRAY_U8,XRT_VALUE_ARRAY_U64};
    for (size_t i = 0; i < 4; ++i) CHECK(view(spec.signature.parameters[i + 2].type).kind == arrays[i]);
    CHECK(view(spec.signature.parameters[6].type).kind == XRT_VALUE_RECORD &&
        view(spec.signature.parameters[7].type).kind == XRT_VALUE_NULLABLE);
    CHECK(view(get(set, callback)).signature.flags == XRT_VALUE_NO_SUSPEND);
    CHECK(view(get(set, c_callback)).signature.flags == (XRT_VALUE_NO_SUSPEND | XRT_VALUE_C_ABI));
    CHECK(view(view(get(set, array_result)).signature.result).kind == XRT_VALUE_ARRAY_STRING);
    XrtValuePlanSpec empty = view(get(set, unit));
    CHECK(!empty.signature.parameter_count && !empty.signature.parameters && view(empty.signature.result).kind == XRT_VALUE_UNIT);
    const XrtValuePlan *seen[32]; size_t count = xr_lir_value_layouts_count(set); CHECK(count <= 32);
    for (size_t i = 0; i < count; ++i) {
        XrLirValueLayoutKey key; const XrtValuePlan *current;
        LAYOUT(xr_lir_value_layouts_at(set, i, &key, &current)); seen[i] = current;
        XrtValuePlanSpec row = view(current);
        if (row.kind != XRT_VALUE_FUNCTION) continue;
        for (size_t p = 0; p <= row.signature.parameter_count; ++p) {
            const XrtValuePlan *child = p == row.signature.parameter_count ? row.signature.result : row.signature.parameters[p].type;
            size_t before = 0; while (before < i && seen[before] != child) ++before;
            CHECK(before < i);
        }
    }
    CHECK(xrt_value_plan_retain((XrtValuePlan *)plan));
    xr_lir_value_layouts_destroy(set); set = NULL;
    XrtBillingStats held = xrt_billing_owner_stats(owner.billing);
    for (size_t allowed = 0; allowed < allocations; ++allowed) {
        xrt_billing_owner_fail_after(owner.billing, allowed);
        CHECK(xr_lir_value_layouts_create(module, roots, 3, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_OOM && !set);
        XrtBillingStats after = xrt_billing_owner_stats(owner.billing);
        CHECK(after.bytes == held.bytes && after.blocks == held.blocks);
    }
    xrt_billing_owner_fail_after(owner.billing, SIZE_MAX);
    XrLirValueLayoutKey rejected = {cell_ref,0,0};
    CHECK(xr_lir_value_layouts_create(module, &rejected, 1, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_UNSUPPORTED && !set);
    xr_lir_module_destroy(module);
    spec = view(plan);
    CHECK(spec.signature.parameter_count == 8 && view(spec.signature.result).signature.flags == XRT_VALUE_NO_SUSPEND);
    static const unsigned char entry = 0;
    int64_t environment = 42;
    XrtFunction borrowed = {&entry,(uintptr_t)&environment,NULL}, copied = {0};
    size_t calls = xrt_billing_owner_stats(owner.billing).allocations;
    MANAGED(xrt_value_copy_empty(plan, &borrowed, &copied));
    CHECK(copied.entry == &entry && copied.environment == (uintptr_t)&environment && !copied.owner);
    xrt_value_drop(plan, &copied);
    CHECK(!copied.entry && !copied.environment && !copied.owner && environment == 42);
    CHECK(xrt_billing_owner_stats(owner.billing).allocations == calls);
    owner_drop(&owner); CHECK(!atomic_load(&owner.audit.final));
    xrt_value_plan_drop((XrtValuePlan *)plan); CHECK(atomic_load(&owner.audit.final) == 1);
    printf("callable signatures: exact contracts, independent dependencies and %zu allocation rollback sites passed\n", allocations);
}
static void slice_builtin_admission(XrLirModule *module) {
    CHECK(lir_module_has_builtin_abi(module)); bool checked = false;
    for (uint32_t n = 0; n < module->intrinsic_count; ++n) {
        XrLirSliceIntrinsic slice;
        if (xr_lir_slice_intrinsic_view(module, module->intrinsics[n].id, &slice) != XR_LIR_OK) continue;
        uint32_t operation = module->intrinsics[n].operation; module->intrinsics[n].operation = 0;
        CHECK(!lir_module_has_builtin_abi(module)); module->intrinsics[n].operation = operation;
        checked = true; break;
    }
    CHECK(checked && lir_module_has_builtin_abi(module));
    XrLirModule *generic = NULL; LIR(xr_lir_module_create(NULL, &generic)); XrLirType fixed = 0;
    LIR(xr_lir_fixed_array_type(generic, XR_LIR_U8, 4, &fixed));
    XrLirIntrinsic entry = {.id=777,.parameters=&fixed,.parameter_count=1,.result=XR_LIR_I64,.scalar_sync=true};
    XrLirRegistry registry = {&entry,1}; LIR(xr_lir_set_registry(generic, &registry));
    LIR(xr_lir_module_finish(generic, NULL)); CHECK(!lir_module_has_builtin_abi(generic));
    xr_lir_module_destroy(generic);
}
static void cell_layouts(void) {
    XrLirModule *module = NULL; LIR(xr_lir_module_create(NULL,&module));
    XrLirType cells[3] = {0}, callable = 0;
    const XrLirType payloads[] = {XR_LIR_I64,XR_LIR_MAP_STRING_I64,XR_LIR_STRING};
    for (size_t n = 0; n < 3; ++n) LIR(xr_lir_cell_type(module,(XrLirCellType){payloads[n],0},&cells[n]));
    LIR(xr_lir_callable_type(module,&(XrLirCallableType){.result=XR_LIR_I64},&callable));
    XrLirFunction target = {0};
    LIR(xr_lir_declare_function(module,&(XrLirFunctionSpec){.name="cells",.name_length=5,
        .parameters=cells,.parameter_count=2,.result=XR_LIR_I64},&target));
    LIR(xr_lir_set_closure_target(module,target,(XrLirClosureTarget){callable,2,false}));
    LIR(xr_lir_define_function(module,target));
    XrLirBlock entry = {0}; XrLirValue input = {0}, result = {0};
    LIR(xr_lir_add_block(module,target,NULL,0,&entry)); LIR(xr_lir_set_entry(module,target,entry));
    LIR(xr_lir_function_parameter(module,target,0,&input));
    LIR(xr_lir_emit_value(module,entry,&(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_I64,
        .operands=&input,.operand_count=1},&result));
    LIR(xr_lir_set_terminator(module,entry,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&result,.operand_count=1}));
    LIR(xr_lir_module_finish(module,NULL));
    Owner owner; owner_new(&owner,SIZE_MAX);
    XrLirValueLayoutKey roots[] = {{cells[0],0,0},{cells[1],0,0},{callable,0,target.id}};
    XrLirValueLayouts *set = NULL;
    XrtBillingStats baseline = xrt_billing_owner_stats(owner.billing);
    LAYOUT(xr_lir_value_layouts_create(module,roots,3,owner.billing,&set));
    size_t allocations = xrt_billing_owner_stats(owner.billing).allocations - baseline.allocations;
    for (size_t n = 0; n < 2; ++n) {
        XrtValuePlanSpec spec = view(get(set,cells[n]));
        CHECK(spec.kind == XRT_VALUE_CELL && spec.size == sizeof(void *) && spec.alignment == alignof(void *));
        CHECK(spec.owned_count == 1 && !spec.owned_offsets[0] && spec.element == get(set,payloads[n]));
    }
    const XrLirOwnershipTypeFact *facts = NULL; uint32_t count = 0;
    LAYOUT(xr_lir_value_layouts_ownership(set,&facts,&count));
    CHECK(count == 2 && facts[0].type == cells[0] && facts[1].type == cells[1] && facts[0].has_owned && facts[1].has_owned);
    const XrtValuePlan *environment = NULL;
    LAYOUT(xr_lir_value_layouts_get(set,roots[2],&environment));
    XrtValuePlanSpec env = view(environment);
    CHECK(env.kind == XRT_VALUE_ENVIRONMENT && env.capture_count == 2 && env.owned_count == 2 &&
        env.captures[0].type == get(set,cells[0]) && env.captures[1].type == get(set,cells[1]) &&
        env.captures[0].offset == 0 && env.captures[1].offset == sizeof(void *) &&
        env.owned_offsets[0] == 0 && env.owned_offsets[1] == sizeof(void *));
    CHECK(xrt_value_plan_retain((XrtValuePlan *)environment));
    xr_lir_value_layouts_destroy(set); set = NULL;
    baseline = xrt_billing_owner_stats(owner.billing);
    for (size_t point = 0; point < allocations; ++point) {
        xrt_billing_owner_fail_after(owner.billing,point);
        CHECK(xr_lir_value_layouts_create(module,roots,3,owner.billing,&set) == XR_LIR_VALUE_LAYOUT_OOM && !set);
        XrtBillingStats after = xrt_billing_owner_stats(owner.billing);
        CHECK(after.bytes == baseline.bytes && after.blocks == baseline.blocks);
    }
    xrt_billing_owner_fail_after(owner.billing,SIZE_MAX);
    XrLirValueLayoutKey rejected = {cells[2],0,0};
    CHECK(xr_lir_value_layouts_create(module,&rejected,1,owner.billing,&set) == XR_LIR_VALUE_LAYOUT_UNSUPPORTED && !set);
    xr_lir_module_destroy(module); owner_drop(&owner); CHECK(!atomic_load(&owner.audit.final));
    env = view(environment); CHECK(env.capture_count == 2 && view(env.captures[1].type).kind == XRT_VALUE_CELL &&
        view(view(env.captures[1].type).element).kind == XRT_VALUE_MAP_STRING_I64);
    xrt_value_plan_drop((XrtValuePlan *)environment); CHECK(atomic_load(&owner.audit.final) == 1);
}
static void slice_layouts(void) {
    XrLirModule *module = NULL, *received = NULL;
    LIR(xr_lir_module_create(NULL, &module));
    XrLirType slices[2] = {0}, imported = 0;
    LIR(xr_lir_slice_type(module, XR_LIR_I64, &slices[0]));
    LIR(xr_lir_slice_type(module, XR_LIR_U8, &slices[1]));
    LIR(xr_lir_module_create(NULL, &received));
    LIR(xr_lir_import_type(received, module, slices[1], &imported));
    XrLirTypeView shape; LIR(xr_lir_type_view(received, imported, &shape));
    CHECK(shape.kind == XR_LIR_TYPE_SLICE && shape.element == XR_LIR_U8 && !shape.count);
    XrLirType rejected_optional = 0;
    CHECK(xr_lir_nullable_type(received, imported, &rejected_optional) == XR_LIR_INVALID_ARGUMENT && !rejected_optional);
    LIR(xr_lir_slice_catalog_install(module));
    for (uint32_t family = 0; family < 2; ++family) {
        XrLirType element = family ? XR_LIR_U8 : XR_LIR_I64;
        for (uint32_t role = XR_LIR_SLICE_OP_NEW; role < XR_LIR_SLICE_OP_CLEANUP; ++role) {
            uint32_t id = 0; XrLirSliceIntrinsic projection;
            XrLirType binding = role == XR_LIR_SLICE_OP_NEW ? XR_LIR_ARRAY_CELL : XR_LIR_TYPE_INVALID;
            LIR(xr_lir_slice_operation(module, (XrLirSliceOperation)role, element, binding, &id));
            LIR(xr_lir_slice_intrinsic_view(module, id, &projection));
            CHECK(projection.id == id && projection.operation == (XrLirSliceOperation)role &&
                projection.element == element && projection.binding_type == binding);
            const XrLirIntrinsic *entry = xr_lir_internal_slice_intrinsic(module, id);
            CHECK(entry && entry->parameters[0] == (binding ? binding : slices[family]));
        }
        uint32_t id = 0;
        LIR(xr_lir_slice_operation(module, XR_LIR_SLICE_OP_NEW, element, XR_LIR_ARRAY_REF, &id));
        LIR(xr_lir_slice_operation(module, XR_LIR_SLICE_OP_NEW, element, XR_LIR_STATE_REF, &id));
        XrLirSliceIntrinsic state_read;
        LIR(xr_lir_slice_intrinsic_view(module, id, &state_read));
        CHECK(state_read.operation == XR_LIR_SLICE_OP_NEW && state_read.binding_type == XR_LIR_STATE_REF &&
            state_read.element == element);
    }
    uint32_t unchanged = 99;
    CHECK(xr_lir_slice_operation(module, XR_LIR_SLICE_OP_CLEANUP, XR_LIR_I64, 0, &unchanged) == XR_LIR_UNSUPPORTED && unchanged == 99);
    CHECK(xr_lir_slice_operation(module, XR_LIR_SLICE_OP_GET, XR_LIR_F64, 0, &unchanged) == XR_LIR_UNSUPPORTED && unchanged == 99);
    LIR(xr_lir_module_finish(module, NULL));
    slice_builtin_admission(module);
    Owner owner; owner_new(&owner, SIZE_MAX);
    XrLirValueLayoutKey roots[] = {{slices[0],0,0}, {slices[1],0,0}};
    XrLirValueLayouts *set = NULL;
    size_t before = xrt_billing_owner_stats(owner.billing).allocations;
    LAYOUT(xr_lir_value_layouts_create(module, roots, 2, owner.billing, &set));
    size_t allocations = xrt_billing_owner_stats(owner.billing).allocations - before;
    const XrtValuePlan *plan = get(set, slices[0]);
    XrtValuePlanSpec spec = view(plan);
    CHECK(spec.kind == XRT_VALUE_SLICE_I64 && spec.size == sizeof(XrtReadonlyArraySlice) &&
        spec.alignment == alignof(XrtReadonlyArraySlice) && !spec.owned_count && !spec.element);
    CHECK(view(get(set, slices[1])).kind == XRT_VALUE_SLICE_U8);
    CHECK(xrt_value_plan_retain((XrtValuePlan *)plan));
    xr_lir_value_layouts_destroy(set); set = NULL;
    XrtBillingStats baseline = xrt_billing_owner_stats(owner.billing);
    for (size_t allowed = 0; allowed < allocations; ++allowed) {
        xrt_billing_owner_fail_after(owner.billing, allowed);
        CHECK(xr_lir_value_layouts_create(module, roots, 2, owner.billing, &set) == XR_LIR_VALUE_LAYOUT_OOM && !set);
        XrtBillingStats after = xrt_billing_owner_stats(owner.billing);
        CHECK(after.bytes == baseline.bytes && after.blocks == baseline.blocks);
    }
    xrt_billing_owner_fail_after(owner.billing, SIZE_MAX);
    xr_lir_module_destroy(module); xr_lir_module_destroy(received);
    owner_drop(&owner); CHECK(!atomic_load(&owner.audit.final));
    CHECK(view(plan).size == sizeof(XrtReadonlyArraySlice) && !view(plan).owned_count);
    xrt_value_plan_drop((XrtValuePlan *)plan); CHECK(atomic_load(&owner.audit.final) == 1);
}
static void inline_array_layouts(void) {
    XrLirModule *module = NULL;
    LIR(xr_lir_module_create(NULL,&module));
    const XrLirRecordField int_fields[] = {{.type=XR_LIR_STRING},{.type=XR_LIR_I64}};
    const XrLirRecordField float_fields[] = {{.type=XR_LIR_STRING},{.type=XR_LIR_F64}};
    XrLirType integers = 0, floats = 0;
    LIR(xr_lir_tuple_type(module,int_fields,2,&integers));
    LIR(xr_lir_tuple_type(module,float_fields,2,&floats));
    LIR(xr_lir_require_inline_array_element(module,floats));
    LIR(xr_lir_require_inline_array_element(module,integers));
    LIR(xr_lir_array_catalog_install(module));
    uint32_t integer_new = 0, float_new = 0;
    LIR(xr_lir_array_operation(module,integers,XR_LIR_ARRAY_OP_NEW,&integer_new));
    LIR(xr_lir_array_operation(module,floats,XR_LIR_ARRAY_OP_NEW,&float_new));
    CHECK(integer_new != float_new);
    uint32_t write = 0;
    CHECK(xr_lir_array_operation(module,integers,XR_LIR_ARRAY_OP_SET,&write) == XR_LIR_UNSUPPORTED && !write);
    LIR(xr_lir_module_finish(module,NULL));

    Owner owner; owner_new(&owner,SIZE_MAX);
    const XrLirValueLayoutKey roots[] = {{XR_LIR_ARRAY,integers,0},{XR_LIR_ARRAY,floats,0}};
    XrLirValueLayouts *layouts = NULL;
    LAYOUT(xr_lir_value_layouts_create(module,roots,2,owner.billing,&layouts));
    const XrtValuePlan *integer_array = NULL, *float_array = NULL;
    uint32_t integer_id = 0, float_id = 0;
    LAYOUT(xr_lir_value_layouts_lookup(layouts,roots[0],&integer_array,&integer_id));
    LAYOUT(xr_lir_value_layouts_lookup(layouts,roots[1],&float_array,&float_id));
    CHECK(integer_id != float_id);
    const XrtValuePlanSpec a = view(integer_array), b = view(float_array);
    CHECK(a.kind == XRT_VALUE_ARRAY_INLINE && b.kind == a.kind &&
        a.owned_count == 1 && b.owned_count == 1 && a.size == sizeof(void *) && b.size == a.size);
    CHECK(a.element == get(layouts,integers) && b.element == get(layouts,floats) && a.element != b.element);
    CHECK(view(a.element).owned_count == 1 && view(b.element).owned_count == 1);
    xr_lir_value_layouts_destroy(layouts);
    xr_lir_module_destroy(module);
    owner_drop(&owner);
    CHECK(atomic_load(&owner.audit.final) == 1);
}
int main(void) {
    natural_layouts(); creator_retirement(); failures_and_boundaries(); inline_overflow(); callable_layouts();
    slice_layouts(); cell_layouts(); inline_array_layouts();
    puts("Admitted LIR value layouts: natural offsets, independent plans, billed rollback and real inline owners passed");
    return 0;
}
