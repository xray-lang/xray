/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_entry_aliases.c - Joint borrowed grants before frame entry
 */
#define XR_COMPILE_DOMAIN_ENTRY original_view_entry_alias_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#undef TRY_VALUE
#define TRY_VALUE(operation) do {status=(operation);if(status!=XR_XIR_VALUE_OK) goto done;} while(0)

enum { ENTRY_RETURN,ENTRY_CANCEL,ENTRY_PANIC };
static XrXirAction entry_alias_failure(XrXirValueStatus status) {
    return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,
        status==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:status==XR_XIR_VALUE_LIMIT?XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_ARGUMENT}};
}
enum { E_ARRAY=256,E_CELL,E_SHARED,E_MUT,E_SOME_SHARED,E_SOME_MUT,E_MIXED,E_MUT_PAIR,E_DAG,E_DEPTH=70,
    E_SOME_MUT_PAIR=E_DAG+E_DEPTH,E_KINDS=24 };
typedef struct EntryAliasState { XrXirValue owned[8],arguments[2];uint32_t pc; } EntryAliasState;
typedef struct EntryAliasWitness {
    XrXirValue roots[2],parents[2],before[8];XrXirViewOriginPin pins[2];XrXirViewAccess caller;
    XrXirValueStatus failure;uint32_t kind,finish,entered,released,restored;int64_t written[2];bool prepared;
} EntryAliasWitness;
static bool entry_alias_rejected(uint32_t kind) {return kind==2 || kind==3 || kind==7 || kind==8 || kind==9 || kind>=19;}
static void entry_alias_types(uint32_t kind,XrXirType *types) {
    static const uint32_t pairs[E_KINDS][2]={
        {E_SHARED,E_SHARED},{E_MUT,E_MUT},{E_MUT,E_MUT},{E_SOME_MUT,E_MUT},{E_SOME_SHARED,E_SHARED},
        {E_MIXED,E_SOME_MUT},{E_MUT_PAIR,E_SOME_SHARED},{E_MUT_PAIR,E_MUT_PAIR},{E_MUT,E_SHARED},
        {E_SHARED,E_MUT},{E_SOME_MUT,E_SHARED},{E_SOME_MUT,E_SHARED},{E_DAG+E_DEPTH-1,E_MUT},{E_SOME_MUT_PAIR,E_SOME_SHARED},
        {E_CELL,E_CELL},{E_CELL,E_SHARED},{E_CELL,E_MUT},{E_SHARED,E_CELL},{E_MUT,E_CELL},
        {E_CELL,E_SHARED},{E_SHARED,E_CELL},{E_CELL,E_CELL},{E_CELL,E_MUT},{E_MUT,E_CELL}};
    types[0]=(XrXirType)pairs[kind][0];types[1]=(XrXirType)pairs[kind][1];
}
static XrXirValueStatus entry_alias_arena(XrCompileResources *resources,XrXirTypeArena **arena) {
    XrXirCallableParameter fields[2+E_DEPTH][2]={{{0}}};XrXirTypeNode nodes[9+E_DEPTH]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)E_ARRAY},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)E_SHARED},{.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)E_MUT}};
    fields[0][0].type=fields[1][0].type=fields[1][1].type=(XrXirType)E_MUT;fields[0][1].type=(XrXirType)E_SHARED;
    for (uint32_t i=0;i<2+E_DEPTH;++i) {
        if (i>=2) fields[i][0].type=fields[i][1].type=(XrXirType)(i==2?E_SHARED:E_DAG+i-3);
        nodes[6+i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[i],.parameter_count=2};
    }
    nodes[8+E_DEPTH]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)E_MUT_PAIR};
    const XrXirTypes types={nodes,9+E_DEPTH,NULL,NULL};const XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    return xr_xir_compile_type_arena_new(&context,&types,arena);
}
static XrXirValueStatus entry_alias_prepare_cells(XrXirCallView *view) {
    EntryAliasState *s=view->state;EntryAliasWitness *w=(EntryAliasWitness *)view->environment;
    XrXirValueStatus status=XR_XIR_VALUE_OK;TRY_VALUE(xr_xir_call_view_access(view,&w->caller));
    if (w->kind==14 || w->kind==21) {
        s->arguments[0]=view->arguments[0];s->arguments[1]=view->arguments[w->kind==21?0:1];
    } else {
        bool first=w->kind==15 || w->kind==16 || w->kind==19 || w->kind==22;
        bool mut=w->kind==16 || w->kind==18 || w->kind==22 || w->kind==23;
        uint32_t root=w->kind>=19?0:1;
        TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)(mut?E_MUT:E_SHARED),root,0,4,s->owned));
        w->parents[0]=s->owned[0];TRY_VALUE(xr_xir_view_origin_pin(w->parents,&w->caller,w->pins));
        s->arguments[first?0:1]=view->arguments[0];s->arguments[first?1:0]=s->owned[0];
    }
    memcpy(w->before,s->owned,sizeof(w->before));w->prepared=true;
