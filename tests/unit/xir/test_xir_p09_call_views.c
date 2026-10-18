/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_call_views.c - Real frame loans, scoped views and terminal revocation
 *
 * KEY CONCEPT:
 *   No test callback supplies view authority; the actual driver owns every scope.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_call_views_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#undef TRY_VALUE
#define TRY_VALUE(operation) do {status=(operation);if(status!=XR_XIR_VALUE_OK) { \
    if(status==XR_XIR_VALUE_BAD_ARGUMENT) fprintf(stderr,"Call views line %d: %s BAD_ARGUMENT\n",__LINE__,#operation); \
    goto done;}} while(0)

enum {CALL_VIEWS_SHARED,CALL_VIEWS_MUT_ESCAPE,CALL_VIEWS_CANCEL,CALL_VIEWS_PANIC};
typedef struct CallViewsState {XrXirValue slots[8];uint32_t pc;} CallViewsState;
typedef struct CallViewsWitness {
    XrXirValue escaped[2];XrXirViewOriginPin physical;XrXirViewAccess saved;
    XrXirCallAccounting *accounting;
    XrXirCallView *releasing_view;
    uint32_t mode,entered,child,exits,releases,parent_reads,reentry_checks;XrXirValueStatus failure;
} CallViewsWitness;
static void call_views_binding_release(void *context) {
    CallViewsWitness *w=context;if (!w->releasing_view) return;
    XrXirCallView *view=w->releasing_view;CallViewsState *state=view->state;
    XrXirViewAccess denied={0};XrXirAction action={.kind=XR_XIR_ACTION_CONTINUE};
    CHECK(xr_xir_call_view_access(view,&denied)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_new(view,(XrXirType)258,0,0,1,&state->slots[7])==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_drop(view,&state->slots[7])==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_return_move(view,&state->slots[7],&action)==XR_XIR_CALL_BUSY);
    ++w->reentry_checks;
}
static XrXirValueStatus call_views_binding_admit(void *context,const XrXirFunctionBinding *binding,
    XrXirType type,uint64_t *work) {
    if (!work || !*work) return XR_XIR_VALUE_LIMIT;
    --*work;return binding && binding->owner==context && binding->release==call_views_binding_release &&
        binding->entry==2 && !binding->capture_count && !binding->captures && type==(XrXirType)264 ?
        XR_XIR_VALUE_OK:XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus call_views_operation(XrXirCallView *view) {
    CallViewsState *state=view->state;CallViewsWitness *w=(CallViewsWitness *)view->environment;
    XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirViewAccess access={0};XrXirValue outside={0},number={XR_XIR_I64,0,77};
    XrXirFaultDetail fault={0};int64_t length=0;XrXirCallView copied=*view;
    CHECK(xr_xir_call_view_access(&copied,&access)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_new(&copied,(XrXirType)259,0,0,4,&state->slots[0])==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_new(view,(XrXirType)259,0,0,4,&outside)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_new(view,(XrXirType)259,2,0,4,&state->slots[0])==XR_XIR_VALUE_BAD_ARGUMENT);
    TRY_VALUE(xr_xir_call_view_access(view,&access));w->saved=access;
    uint64_t baseline=w->accounting->live_bytes;
    for (uint32_t iteration=0;iteration<8;++iteration) {
        TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)259,0,0,4,&state->slots[0]));
        uint64_t work=access.values->work;access.values->work=0;XrXirValue before=state->slots[0];
        CHECK(xr_xir_call_view_drop(view,&state->slots[0])==XR_XIR_VALUE_LIMIT && state->slots[0].payload==before.payload);
        access.values->work=work;TRY_VALUE(xr_xir_call_view_drop(view,&state->slots[0]));
        CHECK(w->accounting->live_bytes==baseline);
    }
    TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)259,0,0,4,&state->slots[0]));
    TRY_VALUE(xr_xir_view_set(&state->slots[0],0,&number,&access,&fault));
    TRY_VALUE(xr_xir_call_view_reborrow(view,(XrXirType)259,&state->slots[0],1,2,&state->slots[1]));
    CHECK(xr_xir_view_len(&state->slots[0],&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    number.payload=88;TRY_VALUE(xr_xir_view_set(&state->slots[1],0,&number,&access,&fault));
    if (w->mode==CALL_VIEWS_MUT_ESCAPE) {
        TRY_VALUE(xr_xir_value_move(&state->slots[1],&w->escaped[1]));
        TRY_VALUE(xr_xir_call_view_drop(view,&state->slots[0]));
        TRY_VALUE(xr_xir_view_get(&w->escaped[1],0,&access,&outside,&fault));CHECK(outside.payload==88);
        xr_xir_value_drop(&outside);return XR_XIR_VALUE_OK;
    }
    TRY_VALUE(xr_xir_call_view_drop(view,&state->slots[1]));
    TRY_VALUE(xr_xir_view_len(&state->slots[0],&access,&length));CHECK(length==4);
    TRY_VALUE(xr_xir_call_view_reborrow(view,(XrXirType)258,&state->slots[0],0,3,&state->slots[1]));
    CHECK(xr_xir_view_set(&state->slots[0],0,&number,&access,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_reborrow(view,(XrXirType)259,&state->slots[1],0,1,&state->slots[3])==XR_XIR_VALUE_BAD_ARGUMENT);
    TRY_VALUE(xr_xir_value_copy(&state->slots[1],&w->escaped[0]));
    TRY_VALUE(xr_xir_view_origin_pin(&state->slots[1],&access,&w->physical));
    TRY_VALUE(xr_xir_borrowed_some_new_move((XrXirType)260,&state->slots[1],&access,&state->slots[2]));
    TRY_VALUE(xr_xir_borrowed_value_admit(&state->slots[2],(XrXirType)260,&access));
    TRY_VALUE(xr_xir_borrowed_some_take(&state->slots[2],&access,&state->slots[1]));
    XrXirFunctionBinding binding={w,call_views_binding_release,2,NULL,0};
    TRY_VALUE(xr_xir_function_new(access.values->domain,(XrXirTypeArena *)access.values->arena,
        (XrXirType)264,&binding,access.values,&state->slots[2]));
    TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)265,&state->slots[1],2,&access,&state->slots[6]));
    w->releasing_view=view;
    status=xr_xir_call_view_drop(view,&state->slots[6]);w->releasing_view=NULL;
    if (status!=XR_XIR_VALUE_OK) goto done;
    CHECK(w->reentry_checks==1);
    TRY_VALUE(xr_xir_call_view_drop(view,&state->slots[0]));
    TRY_VALUE(xr_xir_view_get(&w->escaped[0],0,&access,&outside,&fault));CHECK(outside.payload==77);
    xr_xir_value_drop(&outside);
    TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)262,1,0,3,&state->slots[4]));
    TRY_VALUE(xr_xir_view_get(&state->slots[4],1,&access,&outside,&fault));CHECK(outside.payload==0xc3);
    xr_xir_value_drop(&outside);
    CHECK(xr_xir_call_view_new(view,(XrXirType)263,1,0,1,&state->slots[5])==XR_XIR_VALUE_BAD_ARGUMENT);
