/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_resize_escape.h - Exact bits and final owner lifetime
 *
 * KEY CONCEPT:
 *   Escaping arrays keep arena and captured code alive until the final drop.
 */
#ifndef XIR_ARRAY_RESIZE_ESCAPE_H
#define XIR_ARRAY_RESIZE_ESCAPE_H
static void resize_float_bits(ResizeCompile *run) {
    static const uint64_t bits[]={0,UINT64_C(0x8000000000000000),1,UINT64_C(0x8000000000000001),
        UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),UINT64_C(0x7ff8000000001234),
        UINT64_C(0x7ff0000000000001),UINT64_C(0xfff8123456789abc)};
    XrXirInstance *instance=resize_open(run);XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    uint32_t function=resize_find(run->module,"floatBits");
    XrXirValueAdmission admission={run->program->arena,domain,NULL,NULL,100000,1048576};
    for(unsigned n=0;n<sizeof(bits)/sizeof(bits[0]);++n) {
        XrXirValue input={XR_XIR_F64,0,0},array={0};memcpy(&input.payload,&bits[n],sizeof(input.payload));
        CHECK(xr_xir_array_new(run->program->entries[function].parameters[0],&input,1,&admission,&array)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_start(instance,function,&array,1)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        int64_t length=0;CHECK(xr_xir_array_len(&result,&admission,&length)==XR_XIR_VALUE_OK&&length==2);
        for(int64_t i=0;i<2;++i) {
            XrXirValue element={0};XrXirFaultDetail fault={0};
            CHECK(xr_xir_array_get(&result,i,&admission,&element,&fault)==XR_XIR_VALUE_OK&&element.type==XR_XIR_F64);
            uint64_t actual=0;memcpy(&actual,&element.payload,sizeof(actual));CHECK(actual==bits[n]);xr_xir_value_drop(&element);
        }
        CHECK(xr_xir_array_len(&array,&admission,&length)==XR_XIR_VALUE_OK&&length==1);
        xr_xir_value_drop(&result);xr_xir_value_drop(&array);
    }
    xr_xir_domain_drop(domain);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    puts("resize Float64 arbitrary-bit prefix+fill18 transports physical0");
}
static void resize_escaped(ResizeCompile *run) {
    XrXirInstance *instance=resize_open(run);
    XrXirValue callable=resize_run(instance,resize_find(run->module,"retainedCallable"));
    XrXirValue text=resize_run(instance,resize_find(run->module,"text"));
    XrXirValue alias=resize_run(instance,resize_find(run->module,"saved"));
    XrXirValue functions[2]={{0}};
    XrXirValueAdmission admission=instance_candidate_admission(instance);
    for(int64_t i=0;i<2;++i) {
        XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(&callable,i,&admission,&functions[i],&fault)==XR_XIR_VALUE_OK);
    }
    xr_xir_compile_artifact_free(run->lowered);run->lowered=NULL;
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program);run->program=NULL;run->module=NULL;
    CHECK(resize_stats(&run->context).live_bytes>run->baseline.live_bytes);
    for(int64_t i=0;i<2;++i) {
        const XrXirFunctionBinding *binding=xr_xir_function_binding(&functions[i]);
        CHECK(binding&&binding->owner&&binding->capture_count==1&&binding->captures[0].type==XR_XIR_I64&&binding->captures[0].payload==7);
    }
    XrXirDomain *domain=object_pointer(&text)->domain;CHECK(domain);
    admission=(XrXirValueAdmission){xr_xir_value_arena(&text),domain,NULL,NULL,100000,1048576};
    for(int64_t i=1;i<3;++i) {
        XrXirValue value={0};XrXirFaultDetail fault={0};const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_array_get(&text,i,&admission,&value,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_view(&value,&bytes,&length)&&length==5&&!memcmp(bytes,"a\0中",5));xr_xir_value_drop(&value);
    }
    static const int64_t old[]={1,INT64_MIN,3};resize_array(&alias,old,3,true,1);
    xr_xir_value_drop(&alias);xr_xir_value_drop(&text);xr_xir_value_drop(&callable);
    CHECK(resize_stats(&run->context).live_bytes>run->baseline.live_bytes);
    xr_xir_value_drop(&functions[0]);xr_xir_value_drop(&functions[1]);
    CHECK(!runtime_live&&!runtime_bytes&&resize_stats(&run->context).live_bytes==run->baseline.live_bytes);
    puts("resize escaped Callable/String/nullable alias pins arena/code until final physical0");
}
#endif
