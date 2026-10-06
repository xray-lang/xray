/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effects.c - Bounded least-fixed-point control effects
 *
 * KEY CONCEPT:
 *   Only direct call edges propagate facts; indirect calls remain unknown.
 *   Reverse edges and a deduplicated queue handle recursive components without
 *   recursion or assumptions about function storage order.
 */
#include "xxir_effects.h"
#include "xxir_compile_memory.h"
#include "xxir_defaults_internal.h"
#include "xxir_internal.h"
#include "xxir_types.h"
#include "xxir_interface.h"
#include "xxir_constraint_proof.h"
#include "xxir_operand_roles.h"
#include "../base/xmalloc.h"

typedef struct EffectErrorAtom { XrXirType type; uint32_t variant; } EffectErrorAtom;
struct XrXirEffects {
    uint32_t count, atom_count, atom_capacity, words, source_type_count;
    XrXirFunctionEffects *functions;
    XrXirEffectWitness *witnesses;
    EffectErrorAtom *atoms;
    uint64_t *errors;
    XrXirStatus *task_errors;
    XrXirStatus *go_safe;
    XrXirEffect *task_creation;
};
typedef struct EffectEdge { uint32_t caller, next, instruction; bool cleanup; } EffectEdge;
typedef struct EffectGraph {
    uint32_t *heads, *queue;
    uint8_t *queued;
    EffectEdge *edges;
} EffectGraph;

