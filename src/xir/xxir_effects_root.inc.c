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
#include "xxir_metadata_domain_internal.h"

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
static XrXirStatus effect_root_slot(const EffectRootSeedRecord *request,XrXirRootEffects *facts,
    XrXirRootEffectWitness *required,
    const XrXirCompileContext *work) {
    const XrXirInstruction *op=request->instruction;
    const XrXirSlot *slot=&request->domain->declarations->slots[op->immediate];
    XrXirRootEffectCause cause=XR_XIR_ROOT_CAUSE_MUTABLE_SLOT;
    if (!slot->mutable) {
        XirMetadataProofContext proof={request->domain,{XR_XIR_CONTEXT_FUNCTION,request->function,0}};
        XrXirStatus status=xir_metadata_type_markers_prove(work,&proof,slot->type,XR_XIR_CONSTRAINT_SENDABLE);
        if (status==XR_XIR_OK) return XR_XIR_OK;
        if (status!=XR_XIR_BAD_TYPE) return status;
        cause=XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST;
    }
    (*facts).requires_root=true;
    effect_root_terminal(required,cause,request->ordinal,(uint32_t)op->immediate);
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
XR_FUNC XrXirStatus xr_xir_compile_root_cause_trace_copy(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,XrXirRootCauseTrace **output) {
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    if (!output || *output || !final || function>=final->count ||
        !xir_effects_context_matches(context,effects,final->count)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_public_root_match(context,effects);
    if (status!=XR_XIR_OK) return status;
    if (effect_public_root_context(effects,function))
        return effect_context_owner_trace(context,effects,function,output);
    return effect_invocation_public_trace(context,effects->invocations,function,&final->root[function],output);
}
static XrXirStatus effect_root_callable(const EffectRootSeedRecord *request,XrXirRootEffects *facts,
    XrXirRootEffectWitness *required,XrXirRootEffectWitness *unresolved,
    const XrXirCompileContext *work) {
    if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
    const XirMetadataDomain *module=request->domain;
    uint32_t i=request->ordinal;
    const XrXirInstruction *op=request->instruction;
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
            request->callable_type);
    }
    if (!signature || !xr_xir_callable_flags_valid(signature->flags)) return XR_XIR_BAD_TYPE;
    if (signature->flags & XR_XIR_CALLABLE_ROOT_REQUIRED) {
        (*facts).requires_root = true;
        effect_root_terminal(required, cause, i, UINT32_MAX);
    }
    if (signature->flags & XR_XIR_CALLABLE_ROOT_UNRESOLVED) {
        (*facts).unresolved = true;
        effect_root_terminal(unresolved, cause, i, UINT32_MAX);
    }
    return XR_XIR_OK;
}
/* Explicit classification makes a newly executing opcode fail closed until its
 * authority rule is recorded, even when control effects already know it. */
