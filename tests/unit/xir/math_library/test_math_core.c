/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_math_core.c - Independent numeric and entropy failure oracles
 */
#include "xir/xxir_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <errno.h>
#if XR_OS_WINDOWS
#include <float.h>
#endif
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
typedef struct MathOracle {
    uint32_t operation;
    uint64_t left,right,result;
    XrXirRunStatus status;
    uint32_t ulp;
} MathOracle;
#include "numeric_vectors.h"
static int64_t payload(uint64_t value) {int64_t result=0;memcpy(&result,&value,sizeof(result));return result;}
static uint64_t ordered(uint64_t value) {return value>>63 ? ~value : value^UINT64_C(0x8000000000000000);}
static void numeric_oracles(void) {
    bool covered[XR_XIR_MATH_COUNT]={false};unsigned range=0;
    for (size_t i=0;i<sizeof(math_oracles)/sizeof(*math_oracles);++i) {
        const MathOracle *row=&math_oracles[i];int64_t result=55;
        XrXirRunStatus status=xr_xir_math_evaluate(row->operation,payload(row->left),payload(row->right),&result);
        if (status!=row->status) fprintf(stderr,"numeric row%zu operation%u status%u expected%u\n",i,row->operation,status,row->status);
        CHECK(status==row->status);covered[row->operation]=true;
        if (status!=XR_XIR_RUN_OK) {CHECK(result==55);++range;continue;}
        uint64_t actual=0;memcpy(&actual,&result,sizeof(actual));
        uint64_t a=ordered(actual),b=ordered(row->result),distance=a>b?a-b:b-a;
        if (distance>row->ulp) fprintf(stderr,"numeric row%zu operation%u bits%llx expected%llx ulp%llu\n",i,row->operation,(unsigned long long)actual,(unsigned long long)row->result,(unsigned long long)distance);
        CHECK(distance<=row->ulp);
    }
    CHECK(range==20);
    for (uint32_t operation=1;operation<XR_XIR_MATH_COUNT;++operation) {
        XrXirMathSignature signature;
        CHECK(xr_xir_math_signature(operation,&signature));
        CHECK(signature.random ? !covered[operation] : covered[operation]);
    }
    printf("Independent math reference rows=%zu, all26 pure operations, exact integer/range/zero/NaN and four-ULP transcendental boundary PASS\n",sizeof(math_oracles)/sizeof(*math_oracles));
}
typedef struct EntropyOracle {
    uint64_t words[4];unsigned count,attempts,limit;
    XrXirRunStatus failure;
} EntropyOracle;
static XrXirRunStatus entropy_fill(void *context,unsigned char *bytes,size_t length) {
    EntropyOracle *oracle=context;CHECK(length==8);unsigned ordinal=oracle->attempts++;
    if (ordinal>=oracle->limit) return oracle->failure;
    uint64_t word=oracle->words[ordinal%oracle->count];memcpy(bytes,&word,8);return XR_XIR_RUN_OK;
}
static void random_oracles(void) {
    EntropyOracle oracle={{0},1,0,1,XR_XIR_RUN_STEP_LIMIT};XrXirMathEntropy entropy={&oracle,entropy_fill};int64_t result=55;
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM,entropy,0,0,&result)==XR_XIR_RUN_OK && result==0 && oracle.attempts==1);
    oracle=(EntropyOracle){{UINT64_MAX},1,0,1,XR_XIR_RUN_STEP_LIMIT};
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM,entropy,0,0,&result)==XR_XIR_RUN_OK && result==payload(UINT64_C(0x3fefffffffffffff)));
    oracle=(EntropyOracle){{0,8},2,0,2,XR_XIR_RUN_STEP_LIMIT};
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM_INT,entropy,10,12,&result)==XR_XIR_RUN_OK && result==12 && oracle.attempts==2);
    oracle=(EntropyOracle){{5},1,0,1,XR_XIR_RUN_STEP_LIMIT};
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM_INT,entropy,5,2,&result)==XR_XIR_RUN_OK && result==3 && oracle.attempts==1);
    oracle=(EntropyOracle){{0},1,0,1,XR_XIR_RUN_STEP_LIMIT};
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM_INT,entropy,INT64_MIN,INT64_MAX,&result)==XR_XIR_RUN_OK && result==INT64_MIN);
    oracle=(EntropyOracle){{UINT64_MAX},1,0,1,XR_XIR_RUN_STEP_LIMIT};
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM_INT,entropy,INT64_MIN,INT64_MAX,&result)==XR_XIR_RUN_OK && result==INT64_MAX);
    oracle=(EntropyOracle){{0},1,0,0,XR_XIR_RUN_HOST_ERROR};
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM_INT,entropy,13,13,&result)==XR_XIR_RUN_OK && result==13 && !oracle.attempts);
    result=55;
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM,entropy,0,0,&result)==XR_XIR_RUN_HOST_ERROR && result==55 && oracle.attempts==1);
    oracle=(EntropyOracle){{0},1,0,4,XR_XIR_RUN_STEP_LIMIT};
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM_INT,entropy,0,2,&result)==XR_XIR_RUN_STEP_LIMIT && result==55 && oracle.attempts==5);
    oracle=(EntropyOracle){{0},1,0,0,(XrXirRunStatus)UINT32_MAX};
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM,entropy,0,0,&result)==XR_XIR_RUN_BAD_ARGUMENT && result==55 && oracle.attempts==1);
    puts("Independent entropy: rejection, complete signed range, reversed/equal bounds and metered failure without retry or publication PASS");
}
static void refusal_oracles(void) {
    XrXirMathSignature signature;memset(&signature,0xa5,sizeof(signature));unsigned char before[sizeof(signature)];memcpy(before,&signature,sizeof(signature));
    CHECK(!xr_xir_math_signature(0,&signature) && !memcmp(before,&signature,sizeof(signature)));
    CHECK(!xr_xir_math_signature(UINT32_MAX,&signature) && !memcmp(before,&signature,sizeof(signature)));
    int64_t value=55;EntropyOracle oracle={{0},1,0,1,XR_XIR_RUN_STEP_LIMIT};XrXirMathEntropy entropy={&oracle,entropy_fill};
    CHECK(xr_xir_math_evaluate(XR_XIR_MATH_RANDOM,0,0,&value)==XR_XIR_RUN_BAD_ARGUMENT && value==55);
    CHECK(xr_xir_math_evaluate(XR_XIR_MATH_SQRT,0,1,&value)==XR_XIR_RUN_BAD_ARGUMENT && value==55);
    CHECK(xr_xir_math_evaluate(UINT32_MAX,0,0,&value)==XR_XIR_RUN_BAD_ARGUMENT && value==55);
    CHECK(xr_xir_math_random(XR_XIR_MATH_SQRT,entropy,0,0,&value)==XR_XIR_RUN_BAD_ARGUMENT && value==55 && !oracle.attempts);
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM,entropy,1,0,&value)==XR_XIR_RUN_BAD_ARGUMENT && value==55 && !oracle.attempts);
    CHECK(xr_xir_math_random(XR_XIR_MATH_RANDOM_INT,(XrXirMathEntropy){0},13,13,&value)==XR_XIR_RUN_BAD_ARGUMENT && value==55);
}
static void ambient_environment(void) {
    fenv_t original;CHECK(!feholdexcept(&original));
    const int modes[]={FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    for (size_t i=0;i<sizeof(modes)/sizeof(*modes);++i) {
        CHECK(!fesetround(modes[i]));CHECK(!feraiseexcept(FE_DIVBYZERO));
        int flags=fetestexcept(FE_ALL_EXCEPT);errno=E2BIG;int64_t result=55;
        CHECK(xr_xir_math_evaluate(XR_XIR_MATH_SQRT,INT64_C(0x4000000000000000),0,&result)==XR_XIR_RUN_OK);
        CHECK(result==INT64_C(0x3ff6a09e667f3bcd));
        CHECK(fegetround()==modes[i] && fetestexcept(FE_ALL_EXCEPT)==flags && errno==E2BIG);
        CHECK(xr_xir_math_evaluate(XR_XIR_MATH_SQRT,(-INT64_C(0x4010000000000000)),0,&result)==XR_XIR_RUN_OK);
        CHECK(result==INT64_C(0x7ff8000000000000));
        CHECK(fegetround()==modes[i] && fetestexcept(FE_ALL_EXCEPT)==flags && errno==E2BIG);
    }
#if XR_OS_WINDOWS
    unsigned int control=0;
    CHECK(!_controlfp_s(&control,_DN_FLUSH,_MCW_DN));
    CHECK((control&_MCW_DN)==_DN_FLUSH);
    int64_t result=55;
    CHECK(xr_xir_math_evaluate(XR_XIR_MATH_SQRT,1,0,&result)==XR_XIR_RUN_OK);
    CHECK(result==INT64_C(0x1e60000000000000));
    CHECK(!_controlfp_s(&control,0,0) && (control&_MCW_DN)==_DN_FLUSH);
#endif
    CHECK(!fesetenv(&original));
    puts("Ambient rounding, exception flags, denormal control and errno isolation PASS");
}
int main(void) {ambient_environment();numeric_oracles();random_oracles();refusal_oracles();return 0;}
