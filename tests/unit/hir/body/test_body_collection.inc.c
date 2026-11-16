/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Exact type failures and constant keys retain their original item ordinals. */
#include "hir/xhir_collection.h"
static void collection_receivers(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    XrXirNominalVariant variants[]={{{"A",1},0,0},{{"B",1},0,0}};
    XrXirNominalIdentity identity={.module={"m",1},.name={"Choice",6},.kind=XR_XIR_NOMINAL_ENUM,.variants=variants,.variant_count=2};
    XrXirNominalTable table={.identities=&identity,.count=1};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_NOMINAL}};
    XrXirTypes types={.nodes=nodes,.count=3,.nominals=&table};
    XrHirCollectionEntry entries[]={
        {.key=XR_XIR_I64,.value=XR_XIR_STRING,.constant={.kind=XR_XIR_KEY_SCALAR,.bits=1}},
        {.key=XR_XIR_I64,.value=XR_XIR_STRING,.constant={.kind=XR_XIR_KEY_SCALAR,.bits=2}},
        {.key=XR_XIR_I64,.value=XR_XIR_STRING,.constant={.kind=XR_XIR_KEY_SCALAR,.bits=1}}};
    XrHirCollectionInput input={.kind=XR_HIR_COLLECTION_MAP,.entries=entries,.count=3};
    XrHirCollection out={0};XrHirCollectionError error={0};
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE &&
        error.kind==XR_HIR_COLLECTION_DUPLICATE && error.first==0 && error.second==2);
    entries[2].constant.bits=3;
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_OK &&
        out.kind==XR_HIR_COLLECTION_MAP && out.key==XR_XIR_I64 && out.value==XR_XIR_STRING && out.operands==6);
    entries[1].value=XR_XIR_I64;
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE &&
        error.kind==XR_HIR_COLLECTION_VALUE_TYPE && error.expected==XR_XIR_STRING && error.actual==XR_XIR_I64 && error.first==0 && error.second==1);
    entries[1].value=XR_XIR_STRING;entries[1].key=XR_XIR_STRING;entries[1].constant=(XrXirConstantKey){0};
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE && error.kind==XR_HIR_COLLECTION_KEY_TYPE);
    entries[1].key=XR_XIR_I64;
    entries[0]=(XrHirCollectionEntry){.value=(XrXirType)256,.spread=true};
    entries[2].constant.bits=4;
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_OK && out.operands==5);
    entries[1].constant=(XrXirConstantKey){.kind=XR_XIR_KEY_SCALAR,.bits=4};
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE &&
        error.kind==XR_HIR_COLLECTION_DUPLICATE && error.first==1 && error.second==2);
    input.count=0;
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE && error.kind==XR_HIR_COLLECTION_EMPTY);
    input.has_expected=true;input.expected=(XrXirType)256;
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_OK && out.key==XR_XIR_I64 && out.value==XR_XIR_STRING);
    input.kind=XR_HIR_COLLECTION_ARRAY;
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE && error.kind==XR_HIR_COLLECTION_CONTEXT);
    input.has_expected=false;input.count=2;
    entries[0]=(XrHirCollectionEntry){.value=XR_XIR_I64};entries[1]=(XrHirCollectionEntry){.value=XR_XIR_STRING};
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE &&
        error.kind==XR_HIR_COLLECTION_VALUE_TYPE && error.expected==XR_XIR_I64 && error.actual==XR_XIR_STRING && error.second==1);
    entries[0]=(XrHirCollectionEntry){.value=(XrXirType)257,.spread=true};entries[1]=(XrHirCollectionEntry){.value=XR_XIR_I64};
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_OK && out.value==XR_XIR_I64 && out.operands==2);
    entries[1].value=XR_XIR_STRING;
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE &&
        error.kind==XR_HIR_COLLECTION_VALUE_TYPE && error.first==0 && error.second==1 && error.expected==XR_XIR_I64 && error.actual==XR_XIR_STRING);
    entries[0].value=XR_XIR_I64;
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE && error.kind==XR_HIR_COLLECTION_SPREAD_SOURCE);
    input.kind=XR_HIR_COLLECTION_MAP;
    for(unsigned family=0;family<5;++family) {
        XrXirType key=family==0?XR_XIR_STRING:family==1?XR_XIR_F64:family==2?XR_XIR_F32:family==3?XR_XIR_RUNE:(XrXirType)258;
        entries[0]=(XrHirCollectionEntry){.key=key,.value=XR_XIR_I64};entries[1]=entries[0];
        if(family==0)entries[0].constant=(XrXirConstantKey){.kind=XR_XIR_KEY_STRING,.string={"a\0z",3}};
        else entries[0].constant=(XrXirConstantKey){.kind=family==4?XR_XIR_KEY_ENUM:XR_XIR_KEY_SCALAR,.bits=family==3?0x1f600:0};
        entries[1].constant=entries[0].constant;
        if(family==1)entries[1].constant.bits=UINT64_C(0x8000000000000000);
        if(family==2)entries[1].constant.bits=UINT32_C(0x80000000);
        CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE && error.kind==XR_HIR_COLLECTION_DUPLICATE);
    }
    entries[0]=(XrHirCollectionEntry){.key=XR_XIR_F64,.value=XR_XIR_I64,.constant={.kind=XR_XIR_KEY_SCALAR,.bits=UINT64_C(0x7ff0000000000001)}};
    entries[1]=entries[0];entries[1].constant.bits=UINT64_C(0xfff123456789abcd);
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_BAD_TYPE && error.kind==XR_HIR_COLLECTION_DUPLICATE);
    entries[0]=(XrHirCollectionEntry){.key=XR_XIR_STRING,.value=XR_XIR_I64,.constant={.kind=XR_XIR_KEY_STRING,.string={"a\0x",3}}};
    entries[1]=entries[0];entries[1].constant.string=(XrXirLiteral){"a\0y",3};
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_OK);
    XrCompileResourceStats baseline={0},after={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    size_t live=physical_live,bytes=physical_bytes,first=physical_attempts;
    CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_OK);size_t sites=physical_attempts-first;
    CHECK(sites>0);
    for(size_t point=0;point<sites;++point) {
        out=(XrHirCollection){.key=XR_XIR_RUNE,.value=XR_XIR_RUNE,.count=77,.operands=88};
        error=(XrHirCollectionError){.kind=XR_HIR_COLLECTION_DUPLICATE,.first=77,.second=88};
        physical_fail=physical_attempts+point;
        CHECK(xr_hir_collection_check(&compile,&types,&input,NULL,&out,&error)==XR_XIR_OUT_OF_MEMORY);
        physical_fail=SIZE_MAX;
        CHECK(out.key==XR_XIR_RUNE && out.value==XR_XIR_RUNE && out.count==77 && out.operands==88 &&
            error.kind==XR_HIR_COLLECTION_DUPLICATE && error.first==77 && error.second==88 &&
            physical_live==live && physical_bytes==bytes && xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==baseline.held_bytes);
    }
    Cancel cancellation={0,SIZE_MAX};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_collection_check(&compile,&types,&input,&control,&out,&error)==XR_XIR_OK);size_t polls=cancellation.polls;
    for(size_t point=1;point<=polls;++point) {
        cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_hir_collection_check(&compile,&types,&input,&control,&out,&error)==XR_XIR_CANCELLED && physical_live==live && physical_bytes==bytes);
    }
    char first_key[12289],second_key[12289];memset(first_key,'a',sizeof(first_key));first_key[4095]=0;
    input=(XrHirCollectionInput){.kind=XR_HIR_COLLECTION_MAP,.entries=entries,.count=2};
    for(unsigned duplicate=0;duplicate<2;++duplicate) {
        memcpy(second_key,first_key,sizeof(first_key));if(!duplicate)second_key[sizeof(second_key)-1]='b';
        entries[0]=(XrHirCollectionEntry){.key=XR_XIR_STRING,.value=XR_XIR_I64,
            .constant={.kind=XR_XIR_KEY_STRING,.string={first_key,sizeof(first_key)}}};
        entries[1]=entries[0];entries[1].constant.string.bytes=second_key;
        cancellation=(Cancel){0,SIZE_MAX};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_hir_collection_check(&compile,&types,&input,&control,&out,&error)==(duplicate?XR_XIR_BAD_TYPE:XR_XIR_OK));
        polls=cancellation.polls;CHECK(polls>16);
        for(size_t point=1;point<=polls;++point) {
            cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};
            CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
            out=(XrHirCollection){.count=77,.operands=88};error=(XrHirCollectionError){.first=77,.second=88};
            CHECK(xr_hir_collection_check(&compile,&types,&input,&control,&out,&error)==XR_XIR_CANCELLED &&
                out.count==77 && out.operands==88 && error.first==77 && error.second==88 && physical_live==live && physical_bytes==bytes);
        }
    }
    xr_compile_resources_release(resources);CHECK(!physical_live && !physical_bytes);
}
