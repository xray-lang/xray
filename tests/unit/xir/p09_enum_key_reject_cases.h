/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Payload and malformed metadata never acquire the closed enum key domain. */
static void enum_key_profile_rejections(void) {
    XrXirNominalVariant variants[]={{{"A",1},0,0},{{"B",1},0,0}};
    XrXirNominalIdentity identity={.module={"keys",4},.name={"Choice",6},.arity=1,
        .kind=XR_XIR_NOMINAL_ENUM,.variants=variants,.variant_count=2};
    XrXirNominalTable table={NULL,1,&identity};XrXirType argument=XR_XIR_I64;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={.arguments=&argument,.argument_count=1}};
    XrXirTypes types={&node,1,&table,NULL};XrXirType key=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_key_type(&types,key));
    identity.kind=XR_XIR_NOMINAL_STRUCT;CHECK(!xr_xir_key_type(&types,key));identity.kind=XR_XIR_NOMINAL_ENUM;
    identity.field_count=1;CHECK(!xr_xir_key_type(&types,key));identity.field_count=0;
    variants[1].field_count=1;CHECK(!xr_xir_key_type(&types,key));variants[1].field_count=0;
    variants[1].field_begin=1;CHECK(!xr_xir_key_type(&types,key));variants[1].field_begin=0;
    node.nominal.argument_count=0;CHECK(!xr_xir_key_type(&types,key));node.nominal.argument_count=1;
    argument=XR_XIR_UNIT;CHECK(!xr_xir_key_type(&types,key));argument=XR_XIR_I64;
    identity.name.length=0;CHECK(!xr_xir_key_type(&types,key));identity.name.length=6;
    identity.variants=NULL;CHECK(!xr_xir_key_type(&types,key));identity.variants=variants;
    XrXirNominalDeclaration duplicate={0};table.declarations=&duplicate;CHECK(!xr_xir_key_type(&types,key));table.declarations=NULL;
    node.kind=XR_XIR_TYPE_ARRAY;CHECK(!xr_xir_key_type(&types,key));node.kind=XR_XIR_TYPE_NOMINAL;
    CHECK(xr_xir_key_type(NULL,XR_XIR_I64));
}
static void enum_key_payload_rejections(void) {
    ValueCompileOwner compiler={0};CHECK(value_compile_owner_new(&compiler,value_compile_limits(),65536)==XR_XIR_OK);
    EnumKeyPool payload={0};EnumKeyShape shape={"keys::module","Payload",0,XR_XIR_UNIT,true};
    CHECK(enum_key_pool_new(&compiler,shape,&payload)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={payload.arena,domain,NULL,NULL,UINT64_C(10000000),65536};
    XrXirValue a={0},b={0},field={XR_XIR_I64,0,7};uint64_t hash=77;bool equal=true;
    CHECK(xr_xir_enum_new(payload.key,0,NULL,0,&admission,&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new(payload.key,1,&field,1,&admission,&b)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_key_hash(&a,&admission,&hash)==XR_XIR_VALUE_UNSUPPORTED&&hash==77);
    CHECK(xr_xir_key_hash(&b,&admission,&hash)==XR_XIR_VALUE_UNSUPPORTED&&hash==77);
    CHECK(xr_xir_key_equal(&a,&a,&admission,&equal)==XR_XIR_VALUE_UNSUPPORTED&&equal);
    XrXirNominalIdentity empty={.module={"keys::module",12},.name={"Empty",5},.kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable empty_table={NULL,1,&empty};
    XrXirTypeNode empty_node={.kind=XR_XIR_TYPE_NOMINAL};XrXirTypes empty_types={&empty_node,1,&empty_table,NULL};
    XrXirTypeArena *empty_arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler.context,&empty_types,&empty_arena)==XR_XIR_VALUE_OK);
    XrXirValueAdmission empty_admission={empty_arena,domain,NULL,NULL,UINT64_C(10000000),65536};
    XrXirValue structure={0};
    CHECK(xr_xir_struct_new((XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,NULL,0,&empty_admission,&structure)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_key_hash(&structure,&empty_admission,&hash)==XR_XIR_VALUE_UNSUPPORTED&&hash==77);
    CHECK(xr_xir_key_equal(&structure,&structure,&empty_admission,&equal)==XR_XIR_VALUE_UNSUPPORTED&&equal);
    xr_xir_value_drop(&structure);xr_xir_compile_type_arena_drop(empty_arena);
    xr_xir_value_drop(&a);xr_xir_value_drop(&b);enum_key_pool_drop(&payload);xr_xir_domain_drop(domain);
    value_compile_owner_release(&compiler);CHECK(!live&&!value_compile_live&&!value_compile_bytes);
}

/* A zero-byte logical payload remains a payload. Its tag must never enter the
 * empty-enum key domain, including the otherwise empty variant. */
static void enum_key_zero_width_payload_rejections(void) {
    ValueCompileOwner compiler={0};CHECK(value_compile_owner_new(&compiler,value_compile_limits(),65536)==XR_XIR_OK);
    XrXirNominalVariant variants[]={{{"A",1},0,0},{{"B",1},0,1}};
    XrXirNominalFieldIdentity field={{"value",5},0};
    XrXirNominalIdentity identities[]={
        {.module={"keys",4},.name={"Empty",5},.kind=XR_XIR_NOMINAL_STRUCT},
        {.module={"keys",4},.name={"Payload",7},.kind=XR_XIR_NOMINAL_ENUM,.fields=&field,.field_count=1,
         .variants=variants,.variant_count=2}};
    XrXirNominalTable table={NULL,2,identities};XrXirType field_type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode nodes[]={ {.kind=XR_XIR_TYPE_NOMINAL},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1,.fields=&field_type,.field_count=1}} };
    XrXirTypes types={nodes,2,&table,NULL};XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    CHECK(!xr_xir_key_type(&types,type));XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler.context,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(!xr_xir_compile_type_arena_key_eligible(arena,type));XrXirLayout layout={0};
    CHECK(xr_xir_compile_type_arena_layout(arena,field_type,&layout)&&!layout.size);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,100000,65536};
    XrXirValue empty={0},a={0},b={0};uint64_t hash=77;
    CHECK(xr_xir_struct_new(field_type,NULL,0,&admission,&empty)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new(type,0,NULL,0,&admission,&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new(type,1,&empty,1,&admission,&b)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_key_hash(&a,&admission,&hash)==XR_XIR_VALUE_UNSUPPORTED&&hash==77);
    CHECK(xr_xir_key_hash(&b,&admission,&hash)==XR_XIR_VALUE_UNSUPPORTED&&hash==77);
    xr_xir_value_drop(&a);xr_xir_value_drop(&b);xr_xir_value_drop(&empty);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    /* Unit fields are rejected by the existing nominal structural owner. They
     * must not acquire key eligibility merely because physical width is zero. */
    field_type=XR_XIR_UNIT;CHECK(!xr_xir_key_type(&types,type));arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler.context,&types,&arena)==XR_XIR_VALUE_BAD_ARGUMENT&&!arena);
    value_compile_owner_release(&compiler);CHECK(!live&&!value_compile_live&&!value_compile_bytes);
}
