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
static void *provider_allocate(void *context,size_t bytes) {
    Audit *audit=context; atomic_fetch_add(&audit->calls,1);
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
    if (memory==audit->watched) {
        CHECK(audit->cell && audit->cell->value==audit->published);
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
    OK(xrt_map_cell_create(f.frame,5,map_type,first)); XrtMapCell *cell=(XrtMapCell *)borrow(&f,5);
    OK(xrt_backing_slot_copy(f.frame,6,&cell->managed));
    OK(xrt_map_cell_read_copy(f.frame,7,cell)); CHECK(borrow(&f,7)==&first->managed);
    OK(xrt_managed_slot_drop(f.frame,1));
    XrtBillingStats before=xrt_billing_owner_stats(f.billing); size_t calls=atomic_load(&f.audit.calls);
    OK(xrt_map_cell_write_exclusive(cell,second)); CHECK(cell->value==second&&map_value((XrtMapBacking *)borrow(&f,7))==10);
    const XrtMapBacking *borrowed=NULL; OK(xrt_map_cell_borrow((XrtMapCell *)borrow(&f,6),&borrowed)); CHECK(borrowed==second);
    OK(xrt_map_cell_write_exclusive(cell,second)); CHECK(xr_signed_rc_read(&second->managed.references)==2);
    CHECK(atomic_load(&f.audit.calls)==calls); same_bill(&f,before);
    f.audit.watched=first; f.audit.cell=cell; f.audit.published=second;
    OK(xrt_managed_slot_drop(f.frame,7)); CHECK(f.audit.observed==1); f.audit.watched=NULL;
    before=xrt_billing_owner_stats(f.billing);
    atomic_store(&second->managed.references,INT32_MAX);
    STATUS(xrt_map_cell_write_exclusive(cell,second),XRT_MANAGED_REFCOUNT); CHECK(cell->value==second);
    STATUS(xrt_map_cell_read_copy(f.frame,7,cell),XRT_MANAGED_REFCOUNT); CHECK(!borrow(&f,7));
    atomic_store(&second->managed.references,2); same_bill(&f,before);
    atomic_store(&number->managed.references,-2);
    STATUS(xrt_i64_cell_write_exclusive(number,7),XRT_MANAGED_ARGUMENT); CHECK(number->value==42);
    atomic_store(&number->managed.references,2);
    atomic_store(&cell->managed.references,-2);
    STATUS(xrt_map_cell_write_exclusive(cell,second),XRT_MANAGED_ARGUMENT); CHECK(cell->value==second);
    atomic_store(&cell->managed.references,2);
    OK(xrt_map_cell_read_copy(f.frame,5,cell)); CHECK(borrow(&f,5)==&second->managed);
    CHECK(((XrtMapCell *)borrow(&f,6))->value==second&&xr_signed_rc_read(&second->managed.references)==3);
    xrt_capture_type_drop(&integer_type); xrt_capture_type_drop(&map_type);
    xrt_value_plan_drop(maps); xrt_value_plan_drop(i64); xrt_value_plan_drop(map); xrt_value_plan_drop(integer);
    fixture_done(&f);
    puts("identity I64/Map Cells, snapshot copies and publish-before-release passed");
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
    const XrtValuePlan *capture_type=NULL; const void *payload=NULL;
    OK(xrt_environment_capture_borrow(&environment,0,&capture_type,&payload)); int64_t seed=0; memcpy(&seed,payload,8); CHECK(seed==42);
    capture_type=NULL; payload=NULL; OK(xrt_environment_capture_borrow(&environment,1,&capture_type,&payload));
    XrtMapBacking *saved_map=NULL; memcpy(&saved_map,payload,sizeof(saved_map)); CHECK(map_value(saved_map)==95);
    capture_type=NULL; payload=NULL; OK(xrt_environment_capture_borrow(&environment,2,&capture_type,&payload));
    XrtI64Cell *number=NULL; memcpy(&number,payload,sizeof(number)); OK(xrt_i64_cell_read(number,&seed)); CHECK(seed==7);
    OK(xrt_i64_cell_write_exclusive(number,8));
    capture_type=NULL; payload=NULL; OK(xrt_environment_capture_borrow(&environment,3,&capture_type,&payload));
    XrtMapCell *saved_cell=NULL; memcpy(&saved_cell,payload,sizeof(saved_cell)); CHECK(saved_cell->value==saved_map);
    capture_type=NULL; payload=NULL; STATUS(xrt_environment_capture_borrow(&environment,4,&capture_type,&payload),XRT_MANAGED_BOUNDS); CHECK(!capture_type&&!payload);
    xrt_managed_drop(escaped);
    CHECK(atomic_load(&metadata.audit.finalized)==1&&atomic_load(&objects.audit.finalized)==1&&atomic_load(&data.audit.finalized)==1);
    CHECK(atomic_load(&code_audit.retains)==1&&atomic_load(&code_audit.drops)==2&&atomic_load(&code_audit.finalized)==1);
    puts("escaping captures survive Setup/creator/code/provider retirement with paired views");
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
    OK(xrt_managed_root_drain(f.root)); f.frame=NULL; CHECK(atomic_load(&f.audit.calls)==calls);
    fixture_done(&f);
    puts("empty environment and 100000 escaping capture links retire on the default stack");
}
int main(void) {
    identity_cells(); creator_retirement(); failures_and_pending(); actual_payload_quota(); empty_and_deep_environments();
    puts("escaping environment and necessary Cell kernel passed"); return 0;
}
