/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_returns.c - Returned leaves after actual callee reclamation
 *
 * KEY CONCEPT:
 *   Return adoption precedes inline root retirement and never extends a frame.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_view_returns_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#undef TRY_VALUE
#define TRY_VALUE(operation) do {status=(operation);if(status!=XR_XIR_VALUE_OK) goto done;} while(0)
enum {RR_REF_SHARED,RR_REF_MUT,RR_LOCAL_SHARED,RR_LOCAL_MUT,RR_VIEW_SHARED,RR_VIEW_MUT,RR_MUT_SHARED,
    RR_SOME_SHARED,RR_SOME_MUT,RR_NONE_VIEW,RR_NONE_REF,RR_DAG,RR_PAIR,RR_TUPLE_MUT,
    RR_WRONG_REF,RR_WRONG_VIEW,RR_SAME_ROOT,RR_WRONG_PAIR,RR_CAPTURE,
    RR_DISCARD_SHARED,RR_DISCARD_MUT,RR_REJECT_ZERO,RR_REJECT_ALIAS,RR_KINDS};
enum {RR_RETURN,RR_CANCEL_CHILD,RR_CANCEL_STAGED,RR_CANCEL_ADOPTED,RR_PANIC_ADOPTED,RR_FINISHES};
enum {RR_DAG_DEPTH=70,RR_DAG_BASE=263,RR_CALLABLE=333,RR_PAIR_TYPE=263};
typedef struct ReturnState {XrXirValue slots[6];uint32_t pc;} ReturnState;
typedef struct ReturnWitness {
    XrXirValue kept,retired,original;XrXirViewOriginPin physical;
    XrXirCallAccounting *accounting;uint64_t baseline;uint32_t discarded;
    XrXirValueStatus failure;uint32_t kind,finish,depth,entered,children,exits,releases,staged,received,rejections,functions,frees;
} ReturnWitness;
static bool rr_reference(uint32_t kind) {
    return kind==RR_REF_SHARED || kind==RR_REF_MUT || kind==RR_LOCAL_SHARED || kind==RR_LOCAL_MUT ||
        kind==RR_NONE_REF || kind==RR_WRONG_REF || kind==RR_DISCARD_SHARED || kind==RR_DISCARD_MUT;
}
static bool rr_local(uint32_t kind) {return kind==RR_LOCAL_SHARED || kind==RR_LOCAL_MUT || kind==RR_SAME_ROOT;}
static bool rr_mutable(uint32_t kind) {
    return kind==RR_REF_MUT || kind==RR_LOCAL_MUT || kind==RR_VIEW_MUT || kind==RR_SOME_MUT || kind==RR_TUPLE_MUT || kind==RR_DISCARD_MUT;
}
static XrXirType rr_input(uint32_t kind) {
    return rr_reference(kind)?(XrXirType)257:kind==RR_NONE_VIEW?(XrXirType)260:
        rr_mutable(kind) || kind==RR_MUT_SHARED?(XrXirType)259:(XrXirType)258;
}
static XrXirType rr_result(uint32_t kind) {
    if (kind==RR_SOME_SHARED || kind==RR_NONE_VIEW || kind==RR_NONE_REF) return (XrXirType)260;
    if (kind==RR_SOME_MUT) return (XrXirType)261;
    if (kind==RR_DAG) return (XrXirType)(RR_DAG_BASE+RR_DAG_DEPTH-1);
    if (kind==RR_PAIR || kind==RR_WRONG_PAIR) return (XrXirType)RR_PAIR_TYPE;
    if (kind==RR_TUPLE_MUT) return (XrXirType)262;
    if (kind==RR_REJECT_ZERO || kind==RR_REJECT_ALIAS) return (XrXirType)260;
    return (XrXirType)(rr_mutable(kind)?259:258);
}
static XrXirValueStatus rr_status(XrXirCallStatus status) {
    return status==XR_XIR_CALL_READY?XR_XIR_VALUE_OK:status==XR_XIR_CALL_OOM?XR_XIR_VALUE_OOM:
        status==XR_XIR_CALL_LIMIT?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirAction rr_failure(XrXirValueStatus status) {
    return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,
        status==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:status==XR_XIR_VALUE_LIMIT?XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_ARGUMENT}};
}
static void rr_function_release(void *context) {++((ReturnWitness *)context)->frees;}
static XrXirValueStatus rr_function_admit(void *context,const XrXirFunctionBinding *binding,XrXirType type,uint64_t *work) {
    if (!work || !*work) return XR_XIR_VALUE_LIMIT;
    --*work;return binding && binding->owner==context && binding->release==rr_function_release &&
        binding->entry>=1 && binding->entry<=2 && binding->capture_count==1 && binding->captures &&
        binding->captures[0].type==257 && type==(XrXirType)RR_CALLABLE?XR_XIR_VALUE_OK:XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus rr_function(XrXirCallView *view,uint32_t entry,const XrXirValue *capture,XrXirValue *output) {
    ReturnWitness *w=(ReturnWitness *)view->environment;XrXirValueAdmission *a=xr_xir_call_admission(view);
    const XrXirFunctionBinding binding={w,rr_function_release,entry,capture,1};
    XrXirValueStatus status=xr_xir_function_new(a->domain,(XrXirTypeArena *)a->arena,(XrXirType)RR_CALLABLE,&binding,a,output);
    if (status==XR_XIR_VALUE_OK) ++w->functions;return status;
}
static XrXirValueStatus rr_wrong_root(XrXirCallView *view,const XrXirViewAccess *access) {
    ReturnState *s=view->state;ReturnWitness *w=(ReturnWitness *)view->environment;XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirViewOriginPin pin={0};
    if (w->kind==RR_SAME_ROOT) {
        XrXirViewOriginInfo info={0};TRY_VALUE(xr_xir_view_origin_pin(s->slots,access,&pin));
        TRY_VALUE(xr_xir_view_origin_info(&pin,access->values,&info));TRY_VALUE(xr_xir_value_copy(info.root,s->slots+4));
    } else {
        const XrXirValue items[]={{XR_XIR_I64,0,1},{XR_XIR_I64,0,2},{XR_XIR_I64,0,3},{XR_XIR_I64,0,4}};
        TRY_VALUE(xr_xir_array_new((XrXirType)256,items,4,access->values,s->slots+5));
        TRY_VALUE(xr_xir_cell_new(access->values->domain,(XrXirTypeArena *)access->values->arena,(XrXirType)257,s->slots+5,access->values,s->slots+4));
        xr_xir_value_drop(s->slots+5);
    }
    TRY_VALUE(xr_xir_call_view_local(view,(XrXirType)258,s->slots+4,1,2,s->slots+2));
done:xr_xir_view_origin_unpin(&pin);return status;
}
static XrXirValueStatus rr_reject(XrXirCallView *view,XrXirValue *value) {
    ReturnWitness *w=(ReturnWitness *)view->environment;XrXirValue before=*value;
    XrXirAction action={.kind=XR_XIR_ACTION_CONTINUE,.value={XR_XIR_I64,0,321}};
    XrXirCallStatus result=xr_xir_call_return_move(view,value,&action);
    if (result==XR_XIR_CALL_OOM || result==XR_XIR_CALL_LIMIT) return rr_status(result);
    CHECK(result==XR_XIR_CALL_BAD_ARGUMENT && before.type==value->type && before.payload==value->payload);
    CHECK(action.kind==XR_XIR_ACTION_CONTINUE && action.value.type==XR_XIR_I64 && action.value.payload==321);
    ++w->rejections;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus rr_leaf_prepare(XrXirCallView *view) {
    ReturnState *s=view->state;ReturnWitness *w=(ReturnWitness *)view->environment;XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirViewAccess access={0};TRY_VALUE(xr_xir_call_view_access(view,&access));
    uint32_t p=w->kind==RR_CAPTURE?1u:0u;
    if (rr_reference(w->kind)) TRY_VALUE(xr_xir_call_view_new(view,(XrXirType)(rr_mutable(w->kind)?259:258),p,0,4,s->slots));
    else TRY_VALUE(xr_xir_call_view_argument_take(view,p,s->slots));
    if (w->kind==RR_NONE_VIEW || w->kind==RR_NONE_REF) {
        if (w->kind==RR_NONE_REF) TRY_VALUE(xr_xir_call_view_drop(view,s->slots));
        if (w->kind==RR_NONE_REF) TRY_VALUE(xr_xir_nullable_new((XrXirType)260,NULL,access.values,s->slots));
        TRY_VALUE(xr_xir_value_move(s->slots,s->slots+1));goto done;
    }
    TRY_VALUE(xr_xir_call_view_reborrow(view,(XrXirType)(rr_mutable(w->kind)?259:258),s->slots,1,2,s->slots+1));
    TRY_VALUE(xr_xir_view_origin_pin(s->slots+1,&access,&w->physical));w->retired=s->slots[0];
    if (w->kind==RR_WRONG_REF || w->kind==RR_WRONG_VIEW || w->kind==RR_SAME_ROOT || w->kind==RR_WRONG_PAIR) {
        TRY_VALUE(rr_wrong_root(view,&access));
        if (w->kind!=RR_WRONG_PAIR) {TRY_VALUE(rr_reject(view,s->slots+2));TRY_VALUE(xr_xir_call_view_drop(view,s->slots+2));}
    }
    if (w->kind==RR_PAIR || w->kind==RR_WRONG_PAIR) {
        if (w->kind==RR_PAIR) TRY_VALUE(xr_xir_call_view_reborrow(view,(XrXirType)258,s->slots,2,2,s->slots+2));
        TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)RR_PAIR_TYPE,s->slots+1,2,&access,s->slots+3));
        if (w->kind==RR_WRONG_PAIR) {
            TRY_VALUE(rr_reject(view,s->slots+3));
            TRY_VALUE(xr_xir_borrowed_tuple_unpack_move(s->slots+3,s->slots+1,2,&access));
            TRY_VALUE(xr_xir_call_view_drop(view,s->slots+2));
            TRY_VALUE(xr_xir_call_view_reborrow(view,(XrXirType)258,s->slots,2,2,s->slots+2));
            TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)RR_PAIR_TYPE,s->slots+1,2,&access,s->slots+3));
        }
        TRY_VALUE(xr_xir_value_move(s->slots+3,s->slots+1));
    } else if (w->kind==RR_SOME_SHARED || w->kind==RR_SOME_MUT) {
        TRY_VALUE(xr_xir_borrowed_some_new_move(rr_result(w->kind),s->slots+1,&access,s->slots+3));
        TRY_VALUE(xr_xir_value_move(s->slots+3,s->slots+1));
    } else if (w->kind==RR_TUPLE_MUT) {
        s->slots[2]=(XrXirValue){XR_XIR_I64,0,71};
        TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)262,s->slots+1,2,&access,s->slots+3));
        TRY_VALUE(xr_xir_value_move(s->slots+3,s->slots+1));
    } else if (w->kind==RR_DAG) for (uint32_t i=0;i<RR_DAG_DEPTH;++i) {
        TRY_VALUE(xr_xir_value_copy(s->slots+1,s->slots+2));
        TRY_VALUE(xr_xir_borrowed_tuple_new_move((XrXirType)(RR_DAG_BASE+i),s->slots+1,2,&access,s->slots+3));
        TRY_VALUE(xr_xir_value_move(s->slots+3,s->slots+1));
    }
    TRY_VALUE(xr_xir_call_view_drop(view,s->slots));
