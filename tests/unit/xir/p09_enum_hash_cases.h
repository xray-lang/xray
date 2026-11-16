/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Exact enum keys survive compact storage, COW, spreads and failed allocation. */
#include "p09_enum_key_fixture.h"
static XrXirValueStatus enum_hash_sequence(EnumKeyPool source,EnumKeyPool receiving,XrXirDomain *domain) {
    XrXirValueAdmission admission={receiving.arena,domain,NULL,NULL,UINT64_C(10000000),65536};
    XrXirValue keys[3]={enum_key_value(source,0,domain),enum_key_value(receiving,1,domain),
        enum_key_value(receiving,0,domain)};
    XrXirValue values[]={{XR_XIR_I64,0,7},{XR_XIR_I64,0,9}},map={0},retained={0},read={0},spread={0},array={0};
    XrXirValueStatus status=XR_XIR_VALUE_OK;bool found=false;XrXirMapLiteralDuplicate duplicate={77,88};
    TRY_VALUE(xr_xir_map_new(receiving.map,keys,values,2,&admission,&map));
    TRY_VALUE(xr_xir_map_get(&map,&keys[2],&admission,&read,&found));CHECK(found&&read.payload==7);xr_xir_value_drop(&read);
    TRY_VALUE(xr_xir_value_copy(&map,&retained));
    XrXirValuePlace place={receiving.map,&map.payload,NULL};XrXirValue next={XR_XIR_I64,0,11};
    TRY_VALUE(xr_xir_map_set(&place,&keys[2],&next,&admission));CHECK(map.payload!=retained.payload);
    TRY_VALUE(xr_xir_map_get(&retained,&keys[0],&admission,&read,&found));CHECK(found&&read.payload==7);xr_xir_value_drop(&read);
    TRY_VALUE(xr_xir_map_get(&map,&keys[0],&admission,&read,&found));CHECK(found&&read.payload==11);xr_xir_value_drop(&read);
    XrXirValue repeated_keys[]={keys[0],keys[2]};
    status=xr_xir_map_literal_new(receiving.map,repeated_keys,values,NULL,2,&admission,&spread,&duplicate);
    if(status==XR_XIR_VALUE_OOM)goto done;
    CHECK(status==XR_XIR_VALUE_DUPLICATE_KEY&&!spread.type&&!spread.payload&&duplicate.first==0&&duplicate.second==1);
    XrXirValue overlay_keys[]={{0},keys[0]},overlay_values[]={retained,{XR_XIR_I64,0,5}};
    XrXirMapLiteralKind kinds[]={XR_XIR_MAP_LITERAL_SPREAD,XR_XIR_MAP_LITERAL_ENTRY};
    TRY_VALUE(xr_xir_map_literal_new(receiving.map,overlay_keys,overlay_values,kinds,2,&admission,&spread,&duplicate));
    TRY_VALUE(xr_xir_map_get(&spread,&keys[2],&admission,&read,&found));CHECK(found&&read.payload==5);xr_xir_value_drop(&read);
    TRY_VALUE(xr_xir_map_get(&spread,&keys[1],&admission,&read,&found));CHECK(found&&read.payload==9);xr_xir_value_drop(&read);
    TRY_VALUE(xr_xir_map_keys(&map,receiving.array,&admission,&array));
    XrXirFaultDetail fault={0};TRY_VALUE(xr_xir_array_get(&array,0,&admission,&read,&fault));CHECK(!fault.code);
    bool equal=false;TRY_VALUE(xr_xir_key_equal(&read,&keys[0],&admission,&equal));CHECK(equal);xr_xir_value_drop(&read);
    status=XR_XIR_VALUE_OK;
 done:
    xr_xir_value_drop(&read);xr_xir_value_drop(&array);xr_xir_value_drop(&spread);
    xr_xir_value_drop(&retained);xr_xir_value_drop(&map);
    for(uint32_t k=0;k<3;++k)xr_xir_value_drop(&keys[k]);
    return status;
}
static void enum_hash_receivers(bool faults) {
    ValueCompileOwner compiler={0};CHECK(value_compile_owner_new(&compiler,value_compile_limits(),65536)==XR_XIR_OK);
    EnumKeyPool source={0},receiving={0};EnumKeyShape shape={"keys::module","Choice",0,XR_XIR_I64,false};
    CHECK(enum_key_pool_new(&compiler,shape,&source)==XR_XIR_VALUE_OK);
    shape.shift=1;CHECK(enum_key_pool_new(&compiler,shape,&receiving)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot root={source.key,receiving.key};
    CHECK(xir_compile_type_arena_receive(&compiler.context,receiving.arena,source.arena,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    /* The receiving lineage, rather than a live producer root or compiler
     * request, pins the source's real enum descriptors during every sequence. */
    EnumKeyPool borrowed_source=source;enum_key_pool_drop(&source);value_compile_owner_release(&compiler);
    size_t before=value_allocation_calls,baseline=live;
    XrXirValueStatus baseline_status=enum_hash_sequence(borrowed_source,receiving,domain);
    if(baseline_status!=XR_XIR_VALUE_OK)fprintf(stderr,"enum hash phase=%s status=%u\n",compile_domain_phase,(unsigned)baseline_status);
    CHECK(baseline_status==XR_XIR_VALUE_OK);
    size_t sites=value_allocation_calls-before;CHECK(sites>0&&live==baseline);
    if(faults)for(size_t point=0;point<sites;++point) {
        fail_at=value_allocation_calls+point;
        XrXirValueStatus status=enum_hash_sequence(borrowed_source,receiving,domain);fail_at=SIZE_MAX;
        if(status!=XR_XIR_VALUE_OOM)fprintf(stderr,"enum hash fault=%zu/%zu phase=%s status=%u\n",
            point,sites,compile_domain_phase,(unsigned)status);
        CHECK(status==XR_XIR_VALUE_OOM&&live==baseline);
    }
    xr_xir_domain_drop(domain);enum_key_pool_drop(&source);enum_key_pool_drop(&receiving);
    value_compile_owner_release(&compiler);CHECK(!live&&!value_compile_live&&!value_compile_bytes);
}
