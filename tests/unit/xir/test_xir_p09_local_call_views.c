/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_local_call_views.c - Actual local Cell roots and frame retirement
 *
 * KEY CONCEPT:
 *   A native state owns its local values. Only the real driver scope grants
 *   view access; copied handles cannot bypass a committed parameter loan.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_local_call_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#undef TRY_VALUE
#define TRY_VALUE(operation) do {status=(operation);if(status!=XR_XIR_VALUE_OK) goto done;} while(0)

enum {LOCAL_CALL_SHARED,LOCAL_CALL_MUT,LOCAL_CALL_STRING,LOCAL_CALL_REF,LOCAL_CALL_MODULE,LOCAL_CALL_RELEASE};
enum {LOCAL_CALL_RETURN,LOCAL_CALL_CANCEL,LOCAL_CALL_PANIC};
typedef struct LocalCallState {XrXirValue slots[6];uint32_t pc;} LocalCallState;
typedef struct LocalCallWitness {
    XrXirValue source,escaped;XrXirViewAccess access;XrXirViewOriginPin pin;
    XrXirCallAccounting *accounting;XrXirValueStatus failure;
    uint32_t kind,finish,entered,released,exited,children,collection_reentries;
    XrXirCallView *collecting;
} LocalCallWitness;

static void local_call_binding_release(void *context) {
    LocalCallWitness *w=context;if (!w->collecting) return;
    XrXirCallView *view=w->collecting;LocalCallState *s=view->state;
    XrXirViewAccess denied={0};XrXirAction result={0};
    CHECK(xr_xir_call_view_access(view,&denied)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_local(view,(XrXirType)258,&s->slots[0],0,1,&s->slots[4])==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_drop(view,&s->slots[2])==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_return_move(view,&s->slots[4],&result)==XR_XIR_CALL_BUSY);
    ++w->collection_reentries;
}
static XrXirValueStatus local_call_binding_admit(void *context,const XrXirFunctionBinding *binding,
    XrXirType type,uint64_t *work) {
    if (!work || !*work) return XR_XIR_VALUE_LIMIT;
    --*work;return binding && binding->owner==context && binding->release==local_call_binding_release &&
        binding->entry==1 && !binding->capture_count && !binding->captures && type==(XrXirType)263 ?
        XR_XIR_VALUE_OK:XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus local_call_collect_reentry(XrXirCallView *view) {
    LocalCallState *s=view->state;LocalCallWitness *w=(LocalCallWitness *)view->environment;
    XrXirValueAdmission *a=xr_xir_call_admission(view);XrXirValueStatus status=XR_XIR_VALUE_OK;
    const XrXirFunctionBinding binding={w,local_call_binding_release,1,NULL,0};
    TRY_VALUE(xr_xir_call_view_local(view,(XrXirType)258,&s->slots[0],0,2,&s->slots[2]));
    TRY_VALUE(xr_xir_function_new(a->domain,(XrXirTypeArena *)a->arena,(XrXirType)263,&binding,a,&s->slots[3]));
    TRY_VALUE(xr_xir_array_new((XrXirType)264,&s->slots[3],1,a,&s->slots[4]));
    TRY_VALUE(xr_xir_cell_new(a->domain,(XrXirTypeArena *)a->arena,(XrXirType)265,&s->slots[4],a,&s->slots[5]));
    xr_xir_value_drop(&s->slots[3]);xr_xir_value_drop(&s->slots[4]);
    TRY_VALUE(xr_xir_call_view_local(view,(XrXirType)266,&s->slots[5],0,1,&s->slots[3]));
    xr_xir_value_drop(&s->slots[5]);xr_xir_value_drop(&s->slots[3]);
    w->collecting=view;
    status=xr_xir_call_view_local(view,(XrXirType)258,&s->slots[0],0,2,&s->slots[1]);
    w->collecting=NULL;if (status!=XR_XIR_VALUE_OK) goto done;
    CHECK(w->collection_reentries==1);
    TRY_VALUE(xr_xir_call_view_drop(view,&s->slots[1]));TRY_VALUE(xr_xir_call_view_drop(view,&s->slots[2]));
done:
    return status;
}
static XrXirValueStatus local_call_create(XrXirCallView *view,XrXirType type,
    const XrXirValue *root,int64_t offset,int64_t length,XrXirValue *output) {
    XrXirValue before={0},after={0};
    bool snapshot=xr_xir_cell_unborrowed(root) && !xr_xir_cell_module_storage(root) &&
        !xr_xir_cell_is_projection(root);
    if (snapshot) CHECK(xr_xir_cell_read(root,&before)==XR_XIR_VALUE_OK);
    XrXirValueStatus status=xr_xir_call_view_local(view,type,root,offset,length,output);
    if (snapshot && status!=XR_XIR_VALUE_OK) {
        CHECK(xr_xir_cell_read(root,&after)==XR_XIR_VALUE_OK);
        CHECK(before.type==after.type && before.reserved==after.reserved && before.payload==after.payload);
    }
    xr_xir_value_drop(&before);xr_xir_value_drop(&after);return status;
}
static XrXirValueStatus local_call_operation(XrXirCallView *view) {
    LocalCallState *s=view->state;LocalCallWitness *w=(LocalCallWitness *)view->environment;
    XrXirValueAdmission *a=xr_xir_call_admission(view);XrXirValueStatus status=XR_XIR_VALUE_OK;
    bool string=w->kind==LOCAL_CALL_STRING;XrXirType shared=(XrXirType)(string?261:258);
    XrXirFaultDetail fault={0};XrXirValue outside={0},number={XR_XIR_I64,0,42};
    if (w->kind==LOCAL_CALL_REF) TRY_VALUE(xr_xir_value_copy(&view->arguments[0],&s->slots[0]));
    else TRY_VALUE(xr_xir_cell_new(a->domain,(XrXirTypeArena *)a->arena,
        (XrXirType)(string?260:257),&w->source,a,&s->slots[0]));
    if (w->kind==LOCAL_CALL_MODULE) {
        const XrXirCellPublication published={w,a->domain,0};
        TRY_VALUE(xr_xir_cell_publication_prepare(&s->slots[0],&published));
        xr_xir_cell_publication_commit(&s->slots[0],&published);
    }
    TRY_VALUE(xr_xir_call_view_access(view,&w->access));
    XrXirCallView copied=*view;
    CHECK(xr_xir_call_view_local(&copied,shared,&s->slots[0],0,2,&s->slots[1])==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_local(view,shared,&s->slots[0],0,2,&outside)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirValue borrowed=s->slots[0];
    CHECK(xr_xir_call_view_local(view,shared,&borrowed,0,2,&s->slots[1])==XR_XIR_VALUE_BAD_ARGUMENT);
    s->slots[5]=number;
    CHECK(xr_xir_call_view_local(view,shared,&s->slots[5],0,2,&s->slots[1])==XR_XIR_VALUE_BAD_ARGUMENT);
    s->slots[5]=(XrXirValue){0};
    if (w->kind==LOCAL_CALL_REF || w->kind==LOCAL_CALL_MODULE) {
        status=local_call_create(view,shared,&s->slots[0],0,2,&s->slots[1]);
        if (status==XR_XIR_VALUE_OOM || status==XR_XIR_VALUE_LIMIT) goto done;
        CHECK(status==XR_XIR_VALUE_BAD_ARGUMENT && !s->slots[1].type);status=XR_XIR_VALUE_OK;goto done;
    }
    uint64_t baseline=w->accounting->live_bytes;
    for (uint32_t n=0;n<8;++n) {
        TRY_VALUE(local_call_create(view,shared,&s->slots[0],0,2,&s->slots[1]));
        TRY_VALUE(local_call_create(view,shared,&s->slots[0],0,2,&s->slots[2]));
        TRY_VALUE(xr_xir_call_view_drop(view,&s->slots[1]));
        TRY_VALUE(xr_xir_view_get(&s->slots[2],1,&w->access,&outside,&fault));
        CHECK(outside.payload==(string?0xc3u:22u));xr_xir_value_drop(&outside);
        TRY_VALUE(xr_xir_call_view_drop(view,&s->slots[2]));
        CHECK(w->accounting->live_bytes==baseline);
    }
    if (w->kind==LOCAL_CALL_RELEASE) TRY_VALUE(local_call_collect_reentry(view));
    if (string) {
        CHECK(xr_xir_call_view_local(view,(XrXirType)262,&s->slots[0],0,2,&s->slots[1])==XR_XIR_VALUE_BAD_ARGUMENT);
        TRY_VALUE(local_call_create(view,shared,&s->slots[0],0,3,&s->slots[1]));
        TRY_VALUE(xr_xir_value_copy(&s->slots[1],&w->escaped));
    } else {
        TRY_VALUE(local_call_create(view,(XrXirType)259,&s->slots[0],0,2,&s->slots[1]));
        TRY_VALUE(xr_xir_view_set(&s->slots[1],0,&number,&w->access,&fault));
        if (w->kind==LOCAL_CALL_MUT) TRY_VALUE(xr_xir_value_move(&s->slots[1],&w->escaped));
        else {
            TRY_VALUE(xr_xir_call_view_reborrow(view,shared,&s->slots[1],0,2,&s->slots[2]));
            TRY_VALUE(xr_xir_value_copy(&s->slots[2],&w->escaped));
        }
    }
    TRY_VALUE(xr_xir_view_origin_pin(&w->escaped,&w->access,&w->pin));
done:
    xr_xir_value_drop(&outside);return status;
}
static XrXirAction local_call_resume(XrXirCallView *view) {
    LocalCallState *s=view->state;LocalCallWitness *w=(LocalCallWitness *)view->environment;
    if (view->phase==XR_XIR_CALL_EXIT) {
        ++w->exited;
        CHECK(xr_xir_call_view_local(view,(XrXirType)258,&s->slots[0],0,1,&s->slots[4])==XR_XIR_VALUE_BAD_ARGUMENT);
        if (w->failure==XR_XIR_VALUE_OK && w->escaped.type) {
            int64_t length=0;CHECK(xr_xir_view_len(&w->escaped,&w->access,&length)==XR_XIR_VALUE_OK);
        }
        return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    if (!s->pc++) {
        ++w->entered;w->failure=local_call_operation(view);
        if (w->failure!=XR_XIR_VALUE_OK) {
            CHECK(w->failure==XR_XIR_VALUE_OOM || w->failure==XR_XIR_VALUE_LIMIT);
            return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,
                .value={XR_XIR_I64,0,w->failure==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_LIMIT}};
        }
        return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=1};
    }
    if (s->pc==2) return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    if (w->escaped.type) {int64_t length=0;
        CHECK(xr_xir_view_len(&w->escaped,&w->access,&length)==XR_XIR_VALUE_OK);}
    return w->finish==LOCAL_CALL_PANIC ? xr_xir_call_fault(XR_XIR_RUN_DIVIDE_BY_ZERO) :
        (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,91}};
}
static XrXirAction local_call_child(XrXirCallView *view) {
    LocalCallState *s=view->state;LocalCallWitness *w=(LocalCallWitness *)view->environment;
    XrXirViewAccess access={0};int64_t length=0;++w->children;
    CHECK(xr_xir_call_view_access(view,&access)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_call_view_local(view,(XrXirType)258,&s->slots[0],0,1,&s->slots[1])==XR_XIR_VALUE_BAD_ARGUMENT);
    if (w->escaped.type) {
        CHECK(xr_xir_view_len(&w->escaped,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_view_len(&w->escaped,&w->access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    }
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static void local_call_release(XrXirCallView *view,XrXirCallStatus reason) {
    LocalCallState *s=view->state;LocalCallWitness *w=(LocalCallWitness *)view->environment;(void)reason;
    ++w->released;
    for (uint32_t n=0;n<6;++n) xr_xir_value_drop(&s->slots[n]);
}
static XrXirCellParameterRole local_call_role(void *context,uint32_t entry,uint32_t parameter) {
    (void)context;return !entry && !parameter ? XR_XIR_CELL_ROLE_SCOPED_REF : XR_XIR_CELL_ROLE_UNKNOWN;
}
static XrXirValueStatus local_call_arena(XrCompileResources *resources,XrXirTypeArena **arena) {
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_STRING},{.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)263},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)264},
        {.kind=XR_XIR_TYPE_SLICE,.element=(XrXirType)263},{.kind=XR_XIR_TYPE_MUT_SLICE,.element=(XrXirType)263}};
    const XrXirTypes types={nodes,12,NULL,NULL};
    const XrXirCompileContext c={resources,xr_xir_compile_default_limits()};
    return xr_xir_compile_type_arena_new(&c,&types,arena);
}
static XrXirValueStatus local_call_sequence(XrCompileResources *resources,uint32_t kind,uint32_t finish) {
    XrXirDomain *domain=NULL;XrXirTypeArena *arena=NULL;XrXirCall *call=NULL;XrXirValue root={0},result={0};
    XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirCallAccounting accounting={0};
    LocalCallWitness w={.kind=kind,.finish=finish,.accounting=&accounting};
    TRY_VALUE(local_call_arena(resources,&arena));TRY_VALUE(xr_xir_compile_domain_new(resources,65536,&domain));
    const XrXirDomainBudgetControls controls={65536,65536,1000000,65536,65536};
    TRY_VALUE(xr_xir_domain_budget_bind(domain,&controls));
    XrXirValueAdmission admission={arena,domain,local_call_binding_admit,&w,1000000,65536};
    const XrXirValue fields[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22}};
    if (kind==LOCAL_CALL_STRING) TRY_VALUE(xr_xir_string_new(domain,"h\xc3\xa9",3,&w.source));
    else TRY_VALUE(xr_xir_array_new((XrXirType)256,fields,2,&admission,&w.source));
    if (kind==LOCAL_CALL_REF) TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,&w.source,&admission,&root));
    const XrXirType parameter=(XrXirType)257;
    XrXirCallEntry entries[]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=kind==LOCAL_CALL_REF?&parameter:NULL,
         .parameter_count=kind==LOCAL_CALL_REF?1u:0u,.result=XR_XIR_I64,.state_bytes=sizeof(LocalCallState),
         .resume=local_call_resume,.release=local_call_release,.environment=&w,.flags=XR_XIR_ENTRY_EXIT},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_UNIT,.state_bytes=sizeof(LocalCallState),
         .resume=local_call_child,.environment=&w}};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries;config.entry_count=2;config.accounting=&accounting;config.admission=admission;config.byte_limit=65536;
    XrXirCallBudget budget={.byte_limit=65536,.requested_limit=65536,.resume_limit=64,.work_domain=domain};
    const XrXirCallRequest request={0,kind==LOCAL_CALL_REF?&root:NULL,kind==LOCAL_CALL_REF?1u:0u,
        kind==LOCAL_CALL_REF?local_call_role:NULL,NULL};
    XrXirCallStatus cs=xr_xir_call_new_budgeted(&config,&request,&budget,&call);
    if (cs!=XR_XIR_CALL_READY) {CHECK(cs==XR_XIR_CALL_OOM || cs==XR_XIR_CALL_LIMIT);
        status=cs==XR_XIR_CALL_OOM?XR_XIR_VALUE_OOM:XR_XIR_VALUE_LIMIT;goto done;}
    uint32_t polls=0;
    do {
        XrXirCallResult polled=xr_xir_call_poll_bounded(call,1);cs=polled.status;CHECK(++polls<32);
        if (w.escaped.type) {int64_t length=0;
            CHECK(xr_xir_view_len(&w.escaped,&w.access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);}
        if (cs==XR_XIR_CALL_SUSPENDED) {
            if (finish==LOCAL_CALL_CANCEL) CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);
            else CHECK(xr_xir_call_resume(call,polled.wake)==XR_XIR_CALL_READY);
            cs=XR_XIR_CALL_READY;
        }
    } while(cs==XR_XIR_CALL_READY);
    if (cs==XR_XIR_CALL_OOM || cs==XR_XIR_CALL_LIMIT) {
        status=cs==XR_XIR_CALL_OOM?XR_XIR_VALUE_OOM:XR_XIR_VALUE_LIMIT;goto done;
    }
    CHECK(w.failure==XR_XIR_VALUE_OK && w.entered==1 && w.released==1 && w.exited==1 && w.children==1);
    CHECK(cs==(finish==LOCAL_CALL_CANCEL?XR_XIR_CALL_CANCELLED:
        finish==LOCAL_CALL_PANIC?XR_XIR_CALL_DIVIDE_BY_ZERO:XR_XIR_CALL_RETURNED));
    if (cs==XR_XIR_CALL_RETURNED) {CHECK(xr_xir_call_take_result(call,&result)==cs && result.payload==91);xr_xir_value_drop(&result);}
    if (kind!=LOCAL_CALL_STRING) {XrXirFaultDetail fault={0};
        TRY_VALUE(xr_xir_array_get(&w.source,0,&admission,&result,&fault));CHECK(result.payload==11);xr_xir_value_drop(&result);}
    if (w.pin.body) {XrXirViewOriginInfo info={0};
        TRY_VALUE(xr_xir_view_origin_info(&w.pin,&admission,&info));CHECK(!info.logical_active);}
