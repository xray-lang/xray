/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_arguments.c - Actual cross-frame borrowed parameter owners
 *
 * KEY CONCEPT:
 *   Temporary child loans leave caller owners intact through every exit.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_view_arguments_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#undef TRY_VALUE
#define TRY_VALUE(operation) do {status=(operation);if(status!=XR_XIR_VALUE_OK) goto done;} while(0)
enum {ARG_SHARED,ARG_MUT,ARG_SOME_SHARED,ARG_SOME_MUT,ARG_NONE_SHARED,ARG_NONE_MUT,ARG_DAG,ARG_TUPLE_MUT,ARG_KINDS};
enum {ARG_RETURN,ARG_CANCEL,ARG_PANIC};
enum {ARG_DAG_DEPTH=70,ARG_DAG_BASE=263};
typedef struct ViewArgumentState {XrXirValue slots[5];uint32_t pc;} ViewArgumentState;
typedef struct ViewArgumentWitness {
    XrXirValue parent_leaf,late;XrXirViewAccess parent_access,child_access;XrXirViewOriginPin physical;
    XrXirValueStatus failure;uint32_t kind,finish,entered,children,exits,releases,restored;
} ViewArgumentWitness;
static XrXirType view_argument_type(uint32_t kind) {
    static const XrXirType types[]={258,259,260,261,260,261,ARG_DAG_BASE+ARG_DAG_DEPTH-1,262};
    return types[kind];
}
static XrXirValueStatus view_argument_prepare(XrXirCallView *view) {
    ViewArgumentState *s=view->state;ViewArgumentWitness *w=(ViewArgumentWitness *)view->environment;
    XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirViewAccess access={0};
    TRY_VALUE(xr_xir_call_view_access(view,&access));w->parent_access=access;
    if (w->kind==ARG_NONE_SHARED || w->kind==ARG_NONE_MUT) {
        TRY_VALUE(xr_xir_nullable_new(view_argument_type(w->kind),NULL,access.values,&s->slots[0]));
        goto done;
    }
    bool mutable=w->kind==ARG_MUT || w->kind==ARG_SOME_MUT || w->kind==ARG_TUPLE_MUT;
    TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)(mutable?259:258),0,0,4,&s->slots[0]));
    w->parent_leaf=s->slots[0];
    if (w->kind==ARG_SOME_SHARED || w->kind==ARG_SOME_MUT) {
        TRY_VALUE(xr_xir_borrowed_some_new_move(view_argument_type(w->kind),&s->slots[0],&access,&s->slots[1]));
        TRY_VALUE(xr_xir_value_move(&s->slots[1],&s->slots[0]));
    } else if (w->kind==ARG_TUPLE_MUT) {
        s->slots[1]=(XrXirValue){XR_XIR_I64,0,71};
        TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)262,s->slots,2,&access,&s->slots[2]));
        TRY_VALUE(xr_xir_value_move(&s->slots[2],&s->slots[0]));
    } else if (w->kind==ARG_DAG) for (uint32_t i=0;i<ARG_DAG_DEPTH;++i) {
        TRY_VALUE(xr_xir_value_copy(&s->slots[0],&s->slots[1]));
        TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)(ARG_DAG_BASE+i),s->slots,2,&access,&s->slots[2]));
        TRY_VALUE(xr_xir_value_move(&s->slots[2],&s->slots[0]));
    }
