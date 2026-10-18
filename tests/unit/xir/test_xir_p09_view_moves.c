/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_moves.c - Frame-owned aggregate and PHI publication
 */
#define XR_COMPILE_DOMAIN_ENTRY original_view_move_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#undef TRY_VALUE
#define TRY_VALUE(operation) do {status=(operation);if(status!=XR_XIR_VALUE_OK) goto done;} while(0)
#include "xir/xxir_call_view_moves_internal.h"

enum {ARG_RETURN,ARG_CANCEL,ARG_PANIC};
enum {MOVE_TUPLE,MOVE_SOME_MUT,MOVE_SOME_SHARED,MOVE_UNIT,MOVE_DAG,
    MOVE_SWAP,MOVE_FANOUT,MOVE_SELF,MOVE_MIXED,MOVE_REPLACE,MOVE_LOOP,MOVE_REJECT,
    MOVE_END_WORDS,MOVE_END_REJECT,MOVE_END_RETIRED,MOVE_END_WRAPPERS,MOVE_KINDS};
enum {MOVE_DAG_DEPTH=70,MOVE_DAG_BASE=263,MOVE_UNIT_TYPE=MOVE_DAG_BASE+MOVE_DAG_DEPTH};
typedef struct ViewMoveState {XrXirValue slots[8];int64_t words[8];uint32_t types[8],pc;} ViewMoveState;
typedef struct ViewMoveWitness {
    uint32_t kind,finish,entered,exits,releases;XrXirValueStatus failure;
    XrXirViewOriginPin pin;XrXirViewAccess access;XrXirValue late;
} ViewMoveWitness;
static XrXirValueStatus view_move_arena(XrCompileResources *resources,XrXirTypeArena **arena) {
    XrXirCallableParameter fields[MOVE_DAG_DEPTH+2][3]={{{(XrXirType)259,0},{XR_XIR_I64,0}}};
    XrXirTypeNode nodes[8+MOVE_DAG_DEPTH]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)258},{.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)259},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[0],.parameter_count=2}};
    for (uint32_t i=0;i<MOVE_DAG_DEPTH;++i) {
        fields[i+1][0].type=fields[i+1][1].type=(XrXirType)(i?MOVE_DAG_BASE+i-1:258);
        nodes[7+i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[i+1],.parameter_count=2};
    }
    fields[MOVE_DAG_DEPTH+1][0].type=(XrXirType)258;fields[MOVE_DAG_DEPTH+1][2].type=XR_XIR_I64;
    nodes[7+MOVE_DAG_DEPTH]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[MOVE_DAG_DEPTH+1],.parameter_count=3};
    const XrXirTypes types={nodes,8+MOVE_DAG_DEPTH,NULL,NULL};
    const XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    return xr_xir_compile_type_arena_new(&context,&types,arena);
}
static XrXirCellParameterRole view_move_role(void *context,uint32_t entry,uint32_t parameter) {
    (void)context;return !entry && parameter<2?XR_XIR_CELL_ROLE_SCOPED_REF:XR_XIR_CELL_ROLE_UNKNOWN;
}
static XrXirAction view_move_fault(XrXirValueStatus status) {
    return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,
        status==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:status==XR_XIR_VALUE_LIMIT?XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_ARGUMENT}};
}
static XrXirValueStatus view_move_apply(XrXirCallView *view,const XrXirViewPhiBatch *batch) {
    ViewMoveState *s=view->state;int64_t before[8];memcpy(before,s->words,sizeof(before));
    XrXirValueStatus status=xr_xir_call_view_phi(view,batch);
    if (status!=XR_XIR_VALUE_OK) CHECK(!memcmp(before,s->words,sizeof(before)));
    return status;
}
static XrXirValueStatus view_move_end_apply(XrXirCallView *view,const XrXirViewEndBatch *batch) {
    ViewMoveState *s=view->state;int64_t before[8];memcpy(before,s->words,sizeof(before));
    XrXirValueStatus status=xr_xir_call_view_end_words(view,batch);
    if (status!=XR_XIR_VALUE_OK) CHECK(!memcmp(before,s->words,sizeof(before)));
    return status;
}
static XrXirValueStatus view_move_end_words(XrXirCallView *view) {
    ViewMoveState *s=view->state;ViewMoveWitness *w=(ViewMoveWitness *)view->environment;
    XrXirValueStatus status=XR_XIR_VALUE_OK;XrXirType shared=(XrXirType)258,mutable=(XrXirType)259;
    TRY_VALUE(xr_xir_call_view_new(view,shared,0,0,4,s->slots));
    int64_t leaf=s->slots[0].payload;xr_xir_owned_slot_move(s->words,0,s->slots);s->types[0]=shared;
    TRY_VALUE(xr_xir_call_view_new(view,mutable,1,0,4,s->slots));
    xr_xir_owned_slot_move(s->words,8,s->slots);s->types[1]=mutable;
    XrXirViewPhiTransfer copies[]={{shared,0,16,32},{shared,0,24,40}};
    XrXirViewPhiBatch phi={s->words,sizeof(s->words),copies,2};
    TRY_VALUE(view_move_apply(view,&phi));s->types[2]=s->types[3]=shared;
    if (w->kind==MOVE_END_RETIRED) {
        CallViewBinding *binding=call_view_binding(view->activation,
            &(XrXirValue){shared,0,leaf},&status);
        if (!binding) goto done;
        xr_xir_view_scope_close(&binding->pin);
        CHECK(!xr_xir_view_scope_active(&binding->pin));
    }
    XrXirViewEndWord rows[]={{shared,0},{mutable,8},{(XrXirType)260,48},{shared,56}};
    XrXirViewEndBatch batch={s->words,sizeof(s->words),rows,4};
    if (w->kind==MOVE_END_REJECT) for (uint32_t invalid=0;invalid<8;++invalid) {
        XrXirViewEndBatch bad=batch;XrXirViewEndWord saved=rows[1];int64_t outside[8]={0};
        if (!invalid) rows[1]=rows[0];
        else if (invalid==1) rows[1].offset=1;
        else if (invalid==2) rows[1].offset=64;
        else if (invalid==3) rows[1].type=XR_XIR_I64;
        else if (invalid==4) rows[1].type=shared;
        else if (invalid==5) bad.frame=outside;
        else if (invalid==6) bad.words=(const XrXirViewEndWord *)s->words;
        else bad.count=9;
        status=view_move_end_apply(view,&bad);rows[1]=saved;
        if (status==XR_XIR_VALUE_LIMIT) goto done;
        CHECK(status==XR_XIR_VALUE_BAD_ARGUMENT && s->words[0]==leaf);
    }
    TRY_VALUE(view_move_end_apply(view,&batch));
    CHECK(!s->words[0] && !s->words[1] && s->words[2]==leaf && s->words[3]==leaf);
    TRY_VALUE(xr_xir_call_view_new(view,mutable,1,0,4,s->slots+1));
    if (w->kind!=MOVE_END_RETIRED) {
        status=xr_xir_call_view_new(view,mutable,0,0,4,s->slots+2);
        if (status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_OOM) goto done;
        CHECK(status==XR_XIR_VALUE_BAD_ARGUMENT && !s->slots[2].type);
    }
    batch.words=rows+2;batch.count=1;rows[2]=(XrXirViewEndWord){shared,16};
    TRY_VALUE(view_move_end_apply(view,&batch));
    if (w->kind!=MOVE_END_RETIRED) {
        status=xr_xir_call_view_new(view,mutable,0,0,4,s->slots+2);
        if (status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_OOM) goto done;
        CHECK(status==XR_XIR_VALUE_BAD_ARGUMENT && !s->slots[2].type && s->words[3]==leaf);
    }
    rows[2].offset=24;TRY_VALUE(view_move_end_apply(view,&batch));
    CHECK(!s->words[2] && !s->words[3]);
    TRY_VALUE(xr_xir_call_view_new(view,mutable,0,0,4,s->slots));
done:return status;
}
static XrXirValueStatus view_move_pack(XrXirCallView *view,XrXirOp op,
    XrXirType type,XrXirValue *fields,uint32_t count,XrXirValue *output) {
    XrXirValue before[3],old=*output;CHECK(count<=3);memcpy(before,fields,count*sizeof(*fields));
    XrXirValueStatus status=xr_xir_call_view_aggregate(view,op,type,fields,count,output);
    if (status!=XR_XIR_VALUE_OK) CHECK(!memcmp(before,fields,count*sizeof(*fields)) && !memcmp(&old,output,sizeof(old)));
    return status;
}
static XrXirValueStatus view_move_end_wrappers(XrXirCallView *view) {
    ViewMoveState *s=view->state;XrXirValueStatus status=XR_XIR_VALUE_OK;
    TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)258,0,0,4,s->slots));
    TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)259,1,0,4,s->slots+1));
    TRY_VALUE(view_move_pack(view,XR_XIR_NULLABLE_SOME,(XrXirType)261,s->slots+1,1,s->slots+2));
    xr_xir_owned_slot_move(s->words,0,s->slots+2);s->types[0]=261;
    for (uint32_t i=0;i<MOVE_DAG_DEPTH;++i) {
        TRY_VALUE(xr_xir_value_copy(s->slots,s->slots+1));
        TRY_VALUE(view_move_pack(view,XR_XIR_TUPLE_NEW,(XrXirType)(MOVE_DAG_BASE+i),s->slots,2,s->slots+2));
        TRY_VALUE(xr_xir_value_move(s->slots+2,s->slots));
    }
    xr_xir_owned_slot_move(s->words,8,s->slots);s->types[1]=MOVE_DAG_BASE+MOVE_DAG_DEPTH-1;
    XrXirViewEndWord rows[]={{(XrXirType)260,16},{(XrXirType)261,0},{(XrXirType)s->types[1],8}};
    XrXirViewEndBatch batch={s->words,sizeof(s->words),rows,3};
    TRY_VALUE(view_move_end_apply(view,&batch));CHECK(!s->words[0] && !s->words[1]);
    TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)259,0,0,4,s->slots));
    TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)259,1,0,4,s->slots+1));
