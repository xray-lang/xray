/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_terminal_cases.h - Genuine retired capture authentication
 */
#ifndef XIR_GENERATOR_TERMINAL_CASES_H
#define XIR_GENERATOR_TERMINAL_CASES_H
#include "xir/xxir_generator_internal.h"
#include "xir/xxir_task_internal.h"
static const XrXirCallEntry *terminal_selected[64];
static bool terminal_native[64];
static uint32_t terminal_steps[2],terminal_attack,terminal_mutations,terminal_reads;
static uint32_t terminal_saved_entry,terminal_saved_producer;
static XrXirValue terminal_iterator;
static bool terminal_require_function_worker;
static const XrXirProgram *terminal_execution_program;
static uint32_t terminal_function_worker_demands,terminal_function_worker_steps;
static XirGenerator *terminal_object(const XrXirValue *value) {
    CHECK(value && xr_xir_value_valid(value));
    XirGenerator *generator=(XirGenerator *)(uintptr_t)value->payload;
    CHECK(generator->object.kind==XR_XIR_TYPE_ITERATOR);return generator;
}
static void terminal_retired(const XrXirValue *value) {
    const XirGenerator *generator=terminal_object(value);
    CHECK(atomic_load_explicit(&generator->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL);
    CHECK(!generator->call && !generator->executor && !generator->activation.call);
    CHECK(!generator->activation.queued && !generator->activation.ready_next);
    CHECK(generator->activation.entry!=UINT32_MAX && generator->producer!=UINT32_MAX);
}
/* These observations use the real running callback and existing executor
 * membership. They neither construct a view nor grant execution authority. */
static void terminal_function_worker(XrXirCallView *view,uint32_t entry) {
    if(!terminal_require_function_worker || !(terminal_selected[entry]->flags&XR_XIR_ENTRY_GENERATOR))return;
    CHECK(terminal_iterator.type && view->argument_count==1 &&
        xr_xir_callable_signature(xr_xir_compile_type_arena_types(view->arena),(XrXirType)view->arguments[0].type));
    const XirGenerator *generator=terminal_object(&terminal_iterator);bool child=false;
    CHECK(generator->call==view->activation && generator->activation.entry==entry &&
        xr_xir_task_executor_view_member(generator->executor,view,&child) && child && generator->child);
    const XirEffectInvocationSelection *context=xr_xir_call_invocation(view);
    CHECK(terminal_execution_program && terminal_execution_program->permissions && context &&
        context->owner==terminal_execution_program->permissions->effects && context->target==entry && context->producer==generator->producer &&
        context->node!=UINT32_MAX && context->edge!=UINT32_MAX);++terminal_function_worker_steps;
}
static XrXirAction terminal_resume(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);CHECK(entry<64 && terminal_selected[entry]);
    ++terminal_steps[terminal_native[entry]?1:0];terminal_function_worker(view,entry);
    if(terminal_iterator.type && !terminal_reads) {
        XirGenerator *generator=terminal_object(&terminal_iterator);
        if(atomic_load_explicit(&generator->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL) {
            terminal_retired(&terminal_iterator);++terminal_reads;
            terminal_saved_entry=generator->activation.entry;terminal_saved_producer=generator->producer;
            if(terminal_attack==1)generator->activation.entry=UINT32_MAX;
            if(terminal_attack==2)generator->producer=UINT32_MAX;
            if(terminal_attack==3)atomic_store_explicit(&generator->state,XIR_GENERATOR_PARKED,memory_order_release);
            if(terminal_attack)++terminal_mutations;
        }
    }
    XrXirAction action=terminal_selected[entry]->resume(view);
    if(action.kind==XR_XIR_ACTION_AWAIT_ITERATOR && action.callee!=XR_XIR_WAIT_ITERATOR_CLOSE && !terminal_iterator.type) {
        CHECK(xr_xir_value_copy(&action.value,&terminal_iterator)==XR_XIR_VALUE_OK);
        if(terminal_require_function_worker) {
            bool child=false;const XirGenerator *generator=terminal_object(&terminal_iterator);
            CHECK(!(terminal_selected[entry]->flags&XR_XIR_ENTRY_GENERATOR) &&
                xr_xir_task_executor_view_member(generator->executor,view,&child) && child);
            ++terminal_function_worker_demands;
        }
    }
    return action;
}
static void terminal_execute(XrXirProgram *program,uint32_t entry,uint32_t attack) {
    terminal_attack=attack;terminal_mutations=terminal_reads=0;CHECK(!terminal_iterator.type);
    terminal_function_worker_demands=terminal_function_worker_steps=0;terminal_execution_program=program;
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,1000000).outcome.status;
    XrXirCallStatus expected=attack?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_RETURNED;
    if(status!=expected)fprintf(stderr,"terminal entry%u attack%u status%u expected%u reads%u mutations%u\n",entry,attack,status,expected,terminal_reads,terminal_mutations);
    CHECK(status==expected && terminal_reads==1 && terminal_mutations==(attack?1u:0u));
    if(terminal_require_function_worker) {
        CHECK(terminal_function_worker_demands==1 && terminal_function_worker_steps);
        printf("WORKER_FN_EXECUTION entry%u demand%u producer_steps%u returned17 PASS\n",entry,
            terminal_function_worker_demands,terminal_function_worker_steps);
    }
    if(!attack) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
    } else {
        XirGenerator *generator=(XirGenerator *)(uintptr_t)terminal_iterator.payload;
        generator->activation.entry=terminal_saved_entry;generator->producer=terminal_saved_producer;
        atomic_store_explicit(&generator->state,XIR_GENERATOR_TERMINAL,memory_order_release);
    }
    terminal_retired(&terminal_iterator);xr_xir_value_drop(&terminal_iterator);
    CHECK(xr_xir_instance_free(instance)==(attack?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_READY));
    terminal_execution_program=NULL;
    printf("TERMINAL_CAPTURE entry%u attack%u: real retired object, status%u PASS\n",entry,attack,status);
}
#endif
