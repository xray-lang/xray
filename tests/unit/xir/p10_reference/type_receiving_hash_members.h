/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_hash_members.h - Hash snapshots and set results across real pools
 */
#ifndef TYPE_RECEIVING_HASH_MEMBERS_H
#define TYPE_RECEIVING_HASH_MEMBERS_H
#include "xir/xxir_hash_iteration.h"
#include "xir/xxir_tuple.h"
static void received_hash_members(SourceFixtureOwner *compiler) {
    XrXirCallableParameter birth_fields[]={{XR_XIR_I64,0},{type(0),0}};
    XrXirCallableParameter target_fields[]={{XR_XIR_I64,0},{type(2),0}};
    XrXirTypeNode original[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=type(0)},
        {.kind=XR_XIR_TYPE_SET,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=type(0)},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=birth_fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_ARRAY,.element=type(4)},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_SET,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=type(2)},
        {.kind=XR_XIR_TYPE_ARRAY,.element=type(2)},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=target_fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_ARRAY,.element=type(5)},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING}};
    XrXirTypes pools[]={{original,7,NULL,NULL},{shifted,8,NULL,NULL}};
    XrXirTypeArena *birth=NULL,*target=NULL,*unrelated=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[0],&birth)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&target)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&unrelated)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot roots[]={{type(1),type(3)},{type(2),type(1)},
        {type(3),type(4)},{type(5),type(6)},{type(6),type(7)}};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,birth,roots,5)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    uint64_t physical=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission={birth,domain,NULL,NULL,4000000,128*1024};
    XrXirValue scalar={XR_XIR_I64,0,42},key={XR_XIR_I64,0,7},inner={0},map={0};
    XrXirValue text={0},second={0},left={0},right={0},incoming={0},result={0},item={0};
    CHECK(xr_xir_array_new(type(0),&scalar,1,&admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new(type(1),&key,&inner,1,&admission,&map)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"a",1,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"b",1,&second)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_set_new(type(2),&text,1,&admission,&left)==XR_XIR_VALUE_OK);
    admission.arena=target;
    CHECK(xr_xir_array_new(type(2),&scalar,1,&admission,&incoming)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_set_new(type(1),&second,1,&admission,&right)==XR_XIR_VALUE_OK);
    xr_xir_compile_type_arena_drop(birth);
    XrXirFaultDetail fault={0};bool found=false;int64_t length=-1;
    XrXirValuePathStep steps[]={{XR_XIR_PATH_KEY,type(3),7},{XR_XIR_PATH_INDEX,type(2),0}};
    XrXirValuePath path={steps,2};XrXirValuePlace place={type(3),&map.payload};
    CHECK(xr_xir_value_path_read(&place,&path,&admission,&item,&fault)==XR_XIR_VALUE_OK && item.payload==42);
    xr_xir_value_drop(&item);
    CHECK(xr_xir_map_contains_value(&map,&incoming,&admission,&found)==XR_XIR_VALUE_OK && found);
    CHECK(xr_xir_hash_key_at(&map,0,&admission,&item,&fault)==XR_XIR_VALUE_OK && item.payload==7);
    xr_xir_value_drop(&item);
    CHECK(xr_xir_map_value_at(&map,0,&admission,&item,&fault)==XR_XIR_VALUE_OK && item.payload==inner.payload);
    xr_xir_value_drop(&item);
    for (unsigned projection=0;projection<3;++projection) {
        XrXirValueStatus status=projection==0 ? xr_xir_map_keys(&map,type(2),&admission,&result) :
            projection==1 ? xr_xir_hash_values(&map,type(4),&admission,&result) :
            xr_xir_map_entries(&map,type(6),&admission,&result);
        CHECK(status==XR_XIR_VALUE_OK && xr_xir_value_arena(&result)==target);
        status=xr_xir_array_get(&result,0,&admission,&item,&fault);
        if (status!=XR_XIR_VALUE_OK)
            fprintf(stderr,"received hash projection=%u array=%u status=%u work=%llu\n",
                projection,result.type,status,(unsigned long long)admission.work);
        CHECK(status==XR_XIR_VALUE_OK);
        if (projection<2) CHECK(item.payload==(projection ? inner.payload : 7));
        else {
            XrXirValue field={0};
            CHECK(xr_xir_tuple_get(&item,1,&field)==XR_XIR_VALUE_OK && field.payload==inner.payload);
            xr_xir_value_drop(&field);
        }
        xr_xir_value_drop(&item);xr_xir_value_drop(&result);
    }
    for (unsigned operation=0;operation<4;++operation) {
        XrXirValueStatus status=operation==0 ? xr_xir_set_union(&left,&right,&admission,&result) :
            operation==1 ? xr_xir_set_intersection(&left,&right,&admission,&result) :
            operation==2 ? xr_xir_set_difference(&left,&right,&admission,&result) :
            xr_xir_set_symmetric_difference(&left,&right,&admission,&result);
        CHECK(status==XR_XIR_VALUE_OK && xr_xir_value_arena(&result)==target && result.type==(uint32_t)type(1));
        CHECK(xr_xir_hash_len(&result,&admission,&length)==XR_XIR_VALUE_OK && length==(operation==1?0:operation==2?1:2));
        xr_xir_value_drop(&result);
    }
    found=true;CHECK(xr_xir_set_is_subset(&left,&right,&admission,&found)==XR_XIR_VALUE_OK && !found);
    CHECK(xr_xir_set_is_superset(&left,&left,&admission,&found)==XR_XIR_VALUE_OK && found);
    CHECK(xr_xir_hash_values(&left,type(7),&admission,&result)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&result);
    CHECK(xr_xir_hash_values(&map,type(7),&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    admission.arena=unrelated;found=true;
    CHECK(xr_xir_map_contains_value(&map,&incoming,&admission,&found)==XR_XIR_VALUE_BAD_ARGUMENT && found);
    CHECK(xr_xir_set_union(&left,&right,&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    admission.arena=target;admission.work=0;found=true;
    CHECK(xr_xir_map_contains_value(&map,&incoming,&admission,&found)==XR_XIR_VALUE_LIMIT && found);
    CHECK(xr_xir_map_entries(&map,type(6),&admission,&result)==XR_XIR_VALUE_LIMIT && !result.type);
    CHECK(admission.arena==target && xr_xir_value_arena(&map)==birth);
    xr_xir_value_drop(&right);xr_xir_value_drop(&left);xr_xir_value_drop(&second);xr_xir_value_drop(&text);
    xr_xir_value_drop(&incoming);xr_xir_value_drop(&map);xr_xir_value_drop(&inner);
    CHECK(xr_xir_domain_stats(domain).live_bytes==physical);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(unrelated);
    puts("Received hash membership, ordinal reads, owned projections and all set operations preserve actual birth identity; lookalike and zero work reject; physical0");
}
#endif // TYPE_RECEIVING_HASH_MEMBERS_H
