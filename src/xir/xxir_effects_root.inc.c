/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effects_root.inc.c - Independent root facts on the shared execution graph
 *
 * KEY CONCEPT:
 *   State authority and missing execution proof never erase one another.
 *   Shortest owned causes do not borrow artifacts or depend on edge insertion.
 */
XR_FUNC const XrXirRootEffects *xr_xir_effects_root(const XrXirEffects *effects, uint32_t function) {
    return effects && function < effects->count ? &effects->root[function] : NULL;
}
XR_FUNC const XrXirRootEffectWitness *xr_xir_effects_root_witness(const XrXirEffects *effects, uint32_t function) {
    return effects && function < effects->count && effects->root_witnesses[function].cause ?
        &effects->root_witnesses[function] : NULL;
}
XR_FUNC const XrXirRootEffectWitness *xr_xir_effects_unresolved_witness(const XrXirEffects *effects, uint32_t function) {
    return effects && function < effects->count && effects->unresolved_witnesses[function].cause ?
        &effects->unresolved_witnesses[function] : NULL;
}
static bool effect_root_cause_before(const XrXirRootEffectWitness *a, const XrXirRootEffectWitness *b) {
    if (a->instruction != b->instruction) return a->instruction < b->instruction;
    if (a->callee != b->callee) return a->callee < b->callee;
    if (a->cause != b->cause) return a->cause < b->cause;
    return a->slot < b->slot;
}
static void effect_root_terminal(XrXirRootEffectWitness *witness,
    XrXirRootEffectCause cause, uint32_t instruction, uint32_t slot) {
    XrXirRootEffectWitness candidate = {cause,instruction,UINT32_MAX,slot,0};
    if (!witness->cause || effect_root_cause_before(&candidate,witness)) *witness = candidate;
}
static XrXirStatus effect_root_slot(const XrXirModule *module, XrXirEffects *effects,
    uint32_t f, uint32_t i, const XrXirCompileContext *work) {
    const XrXirInstruction *op = &module->functions[f].instructions[i];
    const XrXirSlot *slot = &module->declarations->slots[op->immediate];
    XrXirRootEffectCause cause = XR_XIR_ROOT_CAUSE_MUTABLE_SLOT;
    if (!slot->mutable) {
        XrXirProofContext proof = {module,{XR_XIR_CONTEXT_FUNCTION,f,0}};
        XrXirStatus status = xr_xir_compile_type_markers_prove(work,&proof,slot->type,XR_XIR_CONSTRAINT_SENDABLE);
        if (status == XR_XIR_OK) return XR_XIR_OK;
        if (status != XR_XIR_BAD_TYPE) return status;
        cause = XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST;
    }
    effects->root[f].requires_root = true;
    effect_root_terminal(&effects->root_witnesses[f],cause,i,(uint32_t)op->immediate);
    return XR_XIR_OK;
}

struct XrXirRootCauseTrace {
    XrXirRootEffects facts;
    uint32_t counts[2];
};
_Static_assert(sizeof(XrXirRootCauseTrace) % _Alignof(XrXirRootCauseStep) == 0,
    "Trailing owned cause records must remain aligned");