done:return status;
}
static XrXirAction rr_child(XrXirCallView *view) {
    ReturnState *s=view->state;ReturnWitness *w=(ReturnWitness *)view->environment;
    if (view->phase==XR_XIR_CALL_EXIT) {++w->exits;return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};}
    uint32_t entry=xr_xir_call_current_entry(view->activation);
    if (!s->pc++) {
        ++w->children;
        if (entry==1 && w->depth==2) {
            if (w->kind==RR_CAPTURE) {
                w->failure=rr_function(view,2,view->arguments,s->slots+4);
                if (w->failure!=XR_XIR_VALUE_OK) return rr_failure(w->failure);
            }
            return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=2,.arguments=view->arguments+(w->kind==RR_CAPTURE),
                .argument_count=1,.value=w->kind==RR_CAPTURE?s->slots[4]:(XrXirValue){0}};
        }
        w->failure=rr_leaf_prepare(view);
        if (w->failure!=XR_XIR_VALUE_OK) return rr_failure(w->failure);
        return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    }
    if (entry==1 && w->depth==2) {
        w->failure=xr_xir_call_view_inbox_take(view,rr_result(w->kind),s->slots+1);
        if (w->failure!=XR_XIR_VALUE_OK) return rr_failure(w->failure);
    }
    XrXirAction action={0};XrXirCallStatus result=xr_xir_call_return_move(view,s->slots+1,&action);
    w->failure=rr_status(result);if (w->failure!=XR_XIR_VALUE_OK) return rr_failure(w->failure);
    ++w->staged;CHECK(!s->slots[1].type);
    CHECK(xr_xir_call_view_reborrow(view,(XrXirType)258,&action.value,0,1,s->slots+2)==XR_XIR_VALUE_BAD_ARGUMENT);
    return action;
}
static XrXirValueStatus rr_parent_prepare(XrXirCallView *view) {
    ReturnState *s=view->state;ReturnWitness *w=(ReturnWitness *)view->environment;XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirValueAdmission *a=xr_xir_call_admission(view);
    if (rr_local(w->kind)) TRY_VALUE(xr_xir_cell_new(a->domain,(XrXirTypeArena *)a->arena,(XrXirType)257,view->arguments,a,s->slots+4));
    if (rr_reference(w->kind)) goto done;
    if (w->kind==RR_NONE_VIEW) {TRY_VALUE(xr_xir_nullable_new((XrXirType)260,NULL,a,s->slots+3));goto done;}
    if (rr_local(w->kind)) TRY_VALUE(xr_xir_call_view_local(view,rr_input(w->kind),s->slots+4,0,4,s->slots+3));
    else TRY_VALUE(xr_xir_call_view_new(view,rr_input(w->kind),0,0,4,s->slots+3));
    w->original=s->slots[3];
    if (w->kind==RR_REJECT_ALIAS) TRY_VALUE(xr_xir_value_copy(s->slots+3,s->slots+4));
    if (w->kind==RR_CAPTURE) {
        TRY_VALUE(xr_xir_call_cell_read(view,view->arguments,s->slots+5));
        TRY_VALUE(xr_xir_cell_new(a->domain,(XrXirTypeArena *)a->arena,(XrXirType)257,s->slots+5,a,s->slots+4));
        xr_xir_value_drop(s->slots+5);TRY_VALUE(rr_function(view,1,s->slots+4,s->slots+2));
    }
done:return status;
}
static XrXirValueStatus rr_parent_take(XrXirCallView *view) {
    ReturnState *s=view->state;ReturnWitness *w=(ReturnWitness *)view->environment;XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirViewAccess access={0};TRY_VALUE(xr_xir_call_view_access(view,&access));
    TRY_VALUE(xr_xir_call_view_inbox_take(view,rr_result(w->kind),s->slots));++w->received;
    CHECK(!view->inbox.value.type && view->inbox.status==XR_XIR_CALL_READY);
    if (w->kind==RR_NONE_VIEW || w->kind==RR_NONE_REF) {
        bool some=true;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(s->slots,&some,&payload) && !some && !payload);
    } else {
        if (w->kind==RR_SOME_SHARED || w->kind==RR_SOME_MUT) {
            TRY_VALUE(xr_xir_borrowed_some_take(s->slots,&access,s->slots+1));TRY_VALUE(xr_xir_value_move(s->slots+1,s->slots));
        } else if (w->kind==RR_PAIR || w->kind==RR_WRONG_PAIR || w->kind==RR_TUPLE_MUT) {
            TRY_VALUE(xr_xir_borrowed_tuple_unpack_move(s->slots,s->slots+1,2,&access));
            if (w->kind!=RR_TUPLE_MUT) {
                XrXirFaultDetail fault={0};TRY_VALUE(xr_xir_view_get(s->slots+2,0,&access,s->slots+5,&fault));
                CHECK(s->slots[5].payload==33);xr_xir_value_drop(s->slots+5);TRY_VALUE(xr_xir_call_view_drop(view,s->slots+2));
            } else {CHECK(s->slots[2].payload==71);xr_xir_value_drop(s->slots+2);}
            TRY_VALUE(xr_xir_value_move(s->slots+1,s->slots));
        } else if (w->kind==RR_DAG) for (uint32_t i=0;i<RR_DAG_DEPTH;++i) {
            TRY_VALUE(xr_xir_tuple_get(s->slots,0,s->slots+1));xr_xir_value_drop(s->slots);
            TRY_VALUE(xr_xir_value_move(s->slots+1,s->slots));
        }
        int64_t length=0;XrXirFaultDetail fault={0};const XrXirValue replacement={XR_XIR_I64,0,77};
        CHECK(xr_xir_view_len(&w->retired,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
        TRY_VALUE(xr_xir_view_len(s->slots,&access,&length));CHECK(length==2);
        TRY_VALUE(xr_xir_view_get(s->slots,0,&access,s->slots+5,&fault));CHECK(s->slots[5].payload==22);xr_xir_value_drop(s->slots+5);
        if (rr_mutable(w->kind)) TRY_VALUE(xr_xir_view_set(s->slots,0,&replacement,&access,&fault));
        if (w->original.type==259) CHECK(xr_xir_view_set(&w->original,0,&replacement,&access,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    }
    TRY_VALUE(xr_xir_value_move(s->slots,&w->kept));
done:return status;
}
static XrXirValueStatus rr_parent_finish(XrXirCallView *view) {
    ReturnState *s=view->state;ReturnWitness *w=(ReturnWitness *)view->environment;XrXirValueStatus status=XR_XIR_VALUE_OK;
    TRY_VALUE(xr_xir_value_move(&w->kept,s->slots));TRY_VALUE(xr_xir_call_view_drop(view,s->slots));
    if (s->slots[3].type) TRY_VALUE(xr_xir_call_view_drop(view,s->slots+3));
    const XrXirValue *root=rr_local(w->kind)?s->slots+4:view->arguments;
    TRY_VALUE(xr_xir_call_cell_read(view,root,s->slots+5));XrXirFaultDetail fault={0};
    TRY_VALUE(xr_xir_array_get(s->slots+5,1,xr_xir_call_admission(view),s->slots,&fault));
    CHECK(s->slots[0].payload==(rr_mutable(w->kind)?77:22));
    xr_xir_value_drop(s->slots);xr_xir_value_drop(s->slots+5);
done:return status;
}
static XrXirAction rr_parent(XrXirCallView *view) {
    ReturnState *s=view->state;ReturnWitness *w=(ReturnWitness *)view->environment;
    if (view->phase==XR_XIR_CALL_EXIT) {++w->exits;return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};}
    uint32_t pc=s->pc++;
    if (!pc) {
        ++w->entered;w->failure=rr_parent_prepare(view);if (w->failure!=XR_XIR_VALUE_OK) return rr_failure(w->failure);
        w->baseline=w->accounting->live_bytes;
        return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=1,
            .arguments=w->kind==RR_REJECT_ZERO?NULL:rr_reference(w->kind)?(rr_local(w->kind)?s->slots+4:view->arguments):s->slots+3,
            .argument_count=w->kind==RR_REJECT_ZERO?0u:w->kind==RR_REJECT_ALIAS?2u:1u,.value=w->kind==RR_CAPTURE?s->slots[2]:(XrXirValue){0}};
    }
    if (w->kind==RR_DISCARD_SHARED || w->kind==RR_DISCARD_MUT) {
        XrXirCallStatus result=xr_xir_call_discard_inbox(view,rr_result(w->kind));
        if (result!=XR_XIR_CALL_READY) {w->failure=rr_status(result);return rr_failure(w->failure);}
        CHECK(w->accounting->live_bytes==w->baseline);
        if (w->physical.body) {
            XrXirViewOriginInfo info={0};w->failure=xr_xir_view_origin_info(&w->physical,xr_xir_call_admission(view),&info);
            if (w->failure!=XR_XIR_VALUE_OK) return rr_failure(w->failure);
            CHECK(!info.logical_active);xr_xir_view_origin_unpin(&w->physical);
        }
        if (++w->discarded<5) return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=1,.arguments=view->arguments,.argument_count=1};
        return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,91}};
    }
    if (pc==1) {
        w->failure=rr_parent_take(view);if (w->failure!=XR_XIR_VALUE_OK) return rr_failure(w->failure);
        return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    }
    if (w->finish==RR_PANIC_ADOPTED) return xr_xir_call_fault(XR_XIR_RUN_DIVIDE_BY_ZERO);
    w->failure=rr_parent_finish(view);if (w->failure!=XR_XIR_VALUE_OK) return rr_failure(w->failure);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,91}};
}
static void rr_release(XrXirCallView *view,XrXirCallStatus reason) {
    ReturnState *s=view->state;ReturnWitness *w=(ReturnWitness *)view->environment;(void)reason;
    CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_BUSY);++w->releases;
    for (uint32_t i=0;i<6;++i) xr_xir_value_drop(s->slots+i);
}
static XrXirCellParameterRole rr_role(void *context,uint32_t entry,uint32_t parameter) {
    ReturnWitness *w=context;
    if (!entry) return !parameter && !rr_local(w->kind)?XR_XIR_CELL_ROLE_SCOPED_REF:XR_XIR_CELL_ROLE_UNKNOWN;
    if (w->kind==RR_CAPTURE && !parameter) return XR_XIR_CELL_ROLE_OWNED_CAPTURE;
    return rr_reference(w->kind) && !parameter?XR_XIR_CELL_ROLE_SCOPED_REF:XR_XIR_CELL_ROLE_UNKNOWN;
}
static XrXirValueStatus rr_arena(XrCompileResources *resources,XrXirTypeArena **output) {
    XrXirCallableParameter fields[RR_DAG_DEPTH+1][2]={{{0}}},parameter={(XrXirType)258,XR_PARAM_READ};
    XrXirTypeNode nodes[78]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)258},{.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)259}};
    fields[0][0].type=(XrXirType)259;fields[0][1].type=XR_XIR_I64;
    nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[0],.parameter_count=2};
    for (uint32_t i=0;i<RR_DAG_DEPTH;++i) {
        fields[i+1][0].type=fields[i+1][1].type=(XrXirType)(i?RR_DAG_BASE+i-1:258);
        nodes[7+i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[i+1],.parameter_count=2};
    }
    nodes[77]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&parameter,.parameter_count=1,.result=(XrXirType)258,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    const XrXirTypes types={nodes,78,NULL,NULL};const XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    return xr_xir_compile_type_arena_new(&context,&types,output);
}
static XrXirValueStatus rr_sequence(XrCompileResources *resources,uint32_t kind,uint32_t finish,uint32_t depth) {
    XrXirDomain *domain=NULL;XrXirTypeArena *arena=NULL;XrXirCall *call=NULL;XrXirValue values[3]={{0}};
    ReturnWitness w={.kind=kind,.finish=finish,.depth=depth};XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirCallAccounting accounting={0};XrXirCallBudget budget={.byte_limit=65536,.requested_limit=65536,.resume_limit=128};
    w.accounting=&accounting;
    const XrXirDomainBudgetControls controls={1048576,1048576,4194304,65536,65536};
    TRY_VALUE(rr_arena(resources,&arena));TRY_VALUE(xr_xir_compile_domain_new(resources,1048576,&domain));
    TRY_VALUE(xr_xir_domain_budget_bind(domain,&controls));budget.work_domain=domain;
    XrXirValueAdmission admission={arena,domain,rr_function_admit,&w,4194304,65536};
    const XrXirValue items[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22},{XR_XIR_I64,0,33},{XR_XIR_I64,0,44}};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,items,4,&admission,values));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,values,&admission,values+1));
    XrXirType parent_type=(XrXirType)(rr_local(kind)?256:257),child_types[2]={(XrXirType)257,rr_input(kind)};
    const XrXirType *parameters=kind==RR_CAPTURE?child_types:child_types+1;uint32_t count=kind==RR_CAPTURE?2u:1u;
    if (kind==RR_REJECT_ZERO) {parameters=NULL;count=0;}
    else if (kind==RR_REJECT_ALIAS) {child_types[0]=child_types[1]=(XrXirType)258;parameters=child_types;count=2;}
    XrXirCallEntry entries[3]={
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=&parent_type,.parameter_count=1,.result=XR_XIR_I64,.state_bytes=sizeof(ReturnState),
         .resume=rr_parent,.release=rr_release,.environment=&w,.flags=XR_XIR_ENTRY_EXIT},
        {.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=parameters,.parameter_count=count,.result=rr_result(kind),.state_bytes=sizeof(ReturnState)+4096,
         .resume=rr_child,.release=rr_release,.environment=&w,.flags=XR_XIR_ENTRY_EXIT}};entries[2]=entries[1];
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries;config.entry_count=3;config.accounting=&accounting;config.admission=admission;config.byte_limit=65536;
    XrXirCallRequest request={0,values+(rr_local(kind)?0:1),1,rr_role,&w};
    XrXirCallStatus terminal=xr_xir_call_new_budgeted(&config,&request,&budget,&call);
    if (terminal!=XR_XIR_CALL_READY) {status=rr_status(terminal);goto done;}
    bool cancelled=false;uint32_t polls=0;
    do {
        XrXirCallResult result=xr_xir_call_poll_bounded(call,1);terminal=result.status;CHECK(++polls<96);
        bool cancel=!cancelled && ((finish==RR_CANCEL_CHILD && terminal==XR_XIR_CALL_SUSPENDED && !w.received) ||
            (finish==RR_CANCEL_STAGED && w.staged && !w.received) || (finish==RR_CANCEL_ADOPTED && w.received));
        if (cancel && (terminal==XR_XIR_CALL_READY || terminal==XR_XIR_CALL_SUSPENDED)) {
            CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);cancelled=true;
        }
        if (terminal==XR_XIR_CALL_SUSPENDED) {
            if (!cancelled) CHECK(xr_xir_call_resume(call,result.wake)==XR_XIR_CALL_READY);
            terminal=XR_XIR_CALL_READY;
        }
    } while (terminal==XR_XIR_CALL_READY);
    if (terminal==XR_XIR_CALL_OOM || terminal==XR_XIR_CALL_LIMIT) {status=rr_status(terminal);goto done;}
    if (kind==RR_REJECT_ZERO || kind==RR_REJECT_ALIAS) {
        CHECK(terminal==XR_XIR_CALL_BAD_ARGUMENT && w.entered==1 && !w.children && !w.staged && !w.received && !w.exits && w.releases==1);
        CHECK(xr_xir_cell_unborrowed(values+1));goto done;
    }
    CHECK(w.failure==XR_XIR_VALUE_OK && terminal==(finish==RR_RETURN?XR_XIR_CALL_RETURNED:finish==RR_PANIC_ADOPTED?XR_XIR_CALL_DIVIDE_BY_ZERO:XR_XIR_CALL_CANCELLED));
    bool discard=kind==RR_DISCARD_SHARED || kind==RR_DISCARD_MUT;
    uint32_t children=discard?depth*5u:depth;
    CHECK(w.entered==1 && w.children==children && w.exits==children+1 && w.releases==children+1);
    CHECK(w.received==(discard || finish==RR_CANCEL_CHILD || finish==RR_CANCEL_STAGED?0u:1u));
    CHECK(w.discarded==(discard?5u:0u));
    if (terminal==XR_XIR_CALL_RETURNED) {CHECK(xr_xir_call_take_result(call,values+2)==terminal && values[2].payload==91);xr_xir_value_drop(values+2);}
    CHECK(xr_xir_cell_unborrowed(values+1));
    if (w.physical.body) {XrXirViewOriginInfo info={0};TRY_VALUE(xr_xir_view_origin_info(&w.physical,&admission,&info));CHECK(!info.logical_active);}
    XrXirFaultDetail fault={0};TRY_VALUE(xr_xir_array_get(values,1,&admission,values+2,&fault));CHECK(values[2].payload==22);xr_xir_value_drop(values+2);
