/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_array_value_cases.h - Array handles in compact class bodies
 *
 * KEY CONCEPT:
 *   Class identity retains logical Array values and their real allocation owners.
 */
#ifndef XIR_CLASS_ARRAY_VALUE_CASES_H
#define XIR_CLASS_ARRAY_VALUE_CASES_H
static XrXirTypeArena *class_array_field_arena(XrXirDomain *domain) {
    XrXirNominalFieldIdentity fields[]={{{"flag",4},0},{{"items",5},XR_XIR_FIELD_MUTABLE},{{"text",4},XR_XIR_FIELD_MUTABLE}};
    XrXirNominalIdentity identity={{"module",6},{"Bag",3},1,0,fields,3,XR_XIR_NOMINAL_CLASS,NULL,0,XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table={NULL,1,&identity};XrXirType field_types[]={XR_XIR_BOOL,(XrXirType)256,(XrXirType)257};
    XrXirTypeNode nodes[]={
        {XR_XIR_TYPE_ARRAY,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0,{0}},
        {XR_XIR_TYPE_ARRAY,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0,{0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,NULL,0,field_types,3}}};
    XrXirTypes types={nodes,3,&table,NULL};
    XrXirBudget budget={.parameters=100,.metadata_bytes=65536,.scratch_bytes=65536,.work=10000};
    XrXirTypeArena *arena=NULL;CHECK(xr_xir_type_arena_new(domain,&types,&budget,&arena)==XR_XIR_VALUE_OK);
    return arena;
}
static void class_array_field_value_cases(void) {
    size_t initial=live;XrXirDomain *domain=NULL,*foreign=NULL;
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&foreign)==XR_XIR_VALUE_OK);
    XrXirTypeArena *arena=class_array_field_arena(domain);
    const XrXirStorageLayout *layout=xr_xir_type_arena_storage(arena,(XrXirType)258);
    CHECK(layout && layout->value.size==8 && layout->value.alignment==8 && layout->body.size==24);
    CHECK(layout->field_offsets[0]==0 && layout->field_offsets[1]==8 && layout->field_offsets[2]==16);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    XrXirValueAdmission remote={arena,foreign,NULL,NULL,1000000,65536};
    XrXirValue forty={XR_XIR_I64,0,40},number={0},text={0},texts={0};
    CHECK(xr_xir_array_new((XrXirType)256,&forty,1,&remote,&number)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(foreign,"mapped",6,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)257,&text,1,&remote,&texts)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={{XR_XIR_BOOL,0,1},number,texts},object={0},alias={0},invalid={0},old={0};
    CHECK(xr_xir_class_new((XrXirType)258,fields,3,&admission,&object)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&object,&alias)==XR_XIR_VALUE_OK && alias.payload==object.payload);
    CHECK(xr_xir_class_get(&object,1,&old)==XR_XIR_VALUE_OK && old.payload==number.payload);
    CHECK(xr_xir_class_set(&object,1,&number,&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_set(&object,0,&fields[0],&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_set(&object,1,&texts,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_set(&object,1,&number,&remote)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirValueAdmission bounded=admission;bounded.work=0;
    CHECK(xr_xir_class_set(&object,1,&number,&bounded)==XR_XIR_VALUE_LIMIT);
    CHECK(xr_xir_class_new((XrXirType)258,fields,2,&admission,&invalid)==XR_XIR_VALUE_BAD_ARGUMENT && !invalid.type);
    XirObject *last=object_pointer(&texts);uint32_t refs=atomic_load(&last->references);
    size_t baseline=live;uint64_t bytes=xr_xir_domain_stats(domain).live_bytes;
    atomic_store(&last->references,UINT32_MAX);
    CHECK(xr_xir_class_new((XrXirType)258,fields,3,&admission,&invalid)==XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!invalid.type && live==baseline && xr_xir_domain_stats(domain).live_bytes==bytes);
    CHECK(xr_xir_class_set(&object,2,&texts,&admission)==XR_XIR_VALUE_REFCOUNT_LIMIT);
    atomic_store(&last->references,refs);
    XrXirValue observed={0};CHECK(xr_xir_class_get(&object,1,&observed)==XR_XIR_VALUE_OK && observed.payload==old.payload);xr_xir_value_drop(&observed);
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass){calls=0;fail_at=pass?pass-1:SIZE_MAX;
        XrXirValue value={0};XrXirValueStatus status=xr_xir_class_new((XrXirType)258,fields,3,&admission,&value);
        if(!pass){CHECK(status==XR_XIR_VALUE_OK);sites=calls;CHECK(sites>0);}else CHECK(status==XR_XIR_VALUE_OOM && !value.type);
        size_t before=calls;xr_xir_value_drop(&value);CHECK(calls==before && live==baseline && xr_xir_domain_stats(domain).live_bytes==bytes);
    }
    fail_at=SIZE_MAX;
    xr_xir_value_drop(&old);xr_xir_value_drop(&alias);xr_xir_value_drop(&number);xr_xir_value_drop(&texts);xr_xir_value_drop(&text);
    CHECK(xr_xir_class_get(&object,2,&observed)==XR_XIR_VALUE_OK);
    xr_xir_type_arena_drop(arena);xr_xir_domain_drop(domain);xr_xir_domain_drop(foreign);
    size_t before=calls;fail_at=calls;xr_xir_value_drop(&object);CHECK(calls==before);
    XrXirValueAdmission retained={xr_xir_value_arena(&observed),NULL,NULL,NULL,64,0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&observed,0,&retained,&text,&fault)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&observed);const char *data=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&text,&data,&length)&&length==6&&!memcmp(data,"mapped",6));
    xr_xir_value_drop(&text);CHECK(calls==before && live==initial);fail_at=SIZE_MAX;
    printf("class Array fields: %zu NEW failure sites, cross-domain backing, physical release PASS\n",sites);
}
#endif