done:return status;
}
static XrXirValueStatus entry_alias_prepare(XrXirCallView *view) {
    EntryAliasState *s=view->state;EntryAliasWitness *w=(EntryAliasWitness *)view->environment;
    if (w->kind>=14) return entry_alias_prepare_cells(view);
    XrXirValueStatus status=XR_XIR_VALUE_OK;TRY_VALUE(xr_xir_call_view_access(view,&w->caller));
    bool mut=w->kind==1 || w->kind==2 || w->kind==3 || (w->kind>=5 && w->kind<=9) || w->kind==11 || w->kind==13;
    TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)(mut?E_MUT:E_SHARED),0,0,4,s->owned));
    w->parents[0]=s->owned[0];TRY_VALUE(xr_xir_view_origin_pin(w->parents,&w->caller,w->pins));
    bool other=w->kind==1 || w->kind==5 || w->kind==6 || w->kind==7 || w->kind==11 || w->kind==12 || w->kind==13;
    if (w->kind==8 || w->kind==9)
        TRY_VALUE(xr_xir_call_view_reborrow(view,(XrXirType)E_SHARED,s->owned,0,4,s->owned+1));
    else if (other) TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)(w->kind==1 || w->kind==6 || w->kind==7 ||
        w->kind==12 || w->kind==13?E_MUT:E_SHARED),1,0,4,s->owned+1));
    if (s->owned[1].type) {w->parents[1]=s->owned[1];TRY_VALUE(xr_xir_view_origin_pin(w->parents+1,&w->caller,w->pins+1));}
    s->arguments[0]=s->owned[0];s->arguments[1]=other?s->owned[1]:s->owned[0];
    if (w->kind==3 || w->kind==4 || w->kind==11) {
        TRY_VALUE(xr_xir_borrowed_some_new_move((XrXirType)(w->kind==4?E_SOME_SHARED:E_SOME_MUT),
            s->owned,&w->caller,s->owned+2));s->arguments[0]=s->owned[2];
    } else if (w->kind==5 || w->kind==6 || w->kind==7 || w->kind==13) {
        TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)(w->kind==5?E_MIXED:E_MUT_PAIR),
            s->owned,2,&w->caller,s->owned+2));s->arguments[0]=s->owned[2];
        if (w->kind==13) {
            TRY_VALUE(xr_xir_borrowed_some_new_move((XrXirType)E_SOME_MUT_PAIR,s->owned+2,&w->caller,s->owned+4));
            TRY_VALUE(xr_xir_value_move(s->owned+4,s->owned+2));s->arguments[0]=s->owned[2];
        }
        if (w->kind==7) s->arguments[1]=s->owned[2];
        else {
            TRY_VALUE(xr_xir_nullable_new((XrXirType)(w->kind==5?E_SOME_MUT:E_SOME_SHARED),NULL,w->caller.values,s->owned+3));
            s->arguments[1]=s->owned[3];
        }
    } else if (w->kind==8 || w->kind==9) {
        s->arguments[0]=s->owned[w->kind==8?0:1];s->arguments[1]=s->owned[w->kind==8?1:0];
    } else if (w->kind==10) {
        TRY_VALUE(xr_xir_nullable_new((XrXirType)E_SOME_MUT,NULL,w->caller.values,s->owned+2));
        s->arguments[0]=s->owned[2];s->arguments[1]=s->owned[0];
    } else if (w->kind==12) {
        TRY_VALUE(xr_xir_value_move(s->owned,s->owned+2));
        for (uint32_t i=0;i<E_DEPTH;++i) {
            TRY_VALUE(xr_xir_value_copy(s->owned+2,s->owned+3));
            TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)(E_DAG+i),s->owned+2,2,&w->caller,s->owned+4));
            TRY_VALUE(xr_xir_value_move(s->owned+4,s->owned+2));
        }
        s->arguments[0]=s->owned[2];
    }
    XrXirType types[2];entry_alias_types(w->kind,types);
    for (uint32_t p=0;p<2;++p) TRY_VALUE(xr_xir_borrowed_value_admit(s->arguments+p,types[p],&w->caller));
    memcpy(w->before,s->owned,sizeof(w->before));w->prepared=true;
