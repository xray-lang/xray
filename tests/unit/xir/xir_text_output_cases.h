/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_text_output_cases.h - Independent typed output and owned UTF8 expectations
 *
 * KEY CONCEPT:
 *   Both backends execute real text operations before one typed provider and
 *   a byte sink validate independent fixed expectations.
 */
#ifndef XIR_TEXT_OUTPUT_CASES_H
#define XIR_TEXT_OUTPUT_CASES_H
#include "xir/xxir_output.h"
#include "xir/xxir_panic.h"
typedef struct TextOutputState {
    XrXirInstance *instance;
    unsigned groups, writes;
    char bytes[64];
    size_t length;
    XrXirValue escaped;
} TextOutputState;
static void text_value(const XrXirValue *value,const char *bytes,size_t count) {
    const char *actual=NULL;size_t length=0;
    CHECK(value->type==XR_XIR_STRING && !value->reserved);
    CHECK(xr_xir_string_view(value,&actual,&length) && length==count);
    CHECK(!count || !memcmp(actual,bytes,count));
}
static XrXirOutputStatus text_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t count) {
    TextOutputState *state=context;
    CHECK(stream==XR_XIR_STDOUT && count<=sizeof(state->bytes)-state->length);
    memcpy(state->bytes+state->length,bytes,count);state->length+=count;++state->writes;
    return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus text_group(void *context,const XrXirOutputGroup *group) {
    TextOutputState *state=context;
    CHECK(state && state->instance && group && group->stream==XR_XIR_STDOUT && group->line);
    CHECK(xr_xir_instance_free(state->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_cancel_current(state->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_start(state->instance,UINT32_MAX,NULL,0)==XR_XIR_CALL_BUSY);
    if(!state->groups) {
        CHECK(group->count==6);
        text_value(&group->values[0],"-5",2);
        CHECK(group->values[1].type==XR_XIR_BOOL && !group->values[1].reserved && group->values[1].payload==1);
        text_value(&group->values[2],"abcd",4);
        CHECK(group->values[3].type==XR_XIR_RUNE && !group->values[3].reserved && group->values[3].payload==0x1f600);
        CHECK(group->values[4].type==XR_XIR_BOOL && !group->values[4].reserved && group->values[4].payload==1);
        text_value(&group->values[5],"abcd",4);
        CHECK(xr_xir_value_copy(&group->values[2],&state->escaped)==XR_XIR_VALUE_OK);
    } else {
        CHECK(state->groups<3 && group->count==1);
        text_value(&group->values[0],state->groups==1?"abcd":"",state->groups==1?4:0);
    }
    ++state->groups;
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,text_bytes,state,sizeof(state->bytes)};
    return xr_xir_output_render(&sink,group);
}
static void text_config(XrXirInstanceConfig *config,TextOutputState *state) {
    CHECK(xr_xir_instance_config_init(config,sizeof(*config))==XR_XIR_CALL_READY);
    config->metadata_limit=UINT64_C(1)*1024*1024;config->value_limit=UINT64_C(1)*1024*1024;
    config->call_limit=UINT64_C(1)*1024*1024;config->poll_limit=1000000;config->depth_limit=64;
    config->output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,text_group,state};
}
#include "xir_text_rune_runtime.h"
static void text_output_pair(XrXirProgram *program,const uint32_t *ids,bool write_stdout) {
    CHECK(program && !runtime_live && !runtime_bytes);
    uint32_t entry=ids[0];
    text_nul_faults(program,ids[3]);
    TextOutputState states[2]={0};XrXirInstance *instances[2]={0};
    XrXirValue nul_escaped[2]={{0},{0}};
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;text_config(&config,&states[i]);
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY && instances[i]);
        states[i].instance=instances[i];
    }
    xr_xir_compile_program_drop(program);program=NULL;
    static const char expected[]="-5 true abcd \xf0\x9f\x98\x80 true abcd\nabcd\n\n";
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result;
        do {result=xr_xir_instance_poll_bounded(instances[i],16);} while(result.outcome.status==XR_XIR_CALL_READY);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && !result.outcome.wake);
        CHECK(xr_xir_panic_empty(&result.outcome.panic));
        CHECK(states[i].groups==3 && states[i].writes==3 && states[i].length==sizeof(expected)-1);
        CHECK(!memcmp(states[i].bytes,expected,sizeof(expected)-1));
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==0);xr_xir_value_drop(&value);
        nul_escaped[i]=text_rune_runtime(instances[i],ids);
        size_t blocks=runtime_live,bytes=runtime_bytes;
        CHECK(xr_xir_instance_stop(instances[i])==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_state(instances[i])==XR_XIR_INSTANCE_DRAINING);
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_BAD_STATE);
        CHECK(runtime_live==blocks && runtime_bytes==bytes);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);instances[i]=NULL;states[i].instance=NULL;
    }
    /* An owned output snapshot survives both execution owners and code owners. */
    for(unsigned i=0;i<2;++i) {
        text_value(&states[i].escaped,"abcd",4);xr_xir_value_drop(&states[i].escaped);
        text_value(&nul_escaped[i],"\0",1);xr_xir_value_drop(&nul_escaped[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    if(write_stdout)CHECK(fwrite(states[0].bytes,1,states[0].length,stdout)==states[0].length);
}
#endif // XIR_TEXT_OUTPUT_CASES_H
