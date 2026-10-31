/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_input_facts.inc.c - Original-coordinate receiving facts
 *
 * KEY CONCEPT:
 *   A receiving owner copies complete construction once. Contextual physical
 *   bodies retain that immutable owner without reinterpreting helper IDs.
 */
#include "xxir_receive_control.h"

typedef struct EffectInputFacts {
    XrCompileResources *resources;
    uint32_t references;
    XrXirConstruction *construction;
} EffectInputFacts;

static XrXirStatus effect_input_shape(const XrXirCompileContext *work, XirEffectInput input) {
    if (!xir_compile_context_valid(work) || !input.module || !input.construction)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xir_receive_poll(input.control);
    return status==XR_XIR_OK?xir_construction_verify(work,input.module,input.construction):status;
}

static void effect_input_facts_free(EffectInputFacts *facts) {
    if (!facts) return;
    XR_CHECK(facts->references,"input facts reference must be live");
    if (--facts->references) return;
    xr_xir_compile_construction_free(facts->construction);
    xr_compile_resources_free(facts);
}

static XrXirStatus effect_input_facts_retain(const XrXirCompileContext *work, EffectInputFacts *facts) {
    if (!xir_compile_context_valid(work) || !facts || !facts->references || facts->resources!=work->resources || !facts->construction)
        return XR_XIR_BAD_STRUCTURE;
    if (facts->references==UINT32_MAX) return XR_XIR_OVERFLOW;
    ++facts->references;return XR_XIR_OK;
}

static XrXirStatus effect_input_facts_new(const XrXirCompileContext *work,
    XirEffectInput input, EffectInputFacts **output) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_input_shape(work,input);
    if (status!=XR_XIR_OK) return status;
    EffectInputFacts *facts=xir_compile_calloc(work,1,sizeof(*facts),&status);
    if (!facts) return status;
    facts->resources=work->resources;facts->references=1;
    status=xir_construction_clone(work,input.module->types,input.construction,
        &facts->construction,input.control);
    if (status!=XR_XIR_OK) { effect_input_facts_free(facts);return status; }
    *output=facts;return XR_XIR_OK;
}

static XrXirStatus effect_input_facts_match(const XrXirCompileContext *work,
    XirEffectInput input, const EffectInputFacts *facts) {
    if (!xir_compile_context_valid(work) || !facts || !facts->references || facts->resources!=work->resources || !facts->construction)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_input_shape(work,input);
    bool same=false;
    if (status==XR_XIR_OK) status=xir_construction_compare(work,input.module->types,
        facts->construction,input.construction,&same,input.control);
    return status==XR_XIR_OK && !same ? XR_XIR_BAD_STRUCTURE : status;
}
