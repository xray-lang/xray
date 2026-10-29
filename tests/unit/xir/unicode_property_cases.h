/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * unicode_property_cases.h - Canonical Unicode callable admission and values
 *
 * KEY CONCEPT:
 *   Read-only native queries borrow input handles and return independent scalars.
 */
#ifndef UNICODE_PROPERTY_CASES_H
#define UNICODE_PROPERTY_CASES_H
#include "xir/xxir_native_callable.h"
#include "xir/xxir_unicode_internal.h"
static void unicode_property_identity(void) {
    const uint32_t ids[]={XR_NATIVE_CALLABLE_UNICODE_PROPERTY_ID,XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY};
    for(size_t i=0;i<sizeof(ids)/sizeof(*ids);++i) {
        XrXirType parameters[]={i?XR_XIR_I64:XR_XIR_STRING,XR_XIR_I64};
        XrXirFunction function={.parameters=parameters,.parameter_count=i?2:1,
            .result=i?XR_XIR_BOOL:XR_XIR_I64,.error_type=XR_XIR_NEVER};
        XrXirFunctionIdentity identity={.promises=XR_XIR_FUNCTION_NO_SUSPEND,.native_callable=ids[i]};
        XrXirDeclarations declarations={.functions=&identity,.entry_function=UINT32_MAX};
        XrXirModule module={.functions=&function,.function_count=1,.declarations=&declarations};
        CHECK(xir_native_callable_verify(&module,0)==XR_XIR_OK);
        identity.exported=1;CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_STRUCTURE);identity.exported=0;
        identity.promises|=XR_XIR_FUNCTION_GENERATOR;CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_STRUCTURE);
        identity.promises=XR_XIR_FUNCTION_NO_SUSPEND;identity.nominal_owner=1;
        CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_STRUCTURE);identity.nominal_owner=0;
        identity.test_role=XR_XIR_TEST_ROLE_TEST;CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_STRUCTURE);identity.test_role=0;
        function.error_type=XR_XIR_UNIT;CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_TYPE);
        function.error_type=XR_XIR_ERROR;CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_TYPE);function.error_type=XR_XIR_NEVER;
        parameters[0]=XR_XIR_BOOL;CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_TYPE);parameters[0]=i?XR_XIR_I64:XR_XIR_STRING;
        XrXirInstruction instruction={.op=XR_XIR_RETURN};function.instructions=&instruction;function.instruction_count=1;
        CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_STRUCTURE);function.instructions=NULL;function.instruction_count=0;
        declarations.entry_function=0;CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_STRUCTURE);declarations.entry_function=UINT32_MAX;
        XrXirSourceModule source_module={.initializer=0};declarations.modules=&source_module;declarations.module_count=1;
        CHECK(xir_native_callable_verify(&module,0)==XR_XIR_BAD_STRUCTURE);declarations.modules=NULL;declarations.module_count=0;
        CHECK(xir_native_callable_verify(&module,0)==XR_XIR_OK);
    }
}
static int unicode_property_core(void) {
    unicode_property_identity();
    XrXirType name[]={XR_XIR_STRING},numbers[]={XR_XIR_I64,XR_XIR_I64},wrong[]={XR_XIR_BOOL,XR_XIR_I64};
    CHECK(xir_native_callable_types(XR_NATIVE_CALLABLE_UNICODE_PROPERTY_ID,NULL,name,1,XR_XIR_I64,XR_XIR_NEVER));
    CHECK(xir_native_callable_types(XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY,NULL,numbers,2,XR_XIR_BOOL,XR_XIR_NEVER));
    CHECK(!xir_native_callable_types(XR_NATIVE_CALLABLE_UNICODE_PROPERTY_ID,NULL,numbers,1,XR_XIR_I64,XR_XIR_NEVER));
    CHECK(!xir_native_callable_types(XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY,NULL,numbers,1,XR_XIR_BOOL,XR_XIR_NEVER));
    CHECK(!xir_native_callable_types(XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY,NULL,wrong,2,XR_XIR_BOOL,XR_XIR_NEVER));
    CHECK(!xir_native_callable_types(XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY,NULL,numbers,2,XR_XIR_I64,XR_XIR_NEVER));
    CHECK(!xir_native_callable_types(XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY,NULL,numbers,2,XR_XIR_BOOL,XR_XIR_I64));
    CHECK(!xir_native_callable_types(UINT32_MAX,NULL,name,1,XR_XIR_I64,XR_XIR_NEVER));
    static const struct {const char *bytes;size_t length;int64_t expected;} names[]={
        {"Lu",2,XR_UP_Lu},{"Uppercase_Letter",16,XR_UP_Lu},{"uPPERcase_lETTER",16,XR_UP_Lu},
        {"han",3,XR_UP_Han},{"",0,-1},{"Lu\0",3,-1},{"\303\251",2,-1},{"unknown",7,-1}
    };
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    for(size_t i=0;i<sizeof(names)/sizeof(*names);++i) {
        XrXirValue text={0},result={0};
        CHECK(xr_xir_string_new(domain,names[i].bytes,names[i].length,&text)==XR_XIR_VALUE_OK);
        XrXirDomainStats before=xr_xir_domain_stats(domain);
        CHECK(xir_unicode_property_id(&text,&result)==XR_XIR_VALUE_OK&&result.type==XR_XIR_I64&&result.payload==names[i].expected);
        XrXirDomainStats after=xr_xir_domain_stats(domain);CHECK(after.allocations==before.allocations&&after.live_bytes==before.live_bytes);
        xr_xir_value_drop(&text);CHECK(result.payload==names[i].expected);xr_xir_value_drop(&result);
    }
    static const struct {int64_t cp,property;bool expected;} ranges[]={
        {'A',XR_UP_Lu,true},{'a',XR_UP_Lu,false},{0x4e2d,XR_UP_Han,true},{0,XR_UP_Any,true},
        {-1,XR_UP_Any,false},{INT64_MIN,XR_UP_Any,false},{INT64_MAX,XR_UP_Any,false},
        {INT64_C(0x100000041),XR_UP_Lu,false},{0x10ffff,XR_UP_Any,true},{0x110000,XR_UP_Any,false},
        {'A',XR_UP_INVALID,false},{'A',-1,false},{'A',XR_UP_COUNT,false},{'A',INT64_C(0x100000002),false}
    };
    for(size_t i=0;i<sizeof(ranges)/sizeof(*ranges);++i) {
        XrXirValue cp={XR_XIR_I64,0,ranges[i].cp},property={XR_XIR_I64,0,ranges[i].property},result={0};
        CHECK(xir_unicode_has_property(&cp,&property,&result)==XR_XIR_VALUE_OK&&result.type==XR_XIR_BOOL&&result.payload==ranges[i].expected);
    }
    XrXirValue bad={XR_XIR_I64,0,1},canary={XR_XIR_I64,0,73},saved=canary,empty={0};
    CHECK(xir_unicode_property_id(&bad,&canary)==XR_XIR_VALUE_BAD_ARGUMENT&&!memcmp(&canary,&saved,sizeof(saved)));
    CHECK(xir_unicode_property_id(&bad,&empty)==XR_XIR_VALUE_BAD_ARGUMENT&&!empty.type);
    XrXirValue boolean={XR_XIR_BOOL,0,1};
    CHECK(xir_unicode_has_property(&boolean,&bad,&empty)==XR_XIR_VALUE_BAD_ARGUMENT&&!empty.type);
    CHECK(xir_unicode_has_property(&bad,&bad,&canary)==XR_XIR_VALUE_BAD_ARGUMENT&&!memcmp(&canary,&saved,sizeof(saved)));
    xr_xir_domain_drop(domain);
    puts("Typed Unicode canonical aliases/NUL, exact kinds/signatures, signed narrowing and borrowed String lifetime PASS");return 0;
}
#endif // UNICODE_PROPERTY_CASES_H
