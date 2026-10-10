/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_cases.h - Capacity bounds and independent owning results
 *
 * KEY CONCEPT:
 *   Capacity is a lower bound, while aliases preserve their copied capacity.
 */
#ifndef XIR_ARRAY_CAPACITY_CASES_H
#define XIR_ARRAY_CAPACITY_CASES_H
static void capacity_goldens(CapacityCompile *run) {
    static const char *const names[]={"empty","zero","absolute","noChange",
        "pushSet","nested","rebound","readTemporary","generic","identity",
        "emptyElement","nullable","shadowControl","widened"};
    XrXirInstance *first=capacity_open(run),*second=capacity_open(run);
    for(unsigned instance=0;instance<2;++instance){
        XrXirInstance *current=instance?second:first;
        for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i){
            XrXirValue value=capacity_run(current,capacity_find(run->module,names[i]));
            CHECK(value.type==XR_XIR_BOOL&&value.payload==1);xr_xir_value_drop(&value);
            printf("capacity normal instance%u %s true\n",instance,names[i]);
        }
        static const char *const failures[]={"negativeNew","negativeReserve"};
        for(unsigned i=0;i<2;++i){
            printf("capacity negative instance%u %s expect422\n",instance,failures[i]);
            XrXirValue value=capacity_run(current,capacity_find(run->module,failures[i]));
            CHECK(value.type==XR_XIR_I64&&value.payload==422);xr_xir_value_drop(&value);
        }
    }
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(second)==XR_XIR_CALL_READY);
    CHECK(!runtime_live&&!runtime_bytes);
    for(unsigned i=0;i<2;++i){
        XrXirInstance *current=capacity_open(run);
        CHECK(xr_xir_instance_start(current,capacity_find(run->module,"overflow"),NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(current,UINT64_MAX).outcome.status==XR_XIR_CALL_LIMIT);
        XrXirValue output={0};
        CHECK(xr_xir_instance_take_result(current,&output)==XR_XIR_CALL_LIMIT);
        CHECK(!output.type&&!output.reserved&&!output.payload);
        XrXirCallResult cache={0},before=cache;
        CHECK(xr_xir_instance_copy_failure(current,&cache)==XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(&cache,&before,sizeof(cache)));
        CHECK(xr_xir_instance_free(current)==XR_XIR_CALL_LIMIT);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    puts("capacity bounds/alias/currentRoot/once12/generic/negative422/overflowLIMIT dual physical0");
}
static void capacity_escape(CapacityCompile *run) {
    XrXirInstance *first=capacity_open(run),*second=capacity_open(run);
    XrXirValue values[2]={capacity_run(first,capacity_find(run->module,"text")),
        capacity_run(second,capacity_find(run->module,"text"))};
    xr_xir_compile_artifact_free(run->lowered);run->lowered=NULL;
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(second)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program);run->program=NULL;run->module=NULL;
    CHECK(runtime_live&&runtime_bytes);
    for(unsigned n=0;n<2;++n){
        XrXirValueAdmission admission={xr_xir_value_arena(&values[n]),object_pointer(&values[n])->domain,
            NULL,NULL,100000,1048576};
        int64_t count=-1,cap=-1;
        CHECK(xr_xir_array_len(&values[n],&admission,&count)==XR_XIR_VALUE_OK&&count==2);
        CHECK(xr_xir_array_capacity(&values[n],&admission,&cap)==XR_XIR_VALUE_OK&&cap>=9&&cap>=count);
        static const char *const expected[]={"a\0中","b\0c"};
        static const size_t sizes[]={5,3};
        for(int64_t i=0;i<2;++i){
            XrXirValue item={0};XrXirFaultDetail fault={0};const char *bytes=NULL;size_t length=0;
            CHECK(xr_xir_array_get(&values[n],i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_string_view(&item,&bytes,&length)&&length==sizes[i]&&!memcmp(bytes,expected[i],length));
            xr_xir_value_drop(&item);
        }
    }
    xr_xir_value_drop(&values[1]);CHECK(runtime_live&&runtime_bytes);
    xr_xir_value_drop(&values[0]);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(capacity_stats(&run->context).live_bytes==run->baseline.live_bytes);
    puts("capacity escaped Array<String> cap>=9 exact NUL UTF8 after two Instances/Program drop physical0");
}
#endif // XIR_ARRAY_CAPACITY_CASES_H
