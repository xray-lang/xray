/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_role_cases.h - Actual consumer role before each producer resume
 */
#ifndef XIR_GENERATOR_ROLE_CASES_H
#define XIR_GENERATOR_ROLE_CASES_H
#include "xir/xxir_generator_internal.h"
static const XrXirCallEntry *role_selected[64];
static bool role_native[64],role_worker,role_flip;
static uint32_t role_steps[2],role_reads,role_attacks,role_caller;
static XrXirValue role_iterator;
static XrXirAction role_resume(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);CHECK(entry<64 && role_selected[entry]);
    ++role_steps[role_native[entry]?1:0];
    if(role_selected[entry]->flags&XR_XIR_ENTRY_GENERATOR) {
        CHECK(role_iterator.type);
        const XirGenerator *generator=(const XirGenerator *)(uintptr_t)role_iterator.payload;
        bool child=!role_worker;
        CHECK(generator->call==view->activation && xr_xir_task_executor_view_member(generator->executor,view,&child));
        CHECK(child==role_worker && generator->child==role_worker);++role_reads;
        if(role_worker) {
            const XirEffectInvocationSelection *context=xr_xir_call_invocation(view);
            CHECK(context && context->caller==role_caller && context->target==entry && context->producer==generator->producer);
        }
    }
    XrXirAction action=role_selected[entry]->resume(view);
    if(action.kind==XR_XIR_ACTION_AWAIT_ITERATOR && action.callee!=XR_XIR_WAIT_ITERATOR_CLOSE) {
        CHECK(!role_iterator.type);role_iterator=action.value;role_caller=entry;
        CHECK(!((const XirGenerator *)(uintptr_t)action.value.payload)->child);
        if(role_flip) {
            XirGenerator *generator=(XirGenerator *)(uintptr_t)action.value.payload;
            generator->child=!role_worker;++role_attacks;
        }
    }
    return action;
}
static void role_execute(XrXirProgram *program,uint32_t entry,bool worker,bool flip) {
    role_worker=worker;role_flip=flip;role_reads=role_attacks=0;role_iterator=(XrXirValue){0};
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,1000000).outcome.status;
    if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"consumer role worker%u flip%u status%u\n",worker,flip,status);
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(role_reads && role_attacks==(flip?1u:0u));
    printf("CONSUMER_ROLE worker%u staleTag%u: actual executor role, %u real producer callbacks, returned17 PASS\n",worker,flip,role_reads);
}
static void role_cases(XrXirProgram *program,const uint32_t entries[2]) {
    for(uint32_t worker=0;worker<2;++worker)for(uint32_t flip=0;flip<2;++flip)
        role_execute(program,entries[worker],worker!=0,flip!=0);
}
#endif
