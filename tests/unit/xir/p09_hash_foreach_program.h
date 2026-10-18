/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_foreach_program.h - Owned Map callback results after their program dies
 */
#ifndef P09_HASH_FOREACH_PROGRAM_H
#define P09_HASH_FOREACH_PROGRAM_H
#include "xir/xxir_program.h"
#include "xir/xxir_array.h"
#include "xir/xxir_map.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
static XrXirValue foreach_call(XrXirInstance *instance,uint32_t entry,uint32_t expected) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result={0};uint32_t polls=0,resumed=0;uint64_t epoch=0,wake=0;
    for(;;) {
        CHECK(++polls<10000);result=xr_xir_instance_poll_bounded(instance,1);
        if(result.outcome.status==XR_XIR_CALL_READY)continue;
        if(result.outcome.status==XR_XIR_CALL_SUSPENDED) {
            CHECK(resumed<expected && result.epoch && result.outcome.wake && result.outcome.wake!=wake);
            if(!epoch)epoch=result.epoch;CHECK(epoch==result.epoch);wake=result.outcome.wake;
            XrXirWaitRequest wait={0};
            CHECK(xr_xir_instance_wait_request(instance,epoch,wake,&wait)==XR_XIR_CALL_READY && wait.kind==XR_XIR_WAIT_YIELD);
            CHECK(!wait.reserved && !wait.after_ms && !wait.subject && !wait.generation && !wait.ticket);
            CHECK(xr_xir_instance_resume(instance,epoch,wake)==XR_XIR_CALL_READY);++resumed;continue;
        }
        if(result.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"forEach entry=%u terminal=%u\n",entry,result.outcome.status);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && resumed==expected);break;
    }
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
#include "xir/xxir_float.h"
static inline void foreach_program_run(XrXirProgram *program,const uint32_t *entries) {
    XrXirValue held[4]={{0}};
    for(uint32_t run=0;run<2;++run) {
        XrXirInstanceConfig config={0};XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        XrXirValue result=foreach_call(instance,entries[0],5);CHECK(result.type==XR_XIR_I64 && result.payload==123);
        for(uint32_t i=0;i<4;++i) {
            XrXirValue value=foreach_call(instance,entries[i+1],1);
            if(!run)held[i]=value;else xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    for(uint32_t i=0;i<4;++i) {
        XrXirValueAdmission admission={xr_xir_value_arena(held+i),domain,NULL,NULL,100000,65536};
        XrXirValue key={XR_XIR_I64,0,i+1},array={0};bool found=false,equal=false;
        CHECK(xr_xir_map_get(held+i,&key,&admission,&array,&found)==XR_XIR_VALUE_OK && found);
        CHECK(xr_xir_map_contains_value(held+i,&array,&admission,&equal)==XR_XIR_VALUE_OK && equal==(i!=1));
        xr_xir_value_drop(held+i);int64_t length=0;
        CHECK(xr_xir_array_len(&array,&admission,&length)==XR_XIR_VALUE_OK && length==(i?1:2));
        for(int64_t j=0;j<length;++j) {
            XrXirValue item={0};XrXirFaultDetail fault={0};int64_t relation=0;
            CHECK(xr_xir_array_get(&array,j,&admission,&item,&fault)==XR_XIR_VALUE_OK && item.type==XR_XIR_F64);
            if(i==1)CHECK(xr_xir_float_relation(XR_XIR_F64,XR_XIR_FLOAT_EQ,item.payload,item.payload,&relation)==XR_XIR_RUN_OK && !relation);
            else {double number=i?(double)(i+1):(double)(j+1);int64_t bits=0;memcpy(&bits,&number,sizeof(bits));
                CHECK(xr_xir_float_relation(XR_XIR_F64,XR_XIR_FLOAT_EQ,item.payload,bits,&relation)==XR_XIR_RUN_OK && relation);}
        }
        xr_xir_value_drop(&array);
    }
    xr_xir_domain_drop(domain);
}
#endif // P09_HASH_FOREACH_PROGRAM_H
