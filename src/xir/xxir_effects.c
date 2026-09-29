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
#include "xxir_internal.h"
#include "xxir_types.h"
#include "../base/xmalloc.h"

typedef struct EffectErrorAtom { XrXirType type; uint32_t variant; } EffectErrorAtom;
struct XrXirEffects {
    uint32_t count, atom_count, words;
    XrXirFunctionEffects *functions;
    EffectErrorAtom *atoms;
    uint64_t *errors;
};
typedef struct EffectEdge { uint32_t caller, next; } EffectEdge;
typedef struct EffectGraph {
    uint32_t *heads, *queue;
    uint8_t *queued;
    EffectEdge *edges;
} EffectGraph;
static bool effect_spend(uint64_t *work, uint64_t amount) {
    if (*work < amount) return false;
    *work -= amount; return true;
}
static void effect_graph_free(EffectGraph *graph) {
    xr_free(graph->heads); xr_free(graph->queue); xr_free(graph->queued); xr_free(graph->edges);
}
void xr_xir_effects_free(XrXirEffects *effects) {
    if (effects) { xr_free(effects->errors); xr_free(effects->atoms); xr_free(effects->functions); xr_free(effects); }
}
const XrXirFunctionEffects *xr_xir_effects_function(const XrXirEffects *effects, uint32_t function) {
    return effects && function < effects->count ? &effects->functions[function] : NULL;
}
/* An unclassified opcode must not acquire an accidental no-effect proof. */
static bool effect_seed(XrXirOp op, XrXirFunctionEffects *effect) {
    switch (op) {
    case XR_XIR_SUSPEND: effect->suspend = XR_XIR_EFFECT_MAY; return true;
    case XR_XIR_THROW: return true;
    case XR_XIR_CALL_INDIRECT:
    case XR_XIR_INVOKE_INDIRECT:
        if (effect->suspend < XR_XIR_EFFECT_UNKNOWN) effect->suspend = XR_XIR_EFFECT_UNKNOWN;
        return true;
    case XR_XIR_CALL: case XR_XIR_INVOKE:
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_CONST_STRING:
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_STORE:
    case XR_XIR_ATOMIC_I64_NEW: case XR_XIR_ATOMIC_I64_LOAD: case XR_XIR_ATOMIC_I64_FETCH_ADD:
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_CONCAT_STRING: case XR_XIR_OUTPUT: case XR_XIR_WRITE_STREAM:
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
    case XR_XIR_FUNCTION_REF: case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ:
    case XR_XIR_CELL_WRITE: case XR_XIR_CONVERT_NUMBER: case XR_XIR_CONST_FLOAT:
    case XR_XIR_NEG_FLOAT: case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT:
    case XR_XIR_LT_FLOAT: case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT:
    case XR_XIR_GE_FLOAT: case XR_XIR_CELL_PLACE: case XR_XIR_SLOT_PLACE:
    case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_GET: case XR_XIR_ARRAY_SET:
    case XR_XIR_ARRAY_PUSH: case XR_XIR_ARRAY_LEN: case XR_XIR_ADD_FLOAT:
    case XR_XIR_SUB_FLOAT: case XR_XIR_MUL_FLOAT: case XR_XIR_DIV_FLOAT:
    case XR_XIR_STRUCT_NEW: case XR_XIR_STRUCT_GET: case XR_XIR_STRUCT_SET:
    case XR_XIR_LOCAL_UNINIT: case XR_XIR_STRING_LEN: case XR_XIR_EQ_STRING:
    case XR_XIR_NE_STRING: case XR_XIR_STRING_CONTAINS: case XR_XIR_STRING_STARTS_WITH:
    case XR_XIR_STRING_ENDS_WITH: case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF:
    case XR_XIR_ENUM_NEW: case XR_XIR_ENUM_TAG: case XR_XIR_ENUM_GET:
    case XR_XIR_MATCH_FAIL: case XR_XIR_ERROR_ERASE: case XR_XIR_INVOKE_RESULT:
    case XR_XIR_INVOKE_ERROR: case XR_XIR_ERROR_IS: case XR_XIR_ERROR_NARROW:
    case XR_XIR_PANIC_CATCH: case XR_XIR_PANIC_CODE: case XR_XIR_PANIC_MESSAGE:
        return true;
    default: return false;
    }
}
static XrXirStatus effect_graph_build(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, XrXirBudget *remaining) {
    uint32_t edges = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (!effect_spend(&remaining->work, function->instruction_count)) return XR_XIR_BUDGET;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            XrXirOp op = function->instructions[i].op;
            if (!effect_seed(op, &effects->functions[f])) return XR_XIR_BAD_STRUCTURE;
            if (op == XR_XIR_CALL || op == XR_XIR_INVOKE) {
                if (edges == UINT32_MAX) return XR_XIR_BUDGET;
                ++edges;
            }
        }
    }
    uint64_t bytes = (uint64_t) module->function_count * (2 * sizeof(uint32_t) + sizeof(uint8_t)) +
        (uint64_t) edges * sizeof(EffectEdge);
    if (bytes > remaining->scratch_bytes || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    graph->heads = xr_calloc(module->function_count, sizeof(*graph->heads));
    graph->queue = xr_calloc(module->function_count, sizeof(*graph->queue));
    graph->queued = xr_calloc(module->function_count, sizeof(*graph->queued));
    if (edges) graph->edges = xr_calloc(edges, sizeof(*graph->edges));
    if (!graph->heads || !graph->queue || !graph->queued || (edges && !graph->edges))
        return XR_XIR_OUT_OF_MEMORY;
    for (uint32_t f = 0; f < module->function_count; ++f) graph->heads[f] = UINT32_MAX;
    uint32_t at = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (!effect_spend(&remaining->work, function->instruction_count)) return XR_XIR_BUDGET;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_CALL && op->op != XR_XIR_INVOKE) continue;
            uint32_t callee = (uint32_t) op->immediate;
            graph->edges[at] = (EffectEdge) {f, graph->heads[callee]};
            graph->heads[callee] = at++;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_propagate(XrXirEffects *effects, EffectGraph *graph, uint64_t *work) {
    uint32_t front = 0, back = 0, pending = effects->count;
    for (uint32_t f = 0; f < effects->count; ++f) { graph->queue[f] = f; graph->queued[f] = 1; }
    while (pending) {
        if (!effect_spend(work, 1)) return XR_XIR_BUDGET;
        uint32_t callee = graph->queue[front];
        front = front + 1 == effects->count ? 0 : front + 1;
        --pending; graph->queued[callee] = 0;
        XrXirFunctionEffects from = effects->functions[callee];
        for (uint32_t edge = graph->heads[callee]; edge != UINT32_MAX; edge = graph->edges[edge].next) {
            if (!effect_spend(work, 1)) return XR_XIR_BUDGET;
            EffectEdge link = graph->edges[edge];
            XrXirFunctionEffects *to = &effects->functions[link.caller];
            bool changed = false;
            if (from.suspend > to->suspend) { to->suspend = from.suspend; changed = true; }
            if (changed && !graph->queued[link.caller]) {
                graph->queue[back] = link.caller;
                back = back + 1 == effects->count ? 0 : back + 1;
                graph->queued[link.caller] = 1; ++pending;
            }
        }
    }
    return XR_XIR_OK;
}
#include "xxir_effect_errors.inc.c"

