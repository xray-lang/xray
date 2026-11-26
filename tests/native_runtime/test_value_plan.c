/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_value_plan.c - Independent metadata and real inline ownership leaves
 *
 * KEY CONCEPT:
 *   Source descriptions retire independently; retained metadata, payload owners
 *   and code providers each finish only after their actual last reference.
 */
#include "runtime/native/xnative_value_plan.h"
#include "runtime/native/xnative_value_image.h"
#include "runtime/native/xnative_string_backing.h"
#include "xray_native_function.h"
#include "base/xmalloc.h"
#include "base/xmax_align.h"
#include <limits.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
#define STATUS(c,s) CHECK((c) == (s))
typedef struct Audit {
    _Atomic(size_t) leases, bytes, blocks, finalized, remaining, calls, frees;
    void *watched[2];
    const void *observed;
    bool retired[2];
    void (*deferred_drop)(void *);
    void *deferred_context;
    void (*observe)(void *, void *);
    void *observer;
} Audit;
typedef struct Fixture {
    Audit audit;
    XrtBillingOwner *billing;
    XrtBackingTypes types;
    XrtManagedRoot *root;
    XrtManagedFrame *frame;
} Fixture;
static bool provider_retain(void *context) {
    atomic_fetch_add(&((Audit *)context)->leases, 1); return true;
}
static void provider_drop(void *context) {
    Audit *audit = context;
    size_t count = atomic_fetch_sub(&audit->leases, 1); CHECK(count);
    if (count == 1) {
        CHECK(!atomic_load(&audit->bytes) && !atomic_load(&audit->blocks));
        atomic_fetch_add(&audit->finalized, 1);
    }
}
static void *provider_allocate(void *context, size_t bytes) {
    Audit *audit = context;
    atomic_fetch_add(&audit->calls, 1);
    size_t remaining = atomic_load(&audit->remaining);
    while (remaining != SIZE_MAX) {
        if (!remaining) return NULL;
        if (atomic_compare_exchange_weak(&audit->remaining, &remaining, remaining - 1)) break;
    }
    if (audit->deferred_drop) {
        void (*drop)(void *) = audit->deferred_drop;
        void *context_to_drop = audit->deferred_context;
        audit->deferred_drop = NULL; audit->deferred_context = NULL;
        drop(context_to_drop);
    }
    void *memory = xr_malloc(bytes);
    if (memory) { atomic_fetch_add(&audit->bytes, bytes); atomic_fetch_add(&audit->blocks, 1); }
    return memory;
}
static void provider_free(void *context, void *memory, size_t bytes) {
    Audit *audit = context;
    if (audit->observe) audit->observe(audit->observer, memory);
    if (audit->observed) {
        for (size_t i = 0; i < 2; ++i) {
            XrtManagedHeader *installed = NULL;
            memcpy(&installed, (const unsigned char *)audit->observed + i * sizeof(installed), sizeof(installed));
            if (audit->watched[i] == memory) { CHECK(!installed); audit->retired[i] = true; }
            if (audit->retired[i]) CHECK(!installed);
        }
    }
    CHECK(atomic_fetch_sub(&audit->bytes, bytes) >= bytes);
    CHECK(atomic_fetch_sub(&audit->blocks, 1));
    atomic_fetch_add(&audit->frees, 1);
    xr_free(memory);
}
static void fixture_init(Fixture *f, size_t limit, bool storage) {
    *f = (Fixture){0}; atomic_init(&f->audit.leases, 1); atomic_init(&f->audit.remaining, SIZE_MAX);
    XrtManagedAllocator allocator = {&f->audit, provider_allocate, provider_free,
        {&f->audit, provider_retain, provider_drop}};
    OK(xrt_billing_owner_new(&allocator, limit, &f->billing));
    if (storage) {
        OK(xrt_backing_types_init(f->billing, &f->types));
        OK(xrt_managed_root_new(f->billing, &f->root));
        OK(xrt_managed_frame_enter(f->root, 4, &f->frame));
    }
}
static void fixture_retire(Fixture *f) {
    OK(xrt_managed_root_destroy(&f->root)); f->frame = NULL;
    xrt_backing_types_drop(&f->types);
    xrt_billing_owner_drop(f->billing); f->billing = NULL;
    provider_drop(&f->audit);
}
static void fixture_done(Fixture *f) {
    fixture_retire(f); CHECK(atomic_load(&f->audit.finalized) == 1);
}
static void same_bill(Fixture *f, XrtBillingStats before) {
    XrtBillingStats after = xrt_billing_owner_stats(f->billing);
    CHECK(before.bytes == after.bytes && before.blocks == after.blocks);
    CHECK(after.bytes == atomic_load(&f->audit.bytes) && after.blocks == atomic_load(&f->audit.blocks));
}
static XrtValuePlan *new_plan(Fixture *f, XrtValuePlanSpec spec) {
    XrtValuePlan *plan = NULL; OK(xrt_value_plan_new(f->billing, &spec, &plan)); return plan;
}
static XrtValuePlan *scalar(Fixture *f, XrtValueKind kind, size_t size, size_t alignment) {
    return new_plan(f, (XrtValuePlanSpec){.kind=kind, .size=size, .alignment=alignment});
}
static XrtValuePlan *reference(Fixture *f, XrtValueKind kind) {
    size_t offset = 0;
    return new_plan(f, (XrtValuePlanSpec){.kind=kind, .size=sizeof(void *), .alignment=alignof(void *),
        .owned_offsets=&offset, .owned_count=1});
}
static XrtManagedHeader *owned_string(Fixture *f, const char *bytes) {
    OK(xrt_string_backing_create(f->frame, 0, &f->types, bytes, strlen(bytes), 0));
    XrtManagedHeader *owned = NULL; OK(xrt_managed_slot_take(f->frame, 0, &owned)); return owned;
}
typedef struct Record { XrtManagedHeader *left, *right; int64_t number; } Record;
static XrtValuePlan *record_plan(Fixture *f) {
    XrtValuePlan *text = reference(f, XRT_VALUE_STRING), *integer = scalar(f, XRT_VALUE_I64, 8, 8);
    char names[][7] = {"left", "right", "number"};
    XrtValueSlotSpec fields[] = {{text,0,names[0],4,0},{text,8,names[1],5,0},{integer,16,names[2],6,0}};
    size_t offsets[] = {0,8};
    XrtValuePlan *plan = new_plan(f, (XrtValuePlanSpec){.kind=XRT_VALUE_RECORD, .size=sizeof(Record), .alignment=alignof(Record),
        .owned_offsets=offsets, .owned_count=2, .fields=fields, .field_count=3});
    memset(fields, 0xa5, sizeof(fields)); memset(names, 'x', sizeof(names)); offsets[0] = SIZE_MAX;
    xrt_value_plan_drop(text); xrt_value_plan_drop(integer);
    return plan;
}
static void inline_owners(void) {
    Fixture metadata, data; fixture_init(&metadata, SIZE_MAX, false); fixture_init(&data, SIZE_MAX, true);
    XrtValuePlan *plan = record_plan(&metadata);
    XrtValuePlanSpec view; OK(xrt_value_plan_view(plan, &view));
    CHECK(view.size == sizeof(Record) && view.owned_offsets[0] == 0 && view.owned_offsets[1] == 8);
    CHECK(!memcmp(view.fields[0].name, "left", 4) && !memcmp(view.fields[2].name, "number", 6));
    Record first = {owned_string(&data,"left"), owned_string(&data,"right"), 42}, second, moved;
    OK(xrt_value_zero(plan, &second)); OK(xrt_value_zero(plan, &moved));
    XrtBillingStats before = xrt_billing_owner_stats(metadata.billing);
    size_t calls = atomic_load(&data.audit.calls);
    OK(xrt_value_copy_empty(plan, &first, &second)); CHECK(second.number == 42 && second.left == first.left);
    CHECK(xr_signed_rc_read(&first.left->references) == 2 && xr_signed_rc_read(&first.right->references) == 2);
    STATUS(xrt_value_copy_empty(plan, &first, &second), XRT_MANAGED_ARGUMENT);
    STATUS(xrt_value_copy_empty(plan, &first, &first), XRT_MANAGED_ARGUMENT);
    STATUS(xrt_value_move_empty(plan, &first, &first), XRT_MANAGED_ARGUMENT);
    OK(xrt_value_move_empty(plan, &second, &moved)); CHECK(!second.left && !second.right && !second.number);
    CHECK(moved.left == first.left && moved.number == 42 && xr_signed_rc_read(&first.left->references) == 2);
    xrt_value_drop(plan, &first);
    CHECK(!first.left && !first.right && !first.number && xr_signed_rc_read(&moved.left->references) == 1);
    CHECK(atomic_load(&data.audit.calls) == calls); same_bill(&metadata, before);
    fixture_retire(&data); CHECK(!atomic_load(&data.audit.finalized));
    data.audit.observed = &moved.left; data.audit.watched[0] = moved.left; data.audit.watched[1] = moved.right;
    xrt_value_drop(plan, &moved);
    CHECK(data.audit.retired[0] && data.audit.retired[1] && atomic_load(&data.audit.finalized) == 1);
    CHECK(!moved.left && !moved.right && !moved.number);
    xrt_value_plan_drop(plan); fixture_done(&metadata);
    puts("inline record copy/move/drop, source retirement and visible sibling clearing passed");
}
static void retain_failure_and_alias(void) {
    Fixture f; fixture_init(&f, SIZE_MAX, true);
    XrtValuePlan *plan = record_plan(&f);
    Record source = {owned_string(&f,"first"), owned_string(&f,"second"), 99}, empty;
    OK(xrt_value_zero(plan, &empty)); empty.number = 77;
    atomic_store(&source.left->references, -1);
    atomic_store(&source.right->references, INT32_MAX);
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    size_t calls = atomic_load(&f.audit.calls);
    STATUS(xrt_value_copy_empty(plan, &source, &empty), XRT_MANAGED_REFCOUNT);
    CHECK(!empty.left && !empty.right && !empty.number);
    CHECK(xr_signed_rc_read(&source.left->references) == -1 && source.number == 99);
    atomic_store(&source.right->references, 1);
    xrt_managed_drop(source.right);
    source.right = source.left; CHECK(xrt_managed_retain(source.right));
    OK(xrt_value_copy_empty(plan, &source, &empty));
    CHECK(xr_signed_rc_read(&source.left->references) == -4);
    xrt_value_drop(plan, &source); CHECK(xr_signed_rc_read(&empty.left->references) == -2);
    xrt_value_drop(plan, &empty);
    CHECK(atomic_load(&f.audit.calls) == calls);
    CHECK(xrt_billing_owner_stats(f.billing).bytes < before.bytes);
    XrtValuePlan *octet = scalar(&f,XRT_VALUE_U8,1,1);
    XrtValuePlan *pod = new_plan(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_FIXED_INT,.size=8,.alignment=1,
        .count=8,.element=octet});
    unsigned char overlapping[16]; memset(overlapping, 0x5a, sizeof(overlapping));
    STATUS(xrt_value_copy_empty(pod, overlapping, overlapping+1), XRT_MANAGED_ARGUMENT);
    STATUS(xrt_value_move_empty(pod, overlapping+1, overlapping), XRT_MANAGED_ARGUMENT);
    for (size_t i = 0; i < sizeof(overlapping); ++i) CHECK(overlapping[i] == 0x5a);
    xrt_value_plan_drop(pod); xrt_value_plan_drop(octet);
    xrt_value_plan_drop(plan); fixture_done(&f);
    puts("inline partial retain rollback, duplicate real owners and overlap rejection passed");
}
static void nullable_values(void) {
    Fixture f; fixture_init(&f, SIZE_MAX, true);
    XrtValuePlan *text = reference(&f, XRT_VALUE_STRING);
    size_t offset = 8;
    XrtValuePlanSpec spec = {.kind=XRT_VALUE_NULLABLE,.size=16,.alignment=8,.owned_offsets=&offset,.owned_count=1,
        .nullable={0,8,text}};
    XrtValuePlan *optional = new_plan(&f,spec);
    size_t outer_offset = 16;
    XrtValuePlan *outer = new_plan(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_NULLABLE,.size=24,.alignment=8,
        .owned_offsets=&outer_offset,.owned_count=1,.nullable={0,8,optional}});
    xrt_value_plan_drop(text); xrt_value_plan_drop(optional);
    alignas(XrMaxAlignment) unsigned char none[24], some[24], copy[24];
    memset(none,0xa5,sizeof(none)); OK(xrt_value_zero(outer,none)); OK(xrt_value_zero(outer,some)); OK(xrt_value_zero(outer,copy));
    XrtManagedHeader *child = (XrtManagedHeader *)(uintptr_t)1;
    memcpy(&child,none+16,sizeof(child)); CHECK(!child && !none[0] && !none[8]);
    OK(xrt_value_copy_empty(outer,none,copy)); CHECK(!memcmp(none,copy,sizeof(none)));
    child = owned_string(&f,"some"); memcpy(some+16,&child,sizeof(child)); some[0] = some[8] = 1;
    OK(xrt_value_copy_empty(outer,some,copy)); CHECK(xr_signed_rc_read(&child->references)==2);
    xrt_value_drop(outer,some); CHECK(xr_signed_rc_read(&child->references)==1);
    CHECK(!memcmp(some,none,sizeof(none))); xrt_value_drop(outer,copy);
    CHECK(!memcmp(copy,none,sizeof(none)));
    XrtValuePlan *unit = scalar(&f,XRT_VALUE_UNIT,0,1);
    OK(xrt_value_zero(unit,NULL)); OK(xrt_value_copy_empty(unit,NULL,NULL)); OK(xrt_value_move_empty(unit,NULL,NULL)); xrt_value_drop(unit,NULL);
    XrtValuePlan *unit_optional = new_plan(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_NULLABLE,.size=1,.alignment=1,.nullable={0,1,unit}});
    uint8_t byte = 0xa5; OK(xrt_value_zero(unit_optional,&byte)); CHECK(!byte);
    xrt_value_plan_drop(unit_optional); xrt_value_plan_drop(unit); xrt_value_plan_drop(outer); fixture_done(&f);
    puts("Nullable None true NULL inventory, nested Some owners and Unit payloads passed");
}
typedef struct CodeAudit { _Atomic(size_t) retains, drops, finalized; bool reject; XrtBillingOwner *billing; size_t final_bytes; } CodeAudit;
typedef struct CodeOwner { _Atomic(size_t) references; CodeAudit *audit; char *constants; } CodeOwner;
static bool code_retain(void *context) {
    CodeOwner *code = context;
    if (code->audit->reject) return false;
    atomic_fetch_add(&code->references,1); atomic_fetch_add(&code->audit->retains,1); return true;
}
static void code_drop(void *context) {
    CodeOwner *code = context; CodeAudit *audit = code->audit;
    atomic_fetch_add(&audit->drops,1);
    if (atomic_fetch_sub(&code->references,1)==1) {
        CHECK(!memcmp(code->constants,"unbound bytes",13));
        if (audit->billing) CHECK(xrt_billing_owner_stats(audit->billing).bytes==audit->final_bytes);
        xr_free(code->constants); xr_free(code); atomic_fetch_add(&audit->finalized,1);
    }
}
static CodeOwner *code_new(CodeAudit *audit) {
    CodeOwner *owner = xr_malloc(sizeof(*owner)); CHECK(owner);
    owner->constants = xr_malloc(14); CHECK(owner->constants); memcpy(owner->constants,"unbound bytes",14);
    atomic_init(&owner->references,1); owner->audit = audit; return owner;
}
static void typed_metadata_and_code(void) {
    Fixture f; fixture_init(&f,SIZE_MAX,false);
    size_t owner_bytes=xrt_billing_owner_stats(f.billing).bytes;
    XrtValuePlan *integer = scalar(&f,XRT_VALUE_I64,8,8), *map = reference(&f,XRT_VALUE_MAP_STRING_I64);
    size_t pointer_offset = 0;
    XrtValuePlan *cell = new_plan(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_CELL,.size=8,.alignment=8,
        .owned_offsets=&pointer_offset,.owned_count=1,.element=map});
    char argument[]="value", capture[]="cache";
    XrtValueParameterSpec parameter={integer,XR_PARAM_READ,false,argument,5};
    size_t function_owner=offsetof(XrtFunction,owner);
    XrtValuePlan *signature = new_plan(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_FUNCTION,.size=sizeof(XrtFunction),.alignment=alignof(XrtFunction),
        .owned_offsets=&function_owner,.owned_count=1,.signature={&parameter,1,integer,XRT_VALUE_NO_SUSPEND,false}});
    CodeAudit audit={0}; CodeOwner *code=code_new(&audit);
    XrtValueSlotSpec captures[]={{.type = integer},{cell,8,capture,5,0},{.type = signature, .offset = 16}};
    size_t owned[]={8,32};
    XrtValuePlan *env = new_plan(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_ENVIRONMENT,.size=40,.alignment=8,
        .owned_offsets=owned,.owned_count=2,.captures=captures,.capture_count=3,.entry={7,signature},
        .code={code,code_retain,code_drop}});
    memset(argument,'x',5); memset(capture,'x',5); memset(captures,0xa5,sizeof(captures));
    memset(&parameter,0xa5,sizeof(parameter)); owned[0]=SIZE_MAX;
    xrt_value_plan_drop(signature); xrt_value_plan_drop(cell); xrt_value_plan_drop(map); xrt_value_plan_drop(integer);
    code_drop(code); CHECK(!atomic_load(&audit.finalized));
    XrtValuePlanSpec view, fn, captured_cell;
    OK(xrt_value_plan_view(env,&view)); CHECK(view.entry.ordinal==7 && view.capture_count==3 && view.owned_offsets[1]==32);
    CHECK(!memcmp(view.captures[1].name,"cache",5));
    OK(xrt_value_plan_view(view.entry.signature,&fn));
    CHECK(fn.signature.flags==XRT_VALUE_NO_SUSPEND && fn.signature.parameter_count==1 && !memcmp(fn.signature.parameters[0].name,"value",5));
    CHECK(fn.signature.parameters[0].mode==XR_PARAM_READ && !fn.signature.parameters[0].escaping);
    OK(xrt_value_plan_view(view.captures[1].type,&captured_cell)); CHECK(captured_cell.kind==XRT_VALUE_CELL && captured_cell.element);
    audit.billing=f.billing; audit.final_bytes=xrt_billing_owner_stats(f.billing).bytes;
    XrtBillingStats held=xrt_billing_owner_stats(f.billing);
    CHECK(held.blocks==6); /* Billing plus five independently owned metadata blocks. */
    XrtManagedLease lease=xrt_value_plan_lease(env);
    XrtRuntimeTypePlan *physical=NULL;
    XrtRuntimeTypeSpec physical_spec={.fixed_size=16,.alignment=8,.capacity_offset=SIZE_MAX,.code=lease};
    OK(xrt_runtime_type_plan_new(f.billing,&physical_spec,&physical));
    CHECK(atomic_load(&audit.retains)==1);
    xrt_value_plan_drop(env); CHECK(!atomic_load(&audit.finalized));
    audit.final_bytes=owner_bytes;
    xrt_billing_owner_close(f.billing);
    fixture_retire(&f); CHECK(!atomic_load(&f.audit.finalized));
    xrt_runtime_type_plan_drop(physical);
    CHECK(atomic_load(&audit.finalized)==1 && atomic_load(&audit.drops)==2 && atomic_load(&f.audit.finalized)==1);
    puts("typed captures, signature copies and independent CodeOwner/provider retirement passed");
}
static void function_shape_admission(void) {
    Fixture f; fixture_init(&f,SIZE_MAX,false);
    size_t owner_bytes=xrt_billing_owner_stats(f.billing).bytes;
    XrtValuePlan *integer=scalar(&f,XRT_VALUE_I64,8,8), *empty=NULL;
    size_t offset=offsetof(XrtFunction,owner);
    CodeAudit audit={0}; CodeOwner *code=code_new(&audit);
    XrtValuePlanSpec function={.kind=XRT_VALUE_FUNCTION,.size=sizeof(XrtFunction),.alignment=alignof(XrtFunction),
        .owned_offsets=&offset,.owned_count=1,.signature={.result=integer},.code={code,code_retain,code_drop}};
    XrtBillingStats baseline=xrt_billing_owner_stats(f.billing);
    size_t calls=atomic_load(&f.audit.calls);
    for (unsigned fault=0;fault<6;++fault) {
        XrtValuePlanSpec rejected=function; size_t wrong=0;
        switch (fault) {
        case 0: rejected.size-=alignof(XrtFunction); break;
        case 1: rejected.size+=alignof(XrtFunction); break;
        case 2: rejected.alignment=alignof(XrtFunction)/2; break;
        case 3: wrong=offsetof(XrtFunction,entry); rejected.owned_offsets=&wrong; break;
        case 4: wrong=offsetof(XrtFunction,environment); rejected.owned_offsets=&wrong; break;
        default: rejected.owned_count=0; rejected.owned_offsets=NULL; break;
        }
        STATUS(xrt_value_plan_new(f.billing,&rejected,&empty),XRT_MANAGED_ARGUMENT);
        CHECK(!empty && atomic_load(&code->references)==1); same_bill(&f,baseline);
    }
    CHECK(atomic_load(&f.audit.calls)==calls && !atomic_load(&audit.retains));
    atomic_store(&f.audit.remaining,0);
    STATUS(xrt_value_plan_new(f.billing,&function,&empty),XRT_MANAGED_OOM);
    CHECK(!empty && atomic_load(&code->references)==1); same_bill(&f,baseline);
    atomic_store(&f.audit.remaining,SIZE_MAX);
    audit.reject=true;
    STATUS(xrt_value_plan_new(f.billing,&function,&empty),XRT_MANAGED_REFCOUNT);
    CHECK(!empty); same_bill(&f,baseline); audit.reject=false;
    xrt_billing_owner_fail_after(f.billing,0);
    STATUS(xrt_value_plan_new(f.billing,&function,&empty),XRT_MANAGED_OOM);
    CHECK(!empty && atomic_load(&code->references)==1); same_bill(&f,baseline);
    xrt_billing_owner_fail_after(f.billing,SIZE_MAX);
    OK(xrt_value_plan_new(f.billing,&function,&empty));
    XrtValuePlanSpec received; OK(xrt_value_plan_view(empty,&received));
    CHECK(received.size==sizeof(XrtFunction) && received.alignment==alignof(XrtFunction) &&
        received.owned_offsets[0]==offsetof(XrtFunction,owner));
    code_drop(code); xrt_value_plan_drop(integer);
    audit.billing=f.billing; audit.final_bytes=owner_bytes;
    fixture_retire(&f); CHECK(!atomic_load(&audit.finalized) && !atomic_load(&f.audit.finalized));
    /* The plan's physical refund precedes the code callback and billing drop. */
    xrt_value_plan_drop(empty);
    CHECK(atomic_load(&audit.finalized)==1 && atomic_load(&f.audit.finalized)==1);
    puts("canonical callable shape: malformed storage rejection, OOM/lease rollback and independent code lifetime passed");
}
static void failures_and_admission(void) {
    Fixture f; fixture_init(&f,8192,false);
    XrtValuePlan *integer=scalar(&f,XRT_VALUE_I64,8,8), *text=reference(&f,XRT_VALUE_STRING);
    XrtValuePlan *octet=scalar(&f,XRT_VALUE_U8,1,1);
    XrtValuePlan *array=reference(&f,XRT_VALUE_ARRAY_I64);
    XrtBillingStats before=xrt_billing_owner_stats(f.billing);
    CodeAudit audit={0}; CodeOwner *code=code_new(&audit);
    XrtValuePlanSpec plain={.kind=XRT_VALUE_I64,.size=8,.alignment=8,.code={code,code_retain,code_drop}};
    XrtValuePlan *empty=NULL;
    atomic_store(&f.audit.remaining,0);
    STATUS(xrt_value_plan_new(f.billing,&plain,&empty),XRT_MANAGED_OOM); CHECK(!empty); same_bill(&f,before);
    atomic_store(&f.audit.remaining,SIZE_MAX);
    xrt_billing_owner_fail_after(f.billing,0);
    STATUS(xrt_value_plan_new(f.billing,&plain,&empty),XRT_MANAGED_OOM); CHECK(!empty); same_bill(&f,before);
    xrt_billing_owner_fail_after(f.billing,SIZE_MAX);
    audit.reject=true;
    STATUS(xrt_value_plan_new(f.billing,&plain,&empty),XRT_MANAGED_REFCOUNT); CHECK(!empty); same_bill(&f,before);
    audit.reject=false;
    XrtValueSlotSpec fields[]={{array,0,"array",5,0}}; size_t zero=0;
    XrtValuePlanSpec record={.kind=XRT_VALUE_RECORD,.size=8,.alignment=8,.fields=fields,.field_count=1,.owned_offsets=&zero,.owned_count=1};
    OK(xrt_value_plan_new(f.billing,&record,&empty));
    XrtValuePlanSpec received;
    OK(xrt_value_plan_view(empty,&received));
    CHECK(received.fields[0].type==array && received.owned_count==1 && received.owned_offsets[0]==0);
    xrt_value_plan_drop(empty); empty=NULL;
    XrtValuePlan *unadmitted=reference(&f,XRT_VALUE_ARRAY_U64);
    fields[0].type=unadmitted;
    STATUS(xrt_value_plan_new(f.billing,&record,&empty),XRT_MANAGED_ARGUMENT); CHECK(!empty);
    XrtValuePlanSpec tuple=record; tuple.kind=XRT_VALUE_TUPLE;
    XrtValueSlotSpec positional={.type = unadmitted}; tuple.fields=&positional;
    STATUS(xrt_value_plan_new(f.billing,&tuple,&empty),XRT_MANAGED_ARGUMENT); CHECK(!empty);
    xrt_value_plan_drop(unadmitted); fields[0].type=array; positional.type=array;
    OK(xrt_value_plan_new(f.billing,&tuple,&empty));
    OK(xrt_value_plan_view(empty,&received));
    CHECK(received.fields[0].type==array && received.owned_count==1 && received.owned_offsets[0]==0);
    xrt_value_plan_drop(empty); empty=NULL;
    same_bill(&f,before);
    XrtValuePlanSpec nullable={.kind=XRT_VALUE_NULLABLE,.size=16,.alignment=8,.owned_offsets=&zero,.owned_count=1,
        .nullable={0,8,array}};
    STATUS(xrt_value_plan_new(f.billing,&nullable,&empty),XRT_MANAGED_ARGUMENT);
    XrtValuePlanSpec cell={.kind=XRT_VALUE_CELL,.size=8,.alignment=8,.owned_offsets=&zero,.owned_count=1,.element=text};
    STATUS(xrt_value_plan_new(f.billing,&cell,&empty),XRT_MANAGED_ARGUMENT);
    fields[0].type=text; size_t wrong=8; record.owned_offsets=&wrong;
    STATUS(xrt_value_plan_new(f.billing,&record,&empty),XRT_MANAGED_ARGUMENT); record.owned_offsets=&zero;
    fields[0].offset=1; STATUS(xrt_value_plan_new(f.billing,&record,&empty),XRT_MANAGED_ARGUMENT); fields[0].offset=0;
    XrtValueParameterSpec invalid={integer,XR_PARAM_READ,true,NULL,0}; size_t owner=16;
    XrtValuePlanSpec function={.kind=XRT_VALUE_FUNCTION,.size=24,.alignment=8,.owned_offsets=&owner,.owned_count=1,
        .signature={&invalid,1,integer,0,false}};
    STATUS(xrt_value_plan_new(f.billing,&function,&empty),XRT_MANAGED_ARGUMENT);
    invalid.escaping=false; function.signature.throws=true;
    STATUS(xrt_value_plan_new(f.billing,&function,&empty),XRT_MANAGED_ARGUMENT);
    XrtValuePlanSpec overflow=record; overflow.field_count=SIZE_MAX;
    STATUS(xrt_value_plan_new(f.billing,&overflow,&empty),XRT_MANAGED_OVERFLOW);
    overflow=record; fields[0].name_length=SIZE_MAX;
    STATUS(xrt_value_plan_new(f.billing,&overflow,&empty),XRT_MANAGED_OVERFLOW); fields[0].name_length=5;
    same_bill(&f,before);
    plain=(XrtValuePlanSpec){.kind=XRT_VALUE_FIXED_INT,.size=16000,.alignment=1,.count=16000,
        .element=octet,.code={code,code_retain,code_drop}};
    /* Names consume actual metadata bytes; the inline payload itself is not allocated. */
    char *large_name=xr_malloc(16000); CHECK(large_name); memset(large_name,'a',16000);
    fields[0]=(XrtValueSlotSpec){text,0,large_name,16000,0}; record.code=(XrtManagedLease){code,code_retain,code_drop};
    STATUS(xrt_value_plan_new(f.billing,&record,&empty),XRT_MANAGED_LIMIT); CHECK(!empty); xr_free(large_name); same_bill(&f,before);
    void *memory=NULL; STATUS(xrt_managed_metadata_allocate(f.billing,0,&memory),XRT_MANAGED_ARGUMENT);
    OK(xrt_managed_metadata_allocate(f.billing,31,&memory)); CHECK(memory);
    void *occupied=memory; STATUS(xrt_managed_metadata_allocate(f.billing,1,&occupied),XRT_MANAGED_ARGUMENT); CHECK(occupied==memory);
    xrt_managed_metadata_free(f.billing,memory,31); same_bill(&f,before);
    xrt_billing_owner_close(f.billing);
    STATUS(xrt_value_plan_new(f.billing,&plain,&empty),XRT_MANAGED_CLOSED); CHECK(!empty); same_bill(&f,before);
    code_drop(code); CHECK(atomic_load(&audit.finalized)==1);
    xrt_value_plan_drop(array); xrt_value_plan_drop(text); xrt_value_plan_drop(integer);
    xrt_value_plan_drop(octet); fixture_done(&f);
    puts("metadata admission, allocation/quota/lease failures and exact cold refunds passed");
}
static void deep_metadata(void) {
    Fixture f; fixture_init(&f,SIZE_MAX,false);
    XrtBillingStats baseline=xrt_billing_owner_stats(f.billing);
    XrtValuePlan *current=scalar(&f,XRT_VALUE_I64,8,8);
    for (size_t i=0;i<100000;++i) {
        XrtValuePlan *next=new_plan(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_NULLABLE,.size=(i+2)*8,.alignment=8,
            .nullable={0,8,current}});
        xrt_value_plan_drop(current); current=next;
    }
    size_t calls=atomic_load(&f.audit.calls);
    atomic_store(&f.audit.remaining,0); xrt_billing_owner_fail_after(f.billing,0);
    xrt_value_plan_drop(current); CHECK(atomic_load(&f.audit.calls)==calls);
    same_bill(&f,baseline); fixture_done(&f);
    puts("100000 immutable metadata dependencies retired iteratively on the default stack");
}
typedef struct RawImageFixture {
    XrtValueImageType types[7];
    XrtValueImageSlot slots[4];
    XrtValueImageParameter parameter;
    size_t owned[5];
    char names[20];
    XrtValueImage image;
} RawImageFixture;
static void raw_image_fixture(RawImageFixture *raw) {
    *raw=(RawImageFixture){0};
    memcpy(raw->names,"leftrightnumbervalue",20);
    raw->owned[0]=0; raw->owned[1]=8; raw->owned[2]=8; raw->owned[3]=16; raw->owned[4]=offsetof(XrtFunction,owner);
    raw->slots[0]=(XrtValueImageSlot){2,0,0,0,4};
    raw->slots[1]=(XrtValueImageSlot){2,0,8,4,5};
    raw->slots[2]=(XrtValueImageSlot){1,0,16,9,6};
    raw->slots[3]=(XrtValueImageSlot){1,0,0,0,0};
    raw->parameter=(XrtValueImageParameter){1,XR_PARAM_READ,0,15,5};
    raw->types[0]=(XrtValueImageType){.kind=XRT_VALUE_I64,.size=8,.alignment=8};
    raw->types[1]=(XrtValueImageType){.kind=XRT_VALUE_STRING,.size=8,.alignment=8,.owned_count=1};
    raw->types[2]=(XrtValueImageType){.kind=XRT_VALUE_RECORD,.size=24,.alignment=8,.owned_count=2,.field_count=3};
    raw->types[3]=(XrtValueImageType){.kind=XRT_VALUE_NULLABLE,.size=32,.alignment=8,
        .first_owned=2,.owned_count=2,.nullable_element=3,.nullable_payload=8};
    raw->types[4]=(XrtValueImageType){.kind=XRT_VALUE_RECORD,.size=1,.alignment=1};
    raw->types[5]=(XrtValueImageType){.kind=XRT_VALUE_FUNCTION,.size=sizeof(XrtFunction),.alignment=alignof(XrtFunction),
        .first_owned=4,.owned_count=1,.parameter_count=1,.result=1,.flags=XRT_VALUE_NO_SUSPEND};
    raw->types[6]=(XrtValueImageType){.kind=XRT_VALUE_ENVIRONMENT,.size=8,.alignment=8,
        .first_capture=3,.capture_count=1,.entry_ordinal=42,.entry_signature=6};
    raw->image=(XrtValueImage){raw->types,7,raw->slots,4,&raw->parameter,1,raw->owned,5,raw->names,20,NULL,0};
}
static void raw_image_retirement(void) {
    Fixture metadata,data; fixture_init(&metadata,SIZE_MAX,false); fixture_init(&data,SIZE_MAX,true);
    RawImageFixture raw; raw_image_fixture(&raw);
    CodeAudit audit={0}; CodeOwner *code=code_new(&audit);
    /* The provider retires the creator during the first receiving allocation. */
    metadata.audit.deferred_drop=code_drop; metadata.audit.deferred_context=code;
    XrtValueImagePlans *set=NULL;
    OK(xrt_value_image_bind(metadata.billing,&raw.image,(XrtManagedLease){code,code_retain,code_drop},&set));
    CHECK(xrt_value_image_count(set)==7 && !metadata.audit.deferred_drop && !atomic_load(&audit.finalized));
    const XrtValuePlan *record=NULL,*nullable=NULL,*environment=NULL;
    OK(xrt_value_image_get(set,3,&record)); OK(xrt_value_image_get(set,4,&nullable));
    OK(xrt_value_image_get(set,7,&environment));
    CHECK(xrt_value_plan_retain((XrtValuePlan *)record) && xrt_value_plan_retain((XrtValuePlan *)nullable) &&
        xrt_value_plan_retain((XrtValuePlan *)environment));
    memset(&raw,0xa5,sizeof(raw));
    xrt_value_image_drop(&set); CHECK(!set);
    fixture_retire(&metadata); CHECK(!atomic_load(&metadata.audit.finalized));
    XrtValuePlanSpec view={0}; OK(xrt_value_plan_view(environment,&view));
    CHECK(view.entry.ordinal==42 && !memcmp(((CodeOwner *)view.code.context)->constants,"unbound bytes",13));
    OK(xrt_value_plan_view(record,&view));
    CHECK(view.field_count==3 && view.owned_count==2 && !memcmp(view.fields[0].name,"left",4));
    Record source={owned_string(&data,"left"),owned_string(&data,"right"),91};
    fixture_retire(&data); CHECK(!atomic_load(&data.audit.finalized));
    Record copied={0}; OK(xrt_value_copy_empty(record,&source,&copied));
    CHECK(copied.number==91 && ((XrtStringBacking *)copied.left)->length==4);
    xrt_value_drop(record,&source);
    typedef struct OptionalRecord { uint8_t present; Record payload; } OptionalRecord;
    _Static_assert(sizeof(OptionalRecord)==32 && offsetof(OptionalRecord,payload)==8,
        "optional records have an inline tag and naturally aligned payload");
    OptionalRecord some={.present=1}, received={0};
    OK(xrt_value_move_empty(record,&copied,&some.payload));
    OK(xrt_value_copy_empty(nullable,&some,&received));
    CHECK(received.present==1 && received.payload.number==91);
    xrt_value_drop(nullable,&some); xrt_value_drop(nullable,&received);
    CHECK(atomic_load(&data.audit.finalized)==1);
    xrt_value_plan_drop((XrtValuePlan *)environment); CHECK(atomic_load(&audit.finalized)==1);
    CHECK(!atomic_load(&metadata.audit.finalized));
    xrt_value_plan_drop((XrtValuePlan *)nullable); xrt_value_plan_drop((XrtValuePlan *)record);
    CHECK(atomic_load(&metadata.audit.finalized)==1);
    puts("raw value image: creator retirement, independent names/code and real inline owner copies passed");
}
static void raw_image_failures(void) {
    Fixture f; fixture_init(&f,8192,false);
    XrtBillingStats baseline=xrt_billing_owner_stats(f.billing);
    RawImageFixture raw; raw_image_fixture(&raw);
    CodeAudit audit={0}; CodeOwner *code=code_new(&audit);
    XrtManagedLease lease={code,code_retain,code_drop};
    XrtValueImagePlans *set=NULL;
    for (unsigned fault=0;fault<10;++fault) {
        raw_image_fixture(&raw);
        switch (fault) {
        case 0: raw.types[2].first_field=UINT32_MAX; break;
        case 1: raw.slots[0].type=3; break;
        case 2: raw.types[5].result=7; break;
        case 3: raw.parameter.mode=UINT32_MAX; break;
        case 4: raw.image.name_bytes=1; break;
        case 5: raw.owned[0]=SIZE_MAX; break;
        case 6: raw.types[6].alignment=3; break;
        case 7: raw.image.types=NULL; break;
        case 8: raw.types[5].size+=alignof(XrtFunction); break;
        default: raw.owned[4]=offsetof(XrtFunction,environment); break;
        }
        STATUS(xrt_value_image_bind(f.billing,&raw.image,lease,&set),XRT_MANAGED_ARGUMENT);
        CHECK(!set && atomic_load(&code->references)==1); same_bill(&f,baseline);
    }
    raw_image_fixture(&raw); audit.reject=true;
    STATUS(xrt_value_image_bind(f.billing,&raw.image,lease,&set),XRT_MANAGED_REFCOUNT);
    audit.reject=false;
    atomic_store(&f.audit.remaining,0);
    STATUS(xrt_value_image_bind(f.billing,&raw.image,lease,&set),XRT_MANAGED_OOM);
    atomic_store(&f.audit.remaining,SIZE_MAX);
    size_t sites=0;
    for (size_t i=0;i<32;++i) {
        xrt_billing_owner_fail_after(f.billing,i);
        XrtManagedStatus status=xrt_value_image_bind(f.billing,&raw.image,lease,&set);
        xrt_billing_owner_fail_after(f.billing,SIZE_MAX);
        if (status==XRT_MANAGED_OK) { xrt_value_image_drop(&set); break; }
        CHECK(status==XRT_MANAGED_OOM && !set); ++sites;
        CHECK(atomic_load(&code->references)==1); same_bill(&f,baseline);
    }
    CHECK(sites>=9 && !set && atomic_load(&code->references)==1); same_bill(&f,baseline);
    char *large=xr_malloc(16000); CHECK(large); memset(large,'x',16000);
    raw.slots[0].name_length=16000; raw.image.names=large; raw.image.name_bytes=16000;
    STATUS(xrt_value_image_bind(f.billing,&raw.image,lease,&set),XRT_MANAGED_LIMIT);
    xr_free(large); CHECK(!set && atomic_load(&code->references)==1); same_bill(&f,baseline);
    XrtValueImage empty={0};
    size_t calls=atomic_load(&f.audit.calls);
    xrt_billing_owner_fail_after(f.billing,0);
    OK(xrt_value_image_bind(f.billing,&empty,(XrtManagedLease){0},&set));
    CHECK(!set && !xrt_value_image_count(set) && atomic_load(&f.audit.calls)==calls);
    xrt_billing_owner_fail_after(f.billing,SIZE_MAX);
    code_drop(code); CHECK(atomic_load(&audit.finalized)==1); fixture_done(&f);
    printf("raw value image: malformed spans/forward references, %zu OOM sites and quota refunds passed\n",sites);
}
static void empty_record_storage(void) {
    Fixture f; fixture_init(&f,SIZE_MAX,false);
    XrtBillingStats baseline=xrt_billing_owner_stats(f.billing);
    XrtValuePlan *record=NULL;
    XrtValuePlanSpec spec={.kind=XRT_VALUE_RECORD,.size=1,.alignment=1};
    OK(xrt_value_plan_new(f.billing,&spec,&record));
    XrtValuePlanSpec view={0}; OK(xrt_value_plan_view(record,&view));
    CHECK(view.size==1 && view.alignment==1 && !view.field_count && !view.owned_count);
    unsigned char source=37,copy=91;
    OK(xrt_value_copy_empty(record,&source,&copy)); CHECK(copy==37 && source==37);
    OK(xrt_value_move_empty(record,&source,&copy)); CHECK(copy==37 && !source);
    xrt_value_drop(record,&copy); CHECK(!copy);
    XrtValuePlan *nullable=new_plan(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_NULLABLE,
        .size=2,.alignment=1,.nullable={0,1,record}});
    unsigned char present[2]={1,0},received[2]={0};
    OK(xrt_value_copy_empty(nullable,present,received)); CHECK(received[0]==1 && !received[1]);
    xrt_value_drop(nullable,received); CHECK(!received[0] && !received[1]);
    XrtValuePlan *invalid=NULL;
    spec.size=0; STATUS(xrt_value_plan_new(f.billing,&spec,&invalid),XRT_MANAGED_ARGUMENT);
    CHECK(!invalid);
    xrt_value_plan_drop(record); xrt_value_plan_drop(nullable);
    same_bill(&f,baseline); fixture_done(&f);
    puts("empty records keep a shared one-byte C11 home without logical fields or owners");
}
typedef struct InlineWatch {
    XrtInlineValueHome home;
    XrtInlineValueSpec spec;
    Record expected;
    XrtManagedHeader *old[2];
    size_t retired;
} InlineWatch;
static void inline_observe(void *context, void *memory) {
    InlineWatch *watch = context;
    if (memory != watch->old[0] && memory != watch->old[1]) return;
    const Record *value = watch->home.value, *temporary = watch->home.temporary;
    CHECK(value->left == watch->expected.left && value->right == watch->expected.right && value->number == watch->expected.number);
    CHECK(!temporary->number);
    STATUS(xrt_inline_value_replace(&watch->home, &watch->spec, NULL), XRT_MANAGED_ARGUMENT);
    STATUS(xrt_managed_slot_drop(watch->home.frame, watch->home.first_owner), XRT_MANAGED_ARGUMENT);
    ++watch->retired;
}
static void inline_frame_replacement(void) {
    Fixture f; fixture_init(&f, SIZE_MAX, true);
    size_t leaves[] = {offsetof(Record, left), offsetof(Record, right)};
    size_t frame_leaves[] = {offsetof(Record, left), offsetof(Record, right),
        sizeof(Record) + offsetof(Record, left), sizeof(Record) + offsetof(Record, right)};
    XrtManagedFrameSpec frame_spec = {4, sizeof(Record) * 2, alignof(Record), frame_leaves};
    XrtManagedFrame *frame = NULL; OK(xrt_managed_frame_enter_layout(f.root, &frame_spec, &frame));
    void *bytes = NULL; size_t extent = 0; OK(xrt_managed_frame_payload(frame, &bytes, &extent));
    CHECK(extent == sizeof(Record) * 2);
    XrtInlineValueHome home = {frame, bytes, (unsigned char *)bytes + sizeof(Record), 0, 2};
    XrtInlineValueSpec spec = {sizeof(Record), alignof(Record), leaves, 2};
    Record initial = {owned_string(&f, "old-left"), owned_string(&f, "old-right"), 7};
    OK(xrt_inline_value_replace(&home, &spec, &initial));
    xrt_managed_drop(initial.left); xrt_managed_drop(initial.right);
    initial = *(Record *)home.value;
    Record next = {owned_string(&f, "next-left"), owned_string(&f, "next-right"), 42};
    XrtBillingStats baseline = xrt_billing_owner_stats(f.billing);
    int32_t references = xr_signed_rc_read(&next.right->references);
    atomic_store(&next.right->references, INT32_MIN);
    STATUS(xrt_inline_value_replace(&home, &spec, &next), XRT_MANAGED_REFCOUNT);
    atomic_store(&next.right->references, references);
    CHECK(!memcmp(home.value, &initial, sizeof(initial)) && !((Record *)home.temporary)->left &&
        !((Record *)home.temporary)->right && !((Record *)home.temporary)->number);
    CHECK(xr_signed_rc_read(&next.left->references) == 1); same_bill(&f, baseline);
    InlineWatch watch = {home, spec, next, {initial.left, initial.right}, 0};
    f.audit.observe = inline_observe; f.audit.observer = &watch;
    atomic_store(&f.audit.remaining, 0);
    size_t allocations = atomic_load(&f.audit.calls);
    OK(xrt_inline_value_replace(&home, &spec, &next));
    f.audit.observe = NULL;
    CHECK(watch.retired == 2 && atomic_load(&f.audit.calls) == allocations);
    xrt_managed_drop(next.left); xrt_managed_drop(next.right);
    next = *(Record *)home.value;
    OK(xrt_inline_value_replace(&home, &spec, home.value));
    CHECK(!memcmp(home.value, &next, sizeof(next)) && xr_signed_rc_read(&next.left->references) == 1 &&
        xr_signed_rc_read(&next.right->references) == 1);
    XrtInlineValueHome invalid = home; invalid.temporary = home.value;
    STATUS(xrt_inline_value_replace(&invalid, &spec, &next), XRT_MANAGED_ARGUMENT);
    invalid = home; ++invalid.temporary_first_owner;
    STATUS(xrt_inline_value_replace(&invalid, &spec, &next), XRT_MANAGED_ARGUMENT);
    STATUS(xrt_inline_value_replace(&home, &spec, home.temporary), XRT_MANAGED_ARGUMENT);
    atomic_store(&f.audit.remaining, SIZE_MAX);
    XrtManagedHeader *pending = NULL;
    OK(xrt_managed_slot_prepare(frame, 2, f.types.string, sizeof(XrtStringBacking) + 2, &pending));
    XrtManagedSlotLocation location; OK(xrt_managed_slot_location(frame, 2, &location));
    STATUS(xrt_inline_value_replace(&home, &spec, &next), XRT_MANAGED_ARGUMENT);
    CHECK(location.pending->value == pending && !memcmp(home.value, &next, sizeof(next)));
    OK(xrt_managed_slot_abort(frame, 2));
    watch = (InlineWatch){home, spec, {0}, {next.left, next.right}, 0};
    f.audit.observe = inline_observe; f.audit.observer = &watch;
    OK(xrt_inline_value_replace(&home, &spec, NULL));
    f.audit.observe = NULL; CHECK(watch.retired == 2);
    CHECK(!((Record *)home.value)->left && !((Record *)home.value)->right && !((Record *)home.value)->number);
    int64_t pod[2] = {7, 42};
    XrtInlineValueHome pod_home = {.value=pod};
    XrtInlineValueSpec pod_spec = {sizeof(pod), alignof(int64_t), NULL, 0};
    OK(xrt_inline_value_replace(&pod_home, &pod_spec, pod)); CHECK(pod[0] == 7 && pod[1] == 42);
    OK(xrt_inline_value_replace(&pod_home, &pod_spec, NULL)); CHECK(!pod[0] && !pod[1]);
    OK(xrt_managed_frame_leave(f.root, frame)); fixture_done(&f);
    puts("inline Frame replacement publishes all leaves and bytes before guarded retirement");
}
static void explicit_record_alignment(void) {
    /* A declaration may raise its record above what its fields need. Field
     * offsets stay natural and only the total extent is padded to the promise. */
    Fixture f; fixture_init(&f, SIZE_MAX, false);
    XrtValuePlan *flag = scalar(&f, XRT_VALUE_BOOL, 1, 1);
    char name[] = "on";
    XrtValueSlotSpec raised[] = {{flag, 0, name, 2, 0}};
    XrtValuePlan *aligned = new_plan(&f, (XrtValuePlanSpec){.kind=XRT_VALUE_RECORD, .size=8, .alignment=8,
        .fields=raised, .field_count=1});
    XrtValuePlanSpec view = {0}; OK(xrt_value_plan_view(aligned, &view));
    CHECK(view.size == 8 && view.alignment == 8 && view.fields[0].offset == 0);
    unsigned char bytes[8] = {1}, copy[8] = {0};
    OK(xrt_value_copy_empty(aligned, bytes, copy)); CHECK(copy[0] == 1);
    xrt_value_drop(aligned, copy);
    /* The same fields without the padded extent cannot carry that promise. */
    XrtValueSlotSpec tight[] = {{flag, 0, name, 2, 0}};
    XrtValuePlanSpec unpadded = {.kind=XRT_VALUE_RECORD, .size=1, .alignment=8,
        .fields=tight, .field_count=1};
    XrtValuePlan *rejected = NULL;
    STATUS(xrt_value_plan_new(f.billing, &unpadded, &rejected), XRT_MANAGED_ARGUMENT);
    CHECK(!rejected);
    /* A Tuple is positional physical layout, never a declaration promise. */
    XrtValueSlotSpec tuple_fields[] = {{flag, 0, NULL, 0, 0}};
    XrtValuePlanSpec tuple = {.kind=XRT_VALUE_TUPLE, .size=8, .alignment=8,
        .fields=tuple_fields, .field_count=1};
    XrtValuePlan *tuple_raised = NULL;
    STATUS(xrt_value_plan_new(f.billing, &tuple, &tuple_raised), XRT_MANAGED_ARGUMENT);
    CHECK(!tuple_raised);
    /* The natural record stays exactly as it was. */
    XrtValueSlotSpec plain_fields[] = {{flag, 0, name, 2, 0}};
    XrtValuePlan *plain = new_plan(&f, (XrtValuePlanSpec){.kind=XRT_VALUE_RECORD, .size=1, .alignment=1,
        .fields=plain_fields, .field_count=1});
    XrtValuePlanSpec plain_view = {0}; OK(xrt_value_plan_view(plain, &plain_view));
    CHECK(plain_view.size == 1 && plain_view.alignment == 1);
    /* An empty record's one private home is padded to its own alignment. */
    XrtValuePlan *empty = new_plan(&f, (XrtValuePlanSpec){.kind=XRT_VALUE_RECORD, .size=8, .alignment=8});
    XrtValuePlanSpec empty_view = {0}; OK(xrt_value_plan_view(empty, &empty_view));
    CHECK(empty_view.size == 8 && empty_view.alignment == 8);
    XrtValuePlanSpec bad_empty = {.kind=XRT_VALUE_RECORD, .size=1, .alignment=8};
    XrtValuePlan *bad_empty_plan = NULL;
    STATUS(xrt_value_plan_new(f.billing, &bad_empty, &bad_empty_plan), XRT_MANAGED_ARGUMENT);
    CHECK(!bad_empty_plan);
    xrt_value_plan_drop(empty);
    xrt_value_plan_drop(plain);
    xrt_value_plan_drop(aligned);
    xrt_value_plan_drop(flag);
    fixture_done(&f);
    puts("a declared record alignment raises the plan and pads its extent");
}
int main(void) {
    raw_image_retirement(); raw_image_failures();
    empty_record_storage(); explicit_record_alignment();
    inline_frame_replacement();
    inline_owners(); retain_failure_and_alias(); nullable_values(); typed_metadata_and_code(); function_shape_admission();
    failures_and_admission(); deep_metadata();
    puts("immutable ValuePlan and inline ownership kernel passed");
    return 0;
}
