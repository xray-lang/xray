/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_existing_node_cases.h - Existing equations under allocation failure
 *
 * KEY CONCEPT:
 *   A settled real graph lookup does not create a node or need more index storage.
 */
#ifndef XIR_INVOCATION_EXISTING_NODE_CASES_H
#define XIR_INVOCATION_EXISTING_NODE_CASES_H

static void invocation_existing_node_literals(void) {
    RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;GenerationRangeTrial trial;
    generation_range_fixture(&trial,1);CHECK(generation_range_prepare(&context,&trial)==XR_XIR_OK);
    const XrXirModule *source=xr_xir_compile_artifact_module(trial.checked);
    EffectBodyMaterial *material=trial.effects.generation;
    CHECK(effect_graph_build_material(source,&trial.effects,&trial.graph,&context)==XR_XIR_OK);
    EffectInvocationWorkspace *owner=NULL;
    CHECK(effect_invocations_receive(&context,source,&trial.effects,&trial.graph,material->declared,&owner)==XR_XIR_OK);
    CHECK(effect_invocations_solve(owner)==XR_XIR_OK && !owner->pending && owner->final.count==8 && owner->node_index_capacity==16);
    uint32_t expected=owner->final.roots[0];CHECK(expected<owner->final.count);
    EffectInvocationNode key=owner->final.nodes[expected];
    uint32_t *index=owner->node_index;size_t capacity=owner->node_index_capacity,attempts=rp_attempts;
    XrCompileResourceStats before=rp_stats(&context);uint32_t output=UINT32_MAX;
    rp_fail_at=rp_attempts;rp_injected=false;
    XrXirStatus status=effect_invocation_intern(owner,key.root,key.body,key.input,&output);
    bool injected=rp_injected;rp_fail_at=SIZE_MAX;
    XrCompileResourceStats after=rp_stats(&context);
    bool stable=status==XR_XIR_OK && output==expected && !injected && rp_attempts==attempts &&
        owner->node_index==index && owner->node_index_capacity==capacity && owner->final.count==8 &&
        after.live_bytes==before.live_bytes && after.allocated_bytes==before.allocated_bytes && after.work>before.work;
    fprintf(stderr,"existing node query status%u injected%u output%u/%u slots%zu/%zu attempts%zu/%zu live%llu/%llu\n",
        (unsigned)status,(unsigned)injected,output,expected,owner->node_index_capacity,capacity,rp_attempts,attempts,
        (unsigned long long)after.live_bytes,(unsigned long long)before.live_bytes);
    effect_invocation_free(owner);effect_graph_free(&trial.graph);invocation_core_storage_free(&trial.effects);
    xr_xir_compile_artifact_free(trial.checked);rp_owner_free(&context,baseline);rp_balanced(held);
    CHECK(stable);
}
#endif // XIR_INVOCATION_EXISTING_NODE_CASES_H