done:
    if (call) CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
    size_t allocations=value_compile_calls,raw=calls;
    xr_xir_value_drop(&root);xr_xir_value_drop(&w.source);xr_xir_value_drop(&result);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    xr_xir_view_origin_unpin(&w.pin);xr_xir_value_drop(&w.escaped);
    CHECK(allocations==value_compile_calls && raw==calls && !live);return status;
}
static void local_call_resources(uint32_t kind,uint32_t finish) {
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);size_t before=value_compile_calls;
    CHECK(local_call_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);size_t sites=value_compile_calls-before;
    XrCompileResourceStats exact=compile_domain_stats(resources);
    CHECK(local_call_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);xr_compile_resources_release(resources);resources=NULL;
    printf("Local Call baseline kind=%u finish=%u sites=%zu blocks=%zu bytes=%zu raw=%zu\n",
        kind,finish,sites,value_compile_live,value_compile_bytes,live);
    CHECK(!value_compile_live && !value_compile_bytes && !live);
    CHECK(sites && sites<256);
    for(size_t site=0;site<sites;++site) {
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        value_compile_injected=false;value_compile_fail_at=value_compile_calls+site;
        CHECK(local_call_sequence(resources,kind,finish)==XR_XIR_VALUE_OOM && value_compile_injected);
        value_compile_fail_at=SIZE_MAX;xr_compile_resources_release(resources);resources=NULL;
        CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    for(uint32_t axis=0;axis<3;++axis) for(uint32_t minus=0;minus<2;++minus) {
        XrCompileResourceLimits cap=limits;
        if(!axis)cap.allocated_bytes=exact.allocated_bytes-minus;
        else if(axis==1)cap.live_bytes=exact.peak_bytes-minus;
        else cap.work=exact.work-minus;
        CHECK(xr_compile_resources_new(&cap,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(local_call_sequence(resources,kind,finish)==(minus?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OK));
        xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    printf("Local Call kind=%u finish=%u: %zu complete allocation failures; same owner/three axes/physical0 PASS\n",kind,finish,sites);
}
int main(void) {
    for(uint32_t kind=0;kind<6;++kind) for(uint32_t finish=0;finish<3;++finish) local_call_resources(kind,finish);
    puts("actual local Call roots/COW/shared/Mut/string/ref-loan denial/scope retirement/late owner PASS");return 0;
}