done:return status;
}
static XrXirValueStatus entry_alias_root(const XrXirValue *value,const XrXirViewAccess *access,
    const EntryAliasWitness *w,uint32_t *root) {
    XrXirViewOriginPin pin={0};XrXirViewOriginInfo info={0};XrXirValueStatus status=xr_xir_view_origin_pin(value,access,&pin);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_view_origin_info(&pin,access->values,&info);
    if (status==XR_XIR_VALUE_OK) {
        CHECK(info.logical_active && info.length==4);bool first=xr_xir_cell_same_owner(info.root,w->roots);
        CHECK(first || xr_xir_cell_same_owner(info.root,w->roots+1));*root=first?0:1;
    }
    xr_xir_view_origin_unpin(&pin);return status;
}
static XrXirValueStatus entry_alias_cell_payload(XrXirCallView *view,const XrXirValue *cell) {
    const EntryAliasWitness *w=view->environment;XrXirValue array={0},item={0};XrXirFaultDetail fault={0};
    XrXirValueStatus status=xr_xir_call_cell_read(view,cell,&array);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_array_get(&array,0,xr_xir_call_admission(view),&item,&fault);
    if (status==XR_XIR_VALUE_OK) {
        uint32_t root=xr_xir_cell_same_owner(cell,w->roots)?0:1;
        CHECK(xr_xir_cell_same_owner(cell,w->roots+root));
        CHECK(item.type==XR_XIR_I64 && item.payload==(w->written[root]?w->written[root]:11));
    }
    xr_xir_value_drop(&item);xr_xir_value_drop(&array);return status;
}
static XrXirValueStatus entry_alias_inspect(XrXirCallView *view) {
    EntryAliasState *s=view->state;EntryAliasWitness *w=(EntryAliasWitness *)view->environment;
    XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirViewAccess access={0};TRY_VALUE(xr_xir_call_view_access(view,&access));
    for (uint32_t p=0;p<2;++p) {
        if (view->arguments[p].type==E_CELL) {
            TRY_VALUE(entry_alias_cell_payload(view,view->arguments+p));
            TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)(w->kind==14 && p?E_SHARED:E_MUT),p,0,4,s->owned+p));
        } else TRY_VALUE(xr_xir_call_view_argument_take(view,p,s->owned+p));
    }
    for (uint32_t p=0;p<2;++p) {
        uint32_t type=s->owned[p].type;
        if (type==E_SOME_MUT || type==E_SOME_SHARED || type==E_SOME_MUT_PAIR) {
            bool some=false;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(s->owned+p,&some,&payload));
            if (some) {TRY_VALUE(xr_xir_borrowed_some_take(s->owned+p,&access,s->owned+4));
                TRY_VALUE(xr_xir_value_move(s->owned+4,s->owned+p));}
        }
        type=s->owned[p].type;
        if (type==E_MIXED || type==E_MUT_PAIR)
            TRY_VALUE(xr_xir_borrowed_tuple_unpack_move(s->owned+p,s->owned+2,2,&access));
        else if (type==E_DAG+E_DEPTH-1) for (uint32_t i=0;i<E_DEPTH;++i) {
            TRY_VALUE(xr_xir_tuple_get(s->owned+p,0,s->owned+4));xr_xir_value_drop(s->owned+p);
            TRY_VALUE(xr_xir_value_move(s->owned+4,s->owned+p));
        }
    }
    uint32_t roots[4]={0};bool mutable[4]={0};
    for (uint32_t p=0;p<4;++p) {
        XrXirValue *value=s->owned+p;if (!value->type) continue;
        if (value->type!=E_SHARED && value->type!=E_MUT) {
            bool some=true;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(value,&some,&payload) && !some && !payload);continue;
        }
        CHECK(value->payload!=w->parents[0].payload && value->payload!=w->parents[1].payload);
        TRY_VALUE(entry_alias_root(value,&access,w,roots+p));mutable[p]=value->type==E_MUT;
        for (uint32_t prior=0;prior<p;++prior) if (s->owned[prior].type==E_SHARED || s->owned[prior].type==E_MUT)
            CHECK(roots[p]!=roots[prior] || (!mutable[p] && !mutable[prior]));
        XrXirValue item={XR_XIR_I64,0,99+roots[p]},read={0};XrXirFaultDetail fault={0};
        if (mutable[p]) {TRY_VALUE(xr_xir_view_set(value,0,&item,&access,&fault));w->written[roots[p]]=item.payload;}
        TRY_VALUE(xr_xir_view_get(value,0,&access,&read,&fault));CHECK(read.payload==(w->written[roots[p]]?w->written[roots[p]]:11));
        xr_xir_value_drop(&read);
    }
