/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_key_path_value.c - Typed key paths retain selectors and publish atomically
 */
#define XR_COMPILE_DOMAIN_ENTRY original_key_path_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
static const char key_path_long_text[]=
    "equal-key-content-with-independent-owners-beyond-the-bounded-short-string-search";

static XrXirTypeArena *key_path_arena(void) {
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_ARRAY, .element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MAP, .element=XR_XIR_STRING, .result=(XrXirType)256},
        {.kind=XR_XIR_TYPE_CELL, .element=(XrXirType)257},
        {.kind=XR_XIR_TYPE_CELL, .element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_CELL, .element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MAP, .element=XR_XIR_F64, .result=(XrXirType)256},
        {.kind=XR_XIR_TYPE_CELL, .element=(XrXirType)261}
    };
    XrXirTypes types={nodes,7,NULL,NULL}; XrXirTypeArena *arena=NULL;
    CHECK(value_compile_arena(&types,65536,value_compile_limits(1048576,1048576,100000),&arena)==XR_XIR_VALUE_OK);
    return arena;
}
static XrXirValue key_path_map(XrXirType type, const XrXirValue *key, XrXirValueAdmission *admission) {
    XrXirValue values[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22}}, row={0}, map={0};
    admission->work=1000000;
    CHECK(xr_xir_array_new((XrXirType)256,values,2,admission,&row)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new(type,key,&row,1,admission,&map)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&row); return map;
}
static int64_t key_path_read(XrXirValue *root, const XrXirValuePath *path, XrXirValueAdmission *admission) {
    XrXirValuePlace place={(XrXirType)root->type,&root->payload};
    XrXirValue output={0}; XrXirFaultDetail fault={0}; admission->work=1000000;
    CHECK(xr_xir_value_path_read(&place,path,admission,&output,&fault)==XR_XIR_VALUE_OK);
    CHECK(output.type==XR_XIR_I64 && !fault.code); return output.payload;
}
static void key_path_cow(XrXirValue *root, const XrXirValuePath *path,
    XrXirValueAdmission *admission, bool full) {
    XrXirValue copy={0}, replacement={XR_XIR_I64,0,99};
    XrXirValuePlace place={(XrXirType)root->type,&copy.payload}; XrXirFaultDetail fault={0};
    CHECK(xr_xir_value_copy(root,&copy)==XR_XIR_VALUE_OK);
    admission->work=1000000; size_t begin=calls;
    CHECK(xr_xir_value_path_write(&place,path,&replacement,admission,&fault)==XR_XIR_VALUE_OK);
    uint64_t work=1000000-admission->work; size_t points=calls-begin;
    CHECK(points>2 && work>1 && key_path_read(&copy,path,admission)==99);
    CHECK(key_path_read(root,path,admission)==11); xr_xir_value_drop(&copy);
    XirHash *map=(XirHash *)object_pointer(root); size_t bytes=0;
    CHECK(hash_data_size(map->capacity,map->stride,&bytes) && bytes<=512);
    unsigned char saved[512]; memcpy(saved,map->data,bytes);
    XrXirValue row={256,0,0};memcpy(&row.payload,hash_span(map,0,true).bytes,sizeof(row.payload));
    XirArray *array=(XirArray *)object_pointer(&row);unsigned char saved_items[16];
    CHECK(array->length==2 && array->stride==8);memcpy(saved_items,array->data,sizeof(saved_items));
    size_t blocks=live; uint64_t live_bytes=admission->domain->stats.live_bytes;
    for(size_t point=0;point<points;++point) {
        if(!full && point && point+1!=points)continue;
        CHECK(xr_xir_value_copy(root,&copy)==XR_XIR_VALUE_OK);
        admission->work=1000000; fail_at=calls+point;
        CHECK(xr_xir_value_path_write(&place,path,&replacement,admission,&fault)==XR_XIR_VALUE_OOM);
        fail_at=SIZE_MAX;
        CHECK(copy.payload==root->payload && live==blocks && admission->domain->stats.live_bytes==live_bytes);
        CHECK(!memcmp(saved,map->data,bytes) && admission->scratch_bytes==65536);
        CHECK(!memcmp(saved_items,array->data,sizeof(saved_items)));
        xr_xir_value_drop(&copy);
    }
    for(uint64_t prefix=full?0:work-1;prefix<=work;++prefix) {
        CHECK(xr_xir_value_copy(root,&copy)==XR_XIR_VALUE_OK); admission->work=prefix;
        CHECK(xr_xir_value_path_write(&place,path,&replacement,admission,&fault)==
            (prefix==work?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        CHECK(prefix==work || copy.payload==root->payload);
        xr_xir_value_drop(&copy);
        CHECK(live==blocks && admission->domain->stats.live_bytes==live_bytes && !memcmp(saved,map->data,bytes));
        CHECK(!memcmp(saved_items,array->data,sizeof(saved_items)));
    }
    printf("Map key path allocations=%zu work=%llu mode=%s original backing and retry PASS\n",
        points,(unsigned long long)work,full?"full":"baseline");
}
static void key_path_loans(XrXirValue *root, XrXirValue *a, XrXirValue *b,
    XrXirValueAdmission *admission) {
    XrXirType cell_type=root->type==257?(XrXirType)258:(XrXirType)262;
    XrXirValue cell={0}, p={0}, q={0}, denied={0}, other={0}, output={0};
    XrXirCellAuthority parent={root,a,1}, child={root,b,2}, grandchild={root,&cell,3};
    XrXirValuePathStep steps[]={ {XR_XIR_PATH_KEY,(XrXirType)root->type,a->payload} };
    XrXirValuePath path={steps,1}; XrXirFaultDetail fault={0}; admission->work=1000000;
    CHECK(xr_xir_cell_new(admission->domain,(XrXirTypeArena *)admission->arena,cell_type,root,admission,&cell)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_project_authorized((XrXirType)259,&cell,&path,&parent,admission,&p)==XR_XIR_VALUE_OK);
    steps[0].selector=b->payload;
    CHECK(xr_xir_cell_project_authorized((XrXirType)259,&cell,&path,&parent,admission,&q)==XR_XIR_VALUE_OK);
    if(a->type==XR_XIR_STRING)
        CHECK(xr_xir_string_new(admission->domain,"different-key",13,&other)==XR_XIR_VALUE_OK);
    else other=(XrXirValue){XR_XIR_F64,0,INT64_C(0x4000000000000000)};
    steps[0].selector=other.payload;
    CHECK(xr_xir_cell_project_authorized((XrXirType)259,&cell,&path,&parent,admission,&denied)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&other);
    if(a->type==XR_XIR_STRING) {
        CHECK(a->payload!=b->payload);
        CHECK(atomic_load(&object_pointer(a)->references)==2 && atomic_load(&object_pointer(b)->references)==2);
        xr_xir_value_drop(a); xr_xir_value_drop(b);
    }
    XrXirCellLoan first={0}, second={0}; admission->work=1000000;
    CHECK(xr_xir_cell_loan_prepare_path(&p,&parent,admission,&fault,&first)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&p,&first,&child);
    CHECK(xr_xir_cell_loan_prepare_path(&denied,&child,admission,&fault,&second)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_fault_empty(fault) && cell_loan_empty(&second) && cell_root(cell_owner(&cell))->borrow_top==&first);
    XrXirCellAuthority stale=child; ++stale.epoch;
    CHECK(xr_xir_cell_loan_prepare_path(&q,&stale,admission,&fault,&second)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(cell_loan_empty(&second) && cell_root(cell_owner(&cell))->borrow_top==&first);
    admission->work=0;
    CHECK(xr_xir_cell_loan_prepare_path(&q,&child,admission,&fault,&second)==XR_XIR_VALUE_LIMIT);
    CHECK(cell_loan_empty(&second) && cell_root(cell_owner(&cell))->borrow_top==&first);
    admission->work=1000000;
    CHECK(xr_xir_cell_loan_prepare(&q,&child,&second)==XR_XIR_VALUE_BAD_ARGUMENT && cell_loan_empty(&second));
    CHECK(xr_xir_cell_loan_prepare_path(&q,&child,admission,&fault,&second)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&q,&second,&grandchild);
    XrXirValuePathStep index={XR_XIR_PATH_INDEX,(XrXirType)256,0}; XrXirValuePath suffix={&index,1};
    CHECK(xr_xir_cell_authorized_path_read(&q,&grandchild,&suffix,admission,&output,&fault)==XR_XIR_VALUE_OK);
    CHECK(output.type==XR_XIR_I64 && output.payload==11);
    xr_xir_cell_loan_release(&second);
    CHECK(xr_xir_cell_authorized_path_read(&q,&grandchild,&suffix,admission,&(XrXirValue){0},&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_cell_loan_release(&first);
    xr_xir_value_drop(&cell); xr_xir_value_drop(&p); xr_xir_value_drop(&q);xr_xir_value_drop(&denied);
    puts("Equal key descriptors reborrow with finite work; retained selectors and exact parent restoration PASS");
}
static void key_path_key_retention_failure(XrXirValue *root, XrXirValue *key,
    XrXirValueAdmission *admission) {
    XrXirValue cell={0}, output={0}; XrXirCellAuthority authority={root,key,4};
    admission->work=1000000;
    CHECK(xr_xir_cell_new(admission->domain,(XrXirTypeArena *)admission->arena,(XrXirType)258,root,admission,&cell)==XR_XIR_VALUE_OK);
    XrXirValuePathStep step={XR_XIR_PATH_KEY,(XrXirType)257,key->payload}; XrXirValuePath path={&step,1};
    XirObject *object=object_pointer(key); uint32_t references=atomic_load(&object->references);
    size_t baseline=live; uint64_t bytes=admission->domain->stats.live_bytes;
    uint32_t cell_refs=atomic_load(&object_pointer(&cell)->references);
    atomic_store(&object->references,UINT32_MAX);
    CHECK(xr_xir_cell_project_authorized((XrXirType)259,&cell,&path,&authority,admission,&output)==XR_XIR_VALUE_REFCOUNT_LIMIT);
    atomic_store(&object->references,references);
    CHECK(!output.type && !output.reserved && !output.payload && live==baseline && admission->domain->stats.live_bytes==bytes);
    CHECK(atomic_load(&object_pointer(&cell)->references)==cell_refs);
    CHECK(xr_xir_cell_project_authorized((XrXirType)259,&cell,&path,&authority,admission,&output)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&output); xr_xir_value_drop(&cell);
    puts("Failed key acquisition rolls back root lease and physical storage; same owner retry PASS");
}
static bool key_path_edge_count(const XrXirValue *value, void *payload, void *context) {
    unsigned *counts=context;CHECK(payload && xr_xir_value_valid(value));
    ++counts[value->type==258?1:0];return true;
}
static void key_path_late_selector(XrXirTypeArena *arena) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    XrXirValue stored={0}, key={0}, cell={0}, projected={0};
    CHECK(xr_xir_string_new(domain,key_path_long_text,strlen(key_path_long_text),&stored)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,key_path_long_text,strlen(key_path_long_text),&key)==XR_XIR_VALUE_OK);
    CHECK(key.payload!=stored.payload);
    XrXirValue map=key_path_map((XrXirType)257,&stored,&admission);
    XrXirCellAuthority authority={&map,&cell,5};
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)258,&map,&admission,&cell)==XR_XIR_VALUE_OK);
    XrXirValuePathStep step={XR_XIR_PATH_KEY,(XrXirType)257,key.payload};XrXirValuePath path={&step,1};
    CHECK(xr_xir_cell_project_authorized((XrXirType)259,&cell,&path,&authority,&admission,&projected)==XR_XIR_VALUE_OK);
    unsigned edges[2]={0};CHECK(graph_edges(object_pointer(&projected),key_path_edge_count,edges) && edges[0]==1 && edges[1]==1);
    xr_xir_value_drop(&cell);xr_xir_value_drop(&map);xr_xir_value_drop(&stored);xr_xir_value_drop(&key);
    xr_xir_domain_close(domain);CHECK(!domain->graph_invalid && xr_xir_value_valid(&projected));
    size_t before=calls;closed_fail_all=true;xr_xir_value_drop(&projected);closed_fail_all=false;
    XrXirDomainStats stats=xr_xir_domain_stats(domain);
    CHECK(calls==before && !domain->graph_invalid && stats.live_bytes==sizeof(*domain));
    CHECK(stats.allocations==stats.frees+1 && !domain->budget.metadata_live && !domain->budget.call_live);
    xr_xir_domain_drop(domain);puts("Closed graph sees the selector edge and final projection release allocates nothing PASS");
}
int main(int argc, char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--full")));
    XrXirTypeArena *arena=key_path_arena(); XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    XrXirValue stored={0}, a={0}, b={0};
    CHECK(xr_xir_string_new(domain,"same-content-key",16,&stored)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"same-content-key",16,&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"same-content-key",16,&b)==XR_XIR_VALUE_OK);
    XrXirValue map=key_path_map((XrXirType)257,&stored,&admission);
    XrXirValuePathStep steps[]={{XR_XIR_PATH_KEY,(XrXirType)257,a.payload},{XR_XIR_PATH_INDEX,(XrXirType)256,0}};
    XrXirValuePath path={steps,2}; key_path_cow(&map,&path,&admission,argc==2);
    key_path_key_retention_failure(&map,&a,&admission);
    xr_xir_value_drop(&map);xr_xir_value_drop(&stored);xr_xir_value_drop(&a);xr_xir_value_drop(&b);
    CHECK(xr_xir_string_new(domain,key_path_long_text,strlen(key_path_long_text),&stored)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,key_path_long_text,strlen(key_path_long_text),&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,key_path_long_text,strlen(key_path_long_text),&b)==XR_XIR_VALUE_OK);
    CHECK(stored.payload!=a.payload && stored.payload!=b.payload && a.payload!=b.payload);
    map=key_path_map((XrXirType)257,&stored,&admission);steps[0].selector=a.payload;
    key_path_cow(&map,&path,&admission,argc==2);key_path_key_retention_failure(&map,&a,&admission);
    key_path_loans(&map,&a,&b,&admission);
    XrXirValue missing={0}, output={0}; XrXirFaultDetail fault={0};
    CHECK(xr_xir_string_new(domain,"missing",7,&missing)==XR_XIR_VALUE_OK);
    steps[0].selector=missing.payload; admission.work=1000000;
    XrXirValuePlace place={(XrXirType)map.type,&map.payload};
    CHECK(xr_xir_value_path_read(&place,&path,&admission,&output,&fault)==XR_XIR_VALUE_BOUNDS && fault.code==431);
    steps[0].selector=stored.payload; steps[1].selector=2;
    CHECK(xr_xir_value_path_read(&place,&path,&admission,&output,&fault)==XR_XIR_VALUE_BOUNDS && fault.code==430);
    xr_xir_value_drop(&missing); xr_xir_value_drop(&stored); xr_xir_value_drop(&map);
    const int64_t pairs[][2]={{0,INT64_MIN},{INT64_C(0x7ff8000000000001),INT64_C(0x7ff0000000000001)}};
    for(unsigned pair=0;pair<2;++pair) {
        a=(XrXirValue){XR_XIR_F64,0,pairs[pair][0]}; b=(XrXirValue){XR_XIR_F64,0,pairs[pair][1]};
        map=key_path_map((XrXirType)261,&a,&admission); key_path_loans(&map,&a,&b,&admission);
        xr_xir_value_drop(&map);
    }
    xr_xir_domain_close(domain); CHECK(!domain->graph_invalid);
    xr_xir_domain_drop(domain);key_path_late_selector(arena);xr_xir_compile_type_arena_drop(arena);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Map key paths, IEEE key classes, fault distinction and final physical0 PASS"); return 0;
}
