/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_cell_borrow_compiler.c - Complete compilation of real loaned captures
 */
#include "xir_generator_cell_borrow_text.h"
#define receiver_compile_source cell_borrow_text
static const char receiver_compile_schema[]="cell-borrow-compiler-faults.v1";
#include "xir_generator_compiler_fault_cases.h"
static uint32_t borrow_compiler_negative=UINT32_MAX;
static uint32_t receiver_compile_entry(const XrXirModule *module) {
    uint32_t ids[2]={UINT32_MAX,UINT32_MAX};const char *names[2]={"run_owned","run_borrowed"};
    for(uint32_t f=0;f<module->function_count;++f)for(uint32_t i=0;i<2;++i)
        if(module->functions[f].name_length==strlen(names[i]) && !memcmp(module->functions[f].name,names[i],strlen(names[i])))
            {CHECK(ids[i]==UINT32_MAX);ids[i]=f;}
    CHECK(ids[0]!=UINT32_MAX && ids[1]!=UINT32_MAX);borrow_compiler_negative=ids[1];return ids[0];
}
static void cell_borrow_execute(XrXirProgram *program,uint32_t entry,bool borrowed) {
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    XrXirCallStatus expected=borrowed?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_RETURNED;
    if(result.outcome.status!=expected)fprintf(stderr,"owned Cell Source entry%u borrowed%u status%u expected%u\n",
        entry,borrowed?1u:0u,result.outcome.status,expected);
    CHECK(result.outcome.status==expected && !result.outcome.wake);
    if(!borrowed) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
    } else CHECK(!result.outcome.value.type);
    CHECK(xr_xir_instance_free(instance)==(borrowed?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_READY));
    printf("OWNED_CELL_SOURCE entry%u borrowed%u status%u PASS\n",entry,borrowed?1u:0u,result.outcome.status);
}
static void receiver_compile_oracle(XrXirProgram *program,uint32_t entry) {
    CHECK(borrow_compiler_negative!=UINT32_MAX);cell_borrow_execute(program,entry,false);
    cell_borrow_execute(program,borrow_compiler_negative,true);xr_xir_compile_program_drop(program);
}