done:return status;
}
static XrXirValueStatus entry_alias_restored(XrXirCallView *view) {
    EntryAliasState *s=view->state;EntryAliasWitness *w=(EntryAliasWitness *)view->environment;
    XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirType types[2];entry_alias_types(w->kind,types);
    for (uint32_t p=0;p<2;++p) {
        if (types[p]==(XrXirType)E_CELL) TRY_VALUE(entry_alias_cell_payload(view,s->arguments+p));
        else TRY_VALUE(xr_xir_borrowed_value_admit(s->arguments+p,types[p],&w->caller));
    }
    for (uint32_t p=0;p<2;++p) if (w->parents[p].type) {
        int64_t length=0;XrXirValue read={0};XrXirFaultDetail fault={0};uint32_t root=0;
        TRY_VALUE(entry_alias_root(w->parents+p,&w->caller,w,&root));
        TRY_VALUE(xr_xir_view_len(w->parents+p,&w->caller,&length));CHECK(length==4);
        TRY_VALUE(xr_xir_view_get(w->parents+p,0,&w->caller,&read,&fault));
        CHECK(read.payload==(w->written[root]?w->written[root]:11));
        if (w->parents[p].type==E_MUT) status=xr_xir_view_set(w->parents+p,0,&read,&w->caller,&fault);
        xr_xir_value_drop(&read);if (status!=XR_XIR_VALUE_OK) goto done;
    }
    ++w->restored;
done:return status;
}
static XrXirAction entry_alias_parent(XrXirCallView *view) {
    EntryAliasState *s=view->state;EntryAliasWitness *w=(EntryAliasWitness *)view->environment;
    if (view->phase==XR_XIR_CALL_EXIT) return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    if (!s->pc++) {
        w->failure=entry_alias_prepare(view);if (w->failure!=XR_XIR_VALUE_OK) return entry_alias_failure(w->failure);
        return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=1,.arguments=s->arguments,.argument_count=2,.flags=XR_XIR_ACTION_PROTECTED};
    }
    CHECK(view->inbox.status==(w->finish==ENTRY_PANIC?XR_XIR_CALL_DIVIDE_BY_ZERO:XR_XIR_CALL_RETURNED));
    w->failure=entry_alias_restored(view);return w->failure==XR_XIR_VALUE_OK?
        (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,91}}:entry_alias_failure(w->failure);
}
static XrXirAction entry_alias_child(XrXirCallView *view) {
    EntryAliasState *s=view->state;EntryAliasWitness *w=(EntryAliasWitness *)view->environment;
    if (view->phase==XR_XIR_CALL_EXIT) return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    if (!s->pc++) {
        ++w->entered;w->failure=entry_alias_inspect(view);
        return w->failure==XR_XIR_VALUE_OK?(XrXirAction){.kind=XR_XIR_ACTION_SUSPEND}:entry_alias_failure(w->failure);
    }
    return w->finish==ENTRY_PANIC?xr_xir_call_fault(XR_XIR_RUN_DIVIDE_BY_ZERO):
        (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,17}};
}
static void entry_alias_release(XrXirCallView *view,bool parent) {
    EntryAliasState *s=view->state;EntryAliasWitness *w=(EntryAliasWitness *)view->environment;
    if (parent && w->prepared) {
        CHECK(!memcmp(s->owned,w->before,sizeof(w->before)));
        for (uint32_t p=0;p<2;++p) CHECK(xr_xir_value_valid(s->arguments+p));
    }
    ++w->released;for (uint32_t p=0;p<8;++p) xr_xir_value_drop(s->owned+p);
}
static void entry_alias_parent_release(XrXirCallView *view,XrXirCallStatus reason) {(void)reason;entry_alias_release(view,true);}
static void entry_alias_child_release(XrXirCallView *view,XrXirCallStatus reason) {(void)reason;entry_alias_release(view,false);}
static XrXirCellParameterRole entry_alias_role(void *context,uint32_t entry,uint32_t parameter) {
    const XrXirType *types=context;
    return parameter<2 && (!entry || (entry==1 && types[parameter]==(XrXirType)E_CELL))?
        XR_XIR_CELL_ROLE_SCOPED_REF:XR_XIR_CELL_ROLE_UNKNOWN;
}
static XrXirValueStatus entry_alias_sequence(XrCompileResources *resources,uint32_t kind,uint32_t finish) {
    XrXirDomain *domain=NULL;XrXirTypeArena *arena=NULL;XrXirCall *call=NULL;XrXirValue values[4]={{0}};
    EntryAliasWitness w={.kind=kind,.finish=finish};XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirCallAccounting accounting={0};
    XrXirCallBudget budget={.byte_limit=65536,.requested_limit=65536,.resume_limit=64};
    const XrXirDomainBudgetControls controls={1048576,1048576,4194304,65536,65536};
    TRY_VALUE(entry_alias_arena(resources,&arena));TRY_VALUE(xr_xir_compile_domain_new(resources,1048576,&domain));
    TRY_VALUE(xr_xir_domain_budget_bind(domain,&controls));budget.work_domain=domain;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,4194304,65536};
    const XrXirValue items[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22},{XR_XIR_I64,0,33},{XR_XIR_I64,0,44}};
    TRY_VALUE(xr_xir_array_new((XrXirType)E_ARRAY,items,4,&admission,values));
    for (uint32_t p=0;p<2;++p) {TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)E_CELL,values,&admission,values+1+p));w.roots[p]=values[1+p];}
    XrXirType types[2],roots[2]={(XrXirType)E_CELL,(XrXirType)E_CELL};entry_alias_types(kind,types);
    const XrXirCallEntry entries[]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=roots,.parameter_count=2,.result=XR_XIR_I64,.state_bytes=sizeof(EntryAliasState),
         .resume=entry_alias_parent,.release=entry_alias_parent_release,.environment=&w,.flags=XR_XIR_ENTRY_EXIT},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=types,.parameter_count=2,.result=XR_XIR_I64,.state_bytes=sizeof(EntryAliasState)+4096,
         .resume=entry_alias_child,.release=entry_alias_child_release,.environment=&w,.flags=XR_XIR_ENTRY_EXIT}};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries;config.entry_count=2;config.accounting=&accounting;config.admission=admission;config.byte_limit=65536;
    const XrXirCallRequest request={0,values+1,2,entry_alias_role,types};
    XrXirCallStatus terminal=xr_xir_call_new_budgeted(&config,&request,&budget,&call);
    if (terminal!=XR_XIR_CALL_READY) {status=terminal==XR_XIR_CALL_OOM?XR_XIR_VALUE_OOM:XR_XIR_VALUE_LIMIT;goto done;}
    uint32_t polls=0;
    do {
        XrXirCallResult result=xr_xir_call_poll_bounded(call,1);terminal=result.status;CHECK(++polls<32);
        if (terminal==XR_XIR_CALL_SUSPENDED) {
            if (finish==ENTRY_CANCEL) CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);
            else CHECK(xr_xir_call_resume(call,result.wake)==XR_XIR_CALL_READY);
            terminal=XR_XIR_CALL_READY;
        }
    } while (terminal==XR_XIR_CALL_READY);
    if (terminal==XR_XIR_CALL_OOM || terminal==XR_XIR_CALL_LIMIT) {status=terminal==XR_XIR_CALL_OOM?XR_XIR_VALUE_OOM:XR_XIR_VALUE_LIMIT;goto done;}
    CHECK(w.failure==XR_XIR_VALUE_OK && w.prepared);
    CHECK(terminal==(entry_alias_rejected(kind)?XR_XIR_CALL_BAD_ARGUMENT:finish==ENTRY_CANCEL?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED));
    CHECK(w.entered==(entry_alias_rejected(kind)?0u:1u));CHECK(w.released==w.entered+1u);
    CHECK(w.restored==(!entry_alias_rejected(kind) && finish!=ENTRY_CANCEL?1u:0u));
    if (terminal==XR_XIR_CALL_RETURNED) {CHECK(xr_xir_call_take_result(call,values+3)==terminal && values[3].payload==91);xr_xir_value_drop(values+3);}
    CHECK(xr_xir_cell_unborrowed(values+1) && xr_xir_cell_unborrowed(values+2));
