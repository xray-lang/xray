/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_entries_float.h - Independent transport bits and actual failures
 */
#ifndef XIR_ARRAY_ENTRIES_FLOAT_H
#define XIR_ARRAY_ENTRIES_FLOAT_H
#if !defined(XR_ENTRIES_MATRIX)
static void entries_float_bits(EntriesCompile *run) {
    static const uint64_t wide[]={0,UINT64_C(0x8000000000000000),1,UINT64_C(0x8000000000000001),
        UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),UINT64_C(0x7ff8000000001234),
        UINT64_C(0x7ff0000000000001),UINT64_C(0xfff8123456789abc)};
    static const uint64_t narrow[]={0,UINT64_C(0x80000000),1,UINT64_C(0x80000001),UINT64_C(0x7f800000),
        UINT64_C(0xff800000),UINT64_C(0x7fc00000)};
    size_t actual_sites=0;
    for(unsigned precision=0;precision<2;++precision) {
        uint32_t function=entries_find(run->module,precision?"shortBits":"floatBits");
        const uint64_t *bits=precision?narrow:wide;size_t count=precision?7:9;
        if(precision) {
            XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
            XrXirValueAdmission admission={run->program->arena,domain,NULL,NULL,100000,1048576};
            static const uint64_t invalid[]={UINT64_C(0x100000000),UINT64_C(0x7f800001),UINT64_C(0xffc00000)};
            for(unsigned n=0;n<3;++n) {
                XrXirValue input={XR_XIR_F32,0,0},out={0};memcpy(&input.payload,&invalid[n],sizeof(input.payload));
                CHECK(xr_xir_array_new(run->program->entries[function].parameters[0],&input,1,&admission,&out)==XR_XIR_VALUE_BAD_ARGUMENT);
                CHECK(!out.type&&!out.payload);
            }
            xr_xir_domain_drop(domain);CHECK(!runtime_live&&!runtime_bytes);
        }
        for(size_t n=0;n<count;++n) {
            XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
            XrXirValueAdmission admission={run->program->arena,domain,NULL,NULL,100000,1048576};
            XrXirValue input={precision?XR_XIR_F32:XR_XIR_F64,0,0},array={0};
            memcpy(&input.payload,&bits[n],sizeof(input.payload));
            CHECK(xr_xir_array_new(run->program->entries[function].parameters[0],&input,1,&admission,&array)==XR_XIR_VALUE_OK);
            const size_t live=runtime_live,bytes=runtime_bytes;
            XrXirInstance *instance=entries_ready(run);runtime_attempts=0;
            CHECK(xr_xir_instance_start(instance,function,&array,1)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
            size_t sites=runtime_attempts;actual_sites+=sites;
            int64_t length=0;CHECK(xr_xir_array_len(&result,&admission,&length)==XR_XIR_VALUE_OK&&length==1);
            XrXirValue item={0},payload={0},index={0};XrXirFaultDetail fault={0};
            CHECK(xr_xir_array_get(&result,0,&admission,&item,&fault)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_tuple_get(&item,0,&index)==XR_XIR_VALUE_OK&&index.type==XR_XIR_I64&&!index.payload);
            CHECK(xr_xir_tuple_get(&item,1,&payload)==XR_XIR_VALUE_OK&&payload.type==input.type&&payload.payload==input.payload);
            xr_xir_value_drop(&index);xr_xir_value_drop(&payload);xr_xir_value_drop(&item);xr_xir_value_drop(&result);
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
            for(size_t failure=0;failure<sites;++failure) {
                instance=entries_ready(run);runtime_attempts=0;runtime_fail_at=failure;
                XrXirCallStatus status=xr_xir_instance_start(instance,function,&array,1);
                while(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
                CHECK(status==XR_XIR_CALL_OOM&&runtime_attempts>failure);runtime_fail_at=SIZE_MAX;
                result=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE&&!result.type&&!result.payload);
                CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
            }
            CHECK(xr_xir_array_len(&array,&admission,&length)==XR_XIR_VALUE_OK&&length==1);
            xr_xir_value_drop(&array);xr_xir_domain_drop(domain);CHECK(!runtime_live&&!runtime_bytes);
        }
    }
    printf("entries f64 arbitrary9/f32 canonical7 bits runtimeOOM%zu physical0\n",actual_sites);
}
#endif
#endif