typedef enum EffectRootSeedRule {
    EFFECT_ROOT_SEED_INVALID, EFFECT_ROOT_SEED_NONE, EFFECT_ROOT_SEED_SLOT,
    EFFECT_ROOT_SEED_INITIALIZER, EFFECT_ROOT_SEED_CALLABLE
} EffectRootSeedRule;
static EffectRootSeedRule effect_root_seed_rule(XrXirOp op) {
    switch (op) {
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_PLACE: case XR_XIR_SLOT_STORE:
        return EFFECT_ROOT_SEED_SLOT;
    case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_GROUP_INIT:return EFFECT_ROOT_SEED_INITIALIZER;
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT: case XR_XIR_CALL_REQUIREMENT:
        return EFFECT_ROOT_SEED_CALLABLE;
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_CONST_STRING:
    case XR_XIR_ATOMIC_NEW: case XR_XIR_ATOMIC_LOAD: case XR_XIR_ATOMIC_STORE:
    case XR_XIR_ATOMIC_ADD: case XR_XIR_ATOMIC_SUB: case XR_XIR_ATOMIC_FETCH_ADD:
    case XR_XIR_ATOMIC_FETCH_SUB: case XR_XIR_ATOMIC_SWAP: case XR_XIR_ATOMIC_COMPARE_EXCHANGE:
    case XR_XIR_ATOMIC_TOGGLE: case XR_XIR_ATOMIC_TO_STRING: case XR_XIR_COPY:
    case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN: case XR_XIR_CONCAT_STRING:
    case XR_XIR_OUTPUT: case XR_XIR_WRITE_STREAM: case XR_XIR_PRINT:
    case XR_XIR_ADD_INT: case XR_XIR_EQ_INT: case XR_XIR_LT_INT:
    case XR_XIR_CALL: case XR_XIR_SUSPEND: case XR_XIR_GENERATOR_YIELD: case XR_XIR_THROW:
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
    case XR_XIR_TUPLE_NEW: case XR_XIR_TUPLE_FIELD: case XR_XIR_GO: case XR_XIR_GENERATOR_NEW:
    case XR_XIR_TASK_AWAIT: case XR_XIR_RANGE_CHECK: case XR_XIR_ARRAY_CAPACITY:
    case XR_XIR_ARRAY_WITH_CAPACITY: case XR_XIR_ARRAY_RESERVE:
        return EFFECT_ROOT_SEED_NONE;
    default:return EFFECT_ROOT_SEED_INVALID;
    }
}
static XrXirStatus effect_root_seed_output(const EffectRootSeedRecord *request,XrXirRootEffects *facts,
    XrXirRootEffectWitness *required,XrXirRootEffectWitness *unresolved,
    const XrXirCompileContext *work) {
    if (!request || !request->domain || !request->instruction || !facts || !required || !unresolved ||
        request->function>=request->domain->function_count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t i=request->ordinal;
    const XrXirInstruction *op=request->instruction;
    switch (effect_root_seed_rule(op->op)) {
    case EFFECT_ROOT_SEED_NONE:return XR_XIR_OK;
    case EFFECT_ROOT_SEED_SLOT:return effect_root_slot(request,facts,required,work);
    case EFFECT_ROOT_SEED_INITIALIZER: {
        uint32_t slot=op->op==XR_XIR_SLOT_GROUP_INIT?
            (uint32_t)((uint64_t)op->immediate>>32):(uint32_t)op->immediate;
        facts->requires_root=true;
        effect_root_terminal(required,XR_XIR_ROOT_CAUSE_INITIALIZER,i,slot);
        return XR_XIR_OK;
    }
    case EFFECT_ROOT_SEED_CALLABLE:return effect_root_callable(request,facts,required,unresolved,work);
    default:return XR_XIR_BAD_STRUCTURE;
    }
}
/* Array ownership and the single receiving-row kernel are separate. Both
 * constructors and independent replay use the same exhaustive authority rule. */
static XrXirStatus effect_root_seed_record(const EffectRootSeedRecord *request,XrXirEffects *effects,
    const XrXirCompileContext *work) {
    if (!request || !effects || request->function>=effects->count || !effects->outcomes_work.root ||
        !effects->outcomes_work.root_witnesses || !effects->outcomes_work.unresolved_witnesses) return XR_XIR_BAD_STRUCTURE;
    return effect_root_seed_output(request,&effects->outcomes_work.root[request->function],
        &effects->outcomes_work.root_witnesses[request->function],&effects->outcomes_work.unresolved_witnesses[request->function],work);
}

static inline XrXirStatus effect_root_seed(const XrXirModule *module,XrXirEffects *effects,
    uint32_t f,uint32_t i,const XrXirCompileContext *work) {
    if (!module || !module->functions || f>=module->function_count ||
        i>=module->functions[f].instruction_count || !module->functions[f].instructions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&module->functions[f].instructions[i];
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    EffectRootSeedRecord request={&domain,op,f,i,XR_XIR_UNIT};
    if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT)
        request.callable_type=xr_xir_operand_type(&module->functions[f],(uint32_t)op->immediate);
    return effect_root_seed_record(&request,effects,work);
}
/* Each node is queued once. Equal-distance parent choices can improve without
 * requeueing because descendants refer to the same node and fixed distance. */
static XrXirStatus effect_root_forest(XrXirEffects *effects, EffectGraph *graph,
    const XrXirCompileContext *work, bool unresolved, bool constant) {
    XrXirRootEffectWitness *witnesses = constant ?
        effects->constant_witnesses+(size_t)unresolved*effects->count :
        unresolved ? effects->outcomes_work.unresolved_witnesses : effects->outcomes_work.root_witnesses;
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
                if (!(effect_contracts_read(effects)[callee].formula.constant_mask&bit)) continue;
            }
            bool fact = unresolved ? effects->outcomes_work.root[edge.caller].unresolved : effects->outcomes_work.root[edge.caller].requires_root;
            if (constant) {
                if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                fact=!!(effect_contracts_read(effects)[edge.caller].formula.constant_mask&bit);
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
        bool fact = unresolved ? effects->outcomes_work.root[f].unresolved : effects->outcomes_work.root[f].requires_root;
        if (constant) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            fact=!!(effect_contracts_read(effects)[f].formula.constant_mask&bit);
        }
        if (fact != (witnesses[f].cause != XR_XIR_ROOT_CAUSE_NONE)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* Constant propagation follows only its authenticated constant forest. The
 * public choice still uses the original ordering across both domains. */
static inline XrXirStatus effect_root_witnesses(XrXirEffects *effects, EffectGraph *graph,
    const XrXirCompileContext *work, bool unresolved) {
    if (!effect_contracts_read(effects)) return effect_root_forest(effects,graph,work,unresolved,false);
    if (!effects->constant_witnesses || !effects->formula_terminals) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_root_forest(effects,graph,work,unresolved,true);
    if (status!=XR_XIR_OK) return status;
    XrXirRootEffectWitness *projected=unresolved?effects->outcomes_work.unresolved_witnesses:effects->outcomes_work.root_witnesses;
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
        bool fact=unresolved?effects->outcomes_work.root[f].unresolved:effects->outcomes_work.root[f].requires_root;
        if (fact!=(to->cause!=XR_XIR_ROOT_CAUSE_NONE)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