done:
    if (call) CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees && w.functions==w.frees);
    size_t before=value_compile_calls,runtime_before=calls;xr_xir_view_origin_unpin(&w.physical);xr_xir_value_drop(&w.kept);
    for (uint32_t i=0;i<3;++i) xr_xir_value_drop(values+i);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(before==value_compile_calls && runtime_before==calls && !live);return status;
}
static void rr_resources(uint32_t kind,uint32_t finish,uint32_t depth) {
    XrCompileResourceLimits limits=compile_domain_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);size_t start=value_compile_calls;
    CHECK(rr_sequence(resources,kind,finish,depth)==XR_XIR_VALUE_OK);size_t points=value_compile_calls-start;
    XrCompileResourceStats measured=compile_domain_stats(resources);
    CHECK(rr_sequence(resources,kind,finish,depth)==XR_XIR_VALUE_OK);xr_compile_resources_release(resources);resources=NULL;
    CHECK(!value_compile_live && !value_compile_bytes && !live && points>10 && points<512);
    for (size_t point=0;point<points;++point) {
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);value_compile_injected=false;value_compile_fail_at=value_compile_calls+point;
        CHECK(rr_sequence(resources,kind,finish,depth)==XR_XIR_VALUE_OOM && value_compile_injected);
        value_compile_fail_at=SIZE_MAX;xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t adjustment=0;adjustment<3;++adjustment) {
        XrCompileResourceLimits bound=limits;uint64_t *limit=axis==0?&bound.allocated_bytes:axis==1?&bound.live_bytes:&bound.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+adjustment-1u;
        CHECK(xr_compile_resources_new(&bound,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(rr_sequence(resources,kind,finish,depth)==(adjustment?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        xr_compile_resources_release(resources);resources=NULL;CHECK(!value_compile_live && !value_compile_bytes && !live);
    }
    printf("View returns kind=%u finish=%u depth=%u: %zu complete allocation failures, same owner2/three axes exact+minus1+plus1/physical0 PASS\n",kind,finish,depth,points);
}
static void rr_run_case(bool normal,uint32_t kind,uint32_t finish,uint32_t depth) {
    if (!normal) {rr_resources(kind,finish,depth);return;}
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits=compile_domain_limits();
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirValueStatus status=rr_sequence(resources,kind,finish,depth);
    printf("normal kind=%u finish=%u depth=%u status=%u\n",kind,finish,depth,(unsigned)status);fflush(stdout);
    CHECK(status==XR_XIR_VALUE_OK);xr_compile_resources_release(resources);CHECK(!value_compile_live && !value_compile_bytes && !live);
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);
    for (uint32_t kind=0;kind<RR_DISCARD_SHARED;++kind) for (uint32_t depth=1;depth<=2;++depth) for (uint32_t finish=0;finish<RR_FINISHES;++finish)
        rr_run_case(normal,kind,finish,depth);
    for (uint32_t kind=RR_DISCARD_SHARED;kind<=RR_DISCARD_MUT;++kind) for (uint32_t depth=1;depth<=2;++depth)
        rr_run_case(normal,kind,RR_RETURN,depth);
    for (uint32_t kind=RR_REJECT_ZERO;kind<RR_KINDS;++kind) rr_run_case(normal,kind,RR_RETURN,1);
    puts("Actual returned leaves, unique signature/captures, ref detachment/shared rebinding, freed callee and all exits PASS");return 0;
}
