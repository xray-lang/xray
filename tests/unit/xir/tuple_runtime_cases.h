/* Fixed typed output and actual allocation ordinals, shared by two backends. */
#ifndef TUPLE_RUNTIME_CASES_H
#define TUPLE_RUNTIME_CASES_H
#include "xir/xxir_tuple.h"
static XrXirOutputStatus tuple_output(void *context,const XrXirOutputGroup *group) {
    unsigned *groups=context;const char *text=NULL;size_t length=0;
    CHECK(group && group->stream==XR_XIR_STDOUT && group->line && group->count==3 && !*groups);
    CHECK(xr_xir_string_view(&group->values[0],&text,&length) && length==5 && !memcmp(text,"tuple",5));
    CHECK(group->values[1].type==XR_XIR_I64 && group->values[1].payload==11);
    CHECK(group->values[2].type==XR_XIR_I64 && group->values[2].payload==7);
    ++*groups;return XR_XIR_OUTPUT_OK;
}
static void tuple_runtime_faults(XrXirProgram *program,uint32_t entry) {
    size_t base_live=runtime_live,base_bytes=runtime_bytes,sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirInstanceConfig config;XrXirInstance *instance=NULL;XrXirValue result={0};unsigned groups=0;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,tuple_output,&groups};
        runtime_attempts=0;runtime_fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,entry,NULL,0);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        if (status==XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
            CHECK(groups==1 && xr_xir_tuple_signature(xr_xir_compile_type_arena_types(xr_xir_value_arena(&result)),(XrXirType)result.type));
        }
        size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if (!pass) {CHECK(status==XR_XIR_CALL_RETURNED);sites=attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_CALL_OOM && attempts>=pass && !result.type);
        if (instance) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        xr_xir_value_drop(&result);CHECK(runtime_live==base_live && runtime_bytes==base_bytes);
    }
    printf("Tuple complete runtime actual OOM ordinals=%zu, typed output fixed oracle, baseline physical restored PASS\n",sites);
}
#endif
