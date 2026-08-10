/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xi_stage.h"
#include "xi_evidence.h"
#include "xi_verify.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include "../base/xmalloc.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct XiStageHandle {
    XiFunc *graph;
    XiStage stage;
    uint64_t generation;
    bool released;
} XiStageHandle;

static void set_error(char *error, size_t error_size, const char *message) {
    if (error && error_size)
        snprintf(error, error_size, "%s", message ? message : "Xi stage transition failed");
}

static bool tree_has_exact_stage(const XiFunc *func, XiStage stage) {
    if (!func || func->stage != stage || func->invariant_mask != xi_stage_invariants(stage))
        return false;
    for (uint16_t i = 0; i < func->nchildren; i++) {
        if (func->children[i] && !tree_has_exact_stage(func->children[i], stage))
            return false;
    }
    return true;
}

static void set_tree_stage(XiFunc *func, XiStage stage) {
    if (!func)
        return;
    func->stage = stage;
    func->invariant_mask = xi_stage_invariants(stage);
    for (uint16_t i = 0; i < func->nchildren; i++)
        set_tree_stage(func->children[i], stage);
}

static void initialize_lowering_facts(XiFunc *func) {
    if (!func)
        return;
    func->lowering_facts.initialized = true;
    func->lowering_facts.coroutine_required = func->entry_type == 2 || func->coro_plan != NULL;
    func->lowering_facts.coroutine_lowered = true;
    func->lowering_facts.callable_required = func->ncaptures != 0;
    func->lowering_facts.callable_lowered =
        !func->lowering_facts.callable_required || func->closure_meta != NULL;
    func->lowering_facts.semantic_ops_lowered = true;
    for (uint16_t i = 0; i < func->nchildren; i++)
        initialize_lowering_facts(func->children[i]);
}

static bool verify_tree(const XiFunc *func, XiStage stage, char *error, size_t error_size) {
    if (!func) {
        set_error(error, error_size, "Xi stage transition received a null graph");
        return false;
    }
    if (!xi_verify_stage(func, stage, error, (int) error_size))
        return false;
    for (uint16_t i = 0; i < func->nchildren; i++) {
        if (func->children[i] && !verify_tree(func->children[i], stage, error, error_size))
            return false;
    }
    return true;
}

static void note_transition_recursive(XiFunc *func) {
    if (!func)
        return;
    xi_evidence_note_rewrite(func, false, true, false, XI_EVD_ALL);
    for (uint16_t i = 0; i < func->nchildren; i++)
        note_transition_recursive(func->children[i]);
}

static XiStageHandle *transition(XiStageHandle *handle, XiStage from, XiStage to, char *error,
                                 size_t error_size) {
    if (!handle || handle->released || !handle->graph || handle->stage != from) {
        set_error(error, error_size, "consumed or wrong-stage Xi handle");
        return NULL;
    }
    if (to != from + 1 || !tree_has_exact_stage(handle->graph, from)) {
        set_error(error, error_size, "Xi graph does not satisfy the exact input-stage contract");
        return NULL;
    }
    if (to == XI_STAGE_SEMANTIC_PLANNED &&
        !xr_semantic_plan_is_verified(handle->graph->semantic_plan)) {
        set_error(error, error_size,
                  "SemanticPlanned transition requires a frozen verified SemanticPlan");
        return NULL;
    }

    set_tree_stage(handle->graph, to);
    if (to == XI_STAGE_LOWERED)
        initialize_lowering_facts(handle->graph);
    if (!verify_tree(handle->graph, to, error, error_size)) {
        set_tree_stage(handle->graph, from);
        return NULL;
    }

    note_transition_recursive(handle->graph);
    handle->stage = to;
    handle->generation++;
    return handle;
}

static XiStageHandle *adopt_stage(XiFunc *graph, XiStage stage, char *error, size_t error_size) {
    if (error && error_size)
        error[0] = '\0';
    if (!tree_has_exact_stage(graph, stage) || !verify_tree(graph, stage, error, error_size)) {
        if (error && error_size && error[0] == '\0')
            set_error(error, error_size, "Xi graph does not satisfy the requested stage");
        return NULL;
    }
    XiStageHandle *handle = (XiStageHandle *) xr_calloc(1, sizeof(*handle));
    if (!handle) {
        set_error(error, error_size, "out of memory allocating Xi stage handle");
        return NULL;
    }
    handle->graph = graph;
    handle->stage = stage;
    handle->generation = 1;
    return handle;
}

XiRawProgram *xi_stage_adopt_raw(XiFunc *graph, char *error, size_t error_size) {
    return (XiRawProgram *) adopt_stage(graph, XI_STAGE_RAW, error, error_size);
}

XiOptimizedProgram *xi_stage_adopt_optimized(XiFunc *graph, char *error, size_t error_size) {
    return (XiOptimizedProgram *) adopt_stage(graph, XI_STAGE_OPTIMIZED, error, error_size);
}

