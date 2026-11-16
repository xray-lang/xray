/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Every finite-work COW failure preserves the complete published enum Map. */
static void enum_hash_budget_receivers(void) {
    ValueCompileOwner compiler={0};CHECK(value_compile_owner_new(&compiler,value_compile_limits(),65536)==XR_XIR_OK);
    EnumKeyPool pool={0};EnumKeyShape shape={"keys::module","Choice",0,XR_XIR_I64,false};
    CHECK(enum_key_pool_new(&compiler,shape,&pool)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={pool.arena,domain,NULL,NULL,UINT64_C(10000000),65536};
    XrXirValue keys[]={enum_key_value(pool,0,domain),enum_key_value(pool,1,domain)};
    XrXirValue values[]={{XR_XIR_I64,0,7},{XR_XIR_I64,0,9}},map={0},snapshot={0},read={0};
    CHECK(xr_xir_map_new(pool.map,keys,values,2,&admission,&map)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&map,&snapshot)==XR_XIR_VALUE_OK);
    XirHash *original=(XirHash *)object_pointer(&map);unsigned char bytes[512];size_t size=0;
    CHECK(hash_data_size(original->capacity,original->stride,&size)&&size<=sizeof(bytes));memcpy(bytes,original->data,size);
    size_t baseline=live;uint64_t physical=xr_xir_domain_stats(domain).live_bytes;
    XrXirValuePlace place={pool.map,&map.payload,NULL};XrXirValue replacement={XR_XIR_I64,0,11};
    uint64_t prefix=0;
    for(;;++prefix) {
        admission.work=prefix;XrXirValueStatus status=xr_xir_map_set(&place,&keys[0],&replacement,&admission);
        if(status==XR_XIR_VALUE_OK)break;
        CHECK(status==XR_XIR_VALUE_LIMIT&&prefix<4096&&map.payload==snapshot.payload&&
            original->length==2&&original->order_count==2&&!memcmp(bytes,original->data,size)&&
            live==baseline&&xr_xir_domain_stats(domain).live_bytes==physical);
    }
    CHECK(prefix>0&&map.payload!=snapshot.payload);admission.work=UINT64_C(10000000);bool found=false;
    CHECK(xr_xir_map_get(&snapshot,&keys[0],&admission,&read,&found)==XR_XIR_VALUE_OK&&found&&read.payload==7);
    xr_xir_value_drop(&read);
    CHECK(xr_xir_map_get(&map,&keys[0],&admission,&read,&found)==XR_XIR_VALUE_OK&&found&&read.payload==11);
    xr_xir_value_drop(&read);
    XrXirDomain *small=NULL;CHECK(xr_xir_domain_new(sizeof(XrXirDomain),&small)==XR_XIR_VALUE_OK);
    uint64_t small_before=xr_xir_domain_stats(small).live_bytes;
    XrXirValueAdmission finite={pool.arena,small,NULL,NULL,UINT64_C(10000000),65536};XrXirValue failed={0};
    CHECK(xr_xir_map_new(pool.map,keys,values,1,&finite,&failed)==XR_XIR_VALUE_LIMIT&&!failed.type&&!failed.payload);
    CHECK(xr_xir_domain_stats(small).live_bytes==small_before);xr_xir_domain_drop(small);
    XrXirDomain *bounded=NULL;CHECK(xr_xir_domain_new(1048576,&bounded)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetControls controls={UINT64_MAX,UINT64_MAX,1,UINT64_MAX,UINT64_MAX};
    CHECK(xr_xir_domain_budget_bind(bounded,&controls)==XR_XIR_VALUE_OK);uint64_t hash=77;
    XrXirValueAdmission budgeted={pool.arena,bounded,NULL,NULL,1000,65536};
    CHECK(xr_xir_key_hash(&keys[0],&budgeted,&hash)==XR_XIR_VALUE_LIMIT&&hash==77);
    CHECK(xr_xir_domain_budget_stats(bounded).work==0);xr_xir_domain_drop(bounded);
    xr_xir_value_drop(&snapshot);xr_xir_value_drop(&map);xr_xir_value_drop(&keys[0]);xr_xir_value_drop(&keys[1]);
    xr_xir_domain_drop(domain);enum_key_pool_drop(&pool);value_compile_owner_release(&compiler);
    CHECK(!live&&!value_compile_live&&!value_compile_bytes);
}

/* Full metadata scans belong to structural admission. Once published, empty
 * maps and keys have the same work contract for small and large enum tables. */
static void enum_hash_sealed_metadata_work(void) {
    ValueCompileOwner compiler={0};CHECK(value_compile_owner_new(&compiler,value_compile_limits(),65536)==XR_XIR_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    uint64_t consumed=0;
    for(uint32_t pass=0;pass<2;++pass) {
        XrXirNominalVariant variants[96]={0};char names[96][8]={{0}};
        uint32_t count=pass?96u:2u;
        for(uint32_t v=0;v<count;++v) {
            int length=snprintf(names[v],sizeof(names[v]),"V%03u",v);CHECK(length==4);
            variants[v]=(XrXirNominalVariant){{names[v],4},0,0};
        }
        XrXirNominalIdentity identity={.module={"keys",4},.name={"Wide",4},.kind=XR_XIR_NOMINAL_ENUM,
            .variants=variants,.variant_count=count};
        XrXirNominalTable table={NULL,1,&identity};
        XrXirType key=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,map_type=(XrXirType)(key+1);
        XrXirTypeNode nodes[]={ {.kind=XR_XIR_TYPE_NOMINAL},
            {.kind=XR_XIR_TYPE_MAP,.element=key,.result=XR_XIR_I64} };
        XrXirTypes types={nodes,2,&table,NULL};XrXirTypeArena *arena=NULL;
        CHECK(xr_xir_compile_type_arena_new(&compiler.context,&types,&arena)==XR_XIR_VALUE_OK);
        memset(variants,0xa5,sizeof(variants));memset(names,0xa5,sizeof(names));
        CHECK(xr_xir_compile_type_arena_key_eligible(arena,key));
        CHECK(!xr_xir_compile_type_arena_key_eligible(NULL,key)&&
            !xr_xir_compile_type_arena_key_eligible(arena,(XrXirType)(key+2)));
        XrXirValueAdmission admission={arena,domain,NULL,NULL,1,65536};XrXirValue map={0},value={0};
        CHECK(xr_xir_map_new(map_type,NULL,NULL,0,&admission,&map)==XR_XIR_VALUE_OK&&!admission.work);
        CHECK(xr_xir_value_valid(&map));xr_xir_value_drop(&map);
        admission.work=1000;CHECK(xr_xir_enum_new(key,0,NULL,0,&admission,&value)==XR_XIR_VALUE_OK);
        admission.work=1000;uint64_t hash=77;
        CHECK(xr_xir_key_hash(&value,&admission,&hash)==XR_XIR_VALUE_OK);
        if(!pass)consumed=1000-admission.work;else CHECK(consumed==1000-admission.work);
        CHECK(consumed>0);xr_xir_value_drop(&value);xr_xir_compile_type_arena_drop(arena);
    }
    xr_xir_domain_drop(domain);value_compile_owner_release(&compiler);
    CHECK(!live&&!value_compile_live&&!value_compile_bytes);
}
