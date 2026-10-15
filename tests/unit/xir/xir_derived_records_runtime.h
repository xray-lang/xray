/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derived_records_runtime.h - Owned ordinary constant table receiving
 */
#ifndef XIR_DERIVED_RECORDS_RUNTIME_H
#define XIR_DERIVED_RECORDS_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include "xir/xxir_struct.h"
#include "xir/xxir_compile_domain.h"
static const XrCompileResourceLimits derived_records_limits={67108864,8388608,128000000};
static void derived_records_value(const XrXirValue *value,XrXirDomain *reader) {
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena && reader);
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);XrXirType element=xr_xir_array_element(types,(XrXirType)value->type);
    const XrXirTypeNode *node=xr_xir_type_node(types,element);CHECK(node && xr_xir_type_is_struct(types,element) && node->nominal.field_count==2);
    CHECK(node->nominal.fields[0]==XR_XIR_I8 && node->nominal.fields[1]==XR_XIR_I8);
    XrXirValueAdmission admission={.arena=arena,.domain=reader,.work=UINT64_MAX,.scratch_bytes=65536};int64_t count=0,capacity=0;
    CHECK(xr_xir_array_len(value,&admission,&count)==XR_XIR_VALUE_OK && count==4);
    CHECK(xr_xir_array_capacity(value,&admission,&capacity)==XR_XIR_VALUE_OK && capacity==4);
    for(int64_t i=0;i<4;++i) {
        XrXirValue row={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(value,i,&admission,&row,&fault)==XR_XIR_VALUE_OK && (XrXirType)row.type==element);
        XrXirValueAdmission child=admission;child.arena=xr_xir_value_arena(&row);CHECK(child.arena);
        for(uint32_t f=0;f<2;++f) {
            XrXirValue field={0};CHECK(xr_xir_struct_get(&row,f,&child,&field)==XR_XIR_VALUE_OK);
            CHECK(field.type==XR_XIR_I8 && !field.reserved && field.payload==(f?i*i:i));xr_xir_value_drop(&field);
        }
        xr_xir_value_drop(&row);
    }
}
static void derived_records_runtime_attempt(XrXirProgram *program,const uint32_t entries[2],XrXirDomain *reader) {
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
                CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==649367177924771840);
                xr_xir_value_drop(&value);
            }else derived_records_value(&value,reader);
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
    if(value.type){derived_records_value(&value,reader);xr_xir_value_drop(&value);}
}
static size_t derived_records_runtime_failures(XrXirProgram *program,const uint32_t entries[2],XrXirDomain *reader,bool inject) {
    size_t baseline=runtime_live,bytes=runtime_bytes;
    runtime_attempts=0;derived_records_runtime_attempt(program,entries,reader);size_t sites=runtime_attempts;
    CHECK(sites && runtime_live==baseline && runtime_bytes==bytes);
    if(inject)for(size_t fault=0;fault<sites;++fault) {
        runtime_attempts=0;runtime_fail_at=fault;derived_records_runtime_attempt(program,entries,reader);
        CHECK(runtime_attempts>fault && runtime_live==baseline && runtime_bytes==bytes);
        runtime_fail_at=SIZE_MAX;
        /* Failure must preserve the same admitted Program for an ordinary retry. */
        derived_records_runtime_attempt(program,entries,reader);
        CHECK(runtime_live==baseline && runtime_bytes==bytes);
    }
    return sites;
}
#endif // XIR_DERIVED_RECORDS_RUNTIME_H
