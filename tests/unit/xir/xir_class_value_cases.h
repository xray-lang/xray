/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_value_cases.h - Class aliasing, transactional fields and physical release
 *
 * KEY CONCEPT:
 *   Identity handles pin their field storage, type arena and physical accounting domain.
 */
#ifndef XIR_CLASS_VALUE_CASES_H
#define XIR_CLASS_VALUE_CASES_H
#include "xir/xxir_class.h"
/* Include in the counted value-runtime amalgam, with calls/live/fail_at. */
static XrXirTypeArena *class_value_arena(XrXirDomain *domain) {
    XrXirNominalFieldIdentity fields[]={{{"count",5},XR_XIR_FIELD_MUTABLE},{{"label",5},0},{{"text",4},XR_XIR_FIELD_MUTABLE}};
    XrXirNominalIdentity identity={{"module",6},{"Counter",7},1,0,fields,3,XR_XIR_NOMINAL_CLASS,NULL,0,XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table={NULL,1,&identity};XrXirType types_[]={XR_XIR_I64,XR_XIR_STRING,XR_XIR_STRING};
    XrXirTypeNode nodes[]={
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,NULL,0,types_,3}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)256,NULL,0,XR_XIR_UNIT,0,0,{0}}};
    XrXirTypes types={nodes,2,&table,NULL};XrXirBudget budget={0};
    budget.scratch_bytes=1048576;budget.metadata_bytes=1048576;budget.work=1000000;budget.parameters=100;
    XrXirTypeArena *arena=NULL;CHECK(xr_xir_type_arena_new(domain,&types,&budget,&arena)==XR_XIR_VALUE_OK);
    memset(fields,0xcc,sizeof(fields));memset(nodes,0xcc,sizeof(nodes));
    return arena;
}
static void class_value_count(const XrXirValue *value,int64_t expected) {
    XrXirValue result={0};CHECK(xr_xir_class_get(value,0,&result)==XR_XIR_VALUE_OK);
    CHECK(result.type==XR_XIR_I64 && result.payload==expected);xr_xir_value_drop(&result);
}
static void class_value_cases(void) {
    size_t initial=live;XrXirDomain *domain=NULL,*foreign=NULL;
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&foreign)==XR_XIR_VALUE_OK);
    XrXirTypeArena *arena=class_value_arena(domain);
    const XrXirStorageLayout *layout=xr_xir_type_arena_storage(arena,(XrXirType)256);
    CHECK(layout && layout->value.size==8 && layout->value.alignment==8);
    CHECK(layout->body.size==24 && layout->body.alignment==8 && layout->field_count==3);
    CHECK(layout->field_offsets[0]==0 && layout->field_offsets[1]==8 && layout->field_offsets[2]==16);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    XrXirValue label={0},text={0};CHECK(xr_xir_string_new(domain,"retained",8,&label)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"before",6,&text)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={{XR_XIR_I64,0,40},label,text};XrXirValue first={0},alias={0},separate={0};
    CHECK(xr_xir_class_new((XrXirType)256,fields,3,&admission,&first)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&first,&alias)==XR_XIR_VALUE_OK && first.payload==alias.payload);
    CHECK(xr_xir_class_new((XrXirType)256,fields,3,&admission,&separate)==XR_XIR_VALUE_OK && first.payload!=separate.payload);
    XrXirValue forty_one={XR_XIR_I64,0,41};
    CHECK(xr_xir_class_set(&alias,0,&forty_one,&admission)==XR_XIR_VALUE_OK);
    class_value_count(&first,41);class_value_count(&separate,40);
    CHECK(xr_xir_class_set(&first,1,&text,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_set(&first,0,&text,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);class_value_count(&first,41);
    XrXirValueAdmission denied=admission;denied.domain=foreign;
    CHECK(xr_xir_class_set(&first,0,&fields[0],&denied)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_value_admit(&first,(XrXirType)256,&denied)==XR_XIR_VALUE_BAD_ARGUMENT);class_value_count(&first,41);
    denied=admission;denied.work=0;CHECK(xr_xir_class_set(&first,0,&fields[0],&denied)==XR_XIR_VALUE_LIMIT);class_value_count(&first,41);
    XrXirValue invalid={0};CHECK(xr_xir_class_new((XrXirType)256,fields,2,&admission,&invalid)==XR_XIR_VALUE_BAD_ARGUMENT && !invalid.type);
    CHECK(xr_xir_struct_new((XrXirType)256,fields,3,&admission,&invalid)==XR_XIR_VALUE_BAD_ARGUMENT && !invalid.type);
    XrXirValue array={0},array_alias={0},element={0};
    CHECK(xr_xir_array_new((XrXirType)257,&first,1,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&array,&array_alias)==XR_XIR_VALUE_OK);
    XrXirValuePlace place={(XrXirType)257,&array.payload};
    CHECK(xr_xir_array_push(&place,&separate,&admission)==XR_XIR_VALUE_OK && array.payload!=array_alias.payload);
    XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&array_alias,0,&admission,&element,&fault)==XR_XIR_VALUE_OK && element.payload==first.payload);
    CHECK(xr_xir_class_set(&element,0,&fields[0],&admission)==XR_XIR_VALUE_OK);class_value_count(&alias,40);
    xr_xir_value_drop(&element);xr_xir_value_drop(&array);xr_xir_value_drop(&array_alias);
    XirObject *new_string=object_pointer(&label);uint32_t refs=atomic_load(&new_string->references);
    atomic_store(&new_string->references,UINT32_MAX);
    CHECK(xr_xir_class_set(&first,2,&label,&admission)==XR_XIR_VALUE_REFCOUNT_LIMIT);
    atomic_store(&new_string->references,refs);
    XirObject *late_string=object_pointer(&text);refs=atomic_load(&late_string->references);
    size_t rollback_live=live;uint64_t rollback_bytes=xr_xir_domain_stats(domain).live_bytes;
    atomic_store(&late_string->references,UINT32_MAX);
    CHECK(xr_xir_class_new((XrXirType)256,fields,3,&admission,&invalid)==XR_XIR_VALUE_REFCOUNT_LIMIT);
    atomic_store(&late_string->references,refs);
    CHECK(!invalid.type && !invalid.payload && live==rollback_live && xr_xir_domain_stats(domain).live_bytes==rollback_bytes);
    XrXirValue observed={0};CHECK(xr_xir_class_get(&first,2,&observed)==XR_XIR_VALUE_OK);
    CHECK(observed.payload==text.payload);xr_xir_value_drop(&observed);
    size_t setter_calls=calls;
    CHECK(xr_xir_class_set(&first,2,&label,&admission)==XR_XIR_VALUE_OK && calls==setter_calls);
    CHECK(xr_xir_class_get(&first,2,&observed)==XR_XIR_VALUE_OK && observed.payload==label.payload);
    xr_xir_value_drop(&observed);
    xr_xir_value_drop(&label);xr_xir_value_drop(&text);xr_xir_value_drop(&alias);xr_xir_value_drop(&separate);
    xr_xir_type_arena_drop(arena);xr_xir_domain_drop(domain);xr_xir_domain_drop(foreign);
    CHECK(xr_xir_class_get(&first,1,&observed)==XR_XIR_VALUE_OK);
    size_t before=calls;xr_xir_value_drop(&first);CHECK(calls==before);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&observed,&bytes,&length));
    CHECK(length==8 && !memcmp(bytes,"retained",8));xr_xir_value_drop(&observed);CHECK(live==initial);
}
static void class_value_allocation_failures(void) {
    size_t initial=live;XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirTypeArena *arena=class_value_arena(domain);XrXirValue text={0};
    CHECK(xr_xir_string_new(domain,"retained",8,&text)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={{XR_XIR_I64,0,40},text,text};size_t baseline=live,sites=0;
    uint64_t bytes=xr_xir_domain_stats(domain).live_bytes;
    for(size_t pass=0;pass<=sites;++pass){
        calls=0;fail_at=pass?pass-1:SIZE_MAX;XrXirValue value={0};
        XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
        XrXirValueStatus status=xr_xir_class_new((XrXirType)256,fields,3,&admission,&value);
        if(!pass){CHECK(status==XR_XIR_VALUE_OK);sites=calls;CHECK(sites>0);}
        else CHECK(status==XR_XIR_VALUE_OOM && !value.type && !value.payload);
        size_t before=calls;xr_xir_value_drop(&value);CHECK(calls==before);
        CHECK(live==baseline && xr_xir_domain_stats(domain).live_bytes==bytes);
    }
    fail_at=SIZE_MAX;xr_xir_value_drop(&text);xr_xir_type_arena_drop(arena);xr_xir_domain_drop(domain);CHECK(live==initial);
}
static void class_array_allocation_failures(void) {
    size_t initial=live;XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirTypeArena *arena=class_value_arena(domain);XrXirValue text={0},object={0};
    CHECK(xr_xir_string_new(domain,"retained",8,&text)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={{XR_XIR_I64,0,40},text,text};
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    CHECK(xr_xir_class_new((XrXirType)256,fields,3,&admission,&object)==XR_XIR_VALUE_OK);
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass){
        XrXirValue array={0},alias={0};fail_at=SIZE_MAX;admission.work=1000000;
        CHECK(xr_xir_array_new((XrXirType)257,&object,1,&admission,&array)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_value_copy(&array,&alias)==XR_XIR_VALUE_OK);
        size_t baseline=live;uint64_t bytes=xr_xir_domain_stats(domain).live_bytes;
        calls=0;fail_at=pass?pass-1:SIZE_MAX;XrXirValuePlace place={(XrXirType)257,&array.payload};
        XrXirValueStatus status=xr_xir_array_push(&place,&object,&admission);
        if(!pass){CHECK(status==XR_XIR_VALUE_OK);sites=calls;CHECK(sites>0);}
        else {CHECK(status==XR_XIR_VALUE_OOM && array.payload==alias.payload);
            CHECK(live==baseline && xr_xir_domain_stats(domain).live_bytes==bytes);}
        fail_at=SIZE_MAX;class_value_count(&object,40);
        size_t before=calls;xr_xir_value_drop(&alias);xr_xir_value_drop(&array);CHECK(calls==before);
    }
    xr_xir_value_drop(&object);xr_xir_value_drop(&text);xr_xir_type_arena_drop(arena);xr_xir_domain_drop(domain);CHECK(live==initial);
}
#endif // XIR_CLASS_VALUE_CASES_H