static void effect_graph_free(EffectGraph *graph) {
    xr_compile_resources_free(graph->heads); xr_compile_resources_free(graph->queue); xr_compile_resources_free(graph->queued); xr_compile_resources_free(graph->edges);
}
void xr_xir_compile_effects_free(XrXirEffects *effects) {
    if (effects) {
        xr_compile_resources_free(effects->errors); xr_compile_resources_free(effects->atoms); xr_compile_resources_free(effects->functions);
        xr_compile_resources_free(effects->witnesses); xr_compile_resources_free(effects->task_errors);
        xr_compile_resources_free(effects->go_safe); xr_compile_resources_free(effects->task_creation);
        xr_compile_resources_free(effects);
    }
}
const XrXirFunctionEffects *xr_xir_effects_function(const XrXirEffects *effects, uint32_t function) {
    return effects && function < effects->count ? &effects->functions[function] : NULL;
}
XR_FUNC XrXirStatus xr_xir_effects_task_errors(const XrXirEffects *effects, uint32_t function) {
    return effects && function < effects->count ? effects->task_errors[function] : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xr_xir_effects_go_safe(const XrXirEffects *effects, uint32_t function) {
    return effects && function < effects->count ? effects->go_safe[function] : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirEffect xr_xir_effects_task_creation(const XrXirEffects *effects, uint32_t function) {
    return effects && function < effects->count ? effects->task_creation[function] : XR_XIR_EFFECT_UNKNOWN;
}
const XrXirEffectWitness *xr_xir_effects_suspend_witness(const XrXirEffects *effects, uint32_t function) {
    return effects && function < effects->count && effects->witnesses[function].cause ?
        &effects->witnesses[function] : NULL;
}
/* An unclassified opcode must not acquire an accidental no-effect proof. */
static bool effect_seed(const XrXirModule *module, const XrXirFunction *function,
    const XrXirInstruction *instruction, XrXirFunctionEffects *effect) {
    XrXirOp op = instruction->op;
    switch (op) {
    case XR_XIR_SUSPEND: case XR_XIR_TIMER_AFTER_MS: case XR_XIR_TASK_AWAIT:
        effect->suspend = XR_XIR_EFFECT_MAY; return true;
    case XR_XIR_GO: return true;
    case XR_XIR_THROW: case XR_XIR_CLEANUP_REGISTER:
    case XR_XIR_CLEANUP_LEAVE: case XR_XIR_CLEANUP_ERROR: return true;
    case XR_XIR_CALL_REQUIREMENT: {
        const XrXirInterfaceTable *table = module->types ? module->types->interfaces : NULL;
        if (!table || instruction->targets[0] >= table->count) return false;
        const XrXirInterfaceDeclaration *declaration = &table->declarations[instruction->targets[0]];
        if (instruction->targets[1] >= declaration->method_count) return false;
        const XrXirTypeNode *signature = xr_xir_callable_signature(module->types,
            declaration->methods[instruction->targets[1]].signature);
        if (!signature) return false;
        if (!(signature->flags & XR_XIR_CALLABLE_NO_SUSPEND) && effect->suspend < XR_XIR_EFFECT_UNKNOWN)
            effect->suspend = XR_XIR_EFFECT_UNKNOWN;
        return true;
    }
    case XR_XIR_CALL_INDIRECT:
    case XR_XIR_INVOKE_INDIRECT: {
        XrXirType type = xr_xir_operand_type(function, (uint32_t)instruction->immediate);
        const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, type);
        if (!signature) return false;
        if (signature->flags & XR_XIR_CALLABLE_NO_SUSPEND) return true;
        if (effect->suspend < XR_XIR_EFFECT_UNKNOWN) effect->suspend = XR_XIR_EFFECT_UNKNOWN;
        return true;
    }
    case XR_XIR_INVOKE_DEFAULT: case XR_XIR_CALL_DEFAULT: case XR_XIR_CALL: case XR_XIR_INVOKE:
    case XR_XIR_CONST_RUNE: case XR_XIR_RUNE_TO_INTEGER: case XR_XIR_INTEGER_TO_RUNE:
    case XR_XIR_LT_STRING: case XR_XIR_LE_STRING: case XR_XIR_GT_STRING: case XR_XIR_GE_STRING:
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_CONST_STRING:
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_STORE: case XR_XIR_SLOT_GROUP_INIT:
    case XR_XIR_ATOMIC_NEW: case XR_XIR_ATOMIC_LOAD: case XR_XIR_ATOMIC_STORE:
    case XR_XIR_ATOMIC_ADD: case XR_XIR_ATOMIC_SUB: case XR_XIR_ATOMIC_FETCH_ADD:
    case XR_XIR_ATOMIC_FETCH_SUB: case XR_XIR_ATOMIC_SWAP: case XR_XIR_ATOMIC_COMPARE_EXCHANGE:
    case XR_XIR_ATOMIC_TOGGLE: case XR_XIR_ATOMIC_TO_STRING:
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_CONCAT_STRING: case XR_XIR_TO_STRING: case XR_XIR_ARRAY_REPEAT: case XR_XIR_NULLABLE_IS_SOME: case XR_XIR_NULLABLE_UNWRAP: case XR_XIR_OUTPUT: case XR_XIR_WRITE_STREAM:
    case XR_XIR_PRINT: case XR_XIR_ADD_INT: case XR_XIR_EQ_INT:
    case XR_XIR_LT_INT: case XR_XIR_JUMP: case XR_XIR_BRANCH:
    case XR_XIR_RETURN: case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ:
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_READ:
    case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_OWNED_LOCAL_WRITE: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
    case XR_XIR_DIV_INT: case XR_XIR_REM_INT: case XR_XIR_NE_INT:
    case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT:
    case XR_XIR_AND_INT: case XR_XIR_OR_INT: case XR_XIR_XOR_INT:
    case XR_XIR_SHL_INT: case XR_XIR_SHR_INT: case XR_XIR_PHI:
    case XR_XIR_FUNCTION_WEAKEN:
    case XR_XIR_CELL_LOCAL_WRITE: case XR_XIR_FUNCTION_REF: case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ:
    case XR_XIR_CELL_WRITE: case XR_XIR_CONVERT_NUMBER: case XR_XIR_CONST_FLOAT:
    case XR_XIR_NEG_FLOAT: case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT:
    case XR_XIR_LT_FLOAT: case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT:
    case XR_XIR_GE_FLOAT: case XR_XIR_CELL_PLACE: case XR_XIR_SLOT_PLACE: case XR_XIR_OBJECT_PLACE:
    case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_PLACE_READ: case XR_XIR_PLACE_WRITE:
    case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_GET: case XR_XIR_ARRAY_SET:
    case XR_XIR_ARRAY_PUSH: case XR_XIR_ARRAY_LEN: case XR_XIR_ADD_FLOAT:
    case XR_XIR_SUB_FLOAT: case XR_XIR_MUL_FLOAT: case XR_XIR_DIV_FLOAT:
    case XR_XIR_STRUCT_NEW: case XR_XIR_STRUCT_GET: case XR_XIR_STRUCT_SET:
    case XR_XIR_CLASS_NEW: case XR_XIR_CLASS_GET: case XR_XIR_CLASS_SET:
    case XR_XIR_LOCAL_UNINIT: case XR_XIR_STRING_LEN: case XR_XIR_EQ_STRING:
    case XR_XIR_NE_STRING: case XR_XIR_STRING_CONTAINS: case XR_XIR_STRING_STARTS_WITH:
    case XR_XIR_STRING_ENDS_WITH: case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF:
    case XR_XIR_ENUM_NEW: case XR_XIR_ENUM_TAG: case XR_XIR_ENUM_GET:
    case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME:
    case XR_XIR_RANGE_CHECK: case XR_XIR_EQUAL: case XR_XIR_ASSERT_CONDITION: case XR_XIR_MATCH_FAIL: case XR_XIR_ERROR_ERASE: case XR_XIR_INVOKE_RESULT:
    case XR_XIR_INVOKE_ERROR: case XR_XIR_INVOKE_DISCARD: case XR_XIR_ERROR_IS: case XR_XIR_ERROR_NARROW:
    case XR_XIR_PANIC_CATCH: case XR_XIR_PANIC_CODE: case XR_XIR_PANIC_MESSAGE:
    case XR_XIR_CLOCK_NANOS: case XR_XIR_UTC_OFFSET_AT:
    case XR_XIR_TUPLE_NEW: case XR_XIR_TUPLE_FIELD:
        return true;
    default: return false;
    }
}
static XrXirStatus effect_graph_build(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, XrXirCompileContext *remaining) {
    XrXirStatus allocation_status = XR_XIR_OK;
    uint32_t edges = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (!xir_compile_work(remaining, function->instruction_count)) return XR_XIR_BUDGET;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            XrXirOp op = function->instructions[i].op;
            if (!effect_seed(module, function, &function->instructions[i], &effects->functions[f]))
                return XR_XIR_BAD_STRUCTURE;
            if (op == XR_XIR_GO) effects->task_creation[f] = XR_XIR_EFFECT_MAY;
            if (op == XR_XIR_INVOKE_DEFAULT || op == XR_XIR_CALL_DEFAULT || op == XR_XIR_CALL ||
                op == XR_XIR_INVOKE || op == XR_XIR_CLEANUP_REGISTER) {
                if (edges == UINT32_MAX) return XR_XIR_BUDGET;
                ++edges;
            }
        }
    }
    uint64_t bytes = (uint64_t) module->function_count * (2 * sizeof(uint32_t) + sizeof(uint8_t)) +
        (uint64_t) edges * sizeof(EffectEdge);
    if ((bytes > SIZE_MAX) || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    graph->heads = xir_compile_calloc(remaining, module->function_count, sizeof(*graph->heads), &allocation_status);
    graph->queue = xir_compile_calloc(remaining, module->function_count, sizeof(*graph->queue), &allocation_status);
    graph->queued = xir_compile_calloc(remaining, module->function_count, sizeof(*graph->queued), &allocation_status);
    if (edges) graph->edges = xir_compile_calloc(remaining, edges, sizeof(*graph->edges), &allocation_status);
    if (!graph->heads || !graph->queue || !graph->queued || (edges && !graph->edges))
        return allocation_status;
    for (uint32_t f = 0; f < module->function_count; ++f) graph->heads[f] = UINT32_MAX;
    uint32_t at = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (!xir_compile_work(remaining, function->instruction_count)) return XR_XIR_BUDGET;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_INVOKE_DEFAULT && op->op != XR_XIR_CALL_DEFAULT && op->op != XR_XIR_CALL &&
                op->op != XR_XIR_INVOKE && op->op != XR_XIR_CLEANUP_REGISTER) continue;
            uint32_t callee = (uint32_t) op->immediate;
            if(op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
                const XrXirDefaultBinding *binding=NULL;
                const uint32_t *identity=xr_xir_default_identity(op);
                XrXirStatus status=xr_xir_compile_default_lookup(remaining, module, identity[0], identity[1], &binding);
                if(status!=XR_XIR_OK) return status;
                if(!binding) return XR_XIR_BAD_STRUCTURE;
                callee=binding->function;
            }
            graph->edges[at] = (EffectEdge) {f, graph->heads[callee], i, op->op == XR_XIR_CLEANUP_REGISTER};
            graph->heads[callee] = at++;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_propagate(XrXirEffects *effects, EffectGraph *graph, const XrXirCompileContext *work) {
    uint32_t front = 0, back = 0, pending = effects->count;
    for (uint32_t f = 0; f < effects->count; ++f) { graph->queue[f] = f; graph->queued[f] = 1; }
    while (pending) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        uint32_t callee = graph->queue[front];
        front = front + 1 == effects->count ? 0 : front + 1;
        --pending; graph->queued[callee] = 0;
        XrXirFunctionEffects from = effects->functions[callee];
        for (uint32_t edge = graph->heads[callee]; edge != UINT32_MAX; edge = graph->edges[edge].next) {
            if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
            EffectEdge link = graph->edges[edge];
            /* Registered cleanup executes with worker authority, but its
             * separate no-throw/no-suspend obligations govern control effects. */
            if (link.cleanup) continue;
            XrXirFunctionEffects *to = &effects->functions[link.caller];
            bool changed = false;
            if (from.suspend > to->suspend) { to->suspend = from.suspend; changed = true; }
            if (effects->task_creation[callee] > effects->task_creation[link.caller]) {
                effects->task_creation[link.caller] = effects->task_creation[callee]; changed = true;
            }
            if (changed && !graph->queued[link.caller]) {
                graph->queue[back] = link.caller;
                back = back + 1 == effects->count ? 0 : back + 1;
                graph->queued[link.caller] = 1; ++pending;
            }
        }
    }
    return XR_XIR_OK;
}
#include "xxir_effect_terms.inc.c"
#include "xxir_effect_errors.inc.c"
#include "xxir_go_safe.inc.c"

