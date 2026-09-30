/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_array_domain_cases.h - Independent domain refunds and COW boundaries
 *
 * KEY CONCEPT:
 *   Field backing retains its real owner while class execution stays in its domain.
 */
#ifndef XIR_CLASS_ARRAY_DOMAIN_CASES_H
#define XIR_CLASS_ARRAY_DOMAIN_CASES_H
static XrXirTypeArena *class_array_independent_arena(XrXirDomain *domain) {
    XrXirNominalFieldIdentity fields[]={{{"flag",4},0},{{"items",5},XR_XIR_FIELD_MUTABLE},{{"text",4},0}};
    XrXirNominalIdentity identity={{"module",6},{"Bag",3},1,0,fields,3,XR_XIR_NOMINAL_CLASS,NULL,0,XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table={NULL,1,&identity};XrXirType field_types[]={XR_XIR_BOOL,(XrXirType)256,(XrXirType)257};
    XrXirTypeNode nodes[]={
        {XR_XIR_TYPE_ARRAY,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0,{0}},
        {XR_XIR_TYPE_ARRAY,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0,{0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,NULL,0,field_types,3}}};
    XrXirTypes types={nodes,3,&table,NULL};XrXirBudget budget=xr_xir_default_budget();
    XrXirTypeArena *arena=NULL;CHECK(xr_xir_type_arena_new(domain,&types,&budget,&arena)==XR_XIR_VALUE_OK);
    return arena;
}

static XrXirValueStatus class_array_stress(XrXirTypeArena *arena,XrXirDomain *owner,XrXirDomain *backing){
 XrXirValue v[9]={{0}};XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirFaultDetail fault={0};
 XrXirValueAdmission local={arena,owner,NULL,NULL,1000000,65536},remote={arena,backing,NULL,NULL,1000000,65536};
 XrXirValue forty={XR_XIR_I64,0,40},forty_one={XR_XIR_I64,0,41};
#define STEP(c) do{status=(c);if(status!=XR_XIR_VALUE_OK)goto done;}while(0)
 STEP(xr_xir_array_new((XrXirType)256,&forty,1,&remote,&v[0]));
 STEP(xr_xir_string_new(backing,"mapped",6,&v[1]));STEP(xr_xir_array_new((XrXirType)257,&v[1],1,&remote,&v[2]));
 XrXirValue fields[]={{XR_XIR_BOOL,0,1},v[0],v[2]};STEP(xr_xir_class_new((XrXirType)258,fields,3,&local,&v[3]));
 STEP(xr_xir_value_copy(&v[3],&v[4]));CHECK(v[3].payload==v[4].payload);
 STEP(xr_xir_class_get(&v[3],1,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&v[3]),.work=10000},&v[5]));STEP(xr_xir_value_copy(&v[5],&v[6]));
 XrXirValuePlace place={(XrXirType)256,&v[5].payload};STEP(xr_xir_array_push(&place,&forty_one,&local));
 CHECK(v[5].payload!=v[6].payload);int64_t count=0;STEP(xr_xir_array_len(&v[0],&local,&count));CHECK(count==1);
 STEP(xr_xir_class_get(&v[3],1,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&v[3]),.work=10000},&v[7]));CHECK(v[7].payload==v[0].payload);xr_xir_value_drop(&v[7]);
 STEP(xr_xir_class_set(&v[4],1,&v[5],&local));STEP(xr_xir_class_get(&v[3],1,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&v[3]),.work=10000},&v[7]));CHECK(v[7].payload==v[5].payload);
 size_t before=runtime_attempts;STEP(xr_xir_class_set(&v[3],1,&v[7],&local));CHECK(runtime_attempts==before);
 CHECK(xr_xir_class_set(&v[3],2,&v[2],&local)==XR_XIR_VALUE_BAD_ARGUMENT);
 CHECK(xr_xir_class_set(&v[3],1,&v[0],&remote)==XR_XIR_VALUE_BAD_ARGUMENT);
 XrXirValueAdmission small=local;small.work=0;CHECK(xr_xir_class_set(&v[3],1,&v[0],&small)==XR_XIR_VALUE_LIMIT);
 STEP(xr_xir_array_get(&v[7],1,&local,&v[8],&fault));CHECK(v[8].payload==41);xr_xir_value_drop(&v[8]);
 STEP(xr_xir_array_len(&v[6],&local,&count));CHECK(count==1);
 done:;
 size_t attempts_before_drop=runtime_attempts;for(uint32_t i=9;i;--i)xr_xir_value_drop(&v[i-1]);CHECK(runtime_attempts==attempts_before_drop);