done:return status;
}
static XrXirValueStatus view_argument_inspect(XrXirCallView *view) {
    ViewArgumentState *s=view->state;ViewArgumentWitness *w=(ViewArgumentWitness *)view->environment;
    XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirViewAccess access={0};XrXirFaultDetail fault={0};int64_t length=0;
    TRY_VALUE(xr_xir_call_view_access(view,&access));w->child_access=access;
    if (w->parent_leaf.type) CHECK(xr_xir_view_len(&w->parent_leaf,&w->parent_access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    TRY_VALUE(xr_xir_call_view_argument_take(view,0,&s->slots[0]));
    CHECK(!view->arguments[0].type);
    CHECK(xr_xir_call_view_argument_take(view,0,&s->slots[1])==XR_XIR_VALUE_BAD_ARGUMENT);
    if (w->kind==ARG_NONE_SHARED || w->kind==ARG_NONE_MUT) {
        bool some=true;const XrXirValue *payload=NULL;
        CHECK(xr_xir_nullable_view(&s->slots[0],&some,&payload) && !some && !payload);goto done;
    }
    if (w->kind==ARG_SOME_SHARED || w->kind==ARG_SOME_MUT) {
        TRY_VALUE(xr_xir_borrowed_some_take(&s->slots[0],&access,&s->slots[1]));
        TRY_VALUE(xr_xir_value_move(&s->slots[1],&s->slots[0]));
    } else if (w->kind==ARG_TUPLE_MUT) {
        TRY_VALUE(xr_xir_borrowed_tuple_unpack_move(&s->slots[0],s->slots+1,2,&access));
        CHECK(s->slots[2].payload==71);xr_xir_value_drop(&s->slots[2]);
        TRY_VALUE(xr_xir_value_move(&s->slots[1],&s->slots[0]));
    } else if (w->kind==ARG_DAG) for (uint32_t i=0;i<ARG_DAG_DEPTH;++i) {
        TRY_VALUE(xr_xir_tuple_get(&s->slots[0],0,&s->slots[1]));
        xr_xir_value_drop(&s->slots[0]);TRY_VALUE(xr_xir_value_move(&s->slots[1],&s->slots[0]));
    }
    CHECK(s->slots[0].payload!=w->parent_leaf.payload);
    TRY_VALUE(xr_xir_view_len(&s->slots[0],&access,&length));CHECK(length==4);
    TRY_VALUE(xr_xir_view_origin_pin(&s->slots[0],&access,&w->physical));
    if (s->slots[0].type==259) {
        const XrXirValue item={XR_XIR_I64,0,99};TRY_VALUE(xr_xir_view_set(&s->slots[0],0,&item,&access,&fault));
        TRY_VALUE(xr_xir_value_move(&s->slots[0],&w->late));
    } else TRY_VALUE(xr_xir_value_copy(&s->slots[0],&w->late));
done:return status;
}
static XrXirAction view_argument_failure(XrXirValueStatus status) {
    return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,
        status==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:status==XR_XIR_VALUE_LIMIT?XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_ARGUMENT}};
}
static XrXirAction view_argument_child(XrXirCallView *view) {
    ViewArgumentState *s=view->state;ViewArgumentWitness *w=(ViewArgumentWitness *)view->environment;
    if (view->phase==XR_XIR_CALL_EXIT) {++w->exits;return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};}
    if (!s->pc++) {
        ++w->children;w->failure=view_argument_inspect(view);
        if (w->failure!=XR_XIR_VALUE_OK) return view_argument_failure(w->failure);
        return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    }
    if (w->finish==ARG_PANIC) return xr_xir_call_fault(XR_XIR_RUN_DIVIDE_BY_ZERO);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,17}};
}
static XrXirAction view_argument_parent(XrXirCallView *view) {
    ViewArgumentState *s=view->state;ViewArgumentWitness *w=(ViewArgumentWitness *)view->environment;
    if (view->phase==XR_XIR_CALL_EXIT) {++w->exits;return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};}
    if (!s->pc++) {
        ++w->entered;w->failure=view_argument_prepare(view);
        if (w->failure!=XR_XIR_VALUE_OK) return view_argument_failure(w->failure);
        return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=1,.arguments=s->slots,.argument_count=1,.flags=XR_XIR_ACTION_PROTECTED};
    }
    CHECK(view->inbox.status==(w->finish==ARG_PANIC?XR_XIR_CALL_DIVIDE_BY_ZERO:XR_XIR_CALL_RETURNED));
    if (w->parent_leaf.type) {
        int64_t length=0;XrXirValue item={0};XrXirFaultDetail fault={0};
        w->failure=xr_xir_view_len(&w->parent_leaf,&w->parent_access,&length);
        if (w->failure!=XR_XIR_VALUE_OK) return view_argument_failure(w->failure);
        CHECK(length==4 && xr_xir_view_len(&w->late,&w->child_access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
        w->failure=xr_xir_view_get(&w->parent_leaf,0,&w->parent_access,&item,&fault);
        if (w->failure!=XR_XIR_VALUE_OK) return view_argument_failure(w->failure);
        CHECK(item.payload==(w->parent_leaf.type==259?99:11));xr_xir_value_drop(&item);++w->restored;
    }
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,91}};
}
static void view_argument_release(XrXirCallView *view,XrXirCallStatus reason) {
    ViewArgumentWitness *w=(ViewArgumentWitness *)view->environment;ViewArgumentState *s=view->state;(void)reason;
    ++w->releases;for (uint32_t i=0;i<5;++i) xr_xir_value_drop(&s->slots[i]);
}
static XrXirCellParameterRole view_argument_role(void *context,uint32_t entry,uint32_t parameter) {
    (void)context;return !entry && !parameter ? XR_XIR_CELL_ROLE_SCOPED_REF:XR_XIR_CELL_ROLE_UNKNOWN;
}
static XrXirValueStatus view_argument_arena(XrCompileResources *resources,XrXirTypeArena **arena) {
    XrXirCallableParameter fields[ARG_DAG_DEPTH+1][2]={{{0}}};
    XrXirTypeNode nodes[7+ARG_DAG_DEPTH]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)258},{.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)259}};
    fields[0][0].type=(XrXirType)259;fields[0][1].type=XR_XIR_I64;
    nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[0],.parameter_count=2};
    for (uint32_t i=0;i<ARG_DAG_DEPTH;++i) {
        fields[i+1][0].type=fields[i+1][1].type=(XrXirType)(i?ARG_DAG_BASE+i-1:258);
        nodes[7+i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[i+1],.parameter_count=2};
    }
    const XrXirTypes types={nodes,7+ARG_DAG_DEPTH,NULL,NULL};
    const XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    return xr_xir_compile_type_arena_new(&context,&types,arena);
}
static XrXirValueStatus view_argument_sequence(XrCompileResources *resources,uint32_t kind,uint32_t finish) {
    XrXirDomain *domain=NULL;XrXirTypeArena *arena=NULL;XrXirValue values[3]={{0}};XrXirCall *call=NULL;
    ViewArgumentWitness witness={.kind=kind,.finish=finish};XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirCallAccounting accounting={0};
    XrXirCallBudget budget={.byte_limit=65536,.requested_limit=65536,.resume_limit=64};
    const XrXirDomainBudgetControls controls={1048576,1048576,4194304,65536,65536};
    TRY_VALUE(view_argument_arena(resources,&arena));TRY_VALUE(xr_xir_compile_domain_new(resources,1048576,&domain));
    TRY_VALUE(xr_xir_domain_budget_bind(domain,&controls));budget.work_domain=domain;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,4194304,65536};
    const XrXirValue items[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22},{XR_XIR_I64,0,33},{XR_XIR_I64,0,44}};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,items,4,&admission,values));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,values,&admission,values+1));
    const XrXirType root=(XrXirType)257,borrowed=view_argument_type(kind);
    const XrXirCallEntry entries[]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=&root,.parameter_count=1,.result=XR_XIR_I64,
         .state_bytes=sizeof(ViewArgumentState),.resume=view_argument_parent,.release=view_argument_release,.environment=&witness,.flags=XR_XIR_ENTRY_EXIT},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=&borrowed,.parameter_count=1,.result=XR_XIR_I64,
         .state_bytes=sizeof(ViewArgumentState),.resume=view_argument_child,.release=view_argument_release,.environment=&witness,.flags=XR_XIR_ENTRY_EXIT}};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries;config.entry_count=2;config.accounting=&accounting;config.admission=admission;config.byte_limit=65536;
    const XrXirCallRequest request={0,values+1,1,view_argument_role,NULL};
    XrXirCallStatus terminal=xr_xir_call_new_budgeted(&config,&request,&budget,&call);
    if (terminal!=XR_XIR_CALL_READY) {status=terminal==XR_XIR_CALL_OOM?XR_XIR_VALUE_OOM:XR_XIR_VALUE_LIMIT;goto done;}
    bool cancelled=false;uint32_t polls=0;
    do {
        XrXirCallResult result=xr_xir_call_poll_bounded(call,1);terminal=result.status;CHECK(++polls<32);
        if (terminal==XR_XIR_CALL_SUSPENDED) {
            if (finish==ARG_CANCEL && !cancelled) {CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);cancelled=true;}
            else CHECK(xr_xir_call_resume(call,result.wake)==XR_XIR_CALL_READY);
            terminal=XR_XIR_CALL_READY;
        }
    } while (terminal==XR_XIR_CALL_READY);
    if (terminal==XR_XIR_CALL_OOM || terminal==XR_XIR_CALL_LIMIT) {
        status=terminal==XR_XIR_CALL_OOM?XR_XIR_VALUE_OOM:XR_XIR_VALUE_LIMIT;goto done;
    }
    CHECK(witness.failure==XR_XIR_VALUE_OK && terminal==(finish==ARG_CANCEL?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED));
    CHECK(witness.entered==1 && witness.children==1 && witness.releases==2 && witness.exits==2);
    CHECK(witness.restored==(finish!=ARG_CANCEL && kind!=ARG_NONE_SHARED && kind!=ARG_NONE_MUT?1u:0u));
    if (terminal==XR_XIR_CALL_RETURNED) {CHECK(xr_xir_call_take_result(call,values+2)==terminal && values[2].payload==91);xr_xir_value_drop(values+2);}
    CHECK(xr_xir_cell_unborrowed(values+1));
    if (witness.physical.body) {
        XrXirViewOriginInfo info={0};TRY_VALUE(xr_xir_view_origin_info(&witness.physical,&admission,&info));CHECK(!info.logical_active);
    }
