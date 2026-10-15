/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derived_pairs_runtime.h - Owned ordinary constant table receiving
 */
#ifndef XIR_DERIVED_PAIRS_RUNTIME_H
#define XIR_DERIVED_PAIRS_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_tuple.h"
static const XrCompileResourceLimits derived_pairs_limits={67108864,8388608,128000000};
static void derived_pairs_value(const XrXirValue *value) {
    const int64_t expected[]={2,3};
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena);
    const XrXirTypeNode *node=xr_xir_tuple_signature(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type);
    CHECK(node && node->parameter_count==2);
    for(uint32_t f=0;f<2;++f) {
        XrXirValue field={0};CHECK(node->parameters[f].type==XR_XIR_I64);
        CHECK(xr_xir_tuple_get(value,f,&field)==XR_XIR_VALUE_OK);
        CHECK(field.type==XR_XIR_I64 && !field.reserved && field.payload==expected[f]);xr_xir_value_drop(&field);
    }
}
static void derived_pairs_runtime_attempt(XrXirProgram *program,const uint32_t entries[2]) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;bool driven_oom=false;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if(status==XR_XIR_CALL_OOM)CHECK(!instance);
    XrXirValue value={0};
    for(unsigned entry=0;entry<2 && (status==XR_XIR_CALL_READY || status==XR_XIR_CALL_RETURNED);++entry) {
        status=xr_xir_instance_start(instance,entries[entry],NULL,0);
        if(status==XR_XIR_CALL_READY) {
            status=xr_xir_instance_poll_bounded(instance,1024).outcome.status;
            driven_oom|=status==XR_XIR_CALL_OOM;
        }
        if(status==XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            if(!entry) {
                CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==32);
                xr_xir_value_drop(&value);
            }else derived_pairs_value(&value);
        }
    }
    CHECK(status==XR_XIR_CALL_RETURNED || (runtime_fail_at!=SIZE_MAX && status==XR_XIR_CALL_OOM));
    if(status==XR_XIR_CALL_OOM) {
        CHECK(!value.type && !value.reserved && !value.payload);
        if(instance) {
            XrXirValue absent={0};
            CHECK(xr_xir_instance_take_result(instance,&absent)==XR_XIR_CALL_BAD_STATE);
            CHECK(!absent.type && !absent.reserved && !absent.payload);
        }
    }
    if(instance) {
        XrXirCallStatus released=xr_xir_instance_free(instance);
        XrXirCallStatus expected=driven_oom?XR_XIR_CALL_OOM:XR_XIR_CALL_READY;
        if(released!=expected)fprintf(stderr,"constant runtime free: actual=%u expected=%u prior=%u fault=%zu attempts=%zu\n",released,expected,status,runtime_fail_at,runtime_attempts);
        CHECK(released==expected);
    }
    if(value.type){derived_pairs_value(&value);xr_xir_value_drop(&value);}
}
static size_t derived_pairs_runtime_failures(XrXirProgram *program,const uint32_t entries[2],bool inject) {
    size_t baseline=runtime_live,bytes=runtime_bytes;
    runtime_attempts=0;derived_pairs_runtime_attempt(program,entries);size_t sites=runtime_attempts;
    CHECK(sites && runtime_live==baseline && runtime_bytes==bytes);
    if(inject)for(size_t fault=0;fault<sites;++fault) {
        runtime_attempts=0;runtime_fail_at=fault;derived_pairs_runtime_attempt(program,entries);
        CHECK(runtime_attempts>fault && runtime_live==baseline && runtime_bytes==bytes);
        runtime_fail_at=SIZE_MAX;
        /* Failure must preserve the same admitted Program for an ordinary retry. */
        derived_pairs_runtime_attempt(program,entries);
        CHECK(runtime_live==baseline && runtime_bytes==bytes);
    }
    return sites;
}
#endif // XIR_DERIVED_PAIRS_RUNTIME_H
