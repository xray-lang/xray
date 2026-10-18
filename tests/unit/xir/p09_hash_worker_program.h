/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_worker_program.h - Terminal Task owners preserve their containers after Program teardown
 */
#ifndef P09_HASH_WORKER_PROGRAM_H
#define P09_HASH_WORKER_PROGRAM_H
#include "xir/xxir_program.h"
#include "xir/xxir_task.h"
#include "xir/xxir_map.h"
#include "xir/xxir_array.h"
#include "xir/xxir_type_arena.h"
static XrXirValue worker_call(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    for(uint32_t tick=0;tick<10000;++tick) {
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1);
        if(result.outcome.status==XR_XIR_CALL_READY)continue;
        if(result.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"hash worker terminal=%u\n",(unsigned)result.outcome.status);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
    }
    CHECK(false);return (XrXirValue){0};
}
static void worker_map_outcome(XrXirValue *map,XrXirDomain *domain) {
    XrXirValueAdmission admission={xr_xir_value_arena(map),domain,NULL,NULL,100000,65536};
    XrXirValue arrays[2]={{0}};int64_t length=0;
    CHECK(xr_xir_hash_len(map,&admission,&length)==XR_XIR_VALUE_OK && length==2);
    for(unsigned index=0;index<2;++index) {
        XrXirValue key={XR_XIR_I64,0,index+1};bool found=false;
        CHECK(xr_xir_map_get(map,&key,&admission,arrays+index,&found)==XR_XIR_VALUE_OK && found);
    }
    xr_xir_value_drop(map);
    for(unsigned index=0;index<2;++index) {
        CHECK(xr_xir_array_len(arrays+index,&admission,&length)==XR_XIR_VALUE_OK && length==(index?1:2));
        for(int64_t at=0;at<length;++at) {
            XrXirValue item={0};XrXirFaultDetail fault={0};
            CHECK(xr_xir_array_get(arrays+index,at,&admission,&item,&fault)==XR_XIR_VALUE_OK);
            CHECK(item.type==XR_XIR_I64 && item.payload==(index?2:7+at));xr_xir_value_drop(&item);
        }
        xr_xir_value_drop(arrays+index);
    }
}
static void worker_set_outcome(XrXirValue *set,XrXirDomain *domain) {
    XrXirValueAdmission admission={xr_xir_value_arena(set),domain,NULL,NULL,100000,65536};int64_t length=0;
    CHECK(xr_xir_hash_len(set,&admission,&length)==XR_XIR_VALUE_OK && length==2);
    const int64_t keys[]={3,4,9};
    for(unsigned index=0;index<3;++index) {
        XrXirValue key={XR_XIR_I64,0,keys[index]};bool found=false;
        CHECK(xr_xir_set_contains(set,&key,&admission,&found)==XR_XIR_VALUE_OK && found==(index!=0));
    }
    xr_xir_value_drop(set);
}
static void worker_program_run(XrXirProgram *program,const uint32_t *entries) {
    XrXirValue held[2]={{0}};
    for(unsigned run=0;run<2;++run) {
        XrXirInstanceConfig config={0};XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        XrXirValue result=worker_call(instance,entries[0]);CHECK(result.type==XR_XIR_I64 && result.payload==123);
        for(unsigned index=0;index<2;++index) {
            XrXirValue value=worker_call(instance,entries[index+1]);
            if(run)xr_xir_value_drop(&value);else held[index]=value;
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    for(unsigned index=0;index<2;++index) {
        XrXirValue copy={0};CHECK(xr_xir_value_copy(held+index,&copy)==XR_XIR_VALUE_OK);
        for(unsigned pass=0;pass<2;++pass) {
            XrXirValue *task=pass?&copy:held+index;XrXirCallResult outcome={0};
            CHECK(xr_xir_task_copy_outcome(task,&outcome)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(task);
            if(index)worker_set_outcome(&outcome.value,domain);else worker_map_outcome(&outcome.value,domain);
            xr_xir_call_result_drop(&outcome);
        }
    }
    xr_xir_domain_drop(domain);
}
#endif // P09_HASH_WORKER_PROGRAM_H
