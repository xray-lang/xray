/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_cell_borrow_cases.h - Actual Cell loans at Generator demand
 */
#ifndef XIR_GENERATOR_CELL_BORROW_CASES_H
#define XIR_GENERATOR_CELL_BORROW_CASES_H
#include "xir/xxir_generator_internal.h"
#include "xir/xxir_task_internal.h"
static const XrXirCallEntry *borrow_selected[64];
static bool borrow_native[64],borrow_negative,borrow_explicit_root;
static uint32_t borrow_callbacks[2],borrow_demands,borrow_producers,borrow_loans;
static XrXirValue borrow_iterator,borrow_cell;
static const XrXirProgram *borrow_program_owner;
static XirGenerator *borrow_object(void) {
    CHECK(borrow_iterator.type && xr_xir_value_valid(&borrow_iterator));
    XirGenerator *generator=(XirGenerator *)(uintptr_t)borrow_iterator.payload;
    CHECK(generator->object.kind==XR_XIR_TYPE_ITERATOR);return generator;
}
static void borrow_retired(void) {
    const XirGenerator *generator=borrow_object();
    CHECK(atomic_load_explicit(&generator->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL);
    CHECK(!generator->call && !generator->executor && !generator->activation.call);
    CHECK(!generator->activation.queued && !generator->activation.ready_next);
    CHECK(generator->activation.entry!=UINT32_MAX && generator->producer!=UINT32_MAX);
}
static void borrow_origin_refused(XrXirCallView *view,const XrXirValue *function,XrXirCallStatus expected) {
    XirEffectProducerView output;unsigned char before[sizeof(output)];memset(&output,0xa5,sizeof(output));
    memcpy(before,&output,sizeof(output));
    CHECK(xr_xir_instance_function_producer(view,function,&output)==expected);
    CHECK(!memcmp(&output,before,sizeof(output)));
}
static void borrow_conversion_refused(XrXirCallView *view,const XrXirValue *function,
    XrXirType type,XrXirCallStatus expected) {
    XrXirValue output={0};size_t attempts=runtime_attempts;
    CHECK(xr_xir_instance_weaken_function(view,type,function,&output)==expected);
    CHECK(!output.type && !output.reserved && !output.payload && runtime_attempts==attempts);
}
static void borrow_origin_attacks(XrXirCallView *view,const XrXirValue *function,const XirEffectProducerView *origin) {
    XirFunctionProducer *producer=(XirFunctionProducer *)(uintptr_t)xr_xir_function_producer(function);
    CHECK(producer);XirFunctionProducer saved=*producer;
    for(uint32_t attack=0;attack<4;++attack) {
        if(!attack)producer->owner=NULL;
        else if(attack==1)producer->function=UINT32_MAX;
        else if(attack==2)producer->instruction=UINT32_MAX;
        else producer->site=UINT32_MAX;
        XrXirCallStatus expected=attack==1?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_BAD_ARGUMENT;
        borrow_origin_refused(view,function,expected);
        if(attack)borrow_conversion_refused(view,function,(XrXirType)function->type,expected);
        *producer=saved;XirEffectProducerView restored={0};
        CHECK(xr_xir_instance_function_producer(view,function,&restored)==XR_XIR_CALL_READY);
        CHECK(restored.site==origin->site && restored.function==origin->function && restored.instruction==origin->instruction &&
            restored.target==origin->target && restored.capture_count==origin->capture_count && restored.type==origin->type);
    }
    borrow_conversion_refused(view,function,origin->type,XR_XIR_CALL_BAD_ARGUMENT);
}
static void borrow_origin_budget(XrXirCallView *view,const XrXirValue *function,uint64_t exact) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);CHECK(admission && exact>1);
    for(uint32_t less=0;less<2;++less) {
        uint64_t cap=exact-less;CHECK(admission->work>=cap);uint64_t tail=admission->work-cap;
        admission->work=cap;XirEffectProducerView output;unsigned char before[sizeof(output)];
        memset(&output,0xa5,sizeof(output));memcpy(before,&output,sizeof(output));
        XrXirCallStatus status=xr_xir_instance_function_producer(view,function,&output);
        CHECK(admission->work<=cap);admission->work+=tail;
        CHECK(status==(less?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
        if(less)CHECK(!memcmp(&output,before,sizeof(output)));
    }
    printf("OWNED_CELL_WEAKEN origin work exact=%llu/minus1, four identity attacks, bad-origin conversion, strengthening refused PASS\n",
        (unsigned long long)exact);
}
static void borrow_conversion_budget(XrXirCallView *view,const XrXirValue *function) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);CHECK(admission);
    uint64_t before=admission->work;XrXirDomainBudgetStats domain=xr_xir_domain_budget_stats(admission->domain);
    CHECK(admit_instance_value(admission,function,(XrXirType)function->type)==XR_XIR_CALL_READY);
    uint64_t value_call=before-admission->work,value_domain=xr_xir_domain_budget_stats(admission->domain).work-domain.work;
    before=admission->work;domain=xr_xir_domain_budget_stats(admission->domain);
    CHECK(xr_xir_instance_argument_weakening_admit(view,(XrXirType)function->type,function)==XR_XIR_CALL_READY);
    uint64_t converted_call=before-admission->work,converted_domain=xr_xir_domain_budget_stats(admission->domain).work-domain.work;
    const XrXirFunctionBinding *binding=xr_xir_function_binding(function);CHECK(binding && binding->capture_count==1);
    before=admission->work;domain=xr_xir_domain_budget_stats(admission->domain);
    CHECK(admission->function(admission->context,binding,(XrXirType)function->type,&admission->work)==XR_XIR_VALUE_OK);
    uint64_t gate_call=before-admission->work,gate_domain=xr_xir_domain_budget_stats(admission->domain).work-domain.work;
    CHECK(converted_call>=value_call && converted_domain>=value_domain);
    printf("OWNED_CELL_WEAKEN relation Call=%llu Domain=%llu, real binding gate Call=%llu Domain=%llu\n",
        (unsigned long long)(converted_call-value_call),(unsigned long long)(converted_domain-value_domain),
        (unsigned long long)gate_call,(unsigned long long)gate_domain);
    CHECK(converted_call-value_call==1 && converted_domain-value_domain==1 && gate_call==2 && gate_domain==2);
}
static void borrow_weakened_origin(XrXirCallView *view,const XrXirValue *function) {
    const XrXirTypes *types=xr_xir_compile_type_arena_types(view->arena);
    const XrXirTypeNode *signature=xr_xir_callable_signature(types,(XrXirType)function->type);
    CHECK(signature && (signature->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    XrXirValueAdmission *admission=xr_xir_call_admission(view);CHECK(admission);
    uint64_t before=admission->work;XirEffectProducerView producer={0};
    XrXirCallStatus status=xr_xir_instance_function_producer(view,function,&producer);
    uint64_t exact=before-admission->work;
    printf("OWNED_CELL_WEAKEN current UNKNOWN carrier producer_status=%u private_origin=%u\n",status,
        xr_xir_function_producer(function)?1u:0u);
    CHECK(status==XR_XIR_CALL_READY);
    const XrXirTypeNode *origin=xr_xir_callable_signature(types,producer.type);
    CHECK(origin && producer.type!=(XrXirType)function->type &&
        (origin->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_NONE);
    borrow_origin_attacks(view,function,&producer);borrow_origin_budget(view,function,exact);
    borrow_conversion_budget(view,function);
    CHECK((signature->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
}
/* These reads borrow the actual producer's root arguments and current consumer
 * view. Only the normal executor and Call driver own execution and loans. */
static void borrow_demand(XrXirCallView *view,const XrXirAction *action) {
    CHECK(!borrow_iterator.type && !borrow_cell.type);
    CHECK(xr_xir_value_copy(&action->value,&borrow_iterator)==XR_XIR_VALUE_OK);
    XirGenerator *generator=borrow_object();bool child=false;
    CHECK(xr_xir_task_executor_view_member(generator->executor,view,&child) &&
        child==(!borrow_explicit_root || borrow_negative));
    XirGeneratorCaptureView captures={0};
    CHECK(xr_xir_call_generator_captures(generator->call,xr_xir_call_admission(view),&captures)==XR_XIR_CALL_READY);
    CHECK(captures.entry==generator->activation.entry && captures.count==1 && captures.values);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&captures.values[0]);
    CHECK(binding && binding->capture_count==1 && binding->captures);
    if(borrow_explicit_root && !borrow_negative)borrow_weakened_origin(view,&captures.values[0]);
    const XrXirValue *cell=&binding->captures[0];
    CHECK(xr_xir_type_is_cell(xr_xir_compile_type_arena_types(view->arena),(XrXirType)cell->type));
    CHECK(!xr_xir_cell_is_projection(cell) && !xr_xir_cell_module_storage(cell));
    CHECK(xr_xir_value_copy(cell,&borrow_cell)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_unborrowed(cell)==!borrow_negative);
    if(borrow_negative) {
        CHECK(view->argument_count==2 && xr_xir_cell_same_owner(cell,&view->arguments[0]));
        XrXirValue value={0};CHECK(xr_xir_call_cell_read(view,&view->arguments[0],&value)==XR_XIR_VALUE_OK);
        CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);++borrow_loans;
    }
    ++borrow_demands;
}
static void borrow_producer(XrXirCallView *view,uint32_t entry) {
    if(!(borrow_selected[entry]->flags&XR_XIR_ENTRY_GENERATOR))return;
    CHECK(!borrow_negative && borrow_demands==1 && view->argument_count==1);
    XirGenerator *generator=borrow_object();bool child=false;
    CHECK(generator->call==view->activation && generator->activation.entry==entry &&
        xr_xir_task_executor_view_member(generator->executor,view,&child) &&
        child==!borrow_explicit_root && generator->child==!borrow_explicit_root);
    const XirEffectInvocationSelection *context=xr_xir_call_invocation(view);
    if(borrow_explicit_root)CHECK(!context);
    else CHECK(borrow_program_owner && borrow_program_owner->permissions && context &&
        context->owner==borrow_program_owner->permissions->effects && context->target==entry &&
        context->producer==generator->producer && context->node!=UINT32_MAX && context->edge!=UINT32_MAX);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&view->arguments[0]);
    CHECK(binding && binding->capture_count==1 && xr_xir_cell_same_owner(&borrow_cell,&binding->captures[0]));
    CHECK(xr_xir_cell_unborrowed(&borrow_cell));++borrow_producers;
}
static XrXirAction borrow_resume(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);CHECK(entry<64 && borrow_selected[entry]);
    ++borrow_callbacks[borrow_native[entry]?1:0];borrow_producer(view,entry);
    XrXirAction action=borrow_selected[entry]->resume(view);
    if(action.kind==XR_XIR_ACTION_AWAIT_ITERATOR && action.callee!=XR_XIR_WAIT_ITERATOR_CLOSE)borrow_demand(view,&action);
    return action;
}
#endif