done:
    xr_xir_value_drop(&outside);return status;
}
static XrXirAction call_views_resume(XrXirCallView *view) {
    CallViewsState *state=view->state;CallViewsWitness *w=(CallViewsWitness *)view->environment;
    if (view->phase==XR_XIR_CALL_EXIT) {
        ++w->exits;
        if (w->failure==XR_XIR_VALUE_OK && w->escaped[0].type) {
            XrXirViewAccess access={0};int64_t length=0;
            CHECK(xr_xir_call_view_access(view,&access)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_view_len(&w->escaped[0],&access,&length)==XR_XIR_VALUE_OK && length==3);
        }
        return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    if (!state->pc++) {
        ++w->entered;w->failure=call_views_operation(view);
        if (w->failure!=XR_XIR_VALUE_OK) return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,
            .value={XR_XIR_I64,0,w->failure==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_LIMIT}};
        return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=2};
    }
    if (state->pc==2) return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    int64_t length=0;const XrXirValue *escaped=&w->escaped[w->mode==CALL_VIEWS_MUT_ESCAPE?1:0];
    CHECK(xr_xir_view_len(escaped,&w->saved,&length)==XR_XIR_VALUE_OK && length==(w->mode==CALL_VIEWS_MUT_ESCAPE?2:3));
    if (w->mode==CALL_VIEWS_PANIC) return xr_xir_call_fault(XR_XIR_RUN_DIVIDE_BY_ZERO);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,91}};
}
static XrXirAction call_views_child(XrXirCallView *view) {
    CallViewsWitness *w=(CallViewsWitness *)view->environment;XrXirViewAccess access={0};int64_t length=0;
    ++w->child;CHECK(xr_xir_call_view_access(view,&access)==XR_XIR_VALUE_OK);
    const XrXirValue *escaped=&w->escaped[w->mode==CALL_VIEWS_MUT_ESCAPE?1:0];
    CHECK(xr_xir_view_len(escaped,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static void call_views_release(XrXirCallView *view,XrXirCallStatus reason) {
    CallViewsWitness *w=(CallViewsWitness *)view->environment;CallViewsState *state=view->state;(void)reason;
    XrXirViewAccess denied={0};CHECK(xr_xir_call_view_access(view,&denied)==XR_XIR_VALUE_BAD_ARGUMENT);
    ++w->releases;for (uint32_t i=0;i<8;++i) xr_xir_value_drop(&state->slots[i]);
}
static XrXirAction call_views_parent(XrXirCallView *view) {
    CallViewsWitness *w=(CallViewsWitness *)view->environment;uint32_t *pc=view->state;
    if (!(*pc)++) return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=1,.arguments=view->arguments,
        .argument_count=2,.flags=XR_XIR_ACTION_PROTECTED};
    CHECK(view->inbox.status==(w->mode==CALL_VIEWS_PANIC?XR_XIR_CALL_DIVIDE_BY_ZERO:XR_XIR_CALL_RETURNED));
    XrXirValue array={0},element={0};XrXirFaultDetail fault={0};int64_t length=0;
    const XrXirValue *old=&w->escaped[w->mode==CALL_VIEWS_MUT_ESCAPE?1:0];
    CHECK(xr_xir_view_len(old,&w->saved,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirValueStatus status=xr_xir_call_cell_read(view,&view->arguments[0],&array);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_array_get(&array,0,xr_xir_call_admission(view),&element,&fault);
    if (status==XR_XIR_VALUE_OK) {CHECK(element.payload==77);++w->parent_reads;}
    xr_xir_value_drop(&array);xr_xir_value_drop(&element);
    if (status!=XR_XIR_VALUE_OK) return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,
        .value={XR_XIR_I64,0,status==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_LIMIT}};
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,91}};
}
static XrXirCellParameterRole call_views_role(void *context,uint32_t entry,uint32_t parameter) {
    (void)context;return entry<2 && parameter<2 ? XR_XIR_CELL_ROLE_SCOPED_REF : XR_XIR_CELL_ROLE_UNKNOWN;
}
static XrXirValueStatus call_views_arena(XrCompileResources *resources,XrXirTypeArena **arena) {
    const XrXirCallableParameter fields[]={{(XrXirType)258,0},{(XrXirType)264,0}};
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)258},{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8},{.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2}};
    const XrXirTypes types={nodes,10,NULL,NULL};
    const XrXirCompileContext c={resources,xr_xir_compile_default_limits()};
    return xr_xir_compile_type_arena_new(&c,&types,arena);
}
static XrXirValueStatus call_views_sequence(XrCompileResources *resources,uint32_t mode,bool nested) {
    XrXirDomain *domain=NULL;XrXirTypeArena *arena=NULL;XrXirValue values[5]={{0}},after={0};
    XrXirCall *call=NULL;CallViewsWitness witness={.mode=mode};XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirCallAccounting accounting={0};XrXirCallBudget budget={.byte_limit=65536,.requested_limit=65536,.resume_limit=64};
    witness.accounting=&accounting;
    const XrXirDomainBudgetControls controls={65536,65536,1000000,65536,65536};
    TRY_VALUE(call_views_arena(resources,&arena));TRY_VALUE(xr_xir_compile_domain_new(resources,65536,&domain));
    TRY_VALUE(xr_xir_domain_budget_bind(domain,&controls));budget.work_domain=domain;
    XrXirValueAdmission admission={arena,domain,call_views_binding_admit,&witness,1000000,65536};
    const XrXirValue items[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22},{XR_XIR_I64,0,33},{XR_XIR_I64,0,44}};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,items,4,&admission,&values[0]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,&values[0],&admission,&values[1]));
    TRY_VALUE(xr_xir_string_new(domain,"h\xc3\xa9",3,&values[2]));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)261,&values[2],&admission,&values[3]));
    XrXirValue arguments[]={values[1],values[3]};const XrXirType parameters[]={(XrXirType)257,(XrXirType)261};
    XrXirCallEntry entries[]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=parameters,.parameter_count=2,.result=XR_XIR_I64,
         .state_bytes=sizeof(CallViewsState),.resume=call_views_resume,.release=call_views_release,
         .environment=&witness,.flags=XR_XIR_ENTRY_EXIT},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=parameters,.parameter_count=2,.result=XR_XIR_I64,
         .state_bytes=sizeof(CallViewsState),.resume=call_views_resume,.release=call_views_release,
         .environment=&witness,.flags=XR_XIR_ENTRY_EXIT},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.resume=call_views_child,.environment=&witness}};
    if (nested) {entries[0].resume=call_views_parent;entries[0].release=NULL;entries[0].state_bytes=sizeof(uint32_t);entries[0].flags=0;}
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries;config.entry_count=3;config.accounting=&accounting;config.admission=admission;config.byte_limit=65536;
    const XrXirCallRequest request={0,arguments,2,call_views_role,NULL};
    XrXirCallStatus cs=xr_xir_call_new_budgeted(&config,&request,&budget,&call);
    if (cs!=XR_XIR_CALL_READY) {status=cs==XR_XIR_CALL_OOM?XR_XIR_VALUE_OOM:XR_XIR_VALUE_LIMIT;goto done;}
    bool cancelled=false;uint32_t polls=0;
    do {
        XrXirCallResult result=xr_xir_call_poll_bounded(call,1);cs=result.status;CHECK(++polls<32);
        if (witness.saved.authorize && witness.escaped[0].type) {int64_t length=0;
            CHECK(xr_xir_view_len(&witness.escaped[0],&witness.saved,&length)==XR_XIR_VALUE_BAD_ARGUMENT);}
        if (cs==XR_XIR_CALL_SUSPENDED) {
            if (mode==CALL_VIEWS_CANCEL && !cancelled) {
                CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);cancelled=true;
            } else CHECK(xr_xir_call_resume(call,result.wake)==XR_XIR_CALL_READY);
            cs=XR_XIR_CALL_READY;
        }
    } while (cs==XR_XIR_CALL_READY);
    if (cs==XR_XIR_CALL_OOM || cs==XR_XIR_CALL_LIMIT) {
        status=cs==XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT;goto done;
    }
    status=witness.failure;
    if (status!=XR_XIR_VALUE_OK) {CHECK(cs==XR_XIR_CALL_OOM || cs==XR_XIR_CALL_LIMIT);goto done;}
    CHECK(cs==(mode==CALL_VIEWS_CANCEL?XR_XIR_CALL_CANCELLED:mode==CALL_VIEWS_PANIC && !nested?XR_XIR_CALL_DIVIDE_BY_ZERO:XR_XIR_CALL_RETURNED));
    CHECK(witness.releases==1 && witness.exits==1);
    CHECK(witness.parent_reads==(nested && mode!=CALL_VIEWS_CANCEL ? 1u:0u));
    if (cs==XR_XIR_CALL_RETURNED) {CHECK(xr_xir_call_take_result(call,&after)==cs && after.payload==91);xr_xir_value_drop(&after);}
    CHECK(xr_xir_cell_unborrowed(&values[1]) && xr_xir_cell_unborrowed(&values[3]));
    XrXirFaultDetail fault={0};
    TRY_VALUE(xr_xir_array_get(&values[0],0,&admission,&after,&fault));CHECK(after.payload==11);xr_xir_value_drop(&after);
    if (witness.physical.body) {
        XrXirViewOriginInfo info={0};TRY_VALUE(xr_xir_view_origin_info(&witness.physical,&admission,&info));CHECK(!info.logical_active);
    }
    TRY_VALUE(xr_xir_array_new((XrXirType)256,NULL,0,&admission,&values[4]));
    TRY_VALUE(xr_xir_cell_write(&values[1],&values[4],&admission));
    for (uint32_t i=0;i<2;++i) if (witness.escaped[i].type) CHECK(xr_xir_value_valid(&witness.escaped[i]));
