/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_inference.c - One bounded incremental evidence collector
 *
 * KEY CONCEPT:
 *   Each value is observed once; exact matching and complete finalization precede
 *   publication, while partial known shapes can guide a later expression.
 */
#include "xxir_type_inference.h"
#include "xxir_compile_memory.h"
#include "../base/xmalloc.h"
#include <string.h>
typedef struct InferenceTask {
    struct InferenceTask *next;
    XrXirInferencePair pair;
    uint32_t depth;
} InferenceTask;
typedef struct InferenceObservation {
    struct InferenceObservation *next;
    XrXirInferencePair pair;
} InferenceObservation;
struct XrXirInferenceState {
    XrXirCompileContext context;
    XrXirType *arguments;
    uint32_t *kinds;
    unsigned char *solved;
    uint32_t prefix_count, count, caller_count;
    InferenceObservation *first, *last;
    XrXirStatus status;
    bool finalized;
};
typedef struct InferenceWalk {
    XrXirInferenceState *state;
    const XrXirTypes *types;
    InferenceTask *first, *last;
} InferenceWalk;
static bool inference_work(XrXirInferenceState *s, uint64_t work) {
    if (s->status != XR_XIR_OK) return false;
    if (!xir_compile_work(&s->context, work)) { s->status = XR_XIR_BUDGET; return false; }
     return true;
}
static void *inference_alloc(XrXirInferenceState *s, uint64_t bytes) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!bytes || s->status != XR_XIR_OK) return NULL;
    if (bytes > SIZE_MAX) { s->status = XR_XIR_BUDGET; return NULL; }

    void *p = xir_compile_calloc(&s->context, 1,(size_t)bytes, &allocation_status);
    if (!p) s->status = allocation_status;
    return p;
}
static void inference_enqueue(InferenceWalk *w, XrXirType formal, XrXirType actual, uint32_t depth) {
    XrXirInferenceState *s = w->state;
    if (!inference_work(s,1)) return;
    if ((uint64_t)depth*sizeof(InferenceTask) > s->context.limits.frame_bytes) { s->status = XR_XIR_BUDGET; return; }
    InferenceTask *task = inference_alloc(s,sizeof(*task));
    if (!task) return;
    task->pair = (XrXirInferencePair){formal,actual}; task->depth = depth;
    if (w->last) w->last->next = task; else w->first = task;
    w->last = task;
}
static void inference_walk_dispose(InferenceWalk *w) {
    while (w->first) { InferenceTask *next = w->first->next; xr_compile_resources_free(w->first); w->first = next; }


}
static XrXirStatus inference_match(XrXirInferenceState *s, const XrXirTypes *types,
    const XrXirType *arguments, uint32_t count, XrXirInferencePair pair) {
    XrXirCompileContext match = s->context;
    XrXirStatus status = xr_xir_compile_type_substitution_matches_between(&match, types, types, arguments, count, pair.formal, pair.actual);
     return status;
}
static void inference_pair(InferenceWalk *w, const InferenceTask *task) {
    XrXirInferenceState *s = w->state;
    if (!inference_work(s,1)) return;
    uint32_t id = (uint32_t)task->pair.formal;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t index = id-XR_XIR_TYPE_PARAMETER_BASE;
        if (index < s->prefix_count) return;
        if (index >= s->count || (task->pair.actual == XR_XIR_UNIT &&
            (!s->kinds || s->kinds[index] != XR_XIR_BINDER_RESULT_VARIABLE)) ||
            xr_xir_type_is_cell(w->types,task->pair.actual)) { s->status = XR_XIR_BAD_TYPE; return; }
        if (s->solved[index]) s->status = inference_match(s,w->types,NULL,0,
            (XrXirInferencePair){s->arguments[index],task->pair.actual});
        else { s->arguments[index] = task->pair.actual; s->solved[index] = 1; }
        return;
    }
    const XrXirTypeNode *from = xr_xir_type_node(w->types,task->pair.formal);
    if (!from) return;
    const XrXirTypeNode *to = xr_xir_type_node(w->types,task->pair.actual);
    if (!to || from->kind != to->kind || from->parameter_count != to->parameter_count || from->flags != to->flags) {
        s->status = XR_XIR_BAD_TYPE; return;
    }
    if (task->depth == UINT32_MAX) { s->status = XR_XIR_BUDGET; return; }
    uint32_t depth = task->depth+1;
    if (from->kind == XR_XIR_TYPE_NOMINAL) {
        if (from->nominal.declaration != to->nominal.declaration ||
            from->nominal.argument_count != to->nominal.argument_count) { s->status = XR_XIR_BAD_TYPE; return; }
        for (uint32_t a = 0; a < from->nominal.argument_count && s->status == XR_XIR_OK; ++a)
            inference_enqueue(w,from->nominal.arguments[a],to->nominal.arguments[a],depth);
    } else if (from->kind == XR_XIR_TYPE_CALLABLE) {
        inference_enqueue(w,from->result,to->result,depth);
        for (uint32_t p = 0; p < from->parameter_count && s->status == XR_XIR_OK; ++p) {
            if (from->parameters[p].mode != to->parameters[p].mode) { s->status = XR_XIR_BAD_TYPE; break; }
            inference_enqueue(w,from->parameters[p].type,to->parameters[p].type,depth);
        }
    } else if (from->kind == XR_XIR_TYPE_ARRAY || from->kind == XR_XIR_TYPE_CELL ||
               from->kind == XR_XIR_TYPE_NULLABLE)
        inference_enqueue(w,from->element,to->element,depth);
    else s->status = XR_XIR_BAD_TYPE;
}
XR_FUNC void xr_xir_compile_inference_dispose(XrXirInferenceState *s) {
    if (!s) return;
    while (s->first) { InferenceObservation *next = s->first->next; xr_compile_resources_free(s->first); s->first = next; }
    xr_compile_resources_free(s->kinds); xr_compile_resources_free(s->solved); xr_compile_resources_free(s->arguments);  xr_compile_resources_free(s);
}
XR_FUNC XrXirStatus xr_xir_compile_inference_begin(const XrXirCompileContext *compile_context, const XrXirInferenceRequest *r, XrXirInferenceState **output) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;

    if (!r || !budget || !output || (!!r->prefix != !!r->prefix_count) || r->prefix_count > 65536 ||
        r->own_count > 65536-r->prefix_count || r->caller_parameter_count > 65536) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(budget, 1)) return XR_XIR_BUDGET;

    XrXirInferenceState *s = xir_compile_calloc(compile_context, 1,sizeof(*s), &allocation_status);
    if (!s) return allocation_status;
    s->context = *compile_context;
    s->prefix_count = r->prefix_count; s->count = r->prefix_count+r->own_count; s->caller_count = r->caller_parameter_count;
    s->arguments = inference_alloc(s,(uint64_t)s->count*sizeof(*s->arguments)); s->solved = inference_alloc(s,s->count);
    if (r->parameter_kinds) {
        s->kinds = inference_alloc(s,(uint64_t)s->count*sizeof(*s->kinds));
        bool result = false;
        for (uint32_t p=0;p<s->count && inference_work(s,1);++p) {
            uint32_t kind = r->parameter_kinds[p];
            if (kind > XR_XIR_BINDER_RESULT_VARIABLE) {s->status=XR_XIR_BAD_STRUCTURE;break;}
            result |= kind == XR_XIR_BINDER_RESULT_VARIABLE;
            s->kinds[p]=kind;
        }
        if (s->status == XR_XIR_OK && (!s->count || !result)) s->status=XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t p = 0; p < s->prefix_count && inference_work(s,1); ++p) {
        if ((r->prefix[p] == XR_XIR_UNIT && (!s->kinds || s->kinds[p] != XR_XIR_BINDER_RESULT_VARIABLE)) ||
            xr_xir_type_is_cell(r->types,r->prefix[p])) { s->status = XR_XIR_BAD_TYPE; break; }
        s->status = xr_xir_compile_type_expression_shape(budget, r->types, r->prefix[p], s->caller_count);
        if (s->status == XR_XIR_OK) { s->arguments[p] = r->prefix[p]; s->solved[p] = 1; }
    }
    XrXirStatus status = s->status;
    if (status == XR_XIR_OK) *output = s; else xr_xir_compile_inference_dispose(s);
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_inference_observe(XrXirInferenceState *s, const XrXirTypes *types, XrXirInferencePair pair) {
    if (!s || s->finalized) return XR_XIR_BAD_STRUCTURE;
    if (s->status != XR_XIR_OK) return s->status;
    s->status = xr_xir_compile_type_expression_shape(&s->context, types, pair.formal, s->count);
    if (s->status == XR_XIR_OK) s->status = xr_xir_compile_type_expression_shape(&s->context, types, pair.actual, s->caller_count);
    InferenceObservation *observation = inference_alloc(s,sizeof(*observation));
    if (observation) {
        observation->pair = pair;
        if (s->last) s->last->next = observation; else s->first = observation;
        s->last = observation;
    }
    InferenceWalk walk = {s,types,NULL,NULL};
    inference_enqueue(&walk,pair.formal,pair.actual,1);
    for (InferenceTask *task = walk.first; task && s->status == XR_XIR_OK; task = task->next) inference_pair(&walk,task);
    inference_walk_dispose(&walk); return s->status;
}
XR_FUNC XrXirStatus xr_xir_compile_inference_expected_known(XrXirInferenceState *s, const XrXirTypes *types, XrXirType formal, XrXirInferenceKnown *output) {
    if (!s || !output || s->finalized) return XR_XIR_BAD_STRUCTURE;
    if (s->status != XR_XIR_OK) return s->status;
    s->status = xr_xir_compile_type_expression_shape(&s->context, types, formal, s->count);
    InferenceWalk walk = {s,types,NULL,NULL}; bool known = true;
    inference_enqueue(&walk,formal,XR_XIR_UNIT,1);
    for (InferenceTask *task = walk.first; task && s->status == XR_XIR_OK; task = task->next) {
        if (!inference_work(s,1)) break;
        uint32_t id = (uint32_t)task->pair.formal;
        if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
            if (id-XR_XIR_TYPE_PARAMETER_BASE >= s->count) { s->status = XR_XIR_BAD_TYPE; break; }
            if (!s->solved[id-XR_XIR_TYPE_PARAMETER_BASE]) known = false;
            continue;
        }
        const XrXirTypeNode *node = xr_xir_type_node(types,task->pair.formal);
        if (!node) continue;
        if (task->depth == UINT32_MAX) { s->status = XR_XIR_BUDGET; break; }
        uint32_t depth = task->depth+1;
        if (node->kind == XR_XIR_TYPE_CALLABLE) {
            inference_enqueue(&walk,node->result,XR_XIR_UNIT,depth);
            for (uint32_t p = 0; p < node->parameter_count && s->status == XR_XIR_OK; ++p)
                inference_enqueue(&walk,node->parameters[p].type,XR_XIR_UNIT,depth);
        } else if (node->kind == XR_XIR_TYPE_NOMINAL) {
            for (uint32_t p = 0; p < node->nominal.argument_count && s->status == XR_XIR_OK; ++p)
                inference_enqueue(&walk,node->nominal.arguments[p],XR_XIR_UNIT,depth);
        } else inference_enqueue(&walk,node->element,XR_XIR_UNIT,depth);
    }
    inference_walk_dispose(&walk);
    if (s->status == XR_XIR_OK) *output = known ?
        (XrXirInferenceKnown){true,s->arguments,s->count} : (XrXirInferenceKnown){0};
    return s->status;
}
XR_FUNC XrXirStatus xr_xir_compile_inference_finalize(XrXirInferenceState *s, const XrXirTypes *types, XrXirType *output, uint32_t output_count) {
    if (!s || s->finalized || output_count != s->count || (!!output != !!output_count)) return XR_XIR_BAD_STRUCTURE;
    if (s->status != XR_XIR_OK) return s->status;
    for (uint32_t p = 0; p < s->count && inference_work(s,1); ++p)
        if (!s->solved[p]) s->status = XR_XIR_BAD_TYPE;
    for (InferenceObservation *item = s->first; item && inference_work(s,1); item = item->next)
        s->status = inference_match(s,types,s->arguments,s->count,item->pair);
    if (s->status == XR_XIR_OK && inference_work(s,s->count)) {
        if (!inference_work(s,(uint64_t)s->count*sizeof(*output))) return s->status;
        if (s->count) memcpy(output,s->arguments,(size_t)s->count*sizeof(*output));
        s->finalized = true;
    }
    return s->status;
}
