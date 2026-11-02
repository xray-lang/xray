/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * class_inheritance_cases.h - Independent output and physical receiver lifetime checks
 *
 * KEY CONCEPT:
 *   A bound virtual method owns its original derived receiver after the local
 *   binding and its producing frame retire. Observation never grants execution.
 */
#ifndef CLASS_INHERITANCE_CASES_H
#define CLASS_INHERITANCE_CASES_H
#include "xir/xxir_program.h"
#include "xir/xxir_value.h"
#include "xir/xxir_output.h"
#include "../cell_owner_value_physical.h"

typedef struct ClassInheritancePlan {
    uint32_t entry, dog, animal, dog_type;
    int64_t answer;
    bool lifetime,hidden;
} ClassInheritancePlan;
typedef struct ClassInheritanceOutput { char bytes[1024];size_t length; } ClassInheritanceOutput;
static const char class_inheritance_expected[]=
    "Cat says meow!\n"
    "Rex (Labrador) says woof!\n"
    "Rex fetches the ball\n"
    "count: 3\n"
    "  Parrot says squawk!\n"
    "  Buddy (Golden) says woof!\n";
static const char class_inheritance_cold_expected[]="cold receiver passed\n";
static const char class_inheritance_hidden_expected[]="hidden receiver passed\n";
static void class_inheritance_golden(void) {
    char bytes[sizeof(class_inheritance_expected)];
    FILE *file=fopen(XR_CLASS_INHERITANCE_GOLDEN,"rb");CHECK(file);
    size_t length=fread(bytes,1,sizeof(bytes),file);
    CHECK(length==121 && fgetc(file)==EOF && !ferror(file) && !fclose(file));
    CHECK(sizeof(class_inheritance_expected)-1==length && !memcmp(bytes,class_inheritance_expected,length));
}
static XrXirOutputStatus class_inheritance_output(void *opaque,const XrXirOutputGroup *group) {
    ClassInheritanceOutput *output=opaque;const char *bytes=NULL;size_t length=0;
    if(!group || group->stream!=XR_XIR_STDOUT || !group->line || group->count!=1 ||
        !xr_xir_string_view(group->values,&bytes,&length) ||
        length>=sizeof(output->bytes)-output->length)return XR_XIR_OUTPUT_ERROR;
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;
    output->bytes[output->length++]='\n';return XR_XIR_OUTPUT_OK;
}
static void class_inheritance_observed(XrXirClassObservation *observation,const ClassInheritancePlan *plan) {
    XrXirClassEvent events[128];uint32_t count=0;uint64_t cursor=0;bool overflow=false;
    CHECK(xr_xir_class_observation_drain(observation,&cursor,events,128,&count,&overflow)==XR_XIR_VALUE_OK);
    CHECK(!overflow && count==6);
    uint64_t serials[2]={0,0};uint32_t phases[2]={0,0};
    for(uint32_t i=0;i<count;++i) {
        const XrXirClassEvent *event=events+i;uint32_t kind=event->declaration==plan->dog?0:1;
        CHECK(event->declaration==(kind?plan->animal:plan->dog) && event->serial);
        CHECK(!event->flags && !event->initialized_fields && event->status==XR_XIR_VALUE_OK);
        if(!kind)CHECK(event->type==plan->dog_type);
        if(event->kind==XR_XIR_CLASS_CONSTRUCT) {
            CHECK(!serials[kind] && !phases[kind]);serials[kind]=event->serial;
        }
        CHECK(event->serial==serials[kind] && event->kind==phases[kind]+1);
        phases[kind]=event->kind;
    }
    CHECK(serials[0] && serials[1] && serials[0]!=serials[1]);
    CHECK(phases[0]==XR_XIR_CLASS_RECLAIM && phases[1]==XR_XIR_CLASS_RECLAIM);
}
static void class_inheritance_execute(XrXirProgram *program,const ClassInheritancePlan *plan) {
    size_t before_live=cell_owner_live,before_bytes=cell_owner_bytes;
    ClassInheritanceOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,class_inheritance_output,&output};
    XrXirInstance *instance=NULL;XrXirClassObservation *observation=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    if(plan->lifetime)CHECK(xr_xir_instance_class_observation_open(instance,128,&observation)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,plan->entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);status=xr_xir_instance_poll_bounded(instance,64).outcome.status;
    }
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==plan->answer);xr_xir_value_drop(&value);
    const char *expected=plan->hidden?class_inheritance_hidden_expected:
        plan->lifetime?class_inheritance_cold_expected:class_inheritance_expected;
    CHECK(output.length==strlen(expected) && !memcmp(output.bytes,expected,output.length));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    if(observation) {class_inheritance_observed(observation,plan);xr_xir_class_observation_drop(&observation);}
    CHECK(cell_owner_live==before_live && cell_owner_bytes==before_bytes);
}
#endif // CLASS_INHERITANCE_CASES_H
