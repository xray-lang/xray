/* Fixed typed output and escaped owned field checks. */
#ifndef TUPLE_DESTRUCTURE_CASES_H
#define TUPLE_DESTRUCTURE_CASES_H
#include "xir/xxir_tuple.h"
static void destructure_string(const XrXirValue *value,const char *expected) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length==strlen(expected) && !memcmp(bytes,expected,length));
}
static XrXirOutputStatus destructure_output(void *context,const XrXirOutputGroup *group) {
    unsigned *groups=context;
    CHECK(group && group->stream==XR_XIR_STDOUT && group->line && group->count==8 && !*groups);
    destructure_string(&group->values[0],"destructure");destructure_string(&group->values[1],"owned中");
    const int64_t expected[]={1,4,44,7};
    for(uint32_t i=0;i<4;++i)CHECK(group->values[i+2].type==XR_XIR_I64 && group->values[i+2].payload==expected[i]);
    destructure_string(&group->values[6],"singleton");
    CHECK(group->values[7].type==XR_XIR_I64 && group->values[7].payload==6);
    ++*groups;return XR_XIR_OUTPUT_OK;
}
static inline void destructure_retained(XrXirValue *result) {
    XrXirValue text={0},array={0},nested={0},number={0},unit={0},single={0},atomic={0};
    CHECK(xr_xir_tuple_get(result,0,&text)==XR_XIR_VALUE_OK);destructure_string(&text,"owned中");
    CHECK(xr_xir_tuple_get(result,1,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(result,2,&nested)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(result,3,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==1);
    xr_xir_value_drop(&number);
    CHECK(xr_xir_tuple_get(result,4,&unit)==XR_XIR_VALUE_OK && !unit.type && !unit.payload);
    CHECK(xr_xir_tuple_get(result,5,&single)==XR_XIR_VALUE_OK);destructure_string(&single,"singleton");
    CHECK(xr_xir_tuple_get(result,6,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==6);
    xr_xir_value_drop(&number);
    CHECK(xr_xir_tuple_get(result,7,&atomic)==XR_XIR_VALUE_OK && xr_xir_value_valid(&atomic));
    xr_xir_value_drop(result);
    destructure_string(&text,"owned中");destructure_string(&single,"singleton");
    XrXirValueAdmission admission={xr_xir_value_arena(&array),NULL,NULL,NULL,64,0};
    XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&array,0,&admission,&number,&fault)==XR_XIR_VALUE_OK && xr_xir_fault_empty(fault) && number.type==XR_XIR_I64 && number.payload==44);
    xr_xir_value_drop(&number);
    CHECK(xr_xir_array_get(&array,1,&admission,&number,&fault)==XR_XIR_VALUE_OK && xr_xir_fault_empty(fault) && number.type==XR_XIR_I64 && number.payload==5);
    xr_xir_value_drop(&number);
    CHECK(xr_xir_tuple_get(&nested,0,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==7);
    xr_xir_value_drop(&number);
    XrXirValue nested_text={0};CHECK(xr_xir_tuple_get(&nested,1,&nested_text)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&nested);destructure_string(&nested_text,"nested");xr_xir_value_drop(&nested_text);
    CHECK(xr_xir_value_valid(&atomic));
    xr_xir_value_drop(&atomic);xr_xir_value_drop(&single);xr_xir_value_drop(&array);xr_xir_value_drop(&text);
}
static void destructure_runtime_faults(XrXirProgram *program,uint32_t entry) {
    size_t base_live=runtime_live,base_bytes=runtime_bytes,sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirInstanceConfig config;XrXirInstance *instance=NULL;XrXirValue result={0};unsigned groups=0;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,destructure_output,&groups};
        runtime_attempts=0;runtime_fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
        if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        if(status==XR_XIR_CALL_RETURNED)CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED && groups==1);
        size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_CALL_RETURNED);sites=attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_CALL_OOM && attempts>=pass && !result.type);
        if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        xr_xir_value_drop(&result);CHECK(runtime_live==base_live && runtime_bytes==base_bytes);
    }
    printf("Tuple destructure full local runtime OOM ordinals=%zu; output fixed; owner baseline restored PASS\n",sites);
}
#endif // TUPLE_DESTRUCTURE_CASES_H
