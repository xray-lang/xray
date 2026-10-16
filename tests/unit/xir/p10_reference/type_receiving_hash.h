/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_hash.h - Hash operations retain actual compact owner coordinates
 */
#ifndef TYPE_RECEIVING_HASH_H
#define TYPE_RECEIVING_HASH_H
#include "xir/xxir_map.h"
static void received_hash_array(const XrXirValue *map,const XrXirValue *key,
    const XrXirValue *expected,XrXirValueAdmission *admission) {
    XrXirValue read={0};bool found=false;
    CHECK(xr_xir_map_get(map,key,admission,&read,&found)==XR_XIR_VALUE_OK && found);
    CHECK(read.type==expected->type && read.payload==expected->payload &&
        xr_xir_value_arena(&read)==xr_xir_value_arena(expected));
    xr_xir_value_drop(&read);
}
static void received_hash_storage(SourceFixtureOwner *compiler) {
    XrXirTypeNode original[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=type(0)},
        {.kind=XR_XIR_TYPE_SET,.element=XR_XIR_STRING}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_SET,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=type(2)}};
    XrXirTypes pools[]={{original,3,NULL,NULL},{shifted,4,NULL,NULL}};
    XrXirTypeArena *birth=NULL,*target=NULL,*unrelated=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[0],&birth)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&target)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&unrelated)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot roots[]={{type(1),type(3)},{type(2),type(1)}};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,birth,roots,2)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    uint64_t physical=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission={birth,domain,NULL,NULL,4000000,128*1024};
    XrXirValue scalar={XR_XIR_I64,0,42},key={XR_XIR_I64,0,7},inner={0},incoming={0};
    XrXirValue map={0},alias={0},deleted={0},cleared={0},set={0},set_alias={0},text={0},new_map={0},foreign={0};
    CHECK(xr_xir_array_new(type(0),&scalar,1,&admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new(type(1),&key,&inner,1,&admission,&map)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"key",3,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_set_new(type(2),&text,1,&admission,&set)==XR_XIR_VALUE_OK);
    admission.arena=target;int64_t length=-731;bool present=false;
    CHECK(xr_xir_hash_len(&map,&admission,&length)==XR_XIR_VALUE_OK && length==1);
    CHECK(admission.arena==target && xr_xir_value_arena(&map)==birth && map.type==(uint32_t)type(1));
    received_hash_array(&map,&key,&inner,&admission);
    CHECK(xr_xir_map_contains_key(&map,&key,&admission,&present)==XR_XIR_VALUE_OK && present);
    CHECK(xr_xir_map_new(type(3),&key,&inner,1,&admission,&new_map)==XR_XIR_VALUE_OK);
    received_hash_array(&new_map,&key,&inner,&admission);
    xr_xir_value_drop(&new_map);
    scalar.payload=63;
    CHECK(xr_xir_array_new(type(2),&scalar,1,&admission,&incoming)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&map,&alias)==XR_XIR_VALUE_OK);
    XrXirValuePlace place={type(3),&map.payload};
    CHECK(xr_xir_map_set(&place,&key,&incoming,&admission)==XR_XIR_VALUE_OK && map.payload!=alias.payload);
    CHECK(xr_xir_value_arena(&map)==birth && map.type==(uint32_t)type(1));
    received_hash_array(&map,&key,&incoming,&admission);received_hash_array(&alias,&key,&inner,&admission);
    key.payload=8;CHECK(xr_xir_map_set(&place,&key,&inner,&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&map,&deleted)==XR_XIR_VALUE_OK);
    XrXirValuePlace deleting={type(3),&deleted.payload};
    CHECK(xr_xir_hash_delete(&deleting,&key,&admission,&present)==XR_XIR_VALUE_OK && present);
    CHECK(xr_xir_map_contains_key(&deleted,&key,&admission,&present)==XR_XIR_VALUE_OK && !present);
    received_hash_array(&map,&key,&inner,&admission);
    CHECK(xr_xir_value_copy(&map,&cleared)==XR_XIR_VALUE_OK);
    XrXirValuePlace clearing={type(3),&cleared.payload};
    CHECK(xr_xir_hash_clear(&clearing,&admission)==XR_XIR_VALUE_OK && cleared.payload!=map.payload);
    CHECK(xr_xir_hash_len(&cleared,&admission,&length)==XR_XIR_VALUE_OK && !length);
    CHECK(xr_xir_hash_clear(&deleting,&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_hash_delete(&place,&key,&admission,&present)==XR_XIR_VALUE_OK && present);
    CHECK(xr_xir_value_copy(&set,&set_alias)==XR_XIR_VALUE_OK);
    XrXirValuePlace set_place={type(1),&set.payload};
    CHECK(xr_xir_set_contains(&set,&text,&admission,&present)==XR_XIR_VALUE_OK && present);
    CHECK(xr_xir_hash_delete(&set_place,&text,&admission,&present)==XR_XIR_VALUE_OK && present);
    CHECK(xr_xir_set_contains(&set_alias,&text,&admission,&present)==XR_XIR_VALUE_OK && present);
    CHECK(xr_xir_set_add(&set_place,&text,&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_hash_clear(&set_place,&admission)==XR_XIR_VALUE_OK);
    uint64_t work=admission.work,scratch=admission.scratch_bytes;int64_t root=map.payload;
    admission.work=0;length=-731;
    CHECK(xr_xir_hash_len(&map,&admission,&length)==XR_XIR_VALUE_LIMIT && length==-731);
    CHECK(admission.arena==target && admission.scratch_bytes==scratch && map.payload==root);
    admission.work=work;admission.arena=unrelated;
    CHECK(xr_xir_hash_len(&map,&admission,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==-731);
    CHECK(xr_xir_map_set(&place,&key,&incoming,&admission)==XR_XIR_VALUE_BAD_ARGUMENT && map.payload==root);
    CHECK(admission.arena==unrelated && admission.scratch_bytes==scratch);
    CHECK(xr_xir_array_new(type(2),&scalar,1,&admission,&foreign)==XR_XIR_VALUE_OK);
    admission.arena=target;
    CHECK(xr_xir_map_set(&place,&key,&foreign,&admission)==XR_XIR_VALUE_BAD_ARGUMENT && map.payload==root);
    XrXirValuePlace wrong_place={type(1),&map.payload};
    CHECK(xr_xir_map_set(&wrong_place,&key,&incoming,&admission)==XR_XIR_VALUE_BAD_ARGUMENT && map.payload==root);
    xr_xir_value_drop(&foreign);
    CHECK(xr_xir_value_admit(&map,type(3),&admission)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&set_alias);xr_xir_value_drop(&set);xr_xir_value_drop(&text);
    xr_xir_value_drop(&cleared);xr_xir_value_drop(&deleted);xr_xir_value_drop(&alias);xr_xir_value_drop(&map);
    xr_xir_value_drop(&incoming);xr_xir_value_drop(&inner);
    CHECK(xr_xir_domain_stats(domain).live_bytes==physical && admission.arena==target && admission.scratch_bytes==scratch);
    xr_xir_compile_type_arena_drop(unrelated);xr_xir_compile_type_arena_drop(target);
    xr_xir_compile_type_arena_drop(birth);xr_xir_domain_drop(domain);
}
#endif // TYPE_RECEIVING_HASH_H
