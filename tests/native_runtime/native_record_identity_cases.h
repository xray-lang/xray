/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_record_identity_cases.h - Nominal declarations survive physical projection
 */
static void native_record_identity_cases(void) {
    char module[]="alpha", name[]="Value", z[]="z", a[]="a";
    XrtCallableParameterSpec first[]={
        {.value={XRT_CALLABLE_SCALAR,XR_POD_I64},.name=z,.name_length=1,.field_flags=XR_XIR_FIELD_MUTABLE},
        {.value={XRT_CALLABLE_STRING,0},.name=a,.name_length=1,.field_flags=XR_XIR_FIELD_PRIVATE},
        {.value={XRT_CALLABLE_ARRAY,0}}};
    XrtCallableParameterSpec second[3]; memcpy(second,first,sizeof(first));
    second[2].value.type=1;
    XrtCallableTypeSpec catalog[4];
    catalog[0]=(XrtCallableTypeSpec){.kind=XRT_CALLABLE_RECORD,.count=2,.parameters=first,.parameter_count=3,
        .module=module,.module_length=5,.name=name,.name_length=5,.flags=XR_XIR_NOMINAL_MODULE_LITERAL,.exported=1};
    catalog[1]=catalog[0];catalog[1].parameters=second;
    catalog[2]=catalog[0];catalog[2].module="other";
    catalog[3]=catalog[0];catalog[3].name="Other";
    XrXirTypeArena *arena=NULL;
    OK(xrt_callable_types_new(&(XrtCallableImage){.metadata=XRT_CALLABLE_IMAGE_HEADER,.types=catalog,.type_count=4},UINT64_MAX,&arena));
    memset(module,'x',5);memset(name,'x',5);z[0]='x';a[0]='x';
    memset(first,0xa5,sizeof(first));memset(second,0xa5,sizeof(second));memset(catalog,0xa5,sizeof(catalog));
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);
    XrXirType base=XR_XIR_CONSTRUCTED_TYPE_BASE+XRT_CALLABLE_PREFIX_COUNT;
    const XrXirTypeNode *one=xr_xir_type_node(types,base),*two=xr_xir_type_node(types,(XrXirType)(base+1));
    const XrXirTypeNode *other_module=xr_xir_type_node(types,(XrXirType)(base+2));
    const XrXirTypeNode *other_name=xr_xir_type_node(types,(XrXirType)(base+3));
    CHECK(one && two && other_module && other_name && one->nominal.declaration==two->nominal.declaration &&
        one->nominal.declaration!=other_module->nominal.declaration && one->nominal.declaration!=other_name->nominal.declaration &&
        one->nominal.argument_count==1 && one->nominal.arguments[0]!=two->nominal.arguments[0]);
    const XrXirNominalIdentity *identity=&types->nominals->identities[one->nominal.declaration];
    CHECK(identity->module.length==5 && !memcmp(identity->module.bytes,"alpha",5) &&
        identity->name.length==5 && !memcmp(identity->name.bytes,"Value",5) && identity->exported==1 &&
        identity->flags==XR_XIR_NOMINAL_MODULE_LITERAL && identity->arity==1 && identity->field_count==2 &&
        identity->fields[0].name.bytes[0]=='z' && identity->fields[0].flags==XR_XIR_FIELD_MUTABLE &&
        identity->fields[1].name.bytes[0]=='a' && identity->fields[1].flags==XR_XIR_FIELD_PRIVATE);
    CHECK(xrt_boxed_type(arena,base) && xrt_boxed_type(arena,(XrXirType)(base+1)));
    xr_xir_compile_type_arena_drop(arena);arena=NULL;
    for(unsigned conflict=0;conflict<9;++conflict) {
        XrtCallableParameterSpec fields[2]={{.value={XRT_CALLABLE_SCALAR,XR_POD_I64},.name="value",.name_length=5},
            {.value={XRT_CALLABLE_SCALAR,XR_POD_I64},.name="value",.name_length=5}};
        catalog[0]=(XrtCallableTypeSpec){.kind=XRT_CALLABLE_RECORD,.count=1,.parameters=fields,.parameter_count=1,
            .module="test",.module_length=4,.name="Value",.name_length=5};
        catalog[1]=catalog[0];catalog[1].parameters=&fields[1];
        if(conflict==0) fields[1].value=(XrtCallableValueSpec){XRT_CALLABLE_STRING,0};
        if(conflict==1) fields[1].name="other";
        if(conflict==2) fields[1].field_flags=XR_XIR_FIELD_MUTABLE;
        if(conflict==3) catalog[1].exported=1;
        if(conflict==4) catalog[1].flags=XR_XIR_NOMINAL_MODULE_LITERAL;
        if(conflict==5) catalog[1].module=NULL;
        if(conflict==6) catalog[1].count=2;
        if(conflict==7) fields[1].field_flags=3;
        if(conflict==8) { catalog[1].parameters=fields;catalog[1].count=2;catalog[1].parameter_count=2; }
        CHECK(xrt_callable_types_new(&(XrtCallableImage){.metadata=XRT_CALLABLE_IMAGE_HEADER,.types=catalog,.type_count=2},UINT64_MAX,&arena)==XR_XIR_VALUE_BAD_ARGUMENT && !arena);
    }
    XrtCallableTypeSpec anonymous={.kind=XRT_CALLABLE_RECORD};
    CHECK(xrt_callable_types_new(&(XrtCallableImage){.metadata=XRT_CALLABLE_IMAGE_HEADER,.types=&anonymous,.type_count=1},UINT64_MAX,&arena)==XR_XIR_VALUE_BAD_ARGUMENT && !arena);
}