done:
    if (call) CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
    size_t before=value_compile_calls,runtime_before=calls;
    xr_xir_view_origin_unpin(&witness.physical);xr_xir_value_drop(&witness.late);
    for (uint32_t i=0;i<3;++i) xr_xir_value_drop(values+i);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(before==value_compile_calls && runtime_before==calls && !live);return status;
}
static void view_argument_resources(uint32_t kind,uint32_t finish) {
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);size_t start=value_compile_calls;
    CHECK(view_argument_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);size_t points=value_compile_calls-start;
    XrCompileResourceStats measured=compile_domain_stats(resources);
    CHECK(view_argument_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);xr_compile_resources_release(resources);resources=NULL;
    CHECK(!value_compile_live && !value_compile_bytes && !live && points>10 && points<512);
    for (size_t point=0;point<points;++point) {
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        value_compile_injected=false;value_compile_fail_at=value_compile_calls+point;
        CHECK(view_argument_sequence(resources,kind,finish)==XR_XIR_VALUE_OOM && value_compile_injected);
        value_compile_fail_at=SIZE_MAX;xr_compile_resources_release(resources);resources=NULL;
        CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t adjustment=0;adjustment<3;++adjustment) {
        XrCompileResourceLimits bound=limits;uint64_t *limit=axis==0?&bound.allocated_bytes:axis==1?&bound.live_bytes:&bound.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+adjustment-1u;
        CHECK(xr_compile_resources_new(&bound,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(view_argument_sequence(resources,kind,finish)==(adjustment?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    printf("View arguments kind=%u finish=%u: %zu complete allocation failures, same owner2/three axes exact+minus1+plus1/physical0 PASS\n",kind,finish,points);
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);
    for (uint32_t kind=0;kind<ARG_KINDS;++kind) for (uint32_t finish=0;finish<3;++finish) {
        if (!normal) {view_argument_resources(kind,finish);continue;}
        XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(view_argument_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);
        CHECK(view_argument_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);
        xr_compile_resources_release(resources);CHECK(!value_compile_live && !value_compile_bytes && !live);
        printf("View arguments normal kind=%u finish=%u owner2/physical0 PASS\n",kind,finish);
    }
    puts("Actual Call borrowed parameters, shared/mutable/Some/None/physical DAG, suspend/cancel/panic/exit/late owner PASS");return 0;
}