done:
    if (call) CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
    size_t before=value_compile_calls,runtime_before=calls;
    for (uint32_t p=0;p<2;++p) xr_xir_view_origin_unpin(w.pins+p);
    for (uint32_t p=0;p<4;++p) xr_xir_value_drop(values+p);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(before==value_compile_calls && runtime_before==calls && !live);return status;
}
static void entry_alias_resources(uint32_t kind,uint32_t finish,bool normal) {
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);size_t first=value_compile_calls;
    CHECK(entry_alias_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);size_t points=value_compile_calls-first;
    XrCompileResourceStats measured=compile_domain_stats(resources);CHECK(entry_alias_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);
    xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live && points>10 && points<512);
    if (!normal) for (size_t p=0;p<points;++p) {
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);value_compile_injected=false;value_compile_fail_at=value_compile_calls+p;
        CHECK(entry_alias_sequence(resources,kind,finish)==XR_XIR_VALUE_OOM && value_compile_injected);value_compile_fail_at=SIZE_MAX;
        xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits bound=limits;uint64_t *limit=axis==0?&bound.allocated_bytes:axis==1?&bound.live_bytes:&bound.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+delta-1u;
        CHECK(xr_compile_resources_new(&bound,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(entry_alias_sequence(resources,kind,finish)==(delta?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    printf("Entry aliases kind=%u finish=%u: %zu allocation points owner2/physical0%s PASS\n",kind,finish,points,
        normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);setvbuf(stdout,NULL,_IONBF,0);
    for (uint32_t kind=0;kind<E_KINDS;++kind) for (uint32_t finish=0;finish<(entry_alias_rejected(kind)?1u:3u);++finish)
        entry_alias_resources(kind,finish,normal);
    puts("Joint temporary grants precede entry; conflicting aliases roll back without changing caller owners");return 0;
}
