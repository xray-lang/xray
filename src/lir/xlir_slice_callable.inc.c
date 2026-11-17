/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_slice_callable.inc.c - Substitute completed effects at actual captures
 */
typedef enum LirSliceCallableCheck {
    LIR_SLICE_CALLABLE_SAFE, LIR_SLICE_CALLABLE_CONFLICT, LIR_SLICE_CALLABLE_UNKNOWN
} LirSliceCallableCheck;
typedef struct LirSliceActuals {
    const uint32_t *captures, *arguments;
    uint32_t capture_count, argument_count, parameter_count, state;
} LirSliceActuals;
typedef struct LirSliceCallable {
    const XrLirModule *module;
    const XrLirFunctionData *function;
    LirSliceFlow *flow;
    XrLirSliceFactsView effects;
    uint32_t function_id;
    size_t first, count;
} LirSliceCallable;

static uint32_t lir_slice_actual(LirSliceActuals actuals,uint32_t parameter) {
    if (parameter >= actuals.parameter_count) return 0;
    if (parameter < actuals.capture_count) return actuals.captures[parameter];
    if (actuals.state && parameter == actuals.parameter_count - 1) return actuals.state;
    parameter -= actuals.capture_count;
    return parameter < actuals.argument_count ? actuals.arguments[parameter] : 0;
}
static const XrLirCallableValueFact *lir_slice_constructor(LirSliceCallable *c,uint32_t value,
    uint32_t earlier_than,LirSliceActuals *actuals) {
    const XrLirCallableFunctionFacts *facts=&c->flow->callables;
    if (!value || value > facts->value_count || !facts->values) return NULL;
    const XrLirCallableValueFact *alias=&facts->values[value];
    uint32_t origin=alias->origin;
    if (alias->kind != XR_LIR_SCOPED_VALUE_FUNCTION || !alias->known_target ||
        !origin || origin > facts->value_count || (earlier_than && origin >= earlier_than)) return NULL;
    const XrLirCallableValueFact *row=&facts->values[origin];
    const XrLirInstructionView *op=xr_lir_internal_value_definition(c->function,origin);
    if (row->operation != XR_LIR_FUNCTION_REF || !op || op->op != XR_LIR_FUNCTION_REF ||
        row->known_target != alias->known_target || op->callee != row->known_target ||
        !row->known_target || row->known_target > c->module->function_count ||
        row->first_capture > facts->capture_count || row->capture_count > facts->capture_count-row->first_capture ||
        row->capture_count != op->operand_count) return NULL;
    const XrLirFunctionData *target=&c->module->functions[row->known_target-1];
    if (target->kind != XR_LIR_FUNCTION_DEFINITION || !target->closure.callable ||
        row->capture_count != target->closure.capture_count + target->closure.state_capture) return NULL;
    for (uint32_t p=0;p<row->capture_count;++p)
        if (facts->captures[row->first_capture+p] != op->operands[p]) return NULL;
    *actuals=(LirSliceActuals){.captures=row->capture_count ? facts->captures+row->first_capture : NULL,
        .capture_count=target->closure.capture_count,.parameter_count=target->parameter_count,
        .state=target->closure.state_capture ? facts->captures[row->first_capture+target->closure.capture_count] : 0};
    return row;
}
static bool lir_slice_conditional_formal(const LirSliceCallable *c,uint32_t value,uint32_t earlier_than) {
    const XrLirCallableFunctionFacts *facts=&c->flow->callables;
    if (!c->effects.callback_loans_complete || !facts->values || !value || value>facts->value_count) return false;
    const XrLirCallableValueFact *alias=&facts->values[value];
    uint32_t origin=alias->origin;
    if (alias->kind!=XR_LIR_SCOPED_VALUE_FUNCTION || alias->known_target ||
        alias->transport!=XR_LIR_CALLABLE_TRANSPORT_BORROW || !origin || origin>facts->value_count ||
        (earlier_than && origin>=earlier_than)) return false;
    const XrLirCallableValueFact *formal=&facts->values[origin];
    uint32_t parameter=formal->parameter;
    if (formal->kind!=XR_LIR_SCOPED_VALUE_FUNCTION || formal->block || !parameter ||
        parameter>c->function->parameter_count || formal->persistent || formal->known_target ||
        formal->transport!=XR_LIR_CALLABLE_TRANSPORT_BORROW ||
        c->function->parameters[parameter-1]!=origin || parameter-1<c->function->closure.capture_count) return false;
    XrLirCallableType signature;
    if (xr_lir_callable_type_view(c->module,formal->type,&signature)!=XR_LIR_OK ||
        (signature.flags&XR_LIR_CALLABLE_C_ABI)) return false;
    XrLirSliceFunctionFact own;
    if (xr_lir_internal_slice_function_fact(c->module,c->function_id,&own)!=XR_LIR_OK || !own.available ||
        own.parameter_count!=c->function->parameter_count || own.callback_loans.first>c->effects.callback_loan_count ||
        own.callback_loans.count>c->effects.callback_loan_count-own.callback_loans.first ||
        (own.callback_loans.count && !c->effects.callback_loans)) return false;
    bool live=false;
    for (size_t n=0;n<c->flow->count;++n) if (c->flow->scratch[n]&2u) {
        LirSliceRoot root=c->flow->roots[n];uint32_t loan=0;
        if (root.kind!=LIR_SLICE_ROOT_INPUT || !root.identity || root.identity>c->function->value_count) return false;
        while (loan<c->function->parameter_count && c->function->parameters[loan]!=root.identity) ++loan;
        if (loan==c->function->parameter_count ||
            !xr_lir_internal_slice_type(c->module,c->function->parameter_types[loan])) return false;
        XrLirSliceEffectKind kind=loan<c->function->closure.capture_count ?
            XR_LIR_SLICE_EFFECT_CAPTURE:XR_LIR_SLICE_EFFECT_PARAMETER;
        bool found=false;
        for (size_t p=0;p<own.callback_loans.count;++p) {
            XrLirSliceCallbackLoan pair=c->effects.callback_loans[own.callback_loans.first+p];
            found|=pair.callback_parameter==parameter-1 && pair.loan_kind==kind && pair.loan_parameter==loan;
        }
        if (!found) return false;
        live=true;
    }
    /* This is a condition on actual callers, not an empty callable body. Each
     * live loan must independently name an entry whose obligation is exported. */
    return live;
}
static bool lir_slice_enqueue_callable(LirSliceCallable *c,uint32_t value,uint32_t earlier_than) {
    LirSliceActuals actuals;
    const XrLirCallableValueFact *row=lir_slice_constructor(c,value,earlier_than,&actuals);
    if (!row) return lir_slice_conditional_formal(c,value,earlier_than);
    uint32_t origin=c->flow->callables.values[value].origin;
    if (!c->flow->callable_seen[origin]) {
        XR_CHECK(c->count < c->flow->callables.value_count,
            "each actual callable constructor enters the bounded effect queue once");
        c->flow->callable_seen[origin]=1;
        c->flow->callable_queue[c->count++]=origin;
    }
    return true;
}
static LirSliceRoot lir_slice_actual_root(const LirSliceCallable *c,uint32_t value) {
    if (!value || value > c->function->value_count) return (LirSliceRoot){0};
    uint32_t slice=c->flow->values[value];
    if (slice) return c->flow->roots[slice-1];
    LirSliceRoot root=c->flow->bindings[value];
    if (root.kind) return root;
    /* A real scalar constructor owns different storage from every real Array
     * constructor. A formal, selected handle or type alone proves nothing. */
    const XrLirInstructionView *op=xr_lir_internal_value_definition(c->function,value);
    if (c->function->values[value-1].type == XR_LIR_I64_CELL && op &&
        op->op == XR_LIR_CELL_NEW && op->type == XR_LIR_I64_CELL)
        return (LirSliceRoot){LIR_SLICE_ROOT_LOCAL,value};
    return (LirSliceRoot){0};
}
static LirSliceCallableCheck lir_slice_unknown_arguments(const LirSliceCallable *c,const XrLirInstructionView *v) {
    for (uint32_t p=1;p<v->operand_count;++p) {
        uint32_t value=v->operands[p];XrLirType type=c->function->values[value-1].type;XrLirCellType cell;
        if (type!=XR_LIR_I64_CELL && type!=XR_LIR_ARRAY_CELL && type!=XR_LIR_ARRAY_REF &&
            type!=XR_LIR_STATE && type!=XR_LIR_STATE_REF &&
            xr_lir_cell_value_type(c->module,type,&cell)!=XR_LIR_OK) continue;
        LirSliceRoot root=type==XR_LIR_STATE ? (LirSliceRoot){LIR_SLICE_ROOT_STATE_ANY,0} : lir_slice_actual_root(c,value);
        if (!root.kind) return LIR_SLICE_CALLABLE_UNKNOWN;
        for (size_t n=0;n<c->flow->count;++n)
            if ((c->flow->scratch[n]&2u) && lir_slice_root_alias(c->flow->roots[n],root,false))
                return LIR_SLICE_CALLABLE_CONFLICT;
    }
    return LIR_SLICE_CALLABLE_SAFE;
}
static LirSliceCallableCheck lir_slice_apply_callable(LirSliceCallable *c,uint32_t function,
    LirSliceActuals actuals,uint32_t constructor) {
    XrLirSliceFunctionFact summary;
    if (xr_lir_internal_slice_function_fact(c->module,function,&summary) != XR_LIR_OK ||
        !summary.available || summary.parameter_count != actuals.parameter_count) return LIR_SLICE_CALLABLE_UNKNOWN;
    const XrLirFunctionData *target=&c->module->functions[function-1];
    /* Ordinary ref parameters have their own exclusive loan. Module WRITE
     * intentionally does not describe that loan, so it cannot erase it. */
    for (uint32_t p=0;p<target->parameter_count;++p) {
        XrLirType type=target->parameter_types[p];
        if (type != XR_LIR_I64_CELL && type != XR_LIR_ARRAY_CELL && type != XR_LIR_ARRAY_REF) continue;
        LirSliceRoot root=lir_slice_actual_root(c,lir_slice_actual(actuals,p));
        if (!root.kind) return LIR_SLICE_CALLABLE_UNKNOWN;
        if (lir_slice_write_conflict(c->flow,root)) return LIR_SLICE_CALLABLE_CONFLICT;
    }
    for (size_t n=0;n<summary.write.count;++n) {
        XrLirSliceEffect term=c->effects.terms[summary.write.first+n];
        if (term.kind == XR_LIR_SLICE_EFFECT_CALLBACK) {
            if (!lir_slice_enqueue_callable(c,lir_slice_actual(actuals,term.identity),constructor))
                return LIR_SLICE_CALLABLE_UNKNOWN;
            continue;
        }
        LirSliceRoot root={0};
        if (term.kind == XR_LIR_SLICE_EFFECT_SLOT) root=(LirSliceRoot){LIR_SLICE_ROOT_STATE,term.identity};
        else if (term.kind == XR_LIR_SLICE_EFFECT_CAPTURE || term.kind == XR_LIR_SLICE_EFFECT_PARAMETER)
            root=lir_slice_actual_root(c,lir_slice_actual(actuals,term.identity));
        if (!root.kind) return LIR_SLICE_CALLABLE_UNKNOWN;
        if (lir_slice_write_conflict(c->flow,root)) return LIR_SLICE_CALLABLE_CONFLICT;
    }
    return LIR_SLICE_CALLABLE_SAFE;
}
static LirSliceCallableCheck lir_slice_callable_check(const XrLirModule *m,const XrLirFunctionData *f,uint32_t function,
    const XrLirInstructionView *v,LirSliceFlow *flow) {
    if (!flow->callable_queue || !flow->callable_seen) return LIR_SLICE_CALLABLE_UNKNOWN;
    LirSliceCallable c={.module=m,.function=f,.flow=flow,.function_id=function};
    if (xr_lir_slice_facts_view(m->slice_facts,&c.effects) != XR_LIR_OK) return LIR_SLICE_CALLABLE_UNKNOWN;
    LirSliceCallableCheck status;
    /* Module-only WRITE is not proof that a local capture is empty. Both call
     * forms check actual callbacks; an indirect receiver is applied once below. */
    uint32_t first=v->op == XR_LIR_CALL_INDIRECT ? 1u : 0u;
    for (uint32_t p=first;p<v->operand_count;++p) {
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(m,f->values[v->operands[p]-1].type,&signature) == XR_LIR_OK &&
            !lir_slice_enqueue_callable(&c,v->operands[p],0)) {
            status=LIR_SLICE_CALLABLE_UNKNOWN;goto done;
        }
    }
    if (v->op == XR_LIR_CALL) {
        LirSliceActuals actuals={.arguments=v->operands,.argument_count=v->operand_count,
            .parameter_count=m->functions[v->callee-1].parameter_count};
        status=lir_slice_apply_callable(&c,v->callee,actuals,0);
    } else {
        LirSliceActuals actuals;
        const XrLirCallableValueFact *row=lir_slice_constructor(&c,v->operands[0],0,&actuals);
        if (!row) {
            status=lir_slice_unknown_arguments(&c,v);
            if (status==LIR_SLICE_CALLABLE_SAFE && !lir_slice_conditional_formal(&c,v->operands[0],0))
                status=LIR_SLICE_CALLABLE_UNKNOWN;
        } else {
            actuals.arguments=v->operands+1; actuals.argument_count=v->operand_count-1;
            status=lir_slice_apply_callable(&c,row->known_target,actuals,flow->callables.values[v->operands[0]].origin);
        }
    }
    while (status == LIR_SLICE_CALLABLE_SAFE && c.first < c.count) {
        uint32_t value=flow->callable_queue[c.first++]; LirSliceActuals actuals;
        const XrLirCallableValueFact *row=lir_slice_constructor(&c,value,0,&actuals);
        if (!row) {status=LIR_SLICE_CALLABLE_UNKNOWN;break;}
        status=lir_slice_apply_callable(&c,row->known_target,actuals,value);
    }
done:
    /* Reset only touched constructors, not every SSA value at every call. */
    for (size_t n=0;n<c.count;++n) flow->callable_seen[flow->callable_queue[n]]=0;
    return status;
}