XrXirStatus xr_xir_effects_analyze(const XrXirArtifact *artifact,
    const XrXirBudget *budget, XrXirEffects **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    if (!module || (module->stage != XR_XIR_CHECKED && module->stage != XR_XIR_LOWERED)) return XR_XIR_BAD_STAGE;
    XrXirBudget remaining = budget ? *budget : xr_xir_default_budget();
    XrXirStatus status = xr_xir_verify_remaining(module, &remaining, NULL);
    if (status != XR_XIR_OK) return status;
    uint64_t bytes = sizeof(XrXirEffects) + (uint64_t) module->function_count * sizeof(XrXirFunctionEffects);
    if (bytes > remaining.metadata_bytes || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirEffects *effects = xr_calloc(1, sizeof(*effects));
    if (!effects) return XR_XIR_OUT_OF_MEMORY;
    effects->count = module->function_count;
    effects->functions = xr_calloc(effects->count, sizeof(*effects->functions));
    if (!effects->functions) { xr_xir_effects_free(effects); return XR_XIR_OUT_OF_MEMORY; }
    EffectGraph graph = {0};
    status = effect_graph_build(module, effects, &graph, &remaining);
    if (status == XR_XIR_OK) status = effect_propagate(effects, &graph, &remaining.work);
    effect_graph_free(&graph);
    if (status != XR_XIR_OK) { xr_xir_effects_free(effects); return status; }
    remaining.metadata_bytes -= bytes;
    status = effect_errors_analyze(module, effects, &remaining);
    if (status != XR_XIR_OK) { xr_xir_effects_free(effects); return status; }
    *output = effects; return XR_XIR_OK;
}
