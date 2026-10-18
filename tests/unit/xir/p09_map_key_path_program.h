/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_map_key_path_program.h - Distinct Instances retain one complete key path program
 */
#ifndef P09_MAP_KEY_PATH_PROGRAM_H
#define P09_MAP_KEY_PATH_PROGRAM_H
#include "xir/xxir_program.h"
static inline uint32_t key_source_entry(const XrXirModule *module) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"entry",5)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    CHECK(found!=UINT32_MAX);return found;
}
static inline void key_source_run(XrXirProgram *program,uint32_t entry,int64_t expected) {
    XrXirInstance *instances[2]={NULL,NULL};
    for(unsigned run=0;run<2;++run) {
        XrXirInstanceConfig config={0};
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instances[run])==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for(unsigned run=0;run<2;++run) {
        CHECK(xr_xir_instance_start(instances[run],entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result={0}; unsigned ticks=0;
        do {
            result=xr_xir_instance_poll_bounded(instances[run],1);CHECK(++ticks<10000);
            if(result.outcome.status==XR_XIR_CALL_SUSPENDED) {
                CHECK(result.epoch && result.outcome.wake);
                CHECK(xr_xir_instance_resume(instances[run],result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
                result.outcome.status=XR_XIR_CALL_READY;
            }
        }
        while(result.outcome.status==XR_XIR_CALL_READY);
        if(result.outcome.status!=XR_XIR_CALL_RETURNED)
            fprintf(stderr,"key source terminal=%u fault=%u\n",result.outcome.status,result.outcome.panic.detail.code);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instances[run],&value)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instances[run])==XR_XIR_CALL_READY);
        if(value.payload!=expected)fprintf(stderr,"key source result=%lld expected=%lld\n",(long long)value.payload,(long long)expected);
        CHECK(value.type==XR_XIR_I64 && value.payload==expected);xr_xir_value_drop(&value);
    }
}
#endif // P09_MAP_KEY_PATH_PROGRAM_H
