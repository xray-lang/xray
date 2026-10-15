/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derived_fibonacci_runtime.h - Owned ordinary constant table receiving
 */
#ifndef XIR_DERIVED_FIBONACCI_RUNTIME_H
#define XIR_DERIVED_FIBONACCI_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_compile_domain.h"
static const XrCompileResourceLimits derived_fibonacci_limits={67108864,8388608,128000000};
static void derived_fibonacci_value(const XrXirValue *value,XrXirDomain *reader) {
    const int64_t expected[]={0,1,1,2,3,5,8,13,21,34};
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena && reader);
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);
    CHECK(xr_xir_array_element(types,(XrXirType)value->type)==XR_XIR_I8);
    XrXirValueAdmission admission={.arena=arena,.domain=reader,.work=UINT64_MAX,.scratch_bytes=65536};
    int64_t length=0,capacity=0;
    CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK && length==10);
    CHECK(xr_xir_array_capacity(value,&admission,&capacity)==XR_XIR_VALUE_OK && capacity==16);
    for(int64_t i=0;i<10;++i) {
        XrXirValue field={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(value,i,&admission,&field,&fault)==XR_XIR_VALUE_OK);
        CHECK(field.type==XR_XIR_I8 && !field.reserved && field.payload==expected[i]);xr_xir_value_drop(&field);
    }
}
static void derived_fibonacci_runtime_attempt(XrXirProgram *program,const uint32_t entries[2],XrXirDomain *reader) {
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
                CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==88);
                xr_xir_value_drop(&value);
            }else derived_fibonacci_value(&value,reader);
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
    if(value.type){derived_fibonacci_value(&value,reader);xr_xir_value_drop(&value);}
}
static size_t derived_fibonacci_runtime_failures(XrXirProgram *program,const uint32_t entries[2],XrXirDomain *reader,bool inject) {
    size_t baseline=runtime_live,bytes=runtime_bytes;
    runtime_attempts=0;derived_fibonacci_runtime_attempt(program,entries,reader);size_t sites=runtime_attempts;
    CHECK(sites && runtime_live==baseline && runtime_bytes==bytes);
    if(inject)for(size_t fault=0;fault<sites;++fault) {
        runtime_attempts=0;runtime_fail_at=fault;derived_fibonacci_runtime_attempt(program,entries,reader);
        CHECK(runtime_attempts>fault && runtime_live==baseline && runtime_bytes==bytes);
        runtime_fail_at=SIZE_MAX;
        /* Failure must preserve the same admitted Program for an ordinary retry. */
        derived_fibonacci_runtime_attempt(program,entries,reader);
        CHECK(runtime_live==baseline && runtime_bytes==bytes);
    }
    return sites;
}
#endif // XIR_DERIVED_FIBONACCI_RUNTIME_H
