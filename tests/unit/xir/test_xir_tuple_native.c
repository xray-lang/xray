/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_native.c - Strict generated C and independent owned expectations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "tuple_runtime_cases.h"
extern const XrXirProgramSpec tuple_source_program;
extern const uint32_t tuple_source_make;
static void native_text(const XrXirValue *value,const char *expected,size_t length) {
    const char *bytes=NULL;size_t count=0;
    CHECK(xr_xir_string_view(value,&bytes,&count) && count==length && !memcmp(bytes,expected,length));
}
int main(void) {
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrXirProgram *program=NULL;XrXirProgramSpec stale=tuple_source_program;stale.target.abi_version=19;
    CHECK(xr_xir_compile_program_seal(context,&stale,&program)==XR_XIR_BAD_LAYOUT && !program);
    CHECK(xr_xir_compile_program_seal(context,&tuple_source_program,&program)==XR_XIR_OK && program);
    tuple_runtime_faults(program,tuple_source_make);
    XrXirValue saved[2]={{0},{0}};
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;XrXirInstance *instance=NULL;unsigned groups=0;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,tuple_output,&groups};
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,tuple_source_make,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED && groups==1);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (unsigned i=0;i<2;++i) {
        XrXirValue first={0},pair={0},number={0},text={0},unit={0};
        CHECK(xr_xir_tuple_get(&saved[i],2,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==1323);
        xr_xir_value_drop(&number);
        CHECK(xr_xir_tuple_get(&saved[i],0,&first)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_tuple_get(&first,0,&number)==XR_XIR_VALUE_OK && number.payload==11);
        CHECK(xr_xir_tuple_get(&first,1,&unit)==XR_XIR_VALUE_OK && !unit.type && !unit.payload);
        CHECK(xr_xir_tuple_get(&first,3,&unit)==XR_XIR_VALUE_OK && !unit.type && !unit.payload);
        CHECK(xr_xir_tuple_get(&first,2,&text)==XR_XIR_VALUE_OK);native_text(&text,"tuple-owner",11);
        CHECK(xr_xir_tuple_get(&saved[i],1,&pair)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&saved[i]);xr_xir_value_drop(&first);native_text(&text,"tuple-owner",11);xr_xir_value_drop(&text);
        xr_xir_value_drop(&number);
        CHECK(xr_xir_tuple_get(&pair,0,&number)==XR_XIR_VALUE_OK && number.payload==7);
        CHECK(xr_xir_tuple_get(&pair,1,&text)==XR_XIR_VALUE_OK);native_text(&text,"generic-owner",13);
        xr_xir_value_drop(&pair);native_text(&text,"generic-owner",13);xr_xir_value_drop(&text);xr_xir_value_drop(&number);
    }
    CHECK(!runtime_live && !runtime_bytes);source_program_owners_free();
    puts("Native independent Tuple oracle order1323/i64=11/generic7/strings/Unit and Program/Instance first-drop physical0 PASS");return 0;
}
