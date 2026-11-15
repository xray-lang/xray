/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * A matching nominal header cannot fabricate the arena's sealed enum value. */
static void enum_key_forged_value_rejections(void) {
    ValueCompileOwner compiler={0};CHECK(value_compile_owner_new(&compiler,value_compile_limits(),65536)==XR_XIR_OK);
    EnumKeyPool pool={0},foreign={0};EnumKeyShape shape={"keys::module","Choice",0,XR_XIR_I64,false};
    CHECK(enum_key_pool_new(&compiler,shape,&pool)==XR_XIR_VALUE_OK);
    CHECK(enum_key_pool_new(&compiler,shape,&foreign)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={pool.arena,domain,NULL,NULL,1000000,65536};
    XrXirValue key=enum_key_value(pool,0,domain),other=enum_key_value(foreign,0,domain);
    XirNominalValue fake={0};xir_object_initialize(&fake.object);
    fake.object.arena=pool.arena;fake.object.type=pool.key;fake.object.kind=XR_XIR_TYPE_NOMINAL;
    const XirNominalValue *record=&fake;XrXirValue invented={(uint32_t)pool.key,0,0};
    memcpy(&invented.payload,&record,sizeof(record));
    XrXirValue map={0},read={0},value={XR_XIR_I64,0,7};bool found=true;
    CHECK(xr_xir_map_new(pool.map,&key,&value,1,&admission,&map)==XR_XIR_VALUE_OK);
    XrXirValue rejected[]={key,key,key,invented,other};
    rejected[0].reserved=1;rejected[1].payload=0;rejected[2].type=(uint32_t)pool.map;
    for(uint32_t i=0;i<5;++i) {
        uint64_t hash=77;bool equal=true;admission.work=1000000;
        CHECK(xr_xir_key_hash(&rejected[i],&admission,&hash)==XR_XIR_VALUE_BAD_ARGUMENT&&hash==77);
        CHECK(xr_xir_key_equal(&key,&rejected[i],&admission,&equal)==XR_XIR_VALUE_BAD_ARGUMENT&&equal);
        found=true;CHECK(xr_xir_map_get(&map,&rejected[i],&admission,&read,&found)==XR_XIR_VALUE_BAD_ARGUMENT&&
            !read.type&&!read.payload&&found);
    }
    XirNominalValue *actual=(XirNominalValue *)object_pointer(&key);
    XrXirDomain *saved_domain=actual->object.domain;actual->object.domain=domain;
    uint64_t hash=77;bool equal=true;
    bool valid=xr_xir_value_valid(&key),header_valid=value_header_valid(&key);
    XrXirValueStatus hashed=xr_xir_key_hash(&key,&admission,&hash);
    XrXirValueStatus compared=xr_xir_key_equal(&key,&key,&admission,&equal);
    XrXirValueStatus mapped=xr_xir_map_get(&map,&key,&admission,&read,&found);
    actual->object.domain=saved_domain;
    CHECK(!valid&&!header_valid&&mapped==XR_XIR_VALUE_BAD_ARGUMENT&&!read.type&&!read.payload);
    CHECK(hashed==XR_XIR_VALUE_BAD_ARGUMENT&&compared==XR_XIR_VALUE_BAD_ARGUMENT&&hash==77&&equal);
    uint32_t saved_kind=actual->object.kind;actual->object.kind=XR_XIR_TYPE_ARRAY;
    hashed=xr_xir_key_hash(&key,&admission,&hash);actual->object.kind=saved_kind;
    CHECK(hashed==XR_XIR_VALUE_UNSUPPORTED&&hash==77);
    CHECK(xr_xir_value_valid(&key));xr_xir_value_drop(&map);xr_xir_value_drop(&key);xr_xir_value_drop(&other);
    xr_xir_domain_drop(domain);enum_key_pool_drop(&pool);enum_key_pool_drop(&foreign);
    value_compile_owner_release(&compiler);CHECK(!live&&!value_compile_live&&!value_compile_bytes);
}
