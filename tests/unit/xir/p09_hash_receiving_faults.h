/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_receiving_faults.h - Cross-owner compact transactions at every failure
 */
#ifndef P09_HASH_RECEIVING_FAULTS_H
#define P09_HASH_RECEIVING_FAULTS_H
typedef struct HashReceivingCase {
    XrXirValue root,alias,key,value,output;
    XrXirValueAdmission admission;
    XrXirValuePlace place;
    unsigned operation;
    bool found;
} HashReceivingCase;
static void hash_receiving_setup(HashReceivingCase *test,unsigned operation,
    XrXirTypeArena *birth,XrXirTypeArena *target,XrXirDomain *domain) {
    *test=(HashReceivingCase){.operation=operation,.admission={birth,domain,NULL,NULL,1000000,65536}};
    XrXirValue scalar=integer(42),array={0},text={0},tuple={0};
    CHECK(xr_xir_array_new((XrXirType)256,&scalar,1,&test->admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"original",8,&text)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={text,array};
    CHECK(xr_xir_tuple_new((XrXirType)257,fields,2,&test->admission,&tuple)==XR_XIR_VALUE_OK);
    XrXirValue keys[4],values[4];
    for (unsigned i=0;i<4;++i) {keys[i]=integer(i);values[i]=tuple;}
    CHECK(xr_xir_map_new((XrXirType)258,keys,values,4,&test->admission,&test->root)==XR_XIR_VALUE_OK);
    if (operation==1 || operation==2 || operation==3)
        CHECK(xr_xir_value_copy(&test->root,&test->alias)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&tuple);xr_xir_value_drop(&array);xr_xir_value_drop(&text);
    test->admission.arena=target;scalar.payload=63;
    CHECK(xr_xir_array_new((XrXirType)257,&scalar,1,&test->admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"replacement",11,&text)==XR_XIR_VALUE_OK);
    fields[0]=text;fields[1]=array;
    CHECK(xr_xir_tuple_new((XrXirType)258,fields,2,&test->admission,&test->value)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array);xr_xir_value_drop(&text);
    test->key=integer(operation==0 ? 42 : 2);
    test->place=(XrXirValuePlace){(XrXirType)259,&test->root.payload};
}
static XrXirValueStatus hash_receiving_operation(HashReceivingCase *test) {
    if (test->operation==2 || test->operation==4)
        return xr_xir_hash_delete(&test->place,&test->key,&test->admission,&test->found);
    if (test->operation==3) return xr_xir_hash_clear(&test->place,&test->admission);
    if (test->operation==5)
        return xr_xir_map_get(&test->root,&test->key,&test->admission,&test->output,&test->found);
    return xr_xir_map_set(&test->place,&test->key,&test->value,&test->admission);
}
static void hash_receiving_finish(HashReceivingCase *test) {
    xr_xir_value_drop(&test->output);xr_xir_value_drop(&test->value);
    xr_xir_value_drop(&test->alias);xr_xir_value_drop(&test->root);
}
static void hash_receiving_trial(HashReceivingCase *test,size_t fail,uint64_t work,bool success) {
    XirHash *map=(XirHash *)object_pointer(&test->root);unsigned char snapshot[512];size_t bytes=0;
    CHECK(hash_data_size(map->capacity,map->stride,&bytes) && bytes<=sizeof(snapshot));
    memcpy(snapshot,map->data,bytes);unsigned char *data=map->data;int64_t payload=test->root.payload;
    const XrXirTypeArena *target=test->admission.arena;
    size_t blocks=live;uint64_t physical=xr_xir_domain_stats(test->admission.domain).live_bytes;
    uint64_t scratch=test->admission.scratch_bytes;
    test->admission.work=work;fail_at=fail;
    XrXirValueStatus status=hash_receiving_operation(test);fail_at=SIZE_MAX;
    CHECK(status==(success ? XR_XIR_VALUE_OK : fail==SIZE_MAX ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_OOM));
    CHECK(test->admission.arena==target && test->admission.scratch_bytes==scratch);
    if (!success) {
        CHECK(test->root.payload==payload && map->data==data && map->length==4 && !memcmp(data,snapshot,bytes));
        CHECK(!test->output.type && !test->output.payload && !test->found);
        CHECK(live==blocks && xr_xir_domain_stats(test->admission.domain).live_bytes==physical);
        if (test->alias.type) CHECK(test->alias.payload==payload);
    } else if (test->operation==5) {
        CHECK(test->found && test->output.type==(uint32_t)257 && xr_xir_value_arena(&test->output)==map->object.arena);
    }
}
static void hash_receiving_faults(void) {
    ValueCompileOwner compiler={0};
    CHECK(value_compile_owner_new(&compiler,(XrCompileResourceLimits){4194304,4194304,16000000},65536)==XR_XIR_OK);
    XrXirCallableParameter old_fields[]={{XR_XIR_STRING,0},{(XrXirType)256,0}};
    XrXirCallableParameter new_fields[]={{XR_XIR_STRING,0},{(XrXirType)257,0}};
    XrXirTypeNode original[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=old_fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=(XrXirType)257}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=new_fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=(XrXirType)258}};
    XrXirTypes pools[]={{original,3,NULL,NULL},{shifted,4,NULL,NULL}};
    XrXirTypeArena *birth=NULL,*target=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler.context,&pools[0],&birth)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler.context,&pools[1],&target)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot root={(XrXirType)258,(XrXirType)259};
    CHECK(xir_compile_type_arena_receive(&compiler.context,target,birth,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    size_t blocks=live;uint64_t physical=xr_xir_domain_stats(domain).live_bytes;
    for (unsigned operation=0;operation<6;++operation) {
        HashReceivingCase normal;hash_receiving_setup(&normal,operation,birth,target,domain);
        size_t start=calls;uint64_t work=normal.admission.work;
        hash_receiving_trial(&normal,SIZE_MAX,work,true);
        size_t sites=calls-start;uint64_t exact=work-normal.admission.work;
        CHECK(exact>1 && exact<20000);hash_receiving_finish(&normal);
        for (size_t site=0;site<sites;++site) {
            HashReceivingCase test;hash_receiving_setup(&test,operation,birth,target,domain);
            start=calls;hash_receiving_trial(&test,start+site,1000000,false);CHECK(calls>start+site);
            hash_receiving_trial(&test,SIZE_MAX,1000000,true);hash_receiving_finish(&test);
            CHECK(live==blocks && xr_xir_domain_stats(domain).live_bytes==physical);
        }
        for (uint64_t prefix=0;prefix<=exact;++prefix) {
            HashReceivingCase test;hash_receiving_setup(&test,operation,birth,target,domain);
            hash_receiving_trial(&test,SIZE_MAX,prefix,prefix==exact);hash_receiving_finish(&test);
            CHECK(live==blocks && xr_xir_domain_stats(domain).live_bytes==physical);
        }
        uint64_t low=0,high=65536;
        while (low<high) {
            uint64_t middle=low+(high-low)/2;
            HashReceivingCase test;hash_receiving_setup(&test,operation,birth,target,domain);
            test.admission.scratch_bytes=middle;
            XrXirValueStatus status=hash_receiving_operation(&test);
            CHECK(status==XR_XIR_VALUE_OK || status==XR_XIR_VALUE_LIMIT);
            CHECK(test.admission.scratch_bytes==middle && test.admission.arena==target);
            hash_receiving_finish(&test);
            CHECK(live==blocks && xr_xir_domain_stats(domain).live_bytes==physical);
            if (status==XR_XIR_VALUE_OK) high=middle;else low=middle+1;
        }
        HashReceivingCase exact_scratch;hash_receiving_setup(&exact_scratch,operation,birth,target,domain);
        exact_scratch.admission.scratch_bytes=low;
        hash_receiving_trial(&exact_scratch,SIZE_MAX,1000000,true);hash_receiving_finish(&exact_scratch);
        if (low) {
            HashReceivingCase short_scratch;hash_receiving_setup(&short_scratch,operation,birth,target,domain);
            short_scratch.admission.scratch_bytes=low-1;
            hash_receiving_trial(&short_scratch,SIZE_MAX,1000000,false);hash_receiving_finish(&short_scratch);
        }
        printf("received hash operation%u actual allocation sites=%zu all work prefixes=0..%llu scratch exact=%llu/minus1 physical0/0 PASS\n",
            operation,sites,(unsigned long long)exact,(unsigned long long)low);
    }
    xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(birth);xr_xir_domain_drop(domain);
    CHECK(value_compile_stats(&compiler.context).live_bytes==compiler.baseline.live_bytes);
    value_compile_owner_release(&compiler);CHECK(!live && !value_compile_live && !value_compile_bytes);
}
#endif // P09_HASH_RECEIVING_FAULTS_H
