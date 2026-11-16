/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_capture_backing.c - Escaping capture owners and necessary identity Cells
 *
 * KEY CONCEPT:
 *   Tests keep real owners across creator retirement and root failures, without
 *   a callable executor, compiler pool or heap allocation for frame closures.
 */
#include "runtime/native/xnative_capture_backing.h"
#include "base/xmalloc.h"
#include <limits.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
#define STATUS(c,s) CHECK((c) == (s))
typedef struct Audit {
    _Atomic(size_t) leases, bytes, blocks, finalized, calls, remaining;
    const void *watched;
    const XrtMapCell *cell;
    const XrtMapBacking *published;
    size_t observed;
    XrtMapCell *guard_cell;
    XrtManagedRoot *guard_root;
    XrtManagedFrame *other_frame;
    const XrtBackingTypes *guard_types;
    const XrtMapBacking *guard_map;
    bool probe_allocate, probe_free, drop_snapshot;
    size_t allocation_probes, retirement_probes, guard_publications;
    const void *guard_retiring;
} Audit;
typedef struct Fixture {
    Audit audit;
    XrtBillingOwner *billing;
    XrtBackingTypes types;
    XrtManagedRoot *root;
    XrtManagedFrame *frame;
} Fixture;
static bool provider_retain(void *context) {
    atomic_fetch_add(&((Audit *)context)->leases,1); return true;
}
static void provider_drop(void *context) {
    Audit *audit=context;
    size_t count=atomic_fetch_sub(&audit->leases,1); CHECK(count);
    if (count==1) {
        CHECK(!atomic_load(&audit->bytes)&&!atomic_load(&audit->blocks));
        atomic_fetch_add(&audit->finalized,1);
    }
}
static void cell_probe(Audit *audit, bool retiring) {
    if (!(retiring ? audit->probe_free : audit->probe_allocate)) return;
    STATUS(xrt_managed_root_drain(audit->guard_root),XRT_MANAGED_ARGUMENT);
    const XrtMapBacking *borrowed = NULL;
    STATUS(xrt_map_cell_borrow(audit->guard_cell,&borrowed),XRT_MANAGED_ARGUMENT); CHECK(!borrowed);
    STATUS(xrt_map_cell_write_exclusive(audit->guard_cell,audit->guard_map),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_map_cell_read_copy(audit->other_frame,0,audit->guard_cell),XRT_MANAGED_ARGUMENT);
    XrtExternalOwner external = {0}; OK(xrt_map_cell_external(audit->guard_cell,&external));
    const XrtManagedHeader *header = NULL;
    STATUS(xrt_place_borrow((XrtPlace){.external=&external,.nulls=XRT_PLACE_NONNULL},&header),XRT_MANAGED_ARGUMENT);
    CHECK(!header);
    XrtMapUpdate clear = {.operation=XRT_MAP_UPDATE_CLEAR}; bool removed = true;
    STATUS(xrt_map_cell_update_exclusive(audit->guard_cell,audit->other_frame,1,audit->guard_types,&clear,&removed),
        XRT_MANAGED_ARGUMENT); CHECK(removed);
    CHECK(atomic_load(&audit->guard_cell->managed.flags) & 1u);
    XrtManagedHeader *snapshot = NULL; OK(xrt_managed_slot_borrow(audit->other_frame,0,&snapshot));
    if (retiring) ++audit->retirement_probes;
    else {
        ++audit->allocation_probes;
        if (audit->drop_snapshot) {
            audit->drop_snapshot = false;
            OK(xrt_managed_slot_drop(audit->other_frame,0));
        }
    }
}
static void *provider_allocate(void *context,size_t bytes) {
    Audit *audit=context; atomic_fetch_add(&audit->calls,1);
    cell_probe(audit,false);
    size_t remaining=atomic_load(&audit->remaining);
    while (remaining!=SIZE_MAX) {
        if (!remaining) return NULL;
        if (atomic_compare_exchange_weak(&audit->remaining,&remaining,remaining-1)) break;
    }
    void *memory=xr_malloc(bytes);
    if (memory) { atomic_fetch_add(&audit->bytes,bytes); atomic_fetch_add(&audit->blocks,1); }
    return memory;
}
static void provider_free(void *context,void *memory,size_t bytes) {
    Audit *audit=context;
    cell_probe(audit,true);
    if (memory==audit->guard_retiring) {
        CHECK(audit->guard_cell->value!=(XrtManagedHeader *)memory &&
            ((const XrtMapBacking *)audit->guard_cell->value)->length==2);
        ++audit->guard_publications;
    }
    if (memory==audit->watched) {
        CHECK(audit->cell && audit->cell->value==&audit->published->managed);
        ++audit->observed;
    }
    CHECK(atomic_fetch_sub(&audit->bytes,bytes)>=bytes);
    CHECK(atomic_fetch_sub(&audit->blocks,1)); xr_free(memory);
}
static void fixture_init(Fixture *f,size_t limit,bool storage) {
    *f=(Fixture){0}; atomic_init(&f->audit.leases,1); atomic_init(&f->audit.remaining,SIZE_MAX);
    XrtManagedAllocator allocator={&f->audit,provider_allocate,provider_free,{&f->audit,provider_retain,provider_drop}};
    OK(xrt_billing_owner_new(&allocator,limit,&f->billing));
    if (storage) {
        OK(xrt_backing_types_init(f->billing,&f->types)); OK(xrt_managed_root_new(f->billing,&f->root));
        OK(xrt_managed_frame_enter(f->root,16,&f->frame));
    }
}
static void fixture_retire(Fixture *f) {
    OK(xrt_managed_root_destroy(&f->root)); f->frame=NULL; xrt_backing_types_drop(&f->types);
    xrt_billing_owner_drop(f->billing); f->billing=NULL; provider_drop(&f->audit);
}
static void fixture_done(Fixture *f) { fixture_retire(f); CHECK(atomic_load(&f->audit.finalized)==1); }
static void same_bill(Fixture *f,XrtBillingStats before) {
    XrtBillingStats after=xrt_billing_owner_stats(f->billing);
    CHECK(after.bytes==before.bytes&&after.blocks==before.blocks);
    CHECK(after.bytes==atomic_load(&f->audit.bytes)&&after.blocks==atomic_load(&f->audit.blocks));
}
static XrtManagedHeader *borrow(Fixture *f,size_t slot) {
    XrtManagedHeader *owner=NULL; OK(xrt_managed_slot_borrow(f->frame,slot,&owner)); return owner;
}
static XrtValuePlan *plan_new(Fixture *f,XrtValuePlanSpec spec) {
    XrtValuePlan *plan=NULL; OK(xrt_value_plan_new(f->billing,&spec,&plan)); return plan;
}
static XrtValuePlan *integer_plan(Fixture *f) {
    return plan_new(f,(XrtValuePlanSpec){.kind=XRT_VALUE_I64,.size=8,.alignment=8});
}
static XrtValuePlan *map_plan(Fixture *f) {
    size_t offset=0;
    return plan_new(f,(XrtValuePlanSpec){.kind=XRT_VALUE_MAP_STRING_I64,.size=8,.alignment=8,.owned_offsets=&offset,.owned_count=1});
}
static XrtValuePlan *cell_plan(Fixture *f,const XrtValuePlan *element) {
    size_t offset=0;
    return plan_new(f,(XrtValuePlanSpec){.kind=XRT_VALUE_CELL,.size=8,.alignment=8,
        .owned_offsets=&offset,.owned_count=1,.element=element});
}
typedef struct FnBits { int64_t (*invoke)(void *); void *environment; XrtManagedHeader *owner; } FnBits;
_Static_assert(sizeof(FnBits)==24&&offsetof(FnBits,owner)==16,"test binding matches its sealed function layout");
static int64_t static_entry(void *environment) { (void)environment; return 0; }
static XrtValuePlan *signature_plan(Fixture *f,const XrtValuePlan *result) {
    size_t offset=offsetof(FnBits,owner);
    return plan_new(f,(XrtValuePlanSpec){.kind=XRT_VALUE_FUNCTION,.size=sizeof(FnBits),.alignment=alignof(FnBits),
        .owned_offsets=&offset,.owned_count=1,.signature={.result=result,.flags=XRT_VALUE_NO_SUSPEND}});
}
typedef struct Mixed { int64_t seed; XrtManagedHeader *map,*integer,*cache; } Mixed;
static XrtValuePlan *mixed_plan(Fixture *f,XrtManagedLease code) {
    XrtValuePlan *integer=integer_plan(f),*map=map_plan(f);
    XrtValuePlan *integer_cell=cell_plan(f,integer),*map_cell=cell_plan(f,map),*signature=signature_plan(f,integer);
    char names[][6]={"seed","map","count","cache"};
    XrtValueSlotSpec captures[]={{integer,0,names[0],4},{map,8,names[1],3},{integer_cell,16,names[2],5},{map_cell,24,names[3],5}};
    size_t offsets[]={8,16,24};
    XrtValuePlan *plan=plan_new(f,(XrtValuePlanSpec){.kind=XRT_VALUE_ENVIRONMENT,.size=sizeof(Mixed),.alignment=alignof(Mixed),
        .owned_offsets=offsets,.owned_count=3,.captures=captures,.capture_count=4,.entry={3,signature},.code=code});
    memset(captures,0xa5,sizeof(captures)); memset(names,'x',sizeof(names)); offsets[0]=SIZE_MAX;
    xrt_value_plan_drop(signature); xrt_value_plan_drop(map_cell); xrt_value_plan_drop(integer_cell);
    xrt_value_plan_drop(map); xrt_value_plan_drop(integer); return plan;
}
static XrtCaptureType *type_new(Fixture *f,const XrtValuePlan *plan) {
    XrtCaptureType *type=NULL; OK(xrt_capture_type_new(f->billing,plan,&type)); return type;
}
static XrtMapBacking *make_map(Fixture *f,size_t slot,int64_t value) {
    OK(xrt_string_backing_create(f->frame,0,&f->types,"key",3,0));
    XrtStringBacking *keys[]={(XrtStringBacking *)borrow(f,0)};
    OK(xrt_map_backing_create(f->frame,slot,&f->types,keys,&value,1)); return (XrtMapBacking *)borrow(f,slot);
}
static int64_t map_value(const XrtMapBacking *map) {
    CHECK(map->length==1);
    for (size_t i=0;i<map->capacity;++i) if (map->rows[i].key) return map->rows[i].value;
    CHECK(false); return 0;
}
static void identity_cells(void) {
    Fixture f; fixture_init(&f,SIZE_MAX,true);
    XrtValuePlan *integer=integer_plan(&f),*map=map_plan(&f),*i64=cell_plan(&f,integer),*maps=cell_plan(&f,map);
    XrtCaptureType *integer_type=type_new(&f,i64),*map_type=type_new(&f,maps);
    XrtMapBacking *first=make_map(&f,1,10),*second=make_map(&f,2,20);
    OK(xrt_i64_cell_create(f.frame,3,integer_type,41)); XrtI64Cell *number=(XrtI64Cell *)borrow(&f,3);
    OK(xrt_backing_slot_copy(f.frame,4,&number->managed)); CHECK(xr_signed_rc_read(&number->managed.references)==2);
    OK(xrt_i64_cell_write_exclusive(number,42)); int64_t answer=0;
    OK(xrt_i64_cell_read((XrtI64Cell *)borrow(&f,4),&answer)); CHECK(answer==42);
    XrtManagedSlotLocation number_domain = {.cell_domain=&number->managed};
    OK(xrt_managed_domain_enter(number_domain)); answer=0;
    STATUS(xrt_i64_cell_read(number,&answer),XRT_MANAGED_ARGUMENT); CHECK(!answer);
    STATUS(xrt_i64_cell_write_exclusive(number,0),XRT_MANAGED_ARGUMENT);
    xrt_managed_domain_leave(number_domain); OK(xrt_i64_cell_read(number,&answer)); CHECK(answer==42);
    OK(xrt_map_cell_create(f.frame,5,map_type,first)); XrtMapCell *cell=(XrtMapCell *)borrow(&f,5);
    OK(xrt_backing_slot_copy(f.frame,6,&cell->managed));
    OK(xrt_map_cell_read_copy(f.frame,7,cell)); CHECK(borrow(&f,7)==&first->managed);
    OK(xrt_managed_slot_drop(f.frame,1));
    XrtBillingStats before=xrt_billing_owner_stats(f.billing); size_t calls=atomic_load(&f.audit.calls);
    OK(xrt_map_cell_write_exclusive(cell,second)); CHECK(cell->value==&second->managed&&map_value((XrtMapBacking *)borrow(&f,7))==10);
    const XrtMapBacking *borrowed=NULL; OK(xrt_map_cell_borrow((XrtMapCell *)borrow(&f,6),&borrowed)); CHECK(borrowed==second);
    OK(xrt_map_cell_write_exclusive(cell,second)); CHECK(xr_signed_rc_read(&second->managed.references)==2);
    CHECK(atomic_load(&f.audit.calls)==calls); same_bill(&f,before);
    f.audit.watched=first; f.audit.cell=cell; f.audit.published=second;
    OK(xrt_managed_slot_drop(f.frame,7)); CHECK(f.audit.observed==1); f.audit.watched=NULL;
    before=xrt_billing_owner_stats(f.billing);
    atomic_store(&second->managed.references,INT32_MAX);
    STATUS(xrt_map_cell_write_exclusive(cell,second),XRT_MANAGED_REFCOUNT); CHECK(cell->value==&second->managed);
    STATUS(xrt_map_cell_read_copy(f.frame,7,cell),XRT_MANAGED_REFCOUNT); CHECK(!borrow(&f,7));
    atomic_store(&second->managed.references,2); same_bill(&f,before);
    atomic_store(&number->managed.references,-2);
    STATUS(xrt_i64_cell_write_exclusive(number,7),XRT_MANAGED_ARGUMENT); CHECK(number->value==42);
    atomic_store(&number->managed.references,2);
    atomic_store(&cell->managed.references,-2);
    STATUS(xrt_map_cell_write_exclusive(cell,second),XRT_MANAGED_ARGUMENT); CHECK(cell->value==&second->managed);
    atomic_store(&cell->managed.references,2);
    OK(xrt_map_cell_read_copy(f.frame,5,cell)); CHECK(borrow(&f,5)==&second->managed);
    CHECK(((XrtMapCell *)borrow(&f,6))->value==&second->managed&&xr_signed_rc_read(&second->managed.references)==3);
    xrt_capture_type_drop(&integer_type); xrt_capture_type_drop(&map_type);
    xrt_value_plan_drop(maps); xrt_value_plan_drop(i64); xrt_value_plan_drop(map); xrt_value_plan_drop(integer);
    fixture_done(&f);
    puts("identity I64/Map Cells, snapshot copies and publish-before-release passed");
}
static void cross_root_cell_guard(void) {
    Fixture f; fixture_init(&f,SIZE_MAX,true);
    XrtValuePlan *map = map_plan(&f), *plan = cell_plan(&f,map);
    XrtCaptureType *type = type_new(&f,plan);
    XrtMapBacking *first = make_map(&f,1,10), *second = make_map(&f,2,20);
    OK(xrt_map_cell_create(f.frame,3,type,first));
    XrtMapCell *cell = (XrtMapCell *)borrow(&f,3);
    XrtManagedRoot *other = NULL; XrtManagedFrame *other_frame = NULL;
    OK(xrt_managed_root_new(f.billing,&other)); OK(xrt_managed_frame_enter(other,8,&other_frame));
    OK(xrt_backing_slot_copy(other_frame,2,&cell->managed));
    xrt_capture_type_drop(&type); xrt_value_plan_drop(plan); xrt_value_plan_drop(map);
    OK(xrt_managed_slot_drop(f.frame,1));
    OK(xrt_map_cell_read_copy(other_frame,0,cell));
    OK(xrt_string_backing_create(f.frame,4,&f.types,"added",5,0));
    XrtStringBacking *key = (XrtStringBacking *)borrow(&f,4);
    XrtMapUpdate update = {XRT_MAP_UPDATE_SET,key,42}; bool removed = true;
    atomic_store(&cell->managed.flags,UINT32_C(0x1000));
    f.audit.guard_cell=cell; f.audit.guard_root=f.root; f.audit.other_frame=other_frame;
    f.audit.guard_types=&f.types; f.audit.guard_map=second; f.audit.guard_retiring=first;
    f.audit.probe_allocate=f.audit.probe_free=true;
    XrtBillingStats before = xrt_billing_owner_stats(f.billing);
    atomic_store(&f.audit.remaining,0);
    STATUS(xrt_map_cell_update_exclusive(cell,f.frame,6,&f.types,&update,&removed),XRT_MANAGED_OOM);
    CHECK(cell->value==&first->managed && removed && !borrow(&f,6)); same_bill(&f,before);
    CHECK(atomic_load(&cell->managed.flags)==UINT32_C(0x1000) && xr_signed_rc_read(&cell->managed.references)==2);
    atomic_store(&f.audit.remaining,SIZE_MAX);
    XrtStringBacking *old_key = NULL;
    for (size_t n=0;n<first->capacity;++n) if (first->rows[n].key) old_key=(XrtStringBacking *)first->rows[n].key;
    CHECK(old_key && xr_signed_rc_read(&old_key->managed.references)==1);
    atomic_store(&old_key->managed.references,INT32_MAX);
    STATUS(xrt_map_cell_update_exclusive(cell,f.frame,6,&f.types,&update,&removed),XRT_MANAGED_REFCOUNT);
    atomic_store(&old_key->managed.references,1);
    CHECK(cell->value==&first->managed && removed && !borrow(&f,6)); same_bill(&f,before);
    f.audit.drop_snapshot=true;
    OK(xrt_map_cell_update_exclusive(cell,f.frame,6,&f.types,&update,&removed));
    f.audit.probe_allocate=f.audit.probe_free=false;
    f.audit.guard_retiring=NULL;
    CHECK(!removed && !borrow(&f,6) && f.audit.allocation_probes>=3 && f.audit.retirement_probes>=2 && f.audit.guard_publications==1);
    CHECK(atomic_load(&cell->managed.flags)==UINT32_C(0x1000) && xr_signed_rc_read(&cell->managed.references)==2);
    const XrtMapBacking *current = NULL; OK(xrt_map_cell_borrow(cell,&current));
    int64_t answer = 0; bool found = false;
    OK(xrt_map_backing_get(current,key,&answer,&found)); CHECK(found && answer==42 && current->length==2);
    XrtExternalOwner external = {0}; OK(xrt_map_cell_external(cell,&external));
    CHECK(!external.busy && external.cell_domain==&cell->managed && external.owner==&cell->value);
    XrtPlace binding = {.external=&external,.nulls=XRT_PLACE_NONNULL};
    XrtPlaceEdge copies[] = {
        {{.frame=other_frame,.slot=3,.nulls=XRT_PLACE_NONNULL},binding,
            {.frame=other_frame,.slot=5,.nulls=XRT_PLACE_NONNULL},XRT_PLACE_TRANSFER_COPY},
        {{.frame=other_frame,.slot=4,.nulls=XRT_PLACE_NONNULL},binding,
            {.frame=other_frame,.slot=6,.nulls=XRT_PLACE_NONNULL},XRT_PLACE_TRANSFER_COPY}};
    OK(xrt_place_edge_transfer(copies,2,NULL));
    CHECK(xr_signed_rc_read(&cell->managed.references)==2);
    XrtManagedHeader *copied = NULL; OK(xrt_managed_slot_borrow(other_frame,3,&copied)); CHECK(copied==cell->value);
    XrtBillingStats single_before=xrt_billing_owner_stats(f.billing);
    OK(xrt_place_edge_transfer(copies,1,NULL));
    CHECK(xr_signed_rc_read(&cell->managed.references)==2 && atomic_load(&cell->managed.flags)==UINT32_C(0x1000));
    same_bill(&f,single_before);
    atomic_fetch_or(&cell->managed.flags,UINT32_C(1));
    STATUS(xrt_place_edge_transfer(copies,1,NULL),XRT_MANAGED_ARGUMENT);
    atomic_fetch_and(&cell->managed.flags,~UINT32_C(1));
    CHECK(xr_signed_rc_read(&cell->managed.references)==2);
    OK(xrt_managed_slot_borrow(other_frame,3,&copied)); CHECK(copied==cell->value);
    same_bill(&f,single_before);
    STATUS(xrt_place_move_replace(copies[0].destination,binding,NULL),XRT_MANAGED_ARGUMENT);
    bool wrong = false; XrtExternalOwner invalid = external; invalid.busy=&wrong;
    const XrtManagedHeader *absent = NULL;
    STATUS(xrt_place_borrow((XrtPlace){.external=&invalid,.nulls=XRT_PLACE_NONNULL},&absent),XRT_MANAGED_ARGUMENT);
    CHECK(!absent && !wrong);
    atomic_store(&cell->managed.references,-2);
    current=NULL; OK(xrt_map_cell_borrow(cell,&current));
    STATUS(xrt_map_cell_write_exclusive(cell,second),XRT_MANAGED_ARGUMENT);
    removed=true; STATUS(xrt_map_cell_update_exclusive(cell,f.frame,6,&f.types,&update,&removed),XRT_MANAGED_ARGUMENT);
    CHECK(removed && !borrow(&f,6)); atomic_store(&cell->managed.references,INT32_MAX);
    current=NULL; STATUS(xrt_map_cell_borrow(cell,&current),XRT_MANAGED_REFCOUNT); CHECK(!current);
    atomic_store(&cell->managed.references,2);
    OK(xrt_map_cell_write_exclusive(cell,second)); current=NULL; OK(xrt_map_cell_borrow(cell,&current)); CHECK(current==second);
    CHECK(atomic_load(&cell->managed.flags)==UINT32_C(0x1000));
    OK(xrt_managed_root_destroy(&other)); fixture_done(&f);
    puts("one Cell guard spans Roots, callbacks, copy edges, creator retirement and precise OOM refunds");
}
typedef struct CodeAudit { _Atomic(size_t) finalized,retains,drops; } CodeAudit;
typedef struct CodeOwner { _Atomic(size_t) references; CodeAudit *audit; char *bytes; } CodeOwner;
static bool code_retain(void *context) {
    CodeOwner *code=context; atomic_fetch_add(&code->references,1); atomic_fetch_add(&code->audit->retains,1); return true;
}
static void code_drop(void *context) {
    CodeOwner *code=context; CodeAudit *audit=code->audit; atomic_fetch_add(&audit->drops,1);
    if (atomic_fetch_sub(&code->references,1)==1) {
        CHECK(!memcmp(code->bytes,"raw code metadata",17)); xr_free(code->bytes); xr_free(code); atomic_fetch_add(&audit->finalized,1);
    }
}
static CodeOwner *code_new(CodeAudit *audit) {
    CodeOwner *code=xr_malloc(sizeof(*code)); CHECK(code); code->bytes=xr_malloc(18); CHECK(code->bytes);
    memcpy(code->bytes,"raw code metadata",18); code->audit=audit; atomic_init(&code->references,1); return code;
}
static void creator_retirement(void) {
    Fixture metadata,objects,data; fixture_init(&metadata,SIZE_MAX,false); fixture_init(&objects,SIZE_MAX,true); fixture_init(&data,SIZE_MAX,true);
    CodeAudit code_audit={0}; CodeOwner *code=code_new(&code_audit);
    XrtValuePlan *plan=mixed_plan(&metadata,(XrtManagedLease){code,code_retain,code_drop});
    XrtValuePlanSpec shape; OK(xrt_value_plan_view(plan,&shape));
    XrtCaptureType *env_type=type_new(&objects,plan),*integer_type=type_new(&objects,shape.captures[2].type),*map_type=type_new(&objects,shape.captures[3].type);
    XrtMapBacking *map=make_map(&data,1,95);
    OK(xrt_i64_cell_create(objects.frame,0,integer_type,7)); OK(xrt_map_cell_create(objects.frame,1,map_type,map));
    Mixed input={42,&map->managed,borrow(&objects,0),borrow(&objects,1)};
    XrtBillingStats before=xrt_billing_owner_stats(objects.billing);
    OK(xrt_environment_create(objects.frame,2,env_type,&input));
    XrtBillingStats after=xrt_billing_owner_stats(objects.billing);
    CHECK(after.bytes-before.bytes==16+sizeof(Mixed)&&after.blocks==before.blocks+1);
    XrtEnvironmentBorrow environment={0}; OK(xrt_environment_borrow(env_type,borrow(&objects,2),&environment));
    CHECK(environment.payload==(const void *)(environment.owner+1));
    OK(xrt_environment_create(objects.frame,2,env_type,environment.payload));
    environment=(XrtEnvironmentBorrow){0}; OK(xrt_environment_borrow(env_type,borrow(&objects,2),&environment));
    XrtManagedHeader *escaped=NULL; OK(xrt_managed_slot_take(objects.frame,2,&escaped));
    xrt_capture_type_drop(&env_type); xrt_capture_type_drop(&integer_type); xrt_capture_type_drop(&map_type);
    xrt_value_plan_drop(plan); code_drop(code);
    fixture_retire(&metadata); fixture_retire(&objects); fixture_retire(&data);
    CHECK(!atomic_load(&metadata.audit.finalized)&&!atomic_load(&objects.audit.finalized)&&!atomic_load(&data.audit.finalized));
    CHECK(!atomic_load(&code_audit.finalized));
    size_t metadata_calls=atomic_load(&metadata.audit.calls),object_calls=atomic_load(&objects.audit.calls);
    size_t data_calls=atomic_load(&data.audit.calls),code_retains=atomic_load(&code_audit.retains),code_drops=atomic_load(&code_audit.drops);
    environment=(XrtEnvironmentBorrow){0}; OK(xrt_environment_borrow_owned(escaped,&environment));
    CHECK(environment.owner==escaped&&environment.plan==plan&&environment.payload==(const void *)(escaped+1));
    XrtValuePlanSpec recovered; OK(xrt_value_plan_view(environment.plan,&recovered));
    CHECK(recovered.entry.ordinal==3&&recovered.entry.signature&&recovered.code.context==code);
    const XrtValuePlan *capture_type=NULL; const void *payload=NULL;
    OK(xrt_environment_capture_borrow(&environment,0,&capture_type,&payload)); int64_t seed=0; memcpy(&seed,payload,8); CHECK(seed==42);
    capture_type=NULL; payload=NULL; OK(xrt_environment_capture_borrow(&environment,1,&capture_type,&payload));
    XrtMapBacking *saved_map=NULL; memcpy(&saved_map,payload,sizeof(saved_map)); CHECK(map_value(saved_map)==95);
    capture_type=NULL; payload=NULL; OK(xrt_environment_capture_borrow(&environment,2,&capture_type,&payload));
    XrtI64Cell *number=NULL; memcpy(&number,payload,sizeof(number)); OK(xrt_i64_cell_read(number,&seed)); CHECK(seed==7);
    OK(xrt_i64_cell_write_exclusive(number,8));
    capture_type=NULL; payload=NULL; OK(xrt_environment_capture_borrow(&environment,3,&capture_type,&payload));
    XrtMapCell *saved_cell=NULL; memcpy(&saved_cell,payload,sizeof(saved_cell)); CHECK(saved_cell->value==&saved_map->managed);
    capture_type=NULL; payload=NULL; STATUS(xrt_environment_capture_borrow(&environment,4,&capture_type,&payload),XRT_MANAGED_BOUNDS); CHECK(!capture_type&&!payload);
    CHECK(atomic_load(&metadata.audit.calls)==metadata_calls&&atomic_load(&objects.audit.calls)==object_calls&&atomic_load(&data.audit.calls)==data_calls);
    CHECK(atomic_load(&code_audit.retains)==code_retains&&atomic_load(&code_audit.drops)==code_drops);
    xrt_managed_drop(escaped);
    CHECK(atomic_load(&metadata.audit.finalized)==1&&atomic_load(&objects.audit.finalized)==1&&atomic_load(&data.audit.finalized)==1);
    CHECK(atomic_load(&code_audit.retains)==1&&atomic_load(&code_audit.drops)==2&&atomic_load(&code_audit.finalized)==1);
    puts("escaping captures survive Setup/creator/code/provider retirement with paired views");
}
static void owned_environment_context(void) {
    Fixture f; fixture_init(&f,SIZE_MAX,true);
    XrtValuePlan *integer=integer_plan(&f),*signature=signature_plan(&f,integer),*cell=cell_plan(&f,integer);
    XrtValueSlotSpec capture={integer,0,"seed",4};
    XrtValuePlan *plan=plan_new(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_ENVIRONMENT,.size=8,.alignment=8,
        .captures=&capture,.capture_count=1,.entry={7,signature}});
    XrtCaptureType *type=type_new(&f,plan),*cell_type=type_new(&f,cell);
    int64_t seed=42; OK(xrt_environment_create(f.frame,0,type,&seed));
    OK(xrt_i64_cell_create(f.frame,1,cell_type,9));
    OK(xrt_string_backing_create(f.frame,2,&f.types,"foreign",7,0));
    /* Matching geometry and an ordinary ValuePlan lease do not establish the
     * environment factory's producer identity. */
    XrtManagedLease ordinary=xrt_value_plan_lease(plan);
    XrtRuntimeTypeSpec physical={.fixed_size=sizeof(XrtManagedHeader)+8,.alignment=alignof(XrtManagedHeader),
        .capacity_offset=SIZE_MAX,.code=ordinary};
    XrtRuntimeTypePlan *runtime=NULL; XrtManagedOps *ops=NULL;
    OK(xrt_runtime_type_plan_new(f.billing,&physical,&runtime)); OK(xrt_managed_ops_bind(f.billing,runtime,&ops));
    XrtManagedHeader *foreign=NULL; OK(xrt_managed_slot_prepare(f.frame,3,ops,physical.fixed_size,&foreign));
    memcpy(foreign+1,&seed,8); OK(xrt_managed_slot_commit(f.frame,3));
    xrt_managed_ops_drop(ops); xrt_runtime_type_plan_drop(runtime);
    xrt_capture_type_drop(&type); xrt_capture_type_drop(&cell_type);
    xrt_value_plan_drop(plan); xrt_value_plan_drop(cell); xrt_value_plan_drop(signature); xrt_value_plan_drop(integer);
    XrtBillingStats before=xrt_billing_owner_stats(f.billing); size_t calls=atomic_load(&f.audit.calls);
    XrtManagedHeader *owner=borrow(&f,0); int32_t references=xr_signed_rc_read(&owner->references);
    atomic_store(&f.audit.remaining,0); xrt_billing_owner_fail_after(f.billing,0); xrt_billing_owner_close(f.billing);
    XrtEnvironmentBorrow view={0}; OK(xrt_environment_borrow_owned(owner,&view));
    CHECK(view.owner==owner&&view.plan==plan&&view.payload==(const void *)(owner+1));
    const XrtValuePlan *capture_type=NULL; const void *payload=NULL;
    OK(xrt_environment_capture_borrow(&view,0,&capture_type,&payload));
    int64_t answer=0; memcpy(&answer,payload,8); CHECK(answer==42);
    STATUS(xrt_environment_borrow_owned(owner,&view),XRT_MANAGED_ARGUMENT);
    CHECK(view.owner==owner&&view.plan==plan&&view.payload==(const void *)(owner+1));
    STATUS(xrt_environment_borrow_owned(owner,NULL),XRT_MANAGED_ARGUMENT);
    XrtEnvironmentBorrow empty={0}; STATUS(xrt_environment_borrow_owned(NULL,&empty),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_environment_borrow_owned(borrow(&f,1),&empty),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_environment_borrow_owned(borrow(&f,2),&empty),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_environment_borrow_owned(foreign,&empty),XRT_MANAGED_ARGUMENT);
    CHECK(!empty.owner&&!empty.plan&&!empty.payload);
    const XrtRuntimeTypeSpec *borrowed=NULL;
    STATUS(xrt_managed_producer_borrow(foreign,provider_retain,ordinary.drop,&borrowed),XRT_MANAGED_ARGUMENT); CHECK(!borrowed);
    OK(xrt_managed_producer_borrow(foreign,ordinary.retain,ordinary.drop,&borrowed));
    CHECK(borrowed->code.context==plan&&borrowed->fixed_size==physical.fixed_size);
    CHECK(xr_signed_rc_read(&owner->references)==references&&atomic_load(&f.audit.calls)==calls); same_bill(&f,before);
    fixture_done(&f);
    puts("retired setup queries reject Cells and foreign Ops without allocation or retain");
}
static void failures_and_pending(void) {
    Fixture f; fixture_init(&f,32768,true);
    XrtValuePlan *plan=mixed_plan(&f,(XrtManagedLease){0}); XrtValuePlanSpec shape; OK(xrt_value_plan_view(plan,&shape));
    XrtBillingStats base=xrt_billing_owner_stats(f.billing);
    for (size_t allowed=0;allowed<3;++allowed) {
        atomic_store(&f.audit.remaining,allowed); XrtCaptureType *empty=NULL;
        STATUS(xrt_capture_type_new(f.billing,plan,&empty),XRT_MANAGED_OOM); CHECK(!empty); same_bill(&f,base);
    }
    atomic_store(&f.audit.remaining,SIZE_MAX);
    XrtCaptureType *env_type=type_new(&f,plan),*i64_type=type_new(&f,shape.captures[2].type),*map_type=type_new(&f,shape.captures[3].type);
    XrtCaptureType *empty_type=NULL;
    STATUS(xrt_capture_type_new(f.billing,shape.entry.signature,&empty_type),XRT_MANAGED_ARGUMENT); CHECK(!empty_type);
    XrtMapBacking *map=make_map(&f,1,1);
    OK(xrt_i64_cell_create(f.frame,2,i64_type,7)); OK(xrt_map_cell_create(f.frame,3,map_type,map));
    Mixed input={99,&map->managed,borrow(&f,2),borrow(&f,3)};
    OK(xrt_environment_create(f.frame,4,env_type,&input)); XrtManagedHeader *old=borrow(&f,4);
    base=xrt_billing_owner_stats(f.billing); size_t calls=atomic_load(&f.audit.calls);
    STATUS(xrt_environment_create(f.frame,4,i64_type,&input),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_i64_cell_create(f.frame,4,env_type,1),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_map_cell_create(f.frame,4,i64_type,map),XRT_MANAGED_ARGUMENT);
    CHECK(atomic_load(&f.audit.calls)==calls&&borrow(&f,4)==old); same_bill(&f,base);
    atomic_store(&f.audit.remaining,0);
    STATUS(xrt_environment_create(f.frame,4,env_type,&input),XRT_MANAGED_OOM);
    STATUS(xrt_i64_cell_create(f.frame,4,i64_type,1),XRT_MANAGED_OOM);
    STATUS(xrt_map_cell_create(f.frame,4,map_type,map),XRT_MANAGED_OOM);
    CHECK(borrow(&f,4)==old); same_bill(&f,base); atomic_store(&f.audit.remaining,SIZE_MAX);
    XrtManagedHeader *last=input.cache; int32_t count=xr_signed_rc_read(&last->references);
    atomic_store(&last->references,INT32_MAX);
    int32_t map_count=xr_signed_rc_read(&map->managed.references),integer_count=xr_signed_rc_read(&input.integer->references);
    STATUS(xrt_environment_create(f.frame,4,env_type,&input),XRT_MANAGED_REFCOUNT);
    CHECK(xr_signed_rc_read(&map->managed.references)==map_count&&xr_signed_rc_read(&input.integer->references)==integer_count);
    atomic_store(&last->references,count); CHECK(borrow(&f,4)==old); same_bill(&f,base);
    map_count=xr_signed_rc_read(&map->managed.references);
    atomic_store(&map->managed.references,INT32_MAX);
    STATUS(xrt_map_cell_create(f.frame,4,map_type,map),XRT_MANAGED_REFCOUNT);
    atomic_store(&map->managed.references,map_count); CHECK(borrow(&f,4)==old); same_bill(&f,base);
    XrtManagedSlotLocation location={0}; OK(xrt_managed_slot_location(f.frame,4,&location));
    *location.busy=true;
    STATUS(xrt_environment_create(f.frame,4,env_type,&input),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_i64_cell_create(f.frame,4,i64_type,1),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_map_cell_read_copy(f.frame,4,(XrtMapCell *)borrow(&f,3)),XRT_MANAGED_ARGUMENT);
    *location.busy=false; CHECK(borrow(&f,4)==old); same_bill(&f,base);
    XrtManagedHeader *foreign=NULL;
    OK(xrt_managed_slot_prepare(f.frame,4,f.types.string,42,&foreign));
    XrtStringBacking *text=(XrtStringBacking *)foreign; text->length=text->runes=1; memcpy(text->bytes,"p",2);
    XrtBillingStats pending=xrt_billing_owner_stats(f.billing);
    STATUS(xrt_environment_create(f.frame,4,env_type,&input),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_i64_cell_create(f.frame,4,i64_type,1),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_map_cell_create(f.frame,4,map_type,map),XRT_MANAGED_ARGUMENT);
    STATUS(xrt_map_cell_read_copy(f.frame,4,(XrtMapCell *)borrow(&f,3)),XRT_MANAGED_ARGUMENT);
    CHECK(borrow(&f,4)==old); same_bill(&f,pending);
    OK(xrt_managed_slot_commit(f.frame,4)); CHECK(borrow(&f,4)==foreign);
    XrtEnvironmentBorrow wrong={0}; STATUS(xrt_environment_borrow(env_type,foreign,&wrong),XRT_MANAGED_ARGUMENT); CHECK(!wrong.owner);
    xrt_billing_owner_close(f.billing);
    STATUS(xrt_environment_create(f.frame,4,env_type,&input),XRT_MANAGED_CLOSED);
    OK(xrt_map_cell_read_copy(f.frame,4,(XrtMapCell *)borrow(&f,3))); CHECK(borrow(&f,4)==&map->managed);
    xrt_capture_type_drop(&env_type); xrt_capture_type_drop(&i64_type); xrt_capture_type_drop(&map_type); xrt_value_plan_drop(plan); fixture_done(&f);
    puts("typed setup failures, partial retains and foreign pending preservation passed");
}
static void actual_payload_quota(void) {
    Fixture metadata,objects; fixture_init(&metadata,SIZE_MAX,false); fixture_init(&objects,32768,true);
    enum { CAPTURES=5000 };
    XrtValuePlan *integer=integer_plan(&metadata),*signature=signature_plan(&metadata,integer);
    XrtValueSlotSpec *captures=xr_malloc(CAPTURES*sizeof(*captures)); CHECK(captures);
    int64_t *values=xr_malloc(CAPTURES*sizeof(*values)); CHECK(values);
    for (size_t i=0;i<CAPTURES;++i) { captures[i]=(XrtValueSlotSpec){integer,i*8,NULL,0}; values[i]=(int64_t)i; }
    XrtValuePlan *plan=plan_new(&metadata,(XrtValuePlanSpec){.kind=XRT_VALUE_ENVIRONMENT,.size=CAPTURES*8,.alignment=8,
        .captures=captures,.capture_count=CAPTURES,.entry={0,signature}});
    xr_free(captures); xrt_value_plan_drop(signature); xrt_value_plan_drop(integer);
    XrtCaptureType *type=type_new(&objects,plan);
    XrtBillingStats before=xrt_billing_owner_stats(objects.billing); size_t calls=atomic_load(&objects.audit.calls);
    STATUS(xrt_environment_create(objects.frame,0,type,values),XRT_MANAGED_LIMIT);
    CHECK(!borrow(&objects,0)&&atomic_load(&objects.audit.calls)==calls); same_bill(&objects,before);
    xr_free(values); xrt_capture_type_drop(&type); xrt_value_plan_drop(plan); fixture_done(&objects); fixture_done(&metadata);
    puts("escaping payload charges its real fixed extent and rejects actual quota exhaustion");
}
static void empty_and_deep_environments(void) {
    Fixture f; fixture_init(&f,SIZE_MAX,true);
    XrtValuePlan *integer=integer_plan(&f),*signature=signature_plan(&f,integer);
    XrtValuePlan *empty=plan_new(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_ENVIRONMENT,.alignment=1,.entry={0,signature}});
    XrtCaptureType *empty_type=type_new(&f,empty);
    XrtBillingStats base=xrt_billing_owner_stats(f.billing); OK(xrt_environment_create(f.frame,1,empty_type,NULL));
    CHECK(xrt_billing_owner_stats(f.billing).bytes==base.bytes+16);
    XrtEnvironmentBorrow view={0}; OK(xrt_environment_borrow(empty_type,borrow(&f,1),&view));
    const XrtValuePlan *payload_type=NULL; const void *payload=NULL;
    STATUS(xrt_environment_capture_borrow(&view,0,&payload_type,&payload),XRT_MANAGED_BOUNDS);
    XrtValueSlotSpec capture={signature,0,"previous",8}; size_t owned=offsetof(FnBits,owner);
    XrtValuePlan *chain=plan_new(&f,(XrtValuePlanSpec){.kind=XRT_VALUE_ENVIRONMENT,.size=sizeof(FnBits),.alignment=alignof(FnBits),
        .owned_offsets=&owned,.owned_count=1,.captures=&capture,.capture_count=1,.entry={0,signature}});
    XrtCaptureType *chain_type=type_new(&f,chain);
    FnBits initial={static_entry,NULL,NULL}; OK(xrt_environment_create(f.frame,0,chain_type,&initial));
    for (size_t i=0;i<100000;++i) {
        XrtManagedHeader *head=borrow(&f,0); FnBits previous={static_entry,head+1,head};
        OK(xrt_environment_create(f.frame,0,chain_type,&previous));
    }
    xrt_capture_type_drop(&empty_type); xrt_capture_type_drop(&chain_type);
    xrt_value_plan_drop(empty); xrt_value_plan_drop(chain); xrt_value_plan_drop(signature); xrt_value_plan_drop(integer);
    size_t calls=atomic_load(&f.audit.calls); atomic_store(&f.audit.remaining,0); xrt_billing_owner_fail_after(f.billing,0);
    view=(XrtEnvironmentBorrow){0}; OK(xrt_environment_borrow_owned(borrow(&f,1),&view));
    XrtValuePlanSpec shape; OK(xrt_value_plan_view(view.plan,&shape)); CHECK(!shape.size&&!shape.capture_count&&!shape.owned_count);
    payload_type=NULL; payload=NULL; STATUS(xrt_environment_capture_borrow(&view,0,&payload_type,&payload),XRT_MANAGED_BOUNDS);
    CHECK(!payload_type&&!payload&&atomic_load(&f.audit.calls)==calls);
    OK(xrt_managed_root_drain(f.root)); f.frame=NULL; CHECK(atomic_load(&f.audit.calls)==calls);
    fixture_done(&f);
    puts("empty environment and 100000 escaping capture links retire on the default stack");
}
int main(void) {
    identity_cells(); cross_root_cell_guard(); creator_retirement(); owned_environment_context(); failures_and_pending(); actual_payload_quota(); empty_and_deep_environments();
    puts("escaping environment and necessary Cell kernel passed"); return 0;
}