done:return status;
}
static XrXirValueStatus view_move_phi(XrXirCallView *view) {
    ViewMoveState *s=view->state;ViewMoveWitness *w=(ViewMoveWitness *)view->environment;
    bool shared=w->kind==MOVE_FANOUT;XrXirType type=(XrXirType)(shared?258:259);
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    TRY_VALUE(xr_xir_call_view_new(view,type,0,0,2,s->slots));
    int64_t first=s->slots[0].payload;xr_xir_owned_slot_move(s->words,0,s->slots);s->types[0]=type;
    TRY_VALUE(xr_xir_call_view_new(view,type,1,2,2,s->slots));
    int64_t second=s->slots[0].payload;xr_xir_owned_slot_move(s->words,8,s->slots);s->types[1]=type;
    XrXirViewPhiTransfer rows[3]={{type,0,8,32},{type,8,0,40},{XR_XIR_I64,16,24,48}};uint32_t count=2;
    if (shared) rows[1].source=0;
    if (w->kind==MOVE_SELF || w->kind==MOVE_REPLACE) {rows[0].destination=w->kind==MOVE_SELF?0:8;count=1;}
    if (w->kind==MOVE_MIXED) {s->words[2]=91;s->words[3]=17;s->types[2]=s->types[3]=XR_XIR_I64;count=3;}
    XrXirViewPhiBatch batch={s->words,sizeof(s->words),rows,count};
    if (w->kind==MOVE_REJECT) {
        const XrXirViewPhiTransfer original=rows[1];
        for (uint32_t invalid=0;invalid<5;++invalid) {
            rows[1]=original;
            if (!invalid) rows[1].source=0;
            else if (invalid==1) rows[1].scratch=32;
            else if (invalid==2) rows[1].destination=8;
            else if (invalid==3) rows[1].source=1;
            else rows[1].scratch=64;
            status=view_move_apply(view,&batch);
            if (status==XR_XIR_VALUE_LIMIT) goto done;
            CHECK(status==XR_XIR_VALUE_BAD_ARGUMENT && s->words[0]==first && s->words[1]==second);
            CHECK(!s->words[4] && !s->words[5]);
        }
        rows[1]=original;
    }
    TRY_VALUE(view_move_apply(view,&batch));
    if (w->kind==MOVE_LOOP) for (uint32_t i=0;i<63;++i) TRY_VALUE(view_move_apply(view,&batch));
    if (w->kind==MOVE_REPLACE) {
        CHECK(!s->words[0] && s->words[1]==first);
        TRY_VALUE(xr_xir_call_view_new(view,type,1,0,4,s->slots+1));
        s->slots[0]=(XrXirValue){type,0,s->words[1]};s->words[1]=0;goto done;
    }
    CHECK(s->words[0]==(shared || count==1 || w->kind==MOVE_LOOP?first:second));
    if (w->kind==MOVE_LOOP) CHECK(s->words[1]==second);
    else
    CHECK(s->words[1]==(count==1?second:first) && !s->words[4] && !s->words[5] && !s->words[6]);
    if (count==3) CHECK(s->words[2]==91 && s->words[3]==91);
    s->slots[0]=(XrXirValue){type,0,s->words[0]};s->words[0]=0;
done:return status;
}
static XrXirValueStatus view_move_aggregate(XrXirCallView *view) {
    ViewMoveState *s=view->state;ViewMoveWitness *w=(ViewMoveWitness *)view->environment;
    XrXirValueStatus status=XR_XIR_VALUE_OK;bool shared=w->kind>=MOVE_SOME_SHARED;
    TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)(shared?258:259),0,0,4,s->slots));
    int64_t leaf=s->slots[0].payload;
    if (shared) TRY_VALUE(xr_xir_value_copy(s->slots,s->slots+4));
    if (w->kind==MOVE_DAG) {
        for (uint32_t i=0;i<MOVE_DAG_DEPTH;++i) {
            TRY_VALUE(xr_xir_value_copy(s->slots,s->slots+1));
            TRY_VALUE(view_move_pack(view,XR_XIR_TUPLE_NEW,(XrXirType)(MOVE_DAG_BASE+i),s->slots,2,s->slots+2));
            TRY_VALUE(xr_xir_value_move(s->slots+2,s->slots));
        }
        for (uint32_t i=0;i<MOVE_DAG_DEPTH;++i) {
            TRY_VALUE(xr_xir_tuple_get(s->slots,0,s->slots+1));xr_xir_value_drop(s->slots);
            TRY_VALUE(xr_xir_value_move(s->slots+1,s->slots));
        }
        CHECK(s->slots[0].payload==leaf);goto done;
    }
    if (w->kind==MOVE_UNIT) {
        s->slots[2]=(XrXirValue){XR_XIR_I64,0,73};
        TRY_VALUE(view_move_pack(view,XR_XIR_TUPLE_NEW,(XrXirType)MOVE_UNIT_TYPE,s->slots,3,s->slots+3));
        CHECK(!s->slots[0].type && !s->slots[1].type && !s->slots[2].type);
        TRY_VALUE(xr_xir_borrowed_tuple_unpack_move(s->slots+3,s->slots,3,&w->access));
        CHECK(s->slots[0].payload==leaf && !s->slots[1].type && s->slots[2].payload==73);goto done;
    }
    uint32_t count=w->kind==MOVE_TUPLE?2:1;
    if (count==2) s->slots[1]=(XrXirValue){XR_XIR_I64,0,71};
    XrXirOp op=count==2?XR_XIR_TUPLE_NEW:XR_XIR_NULLABLE_SOME;
    XrXirType type=(XrXirType)(count==2?262:shared?260:261);XrXirValue outside={0};
    CHECK(xr_xir_call_view_aggregate(view,op,type,s->slots,count,&outside)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_view_aggregate(view,op,type,s->slots,count,s->slots)==XR_XIR_VALUE_BAD_ARGUMENT);
    TRY_VALUE(view_move_pack(view,op,type,s->slots,count,s->slots+2));
    CHECK(!s->slots[0].type && !s->slots[1].type && s->slots[2].type==(uint32_t)type);
    if (count==2) {
        TRY_VALUE(xr_xir_borrowed_tuple_unpack_move(s->slots+2,s->slots,2,&w->access));
        CHECK(s->slots[1].payload==71);xr_xir_value_drop(s->slots+1);
    } else TRY_VALUE(xr_xir_borrowed_some_take(s->slots+2,&w->access,s->slots));
    CHECK(s->slots[0].payload==leaf);
    if (shared) {int64_t length=0;TRY_VALUE(xr_xir_view_len(s->slots+4,&w->access,&length));CHECK(length==4);}
