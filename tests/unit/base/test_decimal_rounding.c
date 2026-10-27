/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_decimal_rounding.c - Independent exact decimal boundary expectations
 */
#include "shared/xr_string_parse_core.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
typedef struct DecimalVector {const char *text;uint64_t expected;} DecimalVector;
#define DECIMAL_CASE(text,bits) {text,bits},
static const DecimalVector vectors[]={
#include "decimal_rounding_vectors.def"
};
#undef DECIMAL_CASE
typedef struct DecimalCounter {uint64_t limit,spent;} DecimalCounter;
static bool decimal_charge(void *pointer,uint64_t units) {
    DecimalCounter *counter=pointer;
    if(units>counter->limit-counter->spent)return false;
    counter->spent+=units;return true;
}
static void decimal_work_prefixes(void) {
    const char text[]=" \t-0.5\r";uint64_t bits=0;
    DecimalCounter complete={UINT64_MAX,0};XrDecimalWork first={&complete,decimal_charge,false};
    CHECK(xr_decimal_float_text_work(&first,text,sizeof(text)-1,64,&bits)==XR_DECIMAL_OK &&
        bits==UINT64_C(0xbfe0000000000000) && complete.spent>1);
    for(uint64_t prefix=0;prefix<complete.spent;++prefix) {
        DecimalCounter counter={prefix,0};XrDecimalWork work={&counter,decimal_charge,false};
        bits=UINT64_C(0xa51234fe98ab67cd);
        CHECK(xr_decimal_float_text_work(&work,text,sizeof(text)-1,64,&bits)==XR_DECIMAL_WORK_LIMIT &&
            work.failed && counter.spent<=prefix && bits==UINT64_C(0xa51234fe98ab67cd));
        uint64_t held=counter.spent;
        CHECK(xr_decimal_float_text_work(&work,text,sizeof(text)-1,64,&bits)==XR_DECIMAL_WORK_LIMIT &&
            counter.spent==held && bits==UINT64_C(0xa51234fe98ab67cd));
    }
    DecimalCounter exact={complete.spent,0};XrDecimalWork work={&exact,decimal_charge,false};
    CHECK(xr_decimal_float_text_work(&work,text,sizeof(text)-1,64,&bits)==XR_DECIMAL_OK && exact.spent==complete.spent);
    bits=42;
    CHECK(xr_decimal_float_text_work(NULL,"1_0",3,64,&bits)==XR_DECIMAL_INVALID && bits==42);
    CHECK(xr_decimal_float_parse_work(NULL,"1_0",3,64,&bits)==XR_DECIMAL_OK && bits==UINT64_C(0x4024000000000000));
    DecimalCounter empty={0,0};XrDecimalWork refused={&empty,decimal_charge,false};bits=42;
    CHECK(xr_decimal_float_text_work(&refused,"0",(size_t)INT32_MAX+1u,64,&bits)==XR_DECIMAL_WORK_LIMIT &&
        refused.failed && !empty.spent && bits==42);
    XrDecimalWork literal={&empty,decimal_charge,false};
    CHECK(xr_decimal_float_parse_work(&literal,"0",(size_t)INT32_MAX+1u,64,&bits)==XR_DECIMAL_INVALID &&
        !literal.failed && !empty.spent && bits==42);
    printf("Runtime text exact/minus-one and all %" PRIu64 " borrowed work prefixes preserve atomic output and sticky failure\n",complete.spent);
}
static void decimal_syntax(void) {
    static const char *const invalid[]={""," ","+","-",".","1e","1e+","1e-","1e999999999999999999999x",
        "0e-999999999999999999999x","nan","inf","Infinity","0x1p0","1.0.0","1 2","1e 2","1\v","1\f","1_0","1e1_0"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
        CHECK(xr_string_parse_float64(invalid[i],strlen(invalid[i])).failure==XR_NUMBER_PARSE_INVALID_SYNTAX);
    CHECK(xr_string_parse_float64(NULL,0).failure==XR_NUMBER_PARSE_INVALID_SYNTAX);
    const char bounded[]={'1','.','2','5','x'};
    XrStringParseFloatResult parsed=xr_string_parse_float64(bounded,4);uint64_t bits=0;
    memcpy(&bits,&parsed.value,sizeof(bits));CHECK(parsed.failure==XR_NUMBER_PARSE_OK && bits==UINT64_C(0x3ff4000000000000));
    const char zero[]={'1','\0','2'};
    CHECK(xr_string_parse_float64(zero,sizeof(zero)).failure==XR_NUMBER_PARSE_INVALID_SYNTAX);
    CHECK(xr_string_parse_int64("9223372036854775807",19).value==INT64_MAX);
    CHECK(xr_string_parse_int64("-9223372036854775808",20).value==INT64_MIN);
    CHECK(xr_string_parse_int64("9223372036854775808",19).failure==XR_NUMBER_PARSE_OUT_OF_RANGE);
}
int main(void) {
    decimal_syntax();decimal_work_prefixes();size_t count=sizeof(vectors)/sizeof(vectors[0]);CHECK(count>8000);
    for(size_t i=0;i<count;++i) {
        XrStringParseFloatResult parsed=xr_string_parse_float64(vectors[i].text,strlen(vectors[i].text));uint64_t bits=0;
        memcpy(&bits,&parsed.value,sizeof(bits));
        if(parsed.failure!=XR_NUMBER_PARSE_OK || bits!=vectors[i].expected)
            fprintf(stderr,"Decimal case%zu status%u actual%016" PRIx64 " expected%016" PRIx64 "\n%s\n",
                i,(unsigned)parsed.failure,bits,vectors[i].expected,vectors[i].text);
        CHECK(parsed.failure==XR_NUMBER_PARSE_OK && bits==vectors[i].expected);
    }
    printf("Exact decimal rounding: %zu rational vectors, whole syntax, bounded spans, original integer extremes and long exponent cancellation PASS\n",count);return 0;
}
