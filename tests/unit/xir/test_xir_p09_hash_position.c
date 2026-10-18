/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_position.c - Owning ordinal reads, bounds and failure publication
 */
#define XR_COMPILE_DOMAIN_ENTRY original_hash_position_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#include "xir/xxir_hash_iteration.h"
static XrXirValueStatus position_read(bool values,const XrXirValue *map,int64_t index,
    XrXirValueAdmission *admission,XrXirValue *output,XrXirFaultDetail *fault) {
    return values?xr_xir_map_value_at(map,index,admission,output,fault):xr_xir_hash_key_at(map,index,admission,output,fault);
}
static void position_faults(bool values,const XrXirValue *map,XrXirValueAdmission *admission,bool full) {
    const uint64_t initial=1000000;admission->work=initial;XrXirValue result={0};XrXirFaultDetail fault={0};
    size_t before=calls,baseline=live;uint64_t scratch=admission->scratch_bytes;
    CHECK(position_read(values,map,0,admission,&result,&fault)==XR_XIR_VALUE_OK);
    size_t points=calls-before;uint64_t exact=initial-admission->work;CHECK(exact && !fault.code);
    xr_xir_value_drop(&result);CHECK(live==baseline && admission->scratch_bytes==scratch);
    XirHash *hash=(XirHash *)object_pointer(map);size_t bytes=0;
    CHECK(hash_data_size(hash->capacity,hash->stride,&bytes) && bytes<=512);
    unsigned char saved[512];if(bytes)memcpy(saved,hash->data,bytes);
    if(full)for(size_t point=0;point<points;++point) {
        admission->work=initial;fail_at=calls+point;
        CHECK(position_read(values,map,0,admission,&result,&fault)==XR_XIR_VALUE_OOM && calls>fail_at);fail_at=SIZE_MAX;
        CHECK(!result.type && !result.reserved && !result.payload && !fault.code && live==baseline && admission->scratch_bytes==scratch);
        CHECK(!bytes || !memcmp(saved,hash->data,bytes));admission->work=initial;
        CHECK(position_read(values,map,0,admission,&result,&fault)==XR_XIR_VALUE_OK);xr_xir_value_drop(&result);CHECK(live==baseline);
    }
    for(uint64_t prefix=full?0:exact-1;prefix<=exact;++prefix) {
        admission->work=prefix;
        CHECK(position_read(values,map,0,admission,&result,&fault)==(prefix==exact?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        if(prefix!=exact)CHECK(!result.type && !result.reserved && !result.payload);
        xr_xir_value_drop(&result);CHECK(live==baseline && admission->scratch_bytes==scratch && (!bytes || !memcmp(saved,hash->data,bytes)));
    }
    printf("hash ordinal values=%u type=%u allocations=%zu exact_work=%llu mode=%s input/refund PASS\n",(unsigned)values,map->type,points,(unsigned long long)exact,full?"full":"baseline");admission->work=initial;
}
static void position_bounds(const XrXirValue *map,int64_t length,XrXirValueAdmission *admission) {
    const int64_t indices[]={-1,length,INT64_MAX};
    for(unsigned i=0;i<3;++i) {
        XrXirValue result={0};XrXirFaultDetail fault={0};admission->work=1000000;
        CHECK(xr_xir_hash_key_at(map,indices[i],admission,&result,&fault)==XR_XIR_VALUE_BOUNDS);
        CHECK(!result.type && !result.reserved && !result.payload && fault.code==430 && fault.index==indices[i] && fault.length==length);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--full")));bool full=argc==2;
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_SET,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=(XrXirType)258}
    };
    XrXirTypes types={nodes,4,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL,*producer=NULL;
    CHECK(value_compile_arena(&types,65536,value_compile_limits(1048576,1048576,100000),&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK && xr_xir_domain_new(65536,&producer)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};XrXirValue strings[3]={{0}};
    for(unsigned i=0;i<3;++i)CHECK(xr_xir_string_new(producer,i==0?"first":i==1?"second":"third",i==0?5:i==1?6:5,strings+i)==XR_XIR_VALUE_OK);
    XrXirValue keys[]={{XR_XIR_I64,0,3},{XR_XIR_I64,0,1},{XR_XIR_I64,0,2}},map={0},set={0},optional={0},nullable={0};
    CHECK(xr_xir_map_new((XrXirType)256,keys,strings,2,&admission,&map)==XR_XIR_VALUE_OK);
    XrXirValuePlace place={(XrXirType)256,&map.payload};bool removed=false;
    CHECK(xr_xir_hash_delete(&place,keys,&admission,&removed)==XR_XIR_VALUE_OK && removed);
    CHECK(xr_xir_map_set(&place,keys,strings+2,&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_set_new((XrXirType)257,keys,3,&admission,&set)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)258,strings,&admission,&nullable)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new((XrXirType)259,keys,&nullable,1,&admission,&optional)==XR_XIR_VALUE_OK);
    for(int64_t i=0;i<2;++i) {
        XrXirValue key={0},value={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_hash_key_at(&map,i,&admission,&key,&fault)==XR_XIR_VALUE_OK && key.payload==(i?3:1));
        CHECK(xr_xir_map_value_at(&map,i,&admission,&value,&fault)==XR_XIR_VALUE_OK);
        const char *bytes=NULL;size_t count=0;const char *expected=i?"third":"second";
        CHECK(xr_xir_string_view(&value,&bytes,&count) && count==strlen(expected) && !memcmp(bytes,expected,count));xr_xir_value_drop(&value);
    }
    for(unsigned i=0;i<3;++i){XrXirValue item={0};XrXirFaultDetail fault={0};CHECK(xr_xir_hash_key_at(&set,i,&admission,&item,&fault)==XR_XIR_VALUE_OK && item.payload==keys[i].payload);}
    position_faults(false,&map,&admission,full);position_faults(true,&map,&admission,full);position_faults(false,&set,&admission,full);position_faults(true,&optional,&admission,full);
    position_bounds(&map,2,&admission);position_bounds(&set,3,&admission);
    XrXirValue result={XR_XIR_I64,0,17};XrXirFaultDetail fault={0};
    CHECK(xr_xir_hash_key_at(&map,0,&admission,&result,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && result.payload==17);result=(XrXirValue){0};
    CHECK(xr_xir_map_value_at(&set,0,&admission,&result,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    CHECK(xr_xir_hash_key_at(&map,0,NULL,&result,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    CHECK(xr_xir_hash_key_at(&map,0,&admission,&result,NULL)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    XirObject *leaf=object_pointer(strings+1);uint32_t refs=atomic_load(&leaf->references);size_t baseline=live;atomic_store(&leaf->references,UINT32_MAX);
    CHECK(xr_xir_map_value_at(&map,0,&admission,&result,&fault)==XR_XIR_VALUE_REFCOUNT_LIMIT && !result.type && live==baseline);atomic_store(&leaf->references,refs);
    xr_xir_domain_close(producer);xr_xir_domain_drop(producer);
    CHECK(xr_xir_map_value_at(&map,0,&admission,&result,&fault)==XR_XIR_VALUE_OK);xr_xir_value_drop(&map);
    const char *bytes=NULL;size_t count=0;CHECK(xr_xir_string_view(&result,&bytes,&count) && count==6 && !memcmp(bytes,"second",6));xr_xir_value_drop(&result);
    xr_xir_value_drop(&set);xr_xir_value_drop(&optional);xr_xir_value_drop(&nullable);for(unsigned i=0;i<3;++i)xr_xir_value_drop(strings+i);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Owned hash ordinal values, reinsert order, E0430, late producer and final physical0 PASS");return 0;
}
