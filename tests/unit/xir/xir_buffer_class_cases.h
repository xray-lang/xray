/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_buffer_class_cases.h - Actual managed fields outlive execution owners
 *
 * KEY CONCEPT:
 *   Checked code wraps and replaces real storage; retirement leaves data only.
 */
#ifndef XIR_BUFFER_CLASS_CASES_H
#define XIR_BUFFER_CLASS_CASES_H
#include "xir/xxir_class.h"
#include "xir/xxir_type_arena.h"
static void buffer_class_cases(XrXirProgram *program) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,10,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue number={0};CHECK(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);
    CHECK(number.type==XR_XIR_I64 && number.payload==21);xr_xir_value_drop(&number);
    CHECK(xr_xir_instance_start(instance,9,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue box={0};CHECK(xr_xir_instance_take_result(instance,&box)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(&box),.work=UINT64_MAX,.scratch_bytes=1048576};
    const XrXirStorageLayout *layout=xr_xir_compile_type_arena_storage(admission.arena,(XrXirType)box.type);
    CHECK(layout && layout->value.size==8 && layout->body.size==16 && layout->field_count==2);
    CHECK(layout->field_offsets[0]==0 && layout->field_offsets[1]==8);
    XrXirValue current={0},keep={0};
    CHECK(xr_xir_class_get(&box,0,&admission,&current)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_get(&box,1,&admission,&keep)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&box);
    int64_t length=0;CHECK(xr_xir_buffer_storage_length(&current,&length)==XR_XIR_VALUE_OK && length==21);
    CHECK(xr_xir_buffer_storage_length(&keep,&length)==XR_XIR_VALUE_OK && length==17);
    xr_xir_value_drop(&current);xr_xir_value_drop(&keep);
}
#endif
