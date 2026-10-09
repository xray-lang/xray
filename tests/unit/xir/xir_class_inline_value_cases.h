/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_inline_value_cases.h - Packed class projections and actual allocation owners
 *
 * KEY CONCEPT:
 *   Inline snapshots allocate in their caller domain; retained leaves keep theirs.
 */
#ifndef XIR_CLASS_INLINE_VALUE_CASES_H
#define XIR_CLASS_INLINE_VALUE_CASES_H
static XrXirTypeArena *class_inline_arena(XrXirDomain *domain) {
    (void)domain;
    XrXirNominalFieldIdentity rf[]={{{"n",1},0},{{"text",4},0},{{"tail",4},0}};
    XrXirNominalFieldIdentity ef[]={{{"value",5},0}};
    XrXirNominalFieldIdentity cf[]={{{"record",6},XR_XIR_FIELD_MUTABLE},{{"choice",6},XR_XIR_FIELD_MUTABLE},
        {{"empty",5},0},{{"text",4},0}};
    XrXirNominalVariant variants[]={{{"None",4},0,0},{{"Full",4},0,1}};
    XrXirNominalIdentity ids[]={
        {{"m",1},{"Record",6},1,0,rf,3,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}},
        {{"m",1},{"Choice",6},1,0,ef,1,XR_XIR_NOMINAL_ENUM,variants,2,0,{0}},
        {{"m",1},{"Empty",5},1,0,NULL,0,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}},
        {{"m",1},{"Owner",5},1,0,cf,4,XR_XIR_NOMINAL_CLASS,NULL,0,XR_XIR_NOMINAL_FINAL,{0}}};
    XrXirNominalTable table={NULL,4,ids};
    XrXirType rfields[]={XR_XIR_I64,XR_XIR_STRING,XR_XIR_I64};
    XrXirType efields[]={XR_XIR_CONSTRUCTED_TYPE_BASE};
    XrXirType cfields[]={XR_XIR_CONSTRUCTED_TYPE_BASE,XR_XIR_CONSTRUCTED_TYPE_BASE+1,
        XR_XIR_CONSTRUCTED_TYPE_BASE+2,XR_XIR_STRING};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,rfields,3}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={1,NULL,0,efields,1}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={2,NULL,0,NULL,0}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={3,NULL,0,cfields,4}}};
    XrXirTypes types={nodes,4,&table,NULL};XrCompileResourceLimits limits = value_compile_limits(65536, 65536, 10000);
    XrXirTypeArena *arena=NULL;CHECK(value_compile_arena(&types, 100, limits, &arena)==XR_XIR_VALUE_OK);
    const XrXirStorageLayout *layout=xr_xir_compile_type_arena_storage(arena,XR_XIR_CONSTRUCTED_TYPE_BASE+3);
    CHECK(layout && layout->value.size==8 && layout->body.size==64);
    return arena;
}
static void class_inline_faults(XrXirValue *object, const XrXirValue *fields,
    XrXirValueAdmission *owner, XrXirValueAdmission *reader) {
    for(unsigned mode=0;mode<3;++mode) {
        size_t sites=0,baseline=live;uint64_t own_bytes=xr_xir_domain_stats(owner->domain).live_bytes;
        uint64_t read_bytes=xr_xir_domain_stats(reader->domain).live_bytes;
        for(size_t pass=0;pass<=sites;++pass) {
            calls=0;fail_at=pass?pass-1:SIZE_MAX;XrXirValue value={0};
            owner->work=reader->work=100000;uint64_t scratch=reader->scratch_bytes;
            XrXirValueStatus status=mode==0 ? xr_xir_class_new(XR_XIR_CONSTRUCTED_TYPE_BASE+3,fields,4,owner,&value) :
                mode==1 ? xr_xir_class_get(object,0,reader,&value) : xr_xir_class_set(object,0,&fields[0],owner);
            if(!pass){CHECK(status==XR_XIR_VALUE_OK);sites=calls;CHECK(sites>0);}
            else CHECK(status==XR_XIR_VALUE_OOM && !value.type);
            size_t before=calls;xr_xir_value_drop(&value);CHECK(calls==before);
            CHECK(live==baseline && reader->scratch_bytes==scratch &&
                xr_xir_domain_stats(owner->domain).live_bytes==own_bytes &&
                xr_xir_domain_stats(reader->domain).live_bytes==read_bytes);
        }
        fail_at=SIZE_MAX;
    }
}
static void class_inline_capabilities(void) {
    XrXirNominalFieldIdentity fields[]={{{"f",1},0}};
    XrXirNominalIdentity id={{"m",1},{"R",1},1,0,fields,1,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}};
    XrXirNominalTable table={NULL,1,&id};XrXirType field=XR_XIR_I64;
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&field,1}},{0}};
    XrXirTypes types={nodes,2,&table,NULL};
    const XrCompileResourceLimits limits=value_compile_limits(0,1024,100);
    const ValueCompileProbe probe={&types,NULL,XR_XIR_CONSTRUCTED_TYPE_BASE,3,65536};
    value_compile_boundaries(&probe,limits);
    ValueCompileOwner owner={0};CHECK(value_compile_owner_new(&owner,limits,65536)==XR_XIR_OK);
    XrXirCompileLimits structural=owner.context.limits;
    CHECK(xr_xir_compile_class_field_verify(&owner.context,&types,XR_XIR_CONSTRUCTED_TYPE_BASE)==XR_XIR_OK);
    field=XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_compile_class_field_verify(&owner.context,&types,XR_XIR_CONSTRUCTED_TYPE_BASE)==XR_XIR_BAD_TYPE);
    field=XR_XIR_CONSTRUCTED_TYPE_BASE+1;
    const uint32_t kinds[]={XR_XIR_TYPE_CALLABLE,XR_XIR_TYPE_CELL,XR_XIR_TYPE_ARRAY};
    for(unsigned i=0;i<3;++i){nodes[1].kind=kinds[i];nodes[1].element=XR_XIR_BOOL;
        CHECK(xr_xir_compile_class_field_verify(&owner.context,&types,XR_XIR_CONSTRUCTED_TYPE_BASE)==
            (i==2 ? XR_XIR_OK : XR_XIR_BAD_TYPE));}
    /* A structurally valid array may still lack class-field capability. */
    const XrXirTypeNode excluded_nodes[]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_CONSTRUCTED_TYPE_BASE}};
    const XrXirTypes excluded_types={excluded_nodes,2,NULL,NULL};
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&excluded_types)==XR_XIR_OK);
    CHECK(xr_xir_compile_class_field_verify(&owner.context,&excluded_types,
        XR_XIR_CONSTRUCTED_TYPE_BASE+1)==XR_XIR_BAD_TYPE);
    field=XR_XIR_ERROR;
    CHECK(xr_xir_compile_class_field_verify(&owner.context,&types,XR_XIR_CONSTRUCTED_TYPE_BASE)==XR_XIR_BAD_TYPE);
    field=XR_XIR_I64;id.kind=XR_XIR_NOMINAL_CLASS;
    CHECK(xr_xir_compile_class_field_verify(&owner.context,&types,XR_XIR_CONSTRUCTED_TYPE_BASE)==XR_XIR_OK);
    id.kind=UINT32_MAX;
    CHECK(xr_xir_compile_class_field_verify(&owner.context,&types,XR_XIR_CONSTRUCTED_TYPE_BASE)==XR_XIR_BAD_TYPE);
    CHECK(!memcmp(&structural,&owner.context.limits,sizeof(structural)));
    CHECK(value_compile_stats(&owner.context).live_bytes==owner.baseline.live_bytes);
    value_compile_owner_release(&owner);
}
static void class_inline_value_cases(void) {
    class_inline_capabilities();
    size_t initial=live;XrXirDomain *owner=NULL,*reader=NULL;
    CHECK(xr_xir_domain_new(1048576,&owner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&reader)==XR_XIR_VALUE_OK);
    XrXirTypeArena *arena=class_inline_arena(owner);
    XrXirValueAdmission a={arena,owner,NULL,NULL,100000,65536},b={arena,reader,NULL,NULL,100000,65536};
    XrXirValue text={0},record={0},choice={0},empty={0},object={0};
    CHECK(xr_xir_string_new(owner,"mapped",6,&text)==XR_XIR_VALUE_OK);
    XrXirValue rf[]={{XR_XIR_I64,0,41},text,{XR_XIR_I64,0,42}};
    CHECK(xr_xir_struct_new(XR_XIR_CONSTRUCTED_TYPE_BASE,rf,3,&a,&record)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_new(XR_XIR_CONSTRUCTED_TYPE_BASE+1,1,&record,1,&a,&choice)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new(XR_XIR_CONSTRUCTED_TYPE_BASE+2,NULL,0,&a,&empty)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={record,choice,empty,text};
    CHECK(xr_xir_class_new(XR_XIR_CONSTRUCTED_TYPE_BASE+3,fields,4,&a,&object)==XR_XIR_VALUE_OK);
    class_inline_faults(&object,fields,&a,&b);
    XrXirValue out={0};XrXirValueAdmission leaf={arena,NULL,NULL,NULL,100,0};
    size_t before=calls;fail_at=calls;
    CHECK(xr_xir_class_get(&object,3,&leaf,&out)==XR_XIR_VALUE_OK && calls==before);xr_xir_value_drop(&out);
    CHECK(xr_xir_class_get(&object,0,&leaf,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);fail_at=SIZE_MAX;
    XrXirTypeArena *foreign=class_inline_arena(owner);
    XrXirValueAdmission wrong={foreign,reader,NULL,NULL,10000,65536};
    CHECK(xr_xir_class_get(&object,0,&wrong,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    xr_xir_compile_type_arena_drop(foreign);
    XirObject *string_object=object_pointer(&text);uint32_t refs=atomic_load(&string_object->references);
    atomic_store(&string_object->references,UINT32_MAX);
    CHECK(xr_xir_class_set(&object,0,&record,&a)==XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(xr_xir_class_get(&object,0,&b,&out)==XR_XIR_VALUE_REFCOUNT_LIMIT && !out.type);
    atomic_store(&string_object->references,refs);
    XirObject *class_object=object_pointer(&object);
    const XrXirStorageLayout *class_layout=class_body_layout(class_object);
    unsigned char *tag=(unsigned char *)((XirClassObject *)class_object+1)+class_layout->field_offsets[1];
    unsigned char saved_tag=*tag;*tag=255;
    CHECK(xr_xir_class_get(&object,1,&b,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    *tag=saved_tag;
    XrXirValueAdmission bounded=b;bounded.work=0;
    CHECK(xr_xir_class_get(&object,0,&bounded,&out)==XR_XIR_VALUE_LIMIT && !out.type);
    bounded=b;bounded.scratch_bytes=0;
    CHECK(xr_xir_class_get(&object,0,&bounded,&out)==XR_XIR_VALUE_LIMIT && !out.type && !bounded.scratch_bytes);
    uint64_t scratch=(uint64_t)xr_xir_compile_type_arena_storage(arena,XR_XIR_CONSTRUCTED_TYPE_BASE)->depth*sizeof(StorageUnpackFrame);
    bounded=b;bounded.scratch_bytes=scratch-1;
    CHECK(xr_xir_class_get(&object,0,&bounded,&out)==XR_XIR_VALUE_LIMIT && !out.type);
    bounded=b;bounded.scratch_bytes=scratch;
    CHECK(xr_xir_class_get(&object,0,&bounded,&out)==XR_XIR_VALUE_OK && bounded.scratch_bytes==scratch);
    uint64_t used=b.work-bounded.work;
    CHECK(object_pointer(&out)->domain==reader);xr_xir_value_drop(&out);
    bounded=b;bounded.work=used;
    CHECK(xr_xir_class_get(&object,0,&bounded,&out)==XR_XIR_VALUE_OK && !bounded.work);xr_xir_value_drop(&out);
    bounded=b;bounded.work=used-1;
    CHECK(xr_xir_class_get(&object,0,&bounded,&out)==XR_XIR_VALUE_LIMIT && !out.type);
    uint64_t limit=reader->limit,base_bytes=xr_xir_domain_stats(reader).live_bytes;
    uint64_t required=scratch+sizeof(XirNominalValue)+3*sizeof(XrXirValue);
    reader->limit=base_bytes+required;bounded=b;
    CHECK(xr_xir_class_get(&object,0,&bounded,&out)==XR_XIR_VALUE_OK);xr_xir_value_drop(&out);
    reader->limit=base_bytes+required-1;bounded=b;
    CHECK(xr_xir_class_get(&object,0,&bounded,&out)==XR_XIR_VALUE_LIMIT && !out.type);
    CHECK(xr_xir_domain_stats(reader).live_bytes==base_bytes);reader->limit=limit;
    CHECK(xr_xir_class_get(&object,2,&b,&out)==XR_XIR_VALUE_OK);xr_xir_value_drop(&out);
    XrXirValue none={0};CHECK(xr_xir_enum_new(XR_XIR_CONSTRUCTED_TYPE_BASE+1,0,NULL,0,&a,&none)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_set(&object,1,&none,&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_get(&object,1,&b,&out)==XR_XIR_VALUE_OK);uint32_t variant=99;
    CHECK(xr_xir_enum_variant(&out,&variant)==XR_XIR_VALUE_OK && variant==0);xr_xir_value_drop(&out);xr_xir_value_drop(&none);
    CHECK(xr_xir_class_get(&object,0,&b,&out)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&object);xr_xir_value_drop(&record);xr_xir_value_drop(&choice);xr_xir_value_drop(&empty);xr_xir_value_drop(&text);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(owner);xr_xir_domain_drop(reader);
    XrXirValue held={0};XrXirValueAdmission retained={xr_xir_value_arena(&out),reader,NULL,NULL,100,65536};
    CHECK(xr_xir_struct_get(&out,1,&retained,&held)==XR_XIR_VALUE_OK);xr_xir_value_drop(&out);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&held,&bytes,&length)&&length==6&&!memcmp(bytes,"mapped",6));
    before=calls;fail_at=calls;xr_xir_value_drop(&held);CHECK(calls==before && live==initial);fail_at=SIZE_MAX;
}
#endif /* XIR_CLASS_INLINE_VALUE_CASES_H */