static XrXirRootCauseStep *effect_root_trace_storage(XrXirRootCauseTrace *trace) {
    return (XrXirRootCauseStep *)(trace + 1);
}
XR_FUNC const XrXirRootEffects *xr_xir_root_cause_trace_facts(const XrXirRootCauseTrace *trace) {
    return trace ? &trace->facts : NULL;
}
XR_FUNC const XrXirRootCauseStep *xr_xir_root_cause_trace_steps(
    const XrXirRootCauseTrace *trace, bool unresolved, uint32_t *count) {
    if (!trace || !count) return NULL;
    *count = trace->counts[unresolved ? 1 : 0];
    return *count ? (const XrXirRootCauseStep *)(trace + 1) + (unresolved ? trace->counts[0] : 0) : NULL;
}
XR_FUNC void xr_xir_compile_root_cause_trace_free(XrXirRootCauseTrace *trace) {
    xr_compile_resources_free(trace);
}
static bool effect_root_trace_terminal(const XrXirEffects *effects, uint32_t function,
    const XrXirRootEffectWitness *witness, bool unresolved) {
    XrXirRootEffectCause cause = witness->cause;
    if (cause==XR_XIR_ROOT_CAUSE_PARAMETER) {
        uint32_t owner=witness->callee==UINT32_MAX ? function : witness->callee;
        return effects->contracts && effects->formula_terminals && owner<effects->count &&
            witness->instruction!=UINT32_MAX && effects->contracts[owner].parameters &&
            witness->slot<effects->contracts[owner].parameter_count &&
            effects->contracts[owner].parameters[witness->slot].kind==XR_XIR_EFFECT_PARAMETER_VARIABLE;
    }
    if (cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL)
        return effects->contracts && effects->formula_terminals && unresolved &&
            witness->callee==UINT32_MAX && witness->slot==UINT32_MAX &&
            witness->instruction!=UINT32_MAX;
    if (cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS || cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER)
        return effects->cells && effects->contracts && effects->formula_terminals &&
            witness->callee==UINT32_MAX && witness->instruction!=UINT32_MAX &&
            (cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS || witness->slot<effects->contracts[function].parameter_count);
    if (cause == XR_XIR_ROOT_CAUSE_INDIRECT || cause == XR_XIR_ROOT_CAUSE_REQUIREMENT)
        return witness->slot == UINT32_MAX && witness->instruction != UINT32_MAX;
    if (unresolved) return false;
    return cause == XR_XIR_ROOT_CAUSE_MUTABLE_SLOT || cause == XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST ||
        cause == XR_XIR_ROOT_CAUSE_INITIALIZER;
}
/* The existing forest supplies the only path. A finite distance and exact
 * descent reject cycles without another graph, recursion, or input borrows. */
