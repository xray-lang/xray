/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_destructure_native.c - Independent generated-C owned destructuring oracle
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "tuple_destructure_cases.h"
extern const XrXirProgramSpec tuple_destructure_program;
extern const uint32_t tuple_destructure_make;
static XrXirOutputStatus native_output(void *context,const XrXirOutputGroup *group) {
    unsigned *count=context;const char *bytes=NULL;size_t length=0;
    CHECK(group && group->stream==XR_XIR_STDOUT && group->line && group->count==8 && !*count);
    CHECK(xr_xir_string_view(&group->values[0],&bytes,&length) && length==11 && !memcmp(bytes,"destructure",11));
    CHECK(xr_xir_string_view(&group->values[1],&bytes,&length) && length==8 && !memcmp(bytes,"owned中",8));
    CHECK(group->values[2].type==XR_XIR_I64 && group->values[2].payload==1);
    CHECK(group->values[3].type==XR_XIR_I64 && group->values[3].payload==4);
    CHECK(group->values[4].type==XR_XIR_I64 && group->values[4].payload==44);
    CHECK(group->values[5].type==XR_XIR_I64 && group->values[5].payload==7);
    CHECK(xr_xir_string_view(&group->values[6],&bytes,&length) && length==9 && !memcmp(bytes,"singleton",9));
    CHECK(group->values[7].type==XR_XIR_I64 && group->values[7].payload==6);
    ++*count;return XR_XIR_OUTPUT_OK;
}
int main(void) {
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,128000000);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(context,&tuple_destructure_program,&program)==XR_XIR_OK && program);
    destructure_runtime_faults(program,tuple_destructure_make);
    XrXirValue escaped={0};XrXirInstance *instance=NULL;XrXirInstanceConfig config;unsigned groups=0;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,native_output,&groups};
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,tuple_destructure_make,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED && groups==1);
    CHECK(xr_xir_instance_take_result(instance,&escaped)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    XrXirValue text={0},array={0},nested={0},atomic={0},number={0};
    CHECK(xr_xir_tuple_get(&escaped,7,&atomic)==XR_XIR_VALUE_OK && xr_xir_value_valid(&atomic));
    CHECK(xr_xir_tuple_get(&escaped,0,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(&escaped,1,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(&escaped,2,&nested)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&escaped);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&text,&bytes,&length) && length==8 && !memcmp(bytes,"owned中",8));
    XrXirValueAdmission admission={xr_xir_value_arena(&array),NULL,NULL,NULL,64,0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&array,1,&admission,&number,&fault)==XR_XIR_VALUE_OK && number.payload==5);xr_xir_value_drop(&number);
    CHECK(xr_xir_array_get(&array,0,&admission,&number,&fault)==XR_XIR_VALUE_OK && number.payload==44);xr_xir_value_drop(&number);
    CHECK(xr_xir_tuple_get(&nested,1,&number)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&nested);CHECK(xr_xir_string_view(&number,&bytes,&length) && length==6 && !memcmp(bytes,"nested",6));
    xr_xir_value_drop(&number);xr_xir_value_drop(&array);xr_xir_value_drop(&text);xr_xir_value_drop(&atomic);
    CHECK(!runtime_live && !runtime_bytes);source_program_owners_free();
    puts("Native independent Tuple local oracle once=1/COW44,5/nested/Unit/generic6/closure/String/Atomic and first-drop physical0 PASS");return 0;
}
