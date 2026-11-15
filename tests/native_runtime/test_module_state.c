/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_state.c - Real fixed values, snapshots and atomic State publication
 */
#include "runtime/native/xnative_module_state.h"
#include "runtime/native/xnative_array_ops.h"
#include "runtime/native/xnative_map_backing.h"
#include "xray_native_values.h"
#include "base/xmalloc.h"
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
#define BAD(c) CHECK((c) == XRT_MANAGED_ARGUMENT)
/* This kernel returns statuses synchronously. The execution boundary and its
 * nonlocal failure mapping are exercised by the execution tests. */
XRAY_API_NORET void xrt_native_managed_failure(XrtManagedStatus status, uint32_t source) {
    fprintf(stderr,"unexpected execution failure %d at %u\n",(int)status,source); abort();
}
typedef struct MaybeString { uint8_t tag; XrtManagedHeader *value; } MaybeString;
typedef struct Pair { XrtManagedHeader *left; MaybeString right; int64_t score; } Pair;
_Static_assert(sizeof(Pair) == 32 && offsetof(Pair, right.value) == 16, "typed C uses the shared inline layout");
static const size_t owned[] = {0,0,0,0,0,0,8,0,16,8,24};
static const XrtValueImageSlot fields[] = {{1,0,0,4},{10,8,4,5},{2,24,9,5}};
static const XrtValueImageType types[] = {
    {.kind=XRT_VALUE_STRING,.size=8,.alignment=8,.owned_count=1},
    {.kind=XRT_VALUE_I64,.size=8,.alignment=8},
    {.kind=XRT_VALUE_BOOL,.size=1,.alignment=1},
    {.kind=XRT_VALUE_RUNE,.size=8,.alignment=8},
    {.kind=XRT_VALUE_ARRAY_I64,.size=8,.alignment=8,.first_owned=1,.owned_count=1},
    {.kind=XRT_VALUE_ARRAY_STRING,.size=8,.alignment=8,.first_owned=2,.owned_count=1},
    {.kind=XRT_VALUE_ARRAY_U8,.size=8,.alignment=8,.first_owned=3,.owned_count=1},
    {.kind=XRT_VALUE_ARRAY_U64,.size=8,.alignment=8,.first_owned=4,.owned_count=1},
    {.kind=XRT_VALUE_MAP_STRING_I64,.size=8,.alignment=8,.first_owned=5,.owned_count=1},
    {.kind=XRT_VALUE_NULLABLE,.size=16,.alignment=8,.first_owned=6,.owned_count=1,
        .nullable_element=1,.nullable_payload=8},
    {.kind=XRT_VALUE_RECORD,.size=32,.alignment=8,.first_owned=7,.owned_count=2,.field_count=3},
    {.kind=XRT_VALUE_NULLABLE,.size=40,.alignment=8,.first_owned=9,.owned_count=2,
        .nullable_element=11,.nullable_payload=8},
    {.kind=XRT_VALUE_RECORD,.size=1,.alignment=1},
    {.kind=XRT_VALUE_NULLABLE,.size=2,.alignment=1,.nullable_element=13,.nullable_payload=1},
    {.kind=XRT_VALUE_FIXED_U8,.size=3,.alignment=1,.count=3},
    {.kind=XRT_VALUE_TUPLE_I64,.size=16,.alignment=8,.count=2}
};
static const XrtValueImage image = {types,16,fields,3,NULL,0,owned,11,"leftrightscore",14};
typedef struct Audit {
    size_t leases, bytes, blocks, calls, observed;
    void *watched[2];
    const Pair *payload;
    Pair expected;
    XrtModuleRef *reference;
    XrtModuleState **state;
    XrtModuleRef *retiring_reference;
    XrtModuleState *retiring_state;
    size_t retirement_checks;
} Audit;
static bool provider_retain(void *context) { ++((Audit *)context)->leases; return true; }
static void provider_drop(void *context) { CHECK(((Audit *)context)->leases); --((Audit *)context)->leases; }
static void *provider_allocate(void *context, size_t bytes) {
    Audit *a = context; ++a->calls;
    void *memory = xr_malloc(bytes);
    if (memory) { a->bytes += bytes; ++a->blocks; }
    return memory;
}
static void provider_free(void *context, void *memory, size_t bytes) {
    Audit *a = context;
    if (a->retiring_reference) {
        const void *unchanged = a;
        BAD(xrt_module_borrow(a->retiring_reference,&unchanged)); CHECK(unchanged == a);
        XrtModuleState *state_owner = a->retiring_state;
        xrt_module_state_drop(&state_owner); CHECK(state_owner == a->retiring_state);
        XrtModuleAccess access;
        XrtManagedStatus cached = xrt_module_cache(a->retiring_state,0,&access);
        CHECK(cached == XRT_MANAGED_OK || cached == XRT_MANAGED_ARGUMENT);
        ++a->retirement_checks;
        if (memory == a->retiring_state) a->retiring_reference = NULL;
    }
    if (memory == a->watched[0] || memory == a->watched[1]) {
        CHECK(a->payload->left == a->expected.left && a->payload->right.value == a->expected.right.value &&
            a->payload->right.tag == a->expected.right.tag && a->payload->score == a->expected.score);
        BAD(xrt_module_replace(a->reference, &a->expected));
        xrt_module_state_clear(*a->state);
        CHECK(a->payload->score == a->expected.score);
        ++a->observed;
    }
    CHECK(a->bytes >= bytes && a->blocks); a->bytes -= bytes; --a->blocks;
    xr_free(memory);
}
static XrtBillingOwner *billing_new(Audit *audit, size_t limit) {
    XrtManagedAllocator allocator = {audit,provider_allocate,provider_free,{audit,provider_retain,provider_drop}};
    XrtBillingOwner *billing = NULL; OK(xrt_billing_owner_new(&allocator,limit,&billing)); return billing;
}
static void same_bill(XrtBillingOwner *billing, XrtBillingStats before) {
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(before.bytes == after.bytes && before.blocks == after.blocks);
}
static XrtModuleLayout *layout_new(XrtBillingOwner *billing, const uint32_t *ids, uint32_t count, bool mutable) {
    XrtModuleSlotSpec slots[20]; CHECK(count <= 20);
    size_t end = 0, alignment = 1;
    for (uint32_t i = 0; i < count; ++i) {
        const XrtValueImageType *type = &types[ids[i] - 1];
        end = (end + type->alignment - 1) & ~(type->alignment - 1);
        slots[i] = (XrtModuleSlotSpec){0,ids[i],mutable ? XRT_MODULE_SLOT_MUTABLE : 0,end,type->size,type->alignment};
        end += type->size; if (type->alignment > alignment) alignment = type->alignment;
    }
    end = (end + alignment - 1) & ~(alignment - 1);
    XrtModuleSpec module = {0};
    XrtModuleLayoutSpec spec = {&image,&module,1,NULL,0,count ? slots : NULL,count,end,alignment};
    XrtModuleLayout *layout = NULL; OK(xrt_module_layout_new(billing,&spec,(XrtManagedLease){0},&layout));
    return layout;
}
static XrtManagedHeader *borrow(XrtManagedFrame *frame, size_t slot) {
    XrtManagedHeader *value = NULL; OK(xrt_managed_slot_borrow(frame,slot,&value)); CHECK(value); return value;
}
static bool empty_slot(XrtManagedFrame *frame, size_t slot) {
    XrtManagedHeader *value=NULL; OK(xrt_managed_slot_borrow(frame,slot,&value)); return !value;
}
static void atomic_record(void) {
    Audit meta = {0}, values = {0};
    XrtBillingOwner *mb = billing_new(&meta,SIZE_MAX), *vb = billing_new(&values,SIZE_MAX);
    XrtModuleLayout *layout = layout_new(mb,(uint32_t[]){11},1,true);
    XrtModuleLayoutView view; OK(xrt_module_layout_view(layout,&view));
    CHECK(view.slots[0].type == 11 && view.slots[0].size == sizeof(Pair));
    const XrtValuePlan *plan = NULL; OK(xrt_module_layout_plan(layout,11,&plan));
    XrtValuePlanSpec ps; OK(xrt_value_plan_view(plan,&ps)); CHECK(ps.owned_count == 2);
    XrtModuleState *state = NULL; OK(xrt_module_state_new(layout,vb,&state));
    xrt_module_layout_drop(&layout); xrt_billing_owner_drop(mb); mb = NULL;
    CHECK(meta.leases == 1 && meta.blocks);
    XrtBackingTypes setup = {0}; OK(xrt_backing_types_init(vb,&setup));
    XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    OK(xrt_managed_root_new(vb,&root)); OK(xrt_managed_frame_enter(root,4,&frame));
    for (size_t i = 0; i < 4; ++i) OK(xrt_string_backing_create(frame,i,&setup,"same",4,0));
    Pair old = {borrow(frame,0),{1,borrow(frame,1)},17};
    XrtModuleRef *reference = NULL;
    BAD(xrt_module_construct(state,0,&old,&reference));
    OK(xrt_module_begin(state,0)); OK(xrt_module_construct(state,0,&old,&reference));
    BAD(xrt_module_ready(state,0)); OK(xrt_module_publish(reference)); OK(xrt_module_ready(state,0));
    OK(xrt_managed_slot_drop(frame,0)); OK(xrt_managed_slot_drop(frame,1));
    const void *payload = NULL; OK(xrt_module_borrow(reference,&payload));
    CHECK(atomic_load(&old.left->references) == 1 && atomic_load(&old.right.value->references) == 1);
    Pair snapshot = {0}; OK(xrt_module_copy(reference,&snapshot));
    CHECK(atomic_load(&old.left->references) == 2 && snapshot.score == 17);
    xrt_value_drop(plan,&snapshot);
    CHECK(atomic_load(&old.left->references) == 1);
    XrtExternalOwner external; OK(xrt_module_external_owner(reference,0,&external));
    XrtPlace to = {.frame=frame,.slot=0,.nulls=XRT_PLACE_NULLABLE};
    XrtPlace from = {.external=&external,.nulls=XRT_PLACE_NULLABLE};
    BAD(xrt_place_move_replace(to,from,NULL));
    CHECK(atomic_load(&old.left->references) == 1);
    Pair replacement = {borrow(frame,2),{0,NULL},42};
    values.watched[0] = old.left; values.watched[1] = old.right.value;
    values.payload = payload; values.expected = replacement; values.reference = reference; values.state = &state;
    size_t calls = values.calls;
    xrt_billing_owner_fail_after(vb,0);
    OK(xrt_module_replace(reference,&replacement)); CHECK(values.calls == calls && values.observed == 2);
    values.watched[0] = values.watched[1] = NULL;
    OK(xrt_managed_slot_drop(frame,2));
    CHECK(atomic_load(&replacement.left->references) == 1);
    OK(xrt_module_replace(reference,payload)); CHECK(atomic_load(&replacement.left->references) == 1);
    Pair invalid = {replacement.left,{2,NULL},90}; BAD(xrt_module_replace(reference,&invalid));
    CHECK(((const Pair *)payload)->score == 42);
    invalid.right.tag = 0; invalid.right.value = borrow(frame,3); BAD(xrt_module_replace(reference,&invalid));
    Pair bad_retain = {replacement.left,{1,borrow(frame,3)},77};
    atomic_store(&bad_retain.right.value->references,INT32_MAX);
    CHECK(xrt_module_replace(reference,&bad_retain) == XRT_MANAGED_REFCOUNT);
    CHECK(atomic_load(&replacement.left->references) == 1 && ((const Pair *)payload)->score == 42);
    atomic_store(&bad_retain.right.value->references,1);
    xrt_billing_owner_close(vb);
    OK(xrt_module_replace(reference,payload));
    OK(xrt_module_copy(reference,&snapshot)); CHECK(snapshot.score == 42); xrt_value_drop(plan,&snapshot);
    xrt_module_state_clear(state); BAD(xrt_module_borrow(reference,&payload));
    meta.retiring_reference = values.retiring_reference = reference;
    meta.retiring_state = values.retiring_state = state;
    xrt_module_state_drop(&state); CHECK(!meta.blocks && !meta.bytes && !meta.leases);
    CHECK(meta.retirement_checks && values.retirement_checks && !values.retiring_reference);
    meta.retiring_reference = NULL;
    OK(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&setup);
    CHECK(values.blocks == 1); xrt_billing_owner_drop(vb);
    CHECK(!values.leases && !values.blocks && !values.bytes);
    puts("inline State snapshots, aliasing, retain rollback and whole-value publication passed");
}
static void family_borrows(void) {
    Audit audit = {0}; XrtBillingOwner *billing = billing_new(&audit,SIZE_MAX);
    XrtBackingTypes setup = {0}; OK(xrt_backing_types_init(billing,&setup));
    XrtManagedRoot *root = NULL; XrtManagedFrame *frame = NULL;
    OK(xrt_managed_root_new(billing,&root)); OK(xrt_managed_frame_enter(root,8,&frame));
    OK(xrt_string_backing_create(frame,0,&setup,"key",3,0));
    XrtStringBacking *key = (XrtStringBacking *)borrow(frame,0);
    int64_t number = 7; uint64_t u64 = 9; uint8_t u8 = 11;
    OK(xrt_array_backing_i64_create(frame,1,&setup,&number,1,2));
    OK(xrt_array_backing_string_create(frame,2,&setup,&key,1,3));
    OK(xrt_array_backing_u8_create(frame,3,&setup,&u8,1,2));
    OK(xrt_array_backing_u64_create(frame,4,&setup,&u64,1,2));
    OK(xrt_map_backing_create(frame,5,&setup,&key,&number,1));
    XrtModuleLayout *layout = layout_new(billing,(uint32_t[]){5,6,7,8,9},5,true);
    XrtModuleState *state = NULL; OK(xrt_module_state_new(layout,billing,&state));
    OK(xrt_module_begin(state,0)); XrtModuleRef *references[5] = {0};
    for (uint32_t i = 0; i < 5; ++i) {
        XrtManagedHeader *owner = borrow(frame,i + 1);
        OK(xrt_module_construct(state,i,&owner,&references[i])); OK(xrt_module_publish(references[i]));
        OK(xrt_managed_slot_drop(frame,i + 1));
    }
    OK(xrt_module_ready(state,0));
    const void *payload = NULL; OK(xrt_module_borrow(references[0],&payload));
    XrtArrayBacking *array = (XrtArrayBacking *)*(XrtManagedHeader *const *)payload;
    CHECK(atomic_load(&array->managed.references) == 1);
    XrtArrayUpdate update = {.operation=XRT_ARRAY_UPDATE_SET,.index=0,
        .item={.kind=XRT_BACKING_ARRAY_I64,.value.i64=42}}; bool applied = false;
    XrtExternalOwner external; OK(xrt_module_external_owner(references[0],0,&external));
    XrtPlace binding={.external=&external,.nulls=XRT_PLACE_NONNULL};
    XrtBillingStats before=xrt_billing_owner_stats(billing); xrt_billing_owner_fail_after(billing,0);
    OK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,frame,6,&setup,&update));
    CHECK(atomic_load(&array->managed.references)==1 && xrt_billing_owner_stats(billing).allocations==before.allocations);
    XrtManagedHeader *snapshot = NULL; OK(xrt_module_copy(references[0],&snapshot));
    CHECK(atomic_load(&array->managed.references) == 2);
    update.item.value.i64 = 99;
    CHECK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,frame,6,&setup,&update)==XRT_MANAGED_OOM);
    OK(xrt_module_borrow(references[0],&payload)); CHECK(*(XrtManagedHeader *const *)payload==&array->managed);
    CHECK(empty_slot(frame,6)); xrt_billing_owner_fail_after(billing,SIZE_MAX);
    OK(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,frame,6,&setup,&update));
    OK(xrt_module_borrow(references[0],&payload)); CHECK(*(XrtManagedHeader *const *)payload!=&array->managed);
    CHECK(empty_slot(frame,6));
    xrt_managed_drop(snapshot);
    OK(xrt_module_borrow(references[4],&payload)); XrtMapBacking *map = (XrtMapBacking *)*(XrtManagedHeader *const *)payload;
    CHECK(atomic_load(&map->managed.references) == 1);
    XrtMapUpdate set = {XRT_MAP_UPDATE_SET,key,42}; bool removed = false;
    OK(xrt_map_backing_try_update(map,&set,&applied,&removed)); CHECK(applied && !removed);
    int64_t value = 0; bool found = false; OK(xrt_map_backing_get(map,key,&value,&found)); CHECK(found && value == 42);
    for (uint32_t i = 0; i < 5; ++i) {
        snapshot = NULL; OK(xrt_module_copy(references[i],&snapshot)); CHECK(snapshot); xrt_managed_drop(snapshot);
    }
    xrt_module_state_drop(&state); xrt_module_layout_drop(&layout);
    OK(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&setup);
    CHECK(audit.blocks == 1); xrt_billing_owner_drop(billing);
    CHECK(!audit.leases && !audit.blocks && !audit.bytes);
    puts("four Array families and Map borrow the actual unique State binding");
}
static void array_state_guards(void) {
    for (unsigned mutable=0;mutable<2;++mutable) {
        Audit audit={0}; XrtBillingOwner *billing=billing_new(&audit,SIZE_MAX);
        XrtBackingTypes setup={0}; OK(xrt_backing_types_init(billing,&setup));
        XrtManagedRoot *root=NULL; XrtManagedFrame *frame=NULL;
        OK(xrt_managed_root_new(billing,&root)); OK(xrt_managed_frame_enter(root,2,&frame));
        int64_t value=7; OK(xrt_array_backing_i64_create(frame,0,&setup,&value,1,2));
        XrtManagedHeader *array=borrow(frame,0);
        XrtModuleLayout *layout=layout_new(billing,(uint32_t[]){5},1,mutable!=0);
        XrtModuleState *state=NULL; OK(xrt_module_state_new(layout,billing,&state)); OK(xrt_module_begin(state,0));
        XrtModuleAccess published; OK(xrt_module_cache(state,0,&published));
        XrtExternalOwner external; OK(xrt_module_external_owner(published.reference,0,&external));
        XrtPlace binding={.external=&external,.nulls=XRT_PLACE_NONNULL};
        XrtArrayUpdate update={XRT_ARRAY_UPDATE_SET,0,{.kind=XRT_BACKING_ARRAY_I64,.value.i64=42}};
        BAD(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,frame,1,&setup,&update));
        if (mutable) {
            XrtModuleRef *initial=NULL; OK(xrt_module_construct(state,0,&array,&initial));
            BAD(xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,frame,1,&setup,&update));
            OK(xrt_module_publish(initial));
        } else OK(xrt_module_initialize(state,0,&array));
        OK(xrt_managed_slot_drop(frame,0)); OK(xrt_module_ready(state,0));
        const XrtManagedHeader *read=NULL; OK(xrt_place_borrow(binding,&read)); CHECK(read==array);
        xrt_billing_owner_fail_after(billing,0);
        XrtManagedStatus status=xrt_array_update_place(binding,XRT_BACKING_ARRAY_I64,frame,1,&setup,&update);
        CHECK(status==(mutable ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT));
        uint64_t bits=0; OK(xrt_array_backing_read_bits((const XrtArrayBacking *)array,XRT_BACKING_ARRAY_I64,0,&bits));
        CHECK(bits==(mutable ? 42u : 7u) && empty_slot(frame,1));
        xrt_module_state_drop(&state); xrt_module_layout_drop(&layout); OK(xrt_managed_root_destroy(&root));
        xrt_backing_types_drop(&setup); xrt_billing_owner_drop(billing);
        CHECK(!audit.leases && !audit.bytes && !audit.blocks);
    }
}
static void allocation_failures(void) {
    Audit audit = {0}; XrtBillingOwner *billing = billing_new(&audit,SIZE_MAX);
    XrtBillingStats baseline = xrt_billing_owner_stats(billing);
    size_t layout_sites = 0;
    for (;; ++layout_sites) {
        xrt_billing_owner_fail_after(billing,layout_sites);
        XrtModuleSpec module = {0}; XrtModuleSlotSpec slot = {0,11,1,0,32,8};
        XrtModuleLayoutSpec spec = {&image,&module,1,NULL,0,&slot,1,32,8};
        XrtModuleLayout *layout = NULL;
        XrtManagedStatus status = xrt_module_layout_new(billing,&spec,(XrtManagedLease){0},&layout);
        if (status == XRT_MANAGED_OK) { xrt_module_layout_drop(&layout); same_bill(billing,baseline); break; }
        CHECK(status == XRT_MANAGED_OOM && !layout); same_bill(billing,baseline); CHECK(layout_sites < 100);
    }
    xrt_billing_owner_fail_after(billing,SIZE_MAX);
    XrtModuleLayout *layout = layout_new(billing,(uint32_t[]){11},1,true);
    baseline = xrt_billing_owner_stats(billing);
    size_t state_sites = 0;
    for (;; ++state_sites) {
        xrt_billing_owner_fail_after(billing,state_sites);
        XrtModuleState *state = NULL; XrtManagedStatus status = xrt_module_state_new(layout,billing,&state);
        if (status == XRT_MANAGED_OK) { xrt_module_state_drop(&state); same_bill(billing,baseline); break; }
        CHECK(status == XRT_MANAGED_OOM && !state); same_bill(billing,baseline); CHECK(state_sites < 10);
    }
    xrt_billing_owner_close(billing); XrtModuleState *state = NULL;
    CHECK(xrt_module_state_new(layout,billing,&state) == XRT_MANAGED_CLOSED && !state);
    xrt_module_layout_drop(&layout); CHECK(audit.blocks == 1);
    xrt_billing_owner_drop(billing); CHECK(!audit.leases && !audit.blocks && !audit.bytes);
    CHECK(layout_sites >= 18 && state_sites >= 3);
    printf("all %zu layout and %zu State reservation failures refund exactly\n",layout_sites,state_sites);
}
static void scalar_admission(void) {
    Audit audit = {0}; XrtBillingOwner *billing = billing_new(&audit,SIZE_MAX);
    XrtModuleLayout *layout = layout_new(billing,(uint32_t[]){3,4,14,15,16},5,false);
    XrtModuleState *state = NULL; OK(xrt_module_state_new(layout,billing,&state)); OK(xrt_module_begin(state,0));
    uint8_t bad = 2; BAD(xrt_module_initialize(state,0,&bad)); bad = 1; OK(xrt_module_initialize(state,0,&bad));
    int64_t rune = 0xd800; BAD(xrt_module_initialize(state,1,&rune)); rune = 0x10ffff; OK(xrt_module_initialize(state,1,&rune));
    uint8_t nullable[2] = {2,77}; BAD(xrt_module_initialize(state,2,nullable)); nullable[0] = 0;
    OK(xrt_module_initialize(state,2,nullable)); uint8_t fixed[] = {1,2,3}; OK(xrt_module_initialize(state,3,fixed));
    int64_t tuple[] = {17,25}; OK(xrt_module_initialize(state,4,tuple)); OK(xrt_module_ready(state,0));
    XrtModuleRef *ref = NULL; OK(xrt_module_reference(state,2,&ref)); const void *value = NULL;
    OK(xrt_module_borrow(ref,&value)); CHECK(!((const uint8_t *)value)[0] && !((const uint8_t *)value)[1]);
    BAD(xrt_module_replace(ref,nullable)); xrt_module_state_drop(&state); xrt_module_layout_drop(&layout);
    CHECK(audit.blocks == 1); xrt_billing_owner_drop(billing);
    CHECK(!audit.leases && !audit.blocks && !audit.bytes);
}
static bool code_retain(void *context) { ++*(size_t *)context; return true; }
static void code_drop(void *context) { CHECK(*(size_t *)context); --*(size_t *)context; }
static void metadata_and_rejection(void) {
    Audit audit = {0}; XrtBillingOwner *billing = billing_new(&audit,SIZE_MAX);
    XrtBillingStats baseline = xrt_billing_owner_stats(billing);
    XrtValueImageType *raw = xr_malloc(sizeof(types)); CHECK(raw); memcpy(raw,types,sizeof(types));
    XrtValueImageSlot *members = xr_malloc(sizeof(fields)); CHECK(members); memcpy(members,fields,sizeof(fields));
    char *names = xr_malloc(14); CHECK(names); memcpy(names,"leftrightscore",14);
    XrtValueImage incoming = image; incoming.types = raw; incoming.slots = members; incoming.names = names;
    XrtModuleSpec module = {0}; XrtModuleSlotSpec slot = {0,11,1,0,32,8};
    XrtModuleLayoutSpec spec = {&incoming,&module,1,NULL,0,&slot,1,32,8};
    XrtModuleLayout *layout = NULL;
    size_t code = 1;
    OK(xrt_module_layout_new(billing,&spec,(XrtManagedLease){&code,code_retain,code_drop},&layout)); CHECK(code == 1);
    memset(raw,0xa5,sizeof(types)); memset(members,0xa5,sizeof(fields)); memset(names,0xa5,14);
    xr_free(raw); xr_free(members); xr_free(names); code_drop(&code); CHECK(!code);
    const XrtValuePlan *plan = NULL; OK(xrt_module_layout_plan(layout,11,&plan));
    XrtValuePlanSpec view; OK(xrt_value_plan_view(plan,&view));
    CHECK(view.field_count == 3 && view.owned_count == 2 && view.owned_offsets[1] == 16 &&
        view.fields[0].name_length == 4 && !memcmp(view.fields[0].name,"left",4));
    xrt_module_layout_drop(&layout); same_bill(billing,baseline);
    spec.values = &image; slot.type = 0;
    BAD(xrt_module_layout_new(billing,&spec,(XrtManagedLease){0},&layout)); CHECK(!layout); same_bill(billing,baseline);
    slot.type = 11; slot.offset = 8; spec.payload_size = 40;
    BAD(xrt_module_layout_new(billing,&spec,(XrtManagedLease){0},&layout)); same_bill(billing,baseline);
    slot.offset = 0; spec.payload_size = 32; slot.size = 8;
    BAD(xrt_module_layout_new(billing,&spec,(XrtManagedLease){0},&layout)); same_bill(billing,baseline);
    slot.size = 32;
    XrtModuleSpec cycle[] = {{0,1},{1,1}}; uint32_t dependencies[] = {1,0};
    spec.modules = cycle; spec.module_count = 2; spec.dependencies = dependencies; spec.dependency_count = 2;
    BAD(xrt_module_layout_new(billing,&spec,(XrtManagedLease){0},&layout)); same_bill(billing,baseline);
    spec.modules = &module; spec.module_count = 1; spec.dependencies = NULL; spec.dependency_count = 0;
    xrt_billing_owner_close(billing);
    CHECK(xrt_module_layout_new(billing,&spec,(XrtManagedLease){0},&layout) == XRT_MANAGED_CLOSED);
    same_bill(billing,baseline); xrt_billing_owner_drop(billing);
    CHECK(!audit.leases && !audit.blocks && !audit.bytes);
    Audit quota = {0}; billing = billing_new(&quota,128);
    CHECK(xrt_module_layout_new(billing,&spec,(XrtManagedLease){0},&layout) == XRT_MANAGED_LIMIT && !layout);
    xrt_billing_owner_drop(billing); CHECK(!quota.leases && !quota.blocks);
    puts("raw descriptions retire independently; invalid layout, cycles, closed and quota preserve output");
}
static void deep_nullable(void) {
    enum { COUNT = 300 };
    Audit audit = {0}; XrtBillingOwner *billing = billing_new(&audit,SIZE_MAX);
    XrtValueImageType *rows = xr_malloc(COUNT * sizeof(*rows)); CHECK(rows);
    rows[0] = (XrtValueImageType){.kind=XRT_VALUE_RECORD,.size=1,.alignment=1};
    for (uint32_t i = 1; i < COUNT; ++i)
        rows[i] = (XrtValueImageType){.kind=XRT_VALUE_NULLABLE,.size=i + 1,.alignment=1,
            .nullable_element=i,.nullable_payload=1};
    XrtValueImage raw = {.types=rows,.type_count=COUNT};
    XrtModuleSpec module = {0}; XrtModuleSlotSpec slot = {0,COUNT,1,0,COUNT,1};
    XrtModuleLayoutSpec spec = {&raw,&module,1,NULL,0,&slot,1,COUNT,1};
    XrtModuleLayout *layout = NULL; OK(xrt_module_layout_new(billing,&spec,(XrtManagedLease){0},&layout));
    xr_free(rows); XrtModuleState *state = NULL; OK(xrt_module_state_new(layout,billing,&state));
    OK(xrt_module_begin(state,0)); uint8_t bytes[COUNT]; memset(bytes,1,sizeof(bytes)); bytes[COUNT - 1] = 0;
    XrtModuleRef *reference = NULL; OK(xrt_module_construct(state,0,bytes,&reference)); OK(xrt_module_publish(reference));
    OK(xrt_module_ready(state,0)); bytes[COUNT - 2] = 2; BAD(xrt_module_replace(reference,bytes));
    xrt_billing_owner_fail_after(billing,0); bytes[0] = 0; OK(xrt_module_replace(reference,bytes));
    const void *payload = NULL; OK(xrt_module_borrow(reference,&payload));
    for (size_t i = 0; i < sizeof(bytes); ++i) CHECK(!((const uint8_t *)payload)[i]);
    xrt_module_state_drop(&state); xrt_module_layout_drop(&layout); xrt_billing_owner_drop(billing);
    CHECK(!audit.leases && !audit.blocks && !audit.bytes);
    puts("deep inline Nullable traversal is iterative and needs no managed frame");
}
int main(void) {
    atomic_record(); family_borrows(); array_state_guards(); allocation_failures(); scalar_admission();
    metadata_and_rejection(); deep_nullable();
    puts("fixed module State kernel passed"); return 0;
}