static XrXirStatus effect_root_trace_length(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, bool unresolved, uint32_t *length) {
    const XrXirRootEffectWitness *forest = unresolved ? effects->unresolved_witnesses : effects->root_witnesses;
    uint32_t current = function, count = 0;
    bool constant=false;
    bool fact = unresolved ? effects->root[current].unresolved : effects->root[current].requires_root;
    if (!fact) {
        if (forest[current].cause != XR_XIR_ROOT_CAUSE_NONE) return XR_XIR_BAD_STRUCTURE;
        *length = 0; return XR_XIR_OK;
    }
    for (;;) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirRootEffectWitness *witness = &forest[current];
        fact = unresolved ? effects->root[current].unresolved : effects->root[current].requires_root;
        if (constant) {
            if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
            fact=!!(effects->contracts[current].formula.constant_mask&
                (unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED));
        }
        if (!fact || count >= effects->count || witness->distance >= effects->count) return XR_XIR_BAD_STRUCTURE;
        ++count;
        if (!witness->distance) {
            if ((witness->cause!=XR_XIR_ROOT_CAUSE_PARAMETER && witness->callee!=UINT32_MAX) ||
                !effect_root_trace_terminal(effects,current,witness,unresolved))
                return XR_XIR_BAD_STRUCTURE;
            if (witness->cause==XR_XIR_ROOT_CAUSE_PARAMETER ||
                witness->cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL ||
                witness->cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS || witness->cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER) {
                if (!xir_compile_work(context,5)) return XR_XIR_BUDGET;
                const XrXirRootEffectWitness *certificate=
                    &effects->formula_terminals[((size_t)constant*2+unresolved)*effects->count+current];
                if (certificate->cause!=witness->cause || certificate->instruction!=witness->instruction ||
                    certificate->callee!=witness->callee || certificate->slot!=witness->slot ||
                    certificate->distance!=witness->distance) return XR_XIR_BAD_STRUCTURE;
            }
            *length = count; return XR_XIR_OK;
        }
        if ((witness->cause != XR_XIR_ROOT_CAUSE_CALL && witness->cause != XR_XIR_ROOT_CAUSE_CLEANUP) ||
            witness->callee >= effects->count || witness->slot != UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        if (effects->contracts) {
            if (!effects->constant_witnesses || !effects->formula_terminals) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
            forest=effects->constant_witnesses+(size_t)unresolved*effects->count;constant=true;
        }
        if (forest[witness->callee].distance!=witness->distance-1) return XR_XIR_BAD_STRUCTURE;
        current = witness->callee;
    }
}
static XrXirStatus effect_root_trace_fill(const XrXirCompileContext *context,
    const XrXirEffects *effects, bool unresolved, uint32_t function, uint32_t count, XrXirRootCauseStep *steps) {
    const XrXirRootEffectWitness *forest=unresolved?effects->unresolved_witnesses:effects->root_witnesses;
    for (uint32_t i = 0; i < count; ++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        XrXirRootEffectWitness witness = forest[function];
        steps[i] = (XrXirRootCauseStep){function,witness.instruction,witness.callee,
            witness.slot,witness.distance,witness.cause};
        if (witness.distance && effects->contracts) {
            if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
            forest=effects->constant_witnesses+(size_t)unresolved*effects->count;
        }
        function = witness.callee;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_root_cause_trace_copy(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, XrXirRootCauseTrace **output) {
    if (effects && effects->contexts)
        return effect_context_owner_trace(context,effects,function,output);
    if (effects && effects->invocations)
        return effect_invocation_public_trace(context,effects->invocations,effects->count,function,
            function<effects->count?&effects->root[function]:NULL,output);
    if (!xir_compile_context_valid(context) || !effects || effects->resources != context->resources ||
        function >= effects->count || !effects->root || !effects->root_witnesses ||
        !effects->unresolved_witnesses || !output || *output) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    uint32_t counts[2] = {0};
    XrXirStatus status = effect_root_trace_length(context,effects,function,false,&counts[0]);
    if (status == XR_XIR_OK) status = effect_root_trace_length(context,effects,function,true,&counts[1]);
    if (status != XR_XIR_OK) return status;
    uint64_t count = (uint64_t)counts[0] + counts[1];
    if (count > (SIZE_MAX - sizeof(XrXirRootCauseTrace)) / sizeof(XrXirRootCauseStep)) return XR_XIR_BUDGET;
    size_t bytes = sizeof(XrXirRootCauseTrace) + (size_t)count * sizeof(XrXirRootCauseStep);
    if (count >= UINT64_MAX - bytes) return XR_XIR_BUDGET;
    status = xir_compile_resource_status(xr_compile_resources_admit(context->resources,bytes,bytes + count + 1));
    if (status != XR_XIR_OK) return status;
    XrXirRootCauseTrace *trace = xir_compile_calloc(context,1,bytes,&status);
    if (!trace) return status;
    XrXirRootCauseStep *steps = effect_root_trace_storage(trace);
    status = effect_root_trace_fill(context,effects,false,function,counts[0],steps);
    if (status == XR_XIR_OK) status = effect_root_trace_fill(context,effects,true,
        function,counts[1],steps + counts[0]);
    if (status == XR_XIR_OK && !xir_compile_work(context,1)) status = XR_XIR_BUDGET;
    if (status != XR_XIR_OK) { xr_xir_compile_root_cause_trace_free(trace); return status; }
    trace->facts = effects->root[function]; trace->counts[0] = counts[0]; trace->counts[1] = counts[1];
    *output = trace; return XR_XIR_OK;
}
static XrXirStatus effect_root_callable(const XrXirModule *module, XrXirEffects *effects,
    uint32_t f, uint32_t i, const XrXirCompileContext *work) {
    if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
    const XrXirInstruction *op = &module->functions[f].instructions[i];
    const XrXirTypeNode *signature = NULL;
    XrXirRootEffectCause cause = XR_XIR_ROOT_CAUSE_INDIRECT;
    if (op->op == XR_XIR_CALL_REQUIREMENT) {
        const XrXirInterfaceTable *table = module->types ? module->types->interfaces : NULL;
        if (!table || op->targets[0] >= table->count ||
            op->targets[1] >= table->declarations[op->targets[0]].method_count) return XR_XIR_BAD_STRUCTURE;
        signature = xr_xir_callable_signature(module->types,
            table->declarations[op->targets[0]].methods[op->targets[1]].signature);
        cause = XR_XIR_ROOT_CAUSE_REQUIREMENT;
    } else {
        signature = xr_xir_callable_signature(module->types,
            xr_xir_operand_type(&module->functions[f], (uint32_t)op->immediate));
    }
    if (!signature || !xr_xir_callable_flags_valid(signature->flags)) return XR_XIR_BAD_TYPE;
    if (signature->flags & XR_XIR_CALLABLE_ROOT_REQUIRED) {
        effects->root[f].requires_root = true;
        effect_root_terminal(&effects->root_witnesses[f], cause, i, UINT32_MAX);
    }
    if (signature->flags & XR_XIR_CALLABLE_ROOT_UNRESOLVED) {
        effects->root[f].unresolved = true;
        effect_root_terminal(&effects->unresolved_witnesses[f], cause, i, UINT32_MAX);
    }
    return XR_XIR_OK;
}
/* Explicit classification makes a newly executing opcode fail closed until its
 * authority rule is recorded, even when control effects already know it. */
static XrXirStatus effect_root_seed(const XrXirModule *module, XrXirEffects *effects,
    uint32_t f, uint32_t i, const XrXirCompileContext *work) {
    const XrXirInstruction *op = &module->functions[f].instructions[i];
    switch (op->op) {
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_PLACE: case XR_XIR_SLOT_STORE:
        return effect_root_slot(module,effects,f,i,work);
    case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_GROUP_INIT: {
        uint32_t slot = op->op == XR_XIR_SLOT_GROUP_INIT ?
            (uint32_t)((uint64_t)op->immediate >> 32) : (uint32_t)op->immediate;
        effects->root[f].requires_root = true;
        effect_root_terminal(&effects->root_witnesses[f],XR_XIR_ROOT_CAUSE_INITIALIZER,i,slot);
        return XR_XIR_OK;
    }
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT: case XR_XIR_CALL_REQUIREMENT:
        return effect_root_callable(module, effects, f, i, work);
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_CONST_STRING:
    case XR_XIR_ATOMIC_NEW: case XR_XIR_ATOMIC_LOAD: case XR_XIR_ATOMIC_STORE:
    case XR_XIR_ATOMIC_ADD: case XR_XIR_ATOMIC_SUB: case XR_XIR_ATOMIC_FETCH_ADD:
    case XR_XIR_ATOMIC_FETCH_SUB: case XR_XIR_ATOMIC_SWAP: case XR_XIR_ATOMIC_COMPARE_EXCHANGE:
    case XR_XIR_ATOMIC_TOGGLE: case XR_XIR_ATOMIC_TO_STRING: case XR_XIR_COPY:
    case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN: case XR_XIR_CONCAT_STRING:
    case XR_XIR_OUTPUT: case XR_XIR_WRITE_STREAM: case XR_XIR_PRINT:
    case XR_XIR_ADD_INT: case XR_XIR_EQ_INT: case XR_XIR_LT_INT:
    case XR_XIR_CALL: case XR_XIR_SUSPEND: case XR_XIR_THROW:
    case XR_XIR_JUMP: case XR_XIR_BRANCH: case XR_XIR_RETURN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_LOCAL_WRITE:
    case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_WRITE:
    case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ: case XR_XIR_OWNED_LOCAL_WRITE:
    case XR_XIR_SUB_INT: case XR_XIR_MUL_INT: case XR_XIR_DIV_INT:
    case XR_XIR_REM_INT: case XR_XIR_NE_INT: case XR_XIR_LE_INT:
    case XR_XIR_GT_INT: case XR_XIR_GE_INT: case XR_XIR_AND_INT:
    case XR_XIR_OR_INT: case XR_XIR_XOR_INT: case XR_XIR_SHL_INT:
    case XR_XIR_SHR_INT: case XR_XIR_PHI: case XR_XIR_FUNCTION_REF:
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE:
    case XR_XIR_CELL_PROJECT:
    case XR_XIR_CONVERT_NUMBER: case XR_XIR_CONST_FLOAT: case XR_XIR_NEG_FLOAT:
    case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT: case XR_XIR_LT_FLOAT:
    case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT: case XR_XIR_GE_FLOAT:
    case XR_XIR_CELL_PLACE: case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_GET:
    case XR_XIR_ARRAY_SET: case XR_XIR_ARRAY_PUSH: case XR_XIR_ARRAY_LEN:
    case XR_XIR_ADD_FLOAT: case XR_XIR_SUB_FLOAT: case XR_XIR_MUL_FLOAT:
    case XR_XIR_DIV_FLOAT: case XR_XIR_STRUCT_NEW: case XR_XIR_STRUCT_GET:
    case XR_XIR_STRUCT_SET: case XR_XIR_LOCAL_UNINIT: case XR_XIR_STRING_LEN:
    case XR_XIR_EQ_STRING: case XR_XIR_NE_STRING: case XR_XIR_STRING_CONTAINS:
    case XR_XIR_STRING_STARTS_WITH: case XR_XIR_STRING_ENDS_WITH: case XR_XIR_STRING_INDEX_OF:
    case XR_XIR_STRING_LAST_INDEX_OF: case XR_XIR_ENUM_NEW: case XR_XIR_ENUM_TAG:
    case XR_XIR_ENUM_GET: case XR_XIR_MATCH_FAIL: case XR_XIR_ERROR_ERASE:
    case XR_XIR_INVOKE: case XR_XIR_INVOKE_RESULT: case XR_XIR_INVOKE_ERROR:
    case XR_XIR_ERROR_IS: case XR_XIR_ERROR_NARROW: case XR_XIR_PANIC_CATCH:
    case XR_XIR_PANIC_CODE: case XR_XIR_PANIC_MESSAGE: case XR_XIR_CLEANUP_REGISTER:
    case XR_XIR_CLEANUP_LEAVE: case XR_XIR_CLEANUP_ERROR: case XR_XIR_CELL_LOCAL_WRITE:
    case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_PLACE_READ:
    case XR_XIR_PLACE_WRITE: case XR_XIR_FUNCTION_WEAKEN: case XR_XIR_CLASS_NEW:
    case XR_XIR_CLASS_GET: case XR_XIR_CLASS_SET: case XR_XIR_CALL_DEFAULT:
    case XR_XIR_INVOKE_DEFAULT: case XR_XIR_ASSERT_CONDITION: case XR_XIR_INVOKE_DISCARD:
    case XR_XIR_EQUAL: case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME:
    case XR_XIR_CLOCK_NANOS: case XR_XIR_UTC_OFFSET_AT: case XR_XIR_TIMER_AFTER_MS:
    case XR_XIR_TO_STRING: case XR_XIR_OBJECT_PLACE: case XR_XIR_ARRAY_REPEAT:
    case XR_XIR_NULLABLE_IS_SOME: case XR_XIR_NULLABLE_UNWRAP: case XR_XIR_CONST_RUNE:
    case XR_XIR_RUNE_TO_INTEGER: case XR_XIR_INTEGER_TO_RUNE: case XR_XIR_LT_STRING:
    case XR_XIR_LE_STRING: case XR_XIR_GT_STRING: case XR_XIR_GE_STRING:
    case XR_XIR_TUPLE_NEW: case XR_XIR_TUPLE_FIELD: case XR_XIR_GO:
    case XR_XIR_TASK_AWAIT: case XR_XIR_RANGE_CHECK: case XR_XIR_ARRAY_CAPACITY:
    case XR_XIR_ARRAY_WITH_CAPACITY: case XR_XIR_ARRAY_RESERVE:
        return XR_XIR_OK;
    default: return XR_XIR_BAD_STRUCTURE;
    }
}
/* Each node is queued once. Equal-distance parent choices can improve without
 * requeueing because descendants refer to the same node and fixed distance. */
static XrXirStatus effect_root_forest(XrXirEffects *effects, EffectGraph *graph,
    const XrXirCompileContext *work, bool unresolved, bool constant) {
    XrXirRootEffectWitness *witnesses = constant ?
        effects->constant_witnesses+(size_t)unresolved*effects->count :
        unresolved ? effects->unresolved_witnesses : effects->root_witnesses;
    uint32_t bit=unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
    uint32_t front = 0, back = 0;
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (witnesses[f].cause) graph->queue[back++] = f;
    }
    while (front < back) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        uint32_t callee = graph->queue[front++];
        for (uint32_t e = graph->heads[callee]; e != UINT32_MAX; e = graph->edges[e].next) {
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            EffectEdge edge = graph->edges[e];
            if (constant) {
                if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                if (!(effects->contracts[callee].formula.constant_mask&bit)) continue;
            }
            bool fact = unresolved ? effects->root[edge.caller].unresolved : effects->root[edge.caller].requires_root;
            if (constant) {
                if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                fact=!!(effects->contracts[edge.caller].formula.constant_mask&bit);
            }
            if (!fact) return XR_XIR_BAD_STRUCTURE;
            XrXirRootEffectWitness candidate = {edge.cleanup ? XR_XIR_ROOT_CAUSE_CLEANUP : XR_XIR_ROOT_CAUSE_CALL,
                edge.instruction,callee,UINT32_MAX,witnesses[callee].distance + 1};
            XrXirRootEffectWitness *to = &witnesses[edge.caller];
            if (!to->cause) {
                if (constant && !xir_compile_work(work,5)) return XR_XIR_BUDGET;
                *to = candidate; graph->queue[back++] = edge.caller;
            } else {
                if (constant && !xir_compile_work(work,5)) return XR_XIR_BUDGET;
                if (candidate.distance == to->distance && effect_root_cause_before(&candidate,to)) {
                    if (constant && !xir_compile_work(work,5)) return XR_XIR_BUDGET;
                    *to = candidate;
                }
            }
        }
    }
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        bool fact = unresolved ? effects->root[f].unresolved : effects->root[f].requires_root;
        if (constant) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            fact=!!(effects->contracts[f].formula.constant_mask&bit);
        }
        if (fact != (witnesses[f].cause != XR_XIR_ROOT_CAUSE_NONE)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* Constant propagation follows only its authenticated constant forest. The
 * public choice still uses the original ordering across both domains. */
static inline XrXirStatus effect_root_witnesses(XrXirEffects *effects, EffectGraph *graph,
    const XrXirCompileContext *work, bool unresolved) {
    if (!effects->contracts) return effect_root_forest(effects,graph,work,unresolved,false);
    if (!effects->constant_witnesses || !effects->formula_terminals) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_root_forest(effects,graph,work,unresolved,true);
    if (status!=XR_XIR_OK) return status;
    XrXirRootEffectWitness *projected=unresolved?effects->unresolved_witnesses:effects->root_witnesses;
    const XrXirRootEffectWitness *constant=effects->constant_witnesses+(size_t)unresolved*effects->count;
    for (uint32_t f=0;f<effects->count;++f) {
        if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
        const XrXirRootEffectWitness *candidate=&constant[f];
        XrXirRootEffectWitness *to=&projected[f];
        if (candidate->cause && (!to->cause || candidate->distance<to->distance ||
            (candidate->distance==to->distance && effect_root_cause_before(candidate,to)))) {
            if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
            *to=*candidate;
            if (!candidate->distance && (candidate->cause==XR_XIR_ROOT_CAUSE_PARAMETER ||
                candidate->cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL ||
                candidate->cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS || candidate->cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER)) {
                if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
                effects->formula_terminals[(size_t)unresolved*effects->count+f]=
                    effects->formula_terminals[(2+(size_t)unresolved)*effects->count+f];
            }
        }
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        bool fact=unresolved?effects->root[f].unresolved:effects->root[f].requires_root;
        if (fact!=(to->cause!=XR_XIR_ROOT_CAUSE_NONE)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