/* A breadth-first forest over final facts cannot inherit a cyclic cause chain
 * from recursive fixed-point updates. Each function enters the queue once. */
static XrXirStatus effect_witnesses(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, const XrXirCompileContext *work) {
    uint32_t front = 0, back = 0;
    for (uint32_t f = 0; f < effects->count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        XrXirEffect fact = effects->functions[f].suspend;
        if (!xir_compile_work(work, (uint64_t)function->instruction_count + 1)) return XR_XIR_BUDGET;
        if (fact == XR_XIR_EFFECT_NONE) continue;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            XrXirOp op = function->instructions[i].op;
            XrXirFunctionEffects local = {0};
            if (!effect_seed(module, function, &function->instructions[i], &local)) return XR_XIR_BAD_STRUCTURE;
            XrXirEffectCause cause = XR_XIR_EFFECT_CAUSE_NONE;
            if (fact == XR_XIR_EFFECT_MAY && (op == XR_XIR_SUSPEND || op == XR_XIR_TIMER_AFTER_MS ||
                op == XR_XIR_TASK_AWAIT)) cause = XR_XIR_EFFECT_CAUSE_SUSPEND;
            else if (fact == XR_XIR_EFFECT_UNKNOWN && local.suspend == XR_XIR_EFFECT_UNKNOWN)
                cause = XR_XIR_EFFECT_CAUSE_INDIRECT;
            if (!cause) continue;
            effects->witnesses[f] = (XrXirEffectWitness){cause, i, UINT32_MAX, 0};
            graph->queue[back++] = f; break;
        }
    }
    while (front < back) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        uint32_t callee = graph->queue[front++];
        for (uint32_t e = graph->heads[callee]; e != UINT32_MAX; e = graph->edges[e].next) {
            if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
            EffectEdge edge = graph->edges[e];
            if (edge.cleanup) continue;
            if (effects->witnesses[edge.caller].cause ||
                effects->functions[edge.caller].suspend != effects->functions[callee].suspend) continue;
            effects->witnesses[edge.caller] = (XrXirEffectWitness){XR_XIR_EFFECT_CAUSE_CALL,
                edge.instruction, callee, effects->witnesses[callee].distance + 1};
            graph->queue[back++] = edge.caller;
        }
    }
    return XR_XIR_OK;
}

