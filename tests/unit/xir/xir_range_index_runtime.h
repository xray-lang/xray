/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_range_index_runtime.h - Scalar bounds execution without Range representation assumptions
 */
#ifndef XIR_RANGE_INDEX_RUNTIME_H
#define XIR_RANGE_INDEX_RUNTIME_H
static const struct {int64_t index,length;} range_index_inputs[]={
    {0,1},{1,2},{INT64_MAX-1,INT64_MAX},{0,INT64_MAX},
    {INT64_MIN,0},{-1,3},{0,0},{3,3},{INT64_MAX,INT64_MAX},
    {0,-1},{INT64_MIN,INT64_MIN},{INT64_MAX,-1}
};
static void range_index_runtime(XrXirProgram *program,const uint32_t entries[2]) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    for(size_t i=0;i<sizeof(range_index_inputs)/sizeof(*range_index_inputs);++i)for(unsigned caught=0;caught<2;++caught) {
        int64_t index=range_index_inputs[i].index,length=range_index_inputs[i].length;
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        XrXirValue args[]={{XR_XIR_I64,0,index},{XR_XIR_I64,0,length}};
        CHECK(xr_xir_instance_start(instance,entries[caught],args,2)==XR_XIR_CALL_READY);
        XrXirCallResult outcome=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome;
        if(length<0)CHECK(outcome.status==XR_XIR_CALL_BAD_STATE && !outcome.panic.detail.code);
        else if(!caught && (index<0 || index>=length)) {
            CHECK(outcome.status==XR_XIR_CALL_BOUNDS && outcome.panic.detail.code==430 &&
                !outcome.panic.detail.reserved && outcome.panic.detail.index==index && outcome.panic.detail.length==length);
        } else {
            CHECK(outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
            int64_t expected=index<0 || index>=length?430:index;
            CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==expected);
            xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance)==(length<0?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_READY));
    }
}
#endif // XIR_RANGE_INDEX_RUNTIME_H
