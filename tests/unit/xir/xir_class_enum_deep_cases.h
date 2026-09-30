/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_enum_deep_cases.h - Inline variants retain only their active payload
 *
 * KEY CONCEPT:
 *   Enum cursor frames remain necessary even when one payload is a class leaf.
 */
#ifndef XIR_CLASS_ENUM_DEEP_CASES_H
#define XIR_CLASS_ENUM_DEEP_CASES_H
static XrXirTypeArena *class_enum_arena(XrXirDomain *domain) {
 XrXirNominalFieldIdentity cf[]={{{"count",5},XR_XIR_FIELD_MUTABLE},{{"text",4},0}};
 XrXirNominalFieldIdentity sf[]={{{"object",6},0},{{"label",5},0}};
 XrXirNominalFieldIdentity ef[]={{{"object",6},0},{{"text",4},0},{{"holder",6},0}};
 XrXirNominalVariant variants[]={{{"Empty",5},0,0},{{"Object",6},0,1},{{"Text",4},1,1},{{"Aggregate",9},2,1}};
 XrXirNominalIdentity ids[]={
 {{"deep",4},{"Counter",7},1,0,cf,2,XR_XIR_NOMINAL_CLASS,NULL,0,XR_XIR_NOMINAL_FINAL},
 {{"deep",4},{"Holder",6},1,0,sf,2,XR_XIR_NOMINAL_STRUCT,NULL,0,0},
 {{"deep",4},{"Choice",6},1,0,ef,3,XR_XIR_NOMINAL_ENUM,variants,4,0}};
 XrXirNominalTable table={NULL,3,ids};
 XrXirType cf_types[]={XR_XIR_I64,XR_XIR_STRING},sf_types[]={(XrXirType)256,XR_XIR_STRING};
 XrXirType ef_types[]={(XrXirType)256,XR_XIR_STRING,(XrXirType)257};
 XrXirTypeNode nodes[]={
 {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,NULL,0,cf_types,2}},
 {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{1,NULL,0,sf_types,2}},
 {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{2,NULL,0,ef_types,3}},
 {XR_XIR_TYPE_ARRAY,(XrXirType)258,NULL,0,XR_XIR_UNIT,0,0,{0}}};
 XrXirTypes types={nodes,4,&table,NULL};XrXirBudget budget=xr_xir_default_budget();XrXirTypeArena *arena=NULL;
 CHECK(xr_xir_type_arena_new(domain,&types,&budget,&arena)==XR_XIR_VALUE_OK);
 memset(ids,0xcc,sizeof(ids));memset(variants,0xcc,sizeof(variants));return arena;
}
static XrXirValueStatus class_enum_sequence(XrXirTypeArena *arena,XrXirDomain *domain,XrXirValue text) {
 XrXirValue v[10]={{0}},fields[]={{XR_XIR_I64,0,40},text};XrXirValueStatus status=XR_XIR_VALUE_OK;
 XrXirValueAdmission a={arena,domain,NULL,NULL,1000000,65536};XrXirFaultDetail fault={0};
#define ESTEP(expr) do{status=(expr);if(status!=XR_XIR_VALUE_OK)goto done;}while(0)
 ESTEP(xr_xir_class_new((XrXirType)256,fields,2,&a,&v[0]));
 XrXirValue holder[]={v[0],text};ESTEP(xr_xir_struct_new((XrXirType)257,holder,2,&a,&v[1]));
 ESTEP(xr_xir_enum_new((XrXirType)258,0,NULL,0,&a,&v[2]));
 ESTEP(xr_xir_enum_new((XrXirType)258,1,&v[0],1,&a,&v[3]));
 ESTEP(xr_xir_enum_new((XrXirType)258,2,&text,1,&a,&v[4]));
 ESTEP(xr_xir_enum_new((XrXirType)258,3,&v[1],1,&a,&v[5]));
 ESTEP(xr_xir_array_new((XrXirType)259,&v[2],4,&a,&v[6]));ESTEP(xr_xir_value_copy(&v[6],&v[7]));
 XrXirValuePlace place={(XrXirType)259,&v[6].payload};ESTEP(xr_xir_array_push(&place,&v[2],&a));CHECK(v[6].payload!=v[7].payload);
 int64_t count=0;ESTEP(xr_xir_array_len(&v[7],&a,&count));CHECK(count==4);
 for(uint32_t variant=0;variant<4;++variant){
 ESTEP(xr_xir_array_get(&v[7],variant,&a,&v[8],&fault));uint32_t tag=99;ESTEP(xr_xir_enum_variant(&v[8],&tag));CHECK(tag==variant);
 if(variant){ESTEP(xr_xir_enum_get(&v[8],variant,0,&a,&v[9]));
 if(variant==1){CHECK(v[9].payload==v[0].payload);XrXirValue n={XR_XIR_I64,0,41};ESTEP(xr_xir_class_set(&v[9],0,&n,&a));}
 if(variant==2){const char *bytes=NULL;size_t size=0;CHECK(xr_xir_string_view(&v[9],&bytes,&size)&&size==4&&!memcmp(bytes,"enum",4));}
 if(variant==3){XrXirValue object={0};ESTEP(xr_xir_struct_get(&v[9],0,&a,&object));CHECK(object.payload==v[0].payload);xr_xir_value_drop(&object);}
 }xr_xir_value_drop(&v[9]);xr_xir_value_drop(&v[8]);}
 XrXirValue n={0};ESTEP(xr_xir_class_get(&v[0],0,&n));CHECK(n.payload==41);xr_xir_value_drop(&n);
 done:;
 size_t calls=runtime_attempts;for(uint32_t i=10;i;--i)xr_xir_value_drop(&v[i-1]);CHECK(runtime_attempts==calls);
#undef ESTEP
 return status;
}
static void class_enum_deep_cases(void) {
 CHECK(!runtime_live&&!runtime_bytes);XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
 XrXirTypeArena *arena=class_enum_arena(domain);XrXirValue text={0};CHECK(xr_xir_string_new(domain,"enum",4,&text)==XR_XIR_VALUE_OK);
 const XrXirStorageLayout *layout=xr_xir_type_arena_storage(arena,(XrXirType)258);
 CHECK(layout->value.size==24&&layout->value.alignment==8&&layout->depth==2&&layout->owned_depth==2);
 CHECK(layout->field_offsets[0]==8&&layout->field_offsets[1]==8&&layout->field_offsets[2]==8);
 CHECK(array_release_depth(arena,(XrXirType)258)==2);
 printf("enum header=%llu depth=%u owned=%u\n",(unsigned long long)array_header_bytes(arena,(XrXirType)258),layout->depth,layout->owned_depth);
 size_t baseline=runtime_live,bytes=runtime_bytes,sites=0;
 for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
 XrXirValueStatus status=class_enum_sequence(arena,domain,text);
 if(!pass){CHECK(status==XR_XIR_VALUE_OK);sites=runtime_attempts;CHECK(sites>0);}else CHECK(status==XR_XIR_VALUE_OOM);
 CHECK(runtime_live==baseline&&runtime_bytes==bytes);}
 runtime_fail_at=SIZE_MAX;printf("inline enum class/string/aggregate OOM %zu sites\n",sites);
 xr_xir_value_drop(&text);xr_xir_type_arena_drop(arena);xr_xir_domain_drop(domain);CHECK(!runtime_live&&!runtime_bytes);
}
#endif
