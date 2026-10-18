/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_value.c - IEEE value membership, failure publication and finite work
 */
#define XR_COMPILE_DOMAIN_ENTRY original_map_value_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
static XrXirTypeArena *map_value_arena(void) {
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=XR_XIR_F64},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_F64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_F64},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=(XrXirType)258},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=(XrXirType)259},
        {.kind=XR_XIR_TYPE_SET,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=(XrXirType)262}
    };
    XrXirTypes types={nodes,8,NULL,NULL}; XrXirTypeArena *arena=NULL;
    CHECK(value_compile_arena(&types,65536,value_compile_limits(1048576,1048576,100000),&arena)==XR_XIR_VALUE_OK);
    return arena;
}
static void map_value_probe(unsigned id,const XrXirValue *map,const XrXirValue *query,
    bool expected,XrXirValueAdmission *admission,bool full) {
    const uint64_t initial=1000000;bool output=!expected;admission->work=initial;
    size_t before=calls,baseline=live;uint64_t scratch=admission->scratch_bytes;
    CHECK(xr_xir_map_contains_value(map,query,admission,&output)==XR_XIR_VALUE_OK && output==expected);
    size_t points=calls-before;uint64_t exact=initial-admission->work;
    CHECK(exact>0 && live==baseline && admission->scratch_bytes==scratch);
    XirHash *hash=(XirHash *)object_pointer(map);size_t bytes=0;
    CHECK(hash_data_size(hash->capacity,hash->stride,&bytes) && bytes<=512);
    unsigned char saved[512];if(bytes)memcpy(saved,hash->data,bytes);
    if(full)for(size_t point=0;point<points;++point) {
        admission->work=initial;output=!expected;fail_at=calls+point;
        CHECK(xr_xir_map_contains_value(map,query,admission,&output)==XR_XIR_VALUE_OOM && calls>fail_at);
        fail_at=SIZE_MAX;
        CHECK(output==!expected && live==baseline && admission->scratch_bytes==scratch);
        CHECK(!bytes || !memcmp(saved,hash->data,bytes));
        admission->work=initial;
        CHECK(xr_xir_map_contains_value(map,query,admission,&output)==XR_XIR_VALUE_OK && output==expected);
        CHECK(live==baseline && admission->scratch_bytes==scratch);
    }
    for(uint64_t prefix=full?0:exact-1;prefix<=exact;++prefix) {
        admission->work=prefix;output=!expected;
        CHECK(xr_xir_map_contains_value(map,query,admission,&output)==(prefix==exact?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        CHECK(output==(prefix==exact?expected:!expected));
        CHECK(live==baseline && admission->scratch_bytes==scratch && (!bytes || !memcmp(saved,hash->data,bytes)));
    }
    printf("Map value query=%u expected=%u allocations=%zu exact_work=%llu mode=%s input/refund PASS\n",
        id,(unsigned)expected,points,(unsigned long long)exact,full?"full":"baseline");
    admission->work=initial;
}
static void map_value_scalars(XrXirValueAdmission *admission,bool full) {
    XrXirValue keys[]={{XR_XIR_I64,0,1},{XR_XIR_I64,0,2}};
    XrXirValue values[]={{XR_XIR_F64,0,INT64_MIN},{XR_XIR_F64,0,INT64_C(0x7ff8000000000000)}};
    XrXirValue zero={XR_XIR_F64,0,0},map={0},empty={0};admission->work=1000000;
    CHECK(xr_xir_map_new((XrXirType)256,keys,values,2,admission,&map)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new((XrXirType)256,NULL,NULL,0,admission,&empty)==XR_XIR_VALUE_OK);
    map_value_probe(0,&map,&zero,true,admission,full);
    map_value_probe(1,&map,values+1,false,admission,full);
    map_value_probe(2,&empty,&zero,false,admission,full);
    bool output=true;
    CHECK(xr_xir_map_contains_value(&map,keys,NULL,&output)==XR_XIR_VALUE_BAD_ARGUMENT && output);
    CHECK(xr_xir_map_contains_value(&map,keys,admission,&output)==XR_XIR_VALUE_BAD_ARGUMENT && output);
    CHECK(xr_xir_map_contains_value(NULL,&zero,admission,&output)==XR_XIR_VALUE_BAD_ARGUMENT && output);
    CHECK(xr_xir_map_contains_value(&map,&zero,admission,NULL)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&map);xr_xir_value_drop(&empty);
}
static void map_value_nested(XrXirValueAdmission *admission,bool full) {
    XrXirValue keys[]={{XR_XIR_I64,0,1},{XR_XIR_I64,0,2}};
    XrXirValue nan={XR_XIR_F64,0,INT64_C(0x7ff8000000000000)},one={XR_XIR_F64,0,INT64_C(0x3ff0000000000000)};
    XrXirValue arrays[2]={{0}},nullable[2]={{0}},map={0},optional={0};admission->work=1000000;
    CHECK(xr_xir_array_new((XrXirType)258,&nan,1,admission,arrays)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)258,&one,1,admission,arrays+1)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new((XrXirType)260,keys,arrays,2,admission,&map)==XR_XIR_VALUE_OK);
    map_value_probe(3,&map,arrays,false,admission,full);
    map_value_probe(4,&map,arrays+1,true,admission,full);
    bool output=false;uint64_t scratch=admission->scratch_bytes;admission->scratch_bytes=0;
    CHECK(xr_xir_map_contains_value(&map,arrays+1,admission,&output)==XR_XIR_VALUE_LIMIT && !output);
    admission->scratch_bytes=scratch;admission->work=1000000;
    CHECK(xr_xir_nullable_new((XrXirType)259,&nan,admission,nullable)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)259,NULL,admission,nullable+1)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new((XrXirType)261,keys,nullable,2,admission,&optional)==XR_XIR_VALUE_OK);
    map_value_probe(5,&optional,nullable,false,admission,full);
    map_value_probe(6,&optional,nullable+1,true,admission,full);
    for(unsigned i=0;i<2;++i){xr_xir_value_drop(arrays+i);xr_xir_value_drop(nullable+i);}
    xr_xir_value_drop(&map);xr_xir_value_drop(&optional);
}
static void map_value_strings(XrXirValueAdmission *admission,bool full) {
    XrXirDomain *producer=NULL;CHECK(xr_xir_domain_new(65536,&producer)==XR_XIR_VALUE_OK);
    XrXirValue keys[]={{XR_XIR_I64,0,1},{XR_XIR_I64,0,2}},strings[3]={{0}},map={0};
    CHECK(xr_xir_string_new(producer,"a\0b",3,strings)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(producer,"x\0y",3,strings+1)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(producer,"a\0c",3,strings+2)==XR_XIR_VALUE_OK);admission->work=1000000;
    CHECK(xr_xir_map_new((XrXirType)257,keys,strings,2,admission,&map)==XR_XIR_VALUE_OK);
    map_value_probe(7,&map,strings,true,admission,full);
    map_value_probe(8,&map,strings+2,false,admission,full);
    XirObject *item=object_pointer(strings);uint32_t refs=atomic_load(&item->references);size_t baseline=live;
    atomic_store(&item->references,UINT32_MAX);bool output=false;
    CHECK(xr_xir_map_contains_value(&map,strings,admission,&output)==XR_XIR_VALUE_REFCOUNT_LIMIT && !output && live==baseline);
    CHECK(atomic_load(&item->references)==UINT32_MAX);atomic_store(&item->references,refs);
    xr_xir_domain_close(producer);xr_xir_domain_drop(producer);
    map_value_probe(9,&map,strings,true,admission,full);
    xr_xir_value_drop(&map);for(unsigned i=0;i<3;++i)xr_xir_value_drop(strings+i);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--full")));
    XrXirTypeArena *arena=map_value_arena();XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};bool full=argc==2;
    map_value_scalars(&admission,full);map_value_nested(&admission,full);map_value_strings(&admission,full);
    XrXirValue empty={0},set={0};admission.work=1000000;
    CHECK(xr_xir_set_new((XrXirType)262,NULL,0,&admission,&set)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new((XrXirType)263,NULL,NULL,0,&admission,&empty)==XR_XIR_VALUE_OK);
    bool output=true;
    CHECK(xr_xir_map_contains_value(&empty,&set,&admission,&output)==XR_XIR_VALUE_BAD_ARGUMENT && output);
    CHECK(xr_xir_map_contains_value(&set,&set,&admission,&output)==XR_XIR_VALUE_BAD_ARGUMENT && output);
    xr_xir_value_drop(&set);xr_xir_value_drop(&empty);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Map value membership: IEEE/array/nullable/string, unsupported empty domain, final physical0 PASS");return 0;
}