done:
    if (call) CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
    size_t before=value_compile_calls,runtime_before=calls;
    xr_xir_view_origin_unpin(&witness.physical);
    for (uint32_t i=0;i<2;++i) xr_xir_value_drop(&witness.escaped[i]);
    for (uint32_t i=0;i<5;++i) xr_xir_value_drop(&values[i]);xr_xir_value_drop(&after);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(before==value_compile_calls && runtime_before==calls && !live);return status;
}
static void call_views_resources(uint32_t mode,bool nested) {
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);size_t start=value_compile_calls;
    CHECK(call_views_sequence(resources,mode,nested)==XR_XIR_VALUE_OK);size_t points=value_compile_calls-start;
    XrCompileResourceStats measured=compile_domain_stats(resources);
    CHECK(call_views_sequence(resources,mode,nested)==XR_XIR_VALUE_OK);xr_compile_resources_release(resources);resources=NULL;
    CHECK(!value_compile_live && !value_compile_bytes && !live && points>10 && points<128);
    for (size_t point=0;point<points;++point) {
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        value_compile_injected=false;value_compile_fail_at=value_compile_calls+point;
        CHECK(call_views_sequence(resources,mode,nested)==XR_XIR_VALUE_OOM && value_compile_injected);
        value_compile_fail_at=SIZE_MAX;xr_compile_resources_release(resources);resources=NULL;
        CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
        XrCompileResourceLimits bound=limits;
        if (!axis) bound.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) bound.live_bytes=measured.peak_bytes-minus;
        else bound.work=measured.work-minus;
        CHECK(xr_compile_resources_new(&bound,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(call_views_sequence(resources,mode,nested)==(minus?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OK));
        xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    printf("Call views mode=%u nested=%u: %zu complete allocation failures, three axes/same owner/physical0 PASS\n",mode,nested,points);
}
int main(void) {
    call_views_resources(CALL_VIEWS_SHARED,false);call_views_resources(CALL_VIEWS_MUT_ESCAPE,false);
    call_views_resources(CALL_VIEWS_SHARED,true);call_views_resources(CALL_VIEWS_MUT_ESCAPE,true);
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(call_views_sequence(resources,CALL_VIEWS_CANCEL,false)==XR_XIR_VALUE_OK);
    CHECK(call_views_sequence(resources,CALL_VIEWS_PANIC,false)==XR_XIR_VALUE_OK);
    CHECK(call_views_sequence(resources,CALL_VIEWS_CANCEL,true)==XR_XIR_VALUE_OK);
    CHECK(call_views_sequence(resources,CALL_VIEWS_PANIC,true)==XR_XIR_VALUE_OK);xr_compile_resources_release(resources);
    CHECK(!value_compile_live && !value_compile_bytes && !live);
    puts("actual Call view roots/reborrow/moves/aggregate/resume/child isolation/exit/revocation/late owner PASS");return 0;
}
