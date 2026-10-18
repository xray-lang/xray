/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_receiver_compiler.c - Complete receiver compiler ownership and fault replays
 */
static const char receiver_compile_source[]=
    "final class Box {const value:i64\n"
    " constructor(value:i64){this.value=value}\n"
    " iterator()->Iterator<i64>{defer{print(401)}\n yield this.value}}\n"
    "fn worker()->i64 {const box=Box(17)\n"
    " const first=box.iterator\n var make=first\n make=box.iterator\n"
    " try{for(value in make()){return value}\n return -2} catch(error){return -1}}\n"
    "export fn run()->i64 {return await go worker()}\n fn main(){}\n";
static const char receiver_compile_schema[]="receiver-compiler-faults.v2";
#include "xir_generator_compiler_fault_cases.h"
static uint32_t receiver_compile_entry(const XrXirModule *module) {
    uint32_t entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==3 && !memcmp(module->functions[f].name,"run",3)) {CHECK(entry==UINT32_MAX);entry=f;}
    return entry;
}
static XrXirOutputStatus receiver_compile_output(void *owner,const XrXirOutputGroup *group) {
    unsigned *count=owner;CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==401);++*count;return XR_XIR_OUTPUT_OK;
}
static void receiver_compile_oracle(XrXirProgram *program,uint32_t entry) {
    unsigned cleanups=0;XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,receiver_compile_output,&cleanups};
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==17 && cleanups==1);xr_xir_value_drop(&value);
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