#undef STEP
 return status;
}
static void class_array_independent_cases(void){
 XrXirDomain *a=NULL,*b=NULL;CHECK(xr_xir_domain_new(1048576,&a)==XR_XIR_VALUE_OK);CHECK(xr_xir_domain_new(1048576,&b)==XR_XIR_VALUE_OK);
 XrXirTypeArena *arena=class_array_independent_arena(a);size_t base=runtime_live,bytes=runtime_bytes,sites=0;
 uint64_t owner_bytes=xr_xir_domain_stats(a).live_bytes,back_bytes=xr_xir_domain_stats(b).live_bytes;
 for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
 XrXirValueStatus status=class_array_stress(arena,a,b);if(!pass){CHECK(status==XR_XIR_VALUE_OK);sites=runtime_attempts;CHECK(sites>0);}else CHECK(status==XR_XIR_VALUE_OOM);
 CHECK(runtime_live==base&&runtime_bytes==bytes);CHECK(xr_xir_domain_stats(a).live_bytes==owner_bytes&&xr_xir_domain_stats(b).live_bytes==back_bytes);}
 runtime_fail_at=SIZE_MAX;printf("class Array cross-backing/COW/alias/const/foreign-domain: %zu OOM sites, both domain refunds PASS\n",sites);
 xr_xir_type_arena_drop(arena);xr_xir_domain_drop(a);xr_xir_domain_drop(b);CHECK(!runtime_live&&!runtime_bytes);
}

static void class_array_retained_independent(void){
 XrXirDomain *a=NULL,*b=NULL;CHECK(xr_xir_domain_new(1048576,&a)==XR_XIR_VALUE_OK);CHECK(xr_xir_domain_new(1048576,&b)==XR_XIR_VALUE_OK);
 XrXirTypeArena *arena=class_array_independent_arena(a);XrXirValueAdmission local={arena,a,NULL,NULL,100000,65536},remote={arena,b,NULL,NULL,100000,65536};
 XrXirValue number={XR_XIR_I64,0,40},numbers={0},text={0},texts={0},object={0},saved={0};
 CHECK(xr_xir_array_new((XrXirType)256,&number,1,&remote,&numbers)==XR_XIR_VALUE_OK);
 CHECK(xr_xir_string_new(b,"retained",8,&text)==XR_XIR_VALUE_OK);CHECK(xr_xir_array_new((XrXirType)257,&text,1,&remote,&texts)==XR_XIR_VALUE_OK);
 XrXirValue fields[]={{XR_XIR_BOOL,0,1},numbers,texts};CHECK(xr_xir_class_new((XrXirType)258,fields,3,&local,&object)==XR_XIR_VALUE_OK);
 xr_xir_value_drop(&numbers);xr_xir_value_drop(&texts);xr_xir_value_drop(&text);xr_xir_type_arena_drop(arena);xr_xir_domain_drop(a);xr_xir_domain_drop(b);
 size_t attempts_before=runtime_attempts;runtime_fail_at=runtime_attempts;
 CHECK(xr_xir_class_get(&object,2,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&object),.work=10000},&saved)==XR_XIR_VALUE_OK);xr_xir_value_drop(&object);
 XrXirValueAdmission retained={xr_xir_value_arena(&saved),NULL,NULL,NULL,64,0};XrXirFaultDetail fault={0};
 CHECK(xr_xir_array_get(&saved,0,&retained,&text,&fault)==XR_XIR_VALUE_OK);xr_xir_value_drop(&saved);
 const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&text,&bytes,&length)&&length==8&&!memcmp(bytes,"retained",8));xr_xir_value_drop(&text);
 CHECK(runtime_attempts==attempts_before&&!runtime_live&&!runtime_bytes);runtime_fail_at=SIZE_MAX;puts("retained class/Array/string GET+drop while allocator fails PASS");
}

#endif
