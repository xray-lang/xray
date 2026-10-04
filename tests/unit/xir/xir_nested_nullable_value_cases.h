/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nested_nullable_value_cases.h - Finite boxed and inline optional traversal
 *
 * KEY CONCEPT:
 *   Shallow headers do not replace finite iterative authority validation.
 */
#ifndef XIR_NESTED_NULLABLE_VALUE_CASES_H
#define XIR_NESTED_NULLABLE_VALUE_CASES_H
static XrXirValueAdmission nested_admission(XrXirDomain *domain,XrXirTypeArena *arena) {
    return (XrXirValueAdmission){arena,domain,NULL,NULL,1000000,1048576};
}
static void nested_storage_vectors(const XrXirCompileContext *context) {
    const size_t live=runtime_live,bytes=runtime_bytes;
    XrXirDomain *domain=NULL;XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(context,&nested_types,&arena)==XR_XIR_VALUE_OK);
    const XrXirStorageLayout *layout=xr_xir_compile_type_arena_storage(arena,NESTED_OUTER);
    CHECK(layout && layout->value.size==24 && layout->value.alignment==8);
    layout=xr_xir_compile_type_arena_storage(arena,NESTED_INNER);
    CHECK(layout && layout->value.size==16 && layout->value.alignment==8);
    XrXirValueAdmission admission=nested_admission(domain,arena);
    XrXirValue scalar={XR_XIR_I64,0,7},inner[2]={{0}},outer[3]={{0}};
    CHECK(xr_xir_nullable_new(NESTED_INNER,NULL,&admission,&inner[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(NESTED_INNER,&scalar,&admission,&inner[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(NESTED_OUTER,NULL,&admission,&outer[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(NESTED_OUTER,&inner[0],&admission,&outer[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(NESTED_OUTER,&inner[1],&admission,&outer[2])==XR_XIR_VALUE_OK);
    for(uint32_t state=0;state<3;++state) {
        unsigned char golden[24]={0},packed[24]={0};StoragePack pack={0};XrXirValue roundtrip={0};
        if(state)golden[0]=1;
        if(state==2){golden[8]=1;golden[16]=7;}
        CHECK(storage_pack_begin(&admission,NESTED_OUTER,&pack)==XR_XIR_VALUE_OK);
        CHECK(storage_pack_value(&pack,&outer[state],packed)==XR_XIR_VALUE_OK && !memcmp(packed,golden,24));
        CHECK(storage_unpack((StorageSpan){NESTED_OUTER,golden},&admission,&roundtrip)==XR_XIR_VALUE_OK);
        nested_shape(&roundtrip,state);xr_xir_value_drop(&roundtrip);storage_pack_end(&pack);
    }
    unsigned char malformed[24]={0};XrXirValue result={0};malformed[0]=2;
    CHECK(storage_unpack((StorageSpan){NESTED_OUTER,malformed},&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    malformed[0]=1;malformed[8]=2;
    CHECK(storage_unpack((StorageSpan){NESTED_OUTER,malformed},&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    malformed[0]=0;memset(malformed+8,0xff,16);
    CHECK(storage_unpack((StorageSpan){NESTED_OUTER,malformed},&admission,&result)==XR_XIR_VALUE_OK);
    nested_shape(&result,0);xr_xir_value_drop(&result);
    XirNominalValue *bad=(XirNominalValue*)object_pointer(&inner[0]);bad->variant=2;
    /* The root header is deliberately shallow. Authority must reach the child. */
    CHECK(xr_xir_value_valid(&outer[1]));
    CHECK(xr_xir_value_admit(&outer[1],NESTED_OUTER,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    bad->variant=0;
    const uint64_t limit=domain->limit,baseline_bytes=domain->stats.live_bytes;
    const size_t record_bytes=sizeof(XirNominalValue)+sizeof(XrXirValue);
    domain->limit=baseline_bytes+record_bytes-1;
    CHECK(xr_xir_nullable_new(NESTED_OUTER,&inner[0],&admission,&result)==XR_XIR_VALUE_LIMIT && !result.type);
    CHECK(domain->stats.live_bytes==baseline_bytes);
    domain->limit=baseline_bytes+record_bytes;
    CHECK(xr_xir_nullable_new(NESTED_OUTER,&inner[0],&admission,&result)==XR_XIR_VALUE_OK);
    nested_shape(&result,1);xr_xir_value_drop(&result);domain->limit=limit;
    uint32_t references=atomic_load(&arena->references);atomic_store(&arena->references,UINT32_MAX);
    CHECK(xr_xir_nullable_new(NESTED_OUTER,&inner[0],&admission,&result)==XR_XIR_VALUE_REFCOUNT_LIMIT && !result.type);
    atomic_store(&arena->references,references);
    XirObject *payload_object=object_pointer(&inner[0]);references=atomic_load(&payload_object->references);
    atomic_store(&payload_object->references,UINT32_MAX);
    CHECK(xr_xir_nullable_new(NESTED_OUTER,&inner[0],&admission,&result)==XR_XIR_VALUE_REFCOUNT_LIMIT && !result.type);
    atomic_store(&payload_object->references,references);
    CHECK(domain->stats.live_bytes==baseline_bytes);
    for(uint32_t i=0;i<3;++i)xr_xir_value_drop(&outer[i]);
    for(uint32_t i=0;i<2;++i)xr_xir_value_drop(&inner[i]);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    CHECK(runtime_live==live && runtime_bytes==bytes);
}
static void nested_deep_admission(const XrXirCompileContext *context,uint32_t count) {
    CHECK(count==32 || count==256);
    const size_t live=runtime_live,bytes=runtime_bytes;
    XrXirTypeNode nodes[256]={{0}};
    for(uint32_t i=0;i<count;++i) {
        nodes[i].kind=XR_XIR_TYPE_NULLABLE;
        nodes[i].element=i?(XrXirType)(256+i-1):XR_XIR_I64;
    }
    XrXirTypes types={nodes,count,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValue value={XR_XIR_I64,0,7};XrXirValueAdmission admission=nested_admission(domain,arena);
    for(uint32_t i=0;i<count;++i){XrXirValue outer={0};
        CHECK(xr_xir_nullable_new((XrXirType)(256+i),&value,&admission,&outer)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&value);value=outer;
    }
    CHECK(xr_xir_value_valid(&value));
    const XrXirType type=(XrXirType)(256+count-1);
    admission=nested_admission(domain,arena);admission.work=count+1;
    CHECK(xr_xir_value_admit(&value,type,&admission)==XR_XIR_VALUE_OK && !admission.work && admission.scratch_bytes==1048576);
    admission=nested_admission(domain,arena);admission.work=count;
    CHECK(xr_xir_value_admit(&value,type,&admission)==XR_XIR_VALUE_LIMIT && !admission.work && admission.scratch_bytes==1048576);
    admission=nested_admission(domain,arena);admission.scratch_bytes=sizeof(ValueAdmissionFrame)-1;
    CHECK(xr_xir_value_admit(&value,type,&admission)==XR_XIR_VALUE_LIMIT &&
        admission.scratch_bytes==sizeof(ValueAdmissionFrame)-1);
    size_t physical=runtime_live,physical_bytes=runtime_bytes;size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass){
        admission=nested_admission(domain,arena);runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
        XrXirValueStatus status=xr_xir_value_admit(&value,type,&admission);
        size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_VALUE_OK);sites=attempts;CHECK(sites && sites<=16);}
        else CHECK(status==XR_XIR_VALUE_OOM && attempts>pass-1);
        CHECK(admission.scratch_bytes==1048576 && runtime_live==physical && runtime_bytes==physical_bytes);
    }
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    CHECK(xr_xir_value_valid(&value));xr_xir_value_drop(&value);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    printf("nested depth=%u admission work exact/minus1, scratch and %zu actual OOM points PASS\n",count,sites);
}
#endif // XIR_NESTED_NULLABLE_VALUE_CASES_H