XrXirStatus xr_xir_compile_effects_infer_verified(const XrXirCompileContext *compile_context, const XrXirModule *module, XrXirEffects **output) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;

    uint64_t bytes = sizeof(XrXirEffects) + (uint64_t) module->function_count *
        (sizeof(XrXirFunctionEffects) + sizeof(XrXirEffectWitness));
    if ((bytes > SIZE_MAX) || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirEffects *effects = xir_compile_calloc(compile_context, 1, sizeof(*effects), &allocation_status);
    if (!effects) return allocation_status;
    effects->count = module->function_count;
    effects->functions = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->functions), &allocation_status);
    effects->witnesses = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->witnesses), &allocation_status);
    effects->task_errors = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->task_errors), &allocation_status);
    effects->go_safe = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->go_safe), &allocation_status);
    effects->task_creation = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->task_creation), &allocation_status);
    if (!effects->functions || !effects->witnesses || !effects->task_errors || !effects->go_safe ||
        !effects->task_creation) { xr_xir_compile_effects_free(effects); return allocation_status; }
    EffectGraph graph = {0};
    XrXirStatus status = effect_graph_build(module, effects, &graph, remaining);
    if (status == XR_XIR_OK) status = effect_propagate(effects, &graph, remaining);
    if (status == XR_XIR_OK) status = effect_go_safe(module, effects, &graph, remaining);
    if (status == XR_XIR_OK) status = effect_witnesses(module, effects, &graph, remaining);
    effect_graph_free(&graph);
    if (status != XR_XIR_OK) { xr_xir_compile_effects_free(effects); return status; }

    status = effect_errors_analyze(module, effects, remaining);
    if (status != XR_XIR_OK) { xr_xir_compile_effects_free(effects); return status; }
    *output = effects; return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_effects_analyze(const XrXirArtifact *artifact, XrXirEffects **output) {
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = artifact->context;
    XrXirCompileContext *budget = &compile_state;
    if (!output) return XR_XIR_BAD_STRUCTURE;

    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    if (!module || (module->stage != XR_XIR_CHECKED && module->stage != XR_XIR_LOWERED)) return XR_XIR_BAD_STAGE;
    XrXirCompileContext remaining = *budget;
    XrXirStatus status = xr_xir_compile_verify(&remaining, module, NULL);
    return status == XR_XIR_OK ? xr_xir_compile_effects_infer_verified(&remaining, module, output) : status;
}