done:return status;
}
static XrXirAction view_move_entry(XrXirCallView *view) {
    ViewMoveState *s=view->state;ViewMoveWitness *w=(ViewMoveWitness *)view->environment;
    if (view->phase==XR_XIR_CALL_EXIT) {
        ++w->exits;XrXirViewPhiTransfer row={(XrXirType)259,0,8,32};
        XrXirViewPhiBatch batch={s->words,sizeof(s->words),&row,1};
        CHECK(xr_xir_call_view_phi(view,&batch)==XR_XIR_VALUE_BAD_ARGUMENT);
        XrXirViewEndWord word={(XrXirType)259,0};
        XrXirViewEndBatch end={s->words,sizeof(s->words),&word,1};
        CHECK(xr_xir_call_view_end_words(view,&end)==XR_XIR_VALUE_BAD_ARGUMENT);
        return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    if (!s->pc++) {
        ++w->entered;w->failure=xr_xir_call_view_access(view,&w->access);
        if (w->failure==XR_XIR_VALUE_OK) w->failure=w->kind==MOVE_END_WRAPPERS?view_move_end_wrappers(view):
            w->kind>=MOVE_END_WORDS?view_move_end_words(view):
            w->kind<MOVE_SWAP?view_move_aggregate(view):view_move_phi(view);
        if (w->failure==XR_XIR_VALUE_OK) w->failure=xr_xir_view_origin_pin(s->slots,&w->access,&w->pin);
        if (w->failure!=XR_XIR_VALUE_OK) return view_move_fault(w->failure);
        CHECK(xr_xir_value_move(s->slots,&w->late)==XR_XIR_VALUE_OK);
        return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    }
    if (w->finish==ARG_PANIC) return xr_xir_call_fault(XR_XIR_RUN_DIVIDE_BY_ZERO);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,91}};
}
static void view_move_release(XrXirCallView *view,XrXirCallStatus reason) {
    (void)reason;ViewMoveState *s=view->state;ViewMoveWitness *w=(ViewMoveWitness *)view->environment;++w->releases;
    for (uint32_t i=0;i<8;++i) {
        xr_xir_value_drop(s->slots+i);
        XrXirValue value={s->types[i],0,s->words[i]};s->words[i]=0;if (value.payload) xr_xir_value_drop(&value);
    }
}
static XrXirValueStatus view_move_sequence(XrCompileResources *resources,uint32_t kind,uint32_t finish) {
    XrXirDomain *domain=NULL;XrXirTypeArena *arena=NULL;XrXirValue values[4]={{0}};XrXirCall *call=NULL;
    ViewMoveWitness witness={.kind=kind,.finish=finish};XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirCallAccounting accounting={0};XrXirCallBudget budget={.byte_limit=65536,.requested_limit=65536,.resume_limit=64};
    const XrXirDomainBudgetControls controls={1048576,1048576,4194304,65536,65536};
    TRY_VALUE(view_move_arena(resources,&arena));TRY_VALUE(xr_xir_compile_domain_new(resources,1048576,&domain));
    TRY_VALUE(xr_xir_domain_budget_bind(domain,&controls));budget.work_domain=domain;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,4194304,65536};
    const XrXirValue items[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22},{XR_XIR_I64,0,33},{XR_XIR_I64,0,44}};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,items,4,&admission,values));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,values,&admission,values+1));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,values,&admission,values+2));
    const XrXirType roots[]={(XrXirType)257,(XrXirType)257};
    const XrXirCallEntry entry={.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=roots,.parameter_count=2,
        .result=XR_XIR_I64,.state_bytes=sizeof(ViewMoveState),.resume=view_move_entry,
        .release=view_move_release,.environment=&witness,.flags=XR_XIR_ENTRY_EXIT};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=&accounting;config.admission=admission;config.byte_limit=65536;
    const XrXirCallRequest request={0,values+1,2,view_move_role,NULL};
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
    CHECK(witness.failure==XR_XIR_VALUE_OK && terminal==(finish==ARG_CANCEL?XR_XIR_CALL_CANCELLED:
        finish==ARG_PANIC?XR_XIR_CALL_DIVIDE_BY_ZERO:XR_XIR_CALL_RETURNED));
    CHECK(witness.entered==1 && witness.releases==1 && witness.exits==1 &&
        xr_xir_cell_unborrowed(values+1) && xr_xir_cell_unborrowed(values+2));
    if (terminal==XR_XIR_CALL_RETURNED) {CHECK(xr_xir_call_take_result(call,values+3)==terminal && values[3].payload==91);xr_xir_value_drop(values+3);}
    int64_t length=0;CHECK(xr_xir_view_len(&witness.late,&witness.access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
done:
    if (call) CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
    size_t before=value_compile_calls,runtime_before=calls;
    xr_xir_view_origin_unpin(&witness.pin);xr_xir_value_drop(&witness.late);
    for (uint32_t i=0;i<4;++i) xr_xir_value_drop(values+i);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(before==value_compile_calls && runtime_before==calls && !live);return status;
}
static void view_move_resources(uint32_t kind,uint32_t finish,bool normal) {
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);size_t start=value_compile_calls;
    CHECK(view_move_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);size_t points=value_compile_calls-start;
    XrCompileResourceStats measured=compile_domain_stats(resources);
    CHECK(view_move_sequence(resources,kind,finish)==XR_XIR_VALUE_OK);xr_compile_resources_release(resources);resources=NULL;
    CHECK(!value_compile_live && !value_compile_bytes && !live && points>10 && points<4096);
    if (!normal) for (size_t point=0;point<points;++point) {
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        value_compile_injected=false;value_compile_fail_at=value_compile_calls+point;
        CHECK(view_move_sequence(resources,kind,finish)==XR_XIR_VALUE_OOM && value_compile_injected);
        value_compile_fail_at=SIZE_MAX;xr_compile_resources_release(resources);resources=NULL;
        CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t adjustment=0;adjustment<3;++adjustment) {
        XrCompileResourceLimits bound=limits;uint64_t *limit=axis==0?&bound.allocated_bytes:axis==1?&bound.live_bytes:&bound.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+adjustment-1u;
        CHECK(xr_compile_resources_new(&bound,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(view_move_sequence(resources,kind,finish)==(adjustment?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    if (!normal && (kind==MOVE_FANOUT || kind==MOVE_END_WORDS) && finish==ARG_RETURN) {
        CHECK(measured.work>1 && measured.work<131072);
        for (uint64_t work=1;work<measured.work;++work) {
            XrCompileResourceLimits bound=limits;bound.work=work;
            CHECK(xr_compile_resources_new(&bound,&resources)==XR_COMPILE_RESOURCE_OK);
            CHECK(view_move_sequence(resources,kind,finish)==XR_XIR_VALUE_LIMIT);
            xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
        }
        printf("View moves kind=%u: all %llu positive work cutoffs preserve owners/physical0 PASS\n",kind,(unsigned long long)(measured.work-1));
    }
    printf("View moves kind=%u finish=%u: %zu allocation points, owner2/physical0%s PASS\n",kind,finish,points,
        normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);
    for (uint32_t kind=0;kind<MOVE_KINDS;++kind) for (uint32_t finish=0;finish<3;++finish) view_move_resources(kind,finish,normal);
    puts("Actual borrowed aggregate/PHI transfers and suspend/cancel/panic/exit/late owners PASS");return 0;
}