XiSemanticPlannedProgram *xi_stage_adopt_semantic_planned(XiFunc *graph, char *error,
                                                          size_t error_size) {
    if (!graph || !xr_semantic_plan_is_verified(graph->semantic_plan)) {
        set_error(error, error_size,
                  "SemanticPlanned adoption requires a frozen verified SemanticPlan");
        return NULL;
    }
    return (XiSemanticPlannedProgram *) adopt_stage(graph, XI_STAGE_SEMANTIC_PLANNED, error,
                                                    error_size);
}

XiReppedProgram *xi_stage_adopt_repped(XiFunc *graph, char *error, size_t error_size) {
    return (XiReppedProgram *) adopt_stage(graph, XI_STAGE_REPPED, error, error_size);
}

#define XI_DEFINE_TRANSITION(function_name, input_type, output_type, from_stage, to_stage)         \
    output_type *function_name(input_type *input, char *error, size_t error_size) {                \
        return (output_type *) transition((XiStageHandle *) input, from_stage, to_stage, error,    \
                                          error_size);                                             \
    }

XI_DEFINE_TRANSITION(xi_program_canonicalize, XiRawProgram, XiCanonicalProgram, XI_STAGE_RAW,
                     XI_STAGE_CANONICAL)
XI_DEFINE_TRANSITION(xi_program_close, XiCanonicalProgram, XiClosedProgram, XI_STAGE_CANONICAL,
                     XI_STAGE_CLOSED)
XI_DEFINE_TRANSITION(xi_program_make_owned, XiClosedProgram, XiOwnedProgram, XI_STAGE_CLOSED,
                     XI_STAGE_OWNED)
XI_DEFINE_TRANSITION(xi_program_lower_semantics, XiOwnedProgram, XiLoweredProgram, XI_STAGE_OWNED,
                     XI_STAGE_LOWERED)
XI_DEFINE_TRANSITION(xi_program_finish_optimization, XiLoweredProgram, XiOptimizedProgram,
                     XI_STAGE_LOWERED, XI_STAGE_OPTIMIZED)
XI_DEFINE_TRANSITION(xi_program_freeze_semantics, XiOptimizedProgram, XiSemanticPlannedProgram,
                     XI_STAGE_OPTIMIZED, XI_STAGE_SEMANTIC_PLANNED)
XI_DEFINE_TRANSITION(xi_program_select_reps, XiSemanticPlannedProgram, XiReppedProgram,
                     XI_STAGE_SEMANTIC_PLANNED, XI_STAGE_REPPED)
XI_DEFINE_TRANSITION(xi_program_plan_backend, XiReppedProgram, XiBackendProgram, XI_STAGE_REPPED,
                     XI_STAGE_BACKEND)

#undef XI_DEFINE_TRANSITION

static XiFunc *borrow_graph(XiStageHandle *handle, XiStage expected) {
    if (!handle || handle->released || handle->stage != expected)
        return NULL;
    return handle->graph;
}

static XiFunc *release_graph(XiStageHandle *handle, XiStage expected) {
    XiFunc *graph = borrow_graph(handle, expected);
    if (!graph)
        return NULL;
    handle->released = true;
    handle->graph = NULL;
    xr_free(handle);
    return graph;
}

#define XI_DEFINE_HANDLE_ACCESSORS(prefix, type, stage_value)                                      \
    XiFunc *prefix##_program_graph(type *program) {                                                \
        return borrow_graph((XiStageHandle *) program, stage_value);                               \
    }                                                                                              \
    XiFunc *prefix##_program_release(type *program) {                                              \
        return release_graph((XiStageHandle *) program, stage_value);                              \
    }

XI_DEFINE_HANDLE_ACCESSORS(xi_raw, XiRawProgram, XI_STAGE_RAW)
XI_DEFINE_HANDLE_ACCESSORS(xi_canonical, XiCanonicalProgram, XI_STAGE_CANONICAL)
XI_DEFINE_HANDLE_ACCESSORS(xi_closed, XiClosedProgram, XI_STAGE_CLOSED)
XI_DEFINE_HANDLE_ACCESSORS(xi_owned, XiOwnedProgram, XI_STAGE_OWNED)
XI_DEFINE_HANDLE_ACCESSORS(xi_lowered, XiLoweredProgram, XI_STAGE_LOWERED)
XI_DEFINE_HANDLE_ACCESSORS(xi_optimized, XiOptimizedProgram, XI_STAGE_OPTIMIZED)
XI_DEFINE_HANDLE_ACCESSORS(xi_semantic_planned, XiSemanticPlannedProgram, XI_STAGE_SEMANTIC_PLANNED)
XI_DEFINE_HANDLE_ACCESSORS(xi_repped, XiReppedProgram, XI_STAGE_REPPED)
XI_DEFINE_HANDLE_ACCESSORS(xi_backend, XiBackendProgram, XI_STAGE_BACKEND)

#undef XI_DEFINE_HANDLE_ACCESSORS
