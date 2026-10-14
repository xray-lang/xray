/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_causes.inc.c - Rooted certificates on the existing equation queue
 *
 * KEY CONCEPT:
 *   Equation outputs are already closed. Certificate propagation changes no
 *   permission fact and requires a strictly descending path to a real terminal.
 */
static bool effect_invocation_atom(const EffectInvocationNode *node,
    uint32_t parameters,uint32_t atom,bool local) {
    if (atom<2) return !!((local?node->local_mask:node->intrinsic_mask)&
        (atom?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED));
    if (atom-2<parameters) {
        uint32_t p=atom-2;
        return !!(effect_invocation_node_row(node,EFFECT_INVOCATION_PARAMETERS,local)[p/64]&(UINT64_C(1)<<(p%64)));
    }
    if (atom-2-parameters<parameters) {
        uint32_t p=atom-2-parameters;
        return !!((local?node->local_cells:node->cells)[p/64]&(UINT64_C(1)<<(p%64)));
    }
    return atom==(uint64_t)parameters*2+2 && (local?node->local_deferred:node->deferred);
}

static XrXirStatus effect_invocation_local(EffectInvocationWorkspace *owner,
    uint32_t node,uint32_t atom,XrXirRootEffectWitness witness) {
    if (!owner || node>=owner->final.count || atom>=owner->final.nodes[node].cause_count ||
        owner->final.nodes[node].expanded || !witness.cause || witness.distance) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionDimensions head={0};XrXirStatus status=effect_invocation_dimensions(owner,owner->final.nodes[node].body,&head);
    if (status!=XR_XIR_OK) return status;
    if (witness.instruction==UINT32_MAX ? witness.cause!=XR_XIR_ROOT_CAUSE_INITIALIZER :
        witness.instruction>=head.instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,6+sizeof(EffectInvocationCause))) return XR_XIR_BUDGET;
    EffectInvocationCause *to=&owner->final.nodes[node].causes[atom];
    if (to->witness.cause && !effect_root_cause_before(&witness,&to->witness)) return XR_XIR_OK;
    *to=(EffectInvocationCause){witness,UINT32_MAX,UINT32_MAX,0};return XR_XIR_OK;
}

/* These records describe the full equation key, not only a numeric callee.
 * The same root basis indexes every actual edge on this certificate walk. */
static XrXirStatus effect_invocation_cause_atom(EffectInvocationWorkspace *owner,
    uint32_t root,uint32_t parameters,uint32_t atom) {
    uint32_t front=0,back=0,pending=0;
    for (uint32_t n=0;n<owner->final.count;++n) {
        if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
        EffectInvocationNode *node=&owner->final.nodes[n];
        if (node->root!=root) continue;
        if (atom>=node->cause_count || node->queued) return XR_XIR_BAD_STRUCTURE;
        EffectInvocationCause *cause=&node->causes[atom];
        bool local=effect_invocation_atom(node,parameters,atom,true);
        if (local!=(cause->witness.cause!=XR_XIR_ROOT_CAUSE_NONE && cause->distance==0))
            return XR_XIR_BAD_STRUCTURE;
        if (!local) {
            if (!xir_compile_work(owner->work,sizeof(*cause))) return XR_XIR_BUDGET;
            *cause=(EffectInvocationCause){.next=UINT32_MAX,.edge=UINT32_MAX,.distance=UINT32_MAX};
            continue;
        }
        if (pending>=owner->final.capacity || !effect_invocation_atom(node,parameters,atom,false))
            return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        owner->graph->queue[back]=n;back=back+1==owner->final.capacity?0:back+1;
        ++pending;node->queued=true;
    }
    while (pending) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        uint32_t n=owner->graph->queue[front];front=front+1==owner->final.capacity?0:front+1;
        if (n>=owner->final.count || !owner->final.nodes[n].queued || owner->final.nodes[n].root!=root)
            return XR_XIR_BAD_STRUCTURE;
        --pending;owner->final.nodes[n].queued=false;
        EffectInvocationCause from=owner->final.nodes[n].causes[atom];
        if (from.distance>=owner->final.count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t e=owner->consumers[n];e!=UINT32_MAX;e=owner->final.edges[e].consumer_next) {
            if (!xir_compile_work(owner->work,6)) return XR_XIR_BUDGET;
            if (e>=owner->final.edge_count || owner->final.edges[e].target!=n) return XR_XIR_BAD_STRUCTURE;
            EffectInvocationEdge edge=owner->final.edges[e];
            if (edge.latent) continue;
            if (edge.caller>=owner->final.count || owner->final.nodes[edge.caller].root!=root)
                return XR_XIR_BAD_STRUCTURE;
            EffectInvocationNode *node=&owner->final.nodes[edge.caller];
            if (!effect_invocation_atom(node,parameters,atom,false)) return XR_XIR_BAD_STRUCTURE;
            EffectBodyInstructionShape shape={0};
            XrXirStatus status=effect_invocation_shape(owner,node->body,edge.instruction,&shape);
            if (status!=XR_XIR_OK) return status;
            XrXirRootEffectWitness witness={shape.op==XR_XIR_CLEANUP_REGISTER?
                XR_XIR_ROOT_CAUSE_CLEANUP:XR_XIR_ROOT_CAUSE_CALL,edge.instruction,
                owner->final.nodes[n].body,UINT32_MAX,from.distance+1};
            EffectInvocationCause *to=&node->causes[atom];
            if (to->distance<witness.distance || (to->distance==witness.distance &&
                !effect_root_cause_before(&witness,&to->witness))) continue;
            if (!xir_compile_work(owner->work,sizeof(*to))) return XR_XIR_BUDGET;
            *to=(EffectInvocationCause){witness,n,e,witness.distance};
            if (node->queued) continue;
            if (pending>=owner->final.capacity) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            owner->graph->queue[back]=edge.caller;back=back+1==owner->final.capacity?0:back+1;
            ++pending;node->queued=true;
        }
    }
    for (uint32_t n=0;n<owner->final.count;++n) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&owner->final.nodes[n];
        if (node->root!=root) continue;
        const EffectInvocationCause *cause=&node->causes[atom];
        bool fact=effect_invocation_atom(node,parameters,atom,false);
        if (fact!=(cause->distance!=UINT32_MAX) || node->queued ||
            (fact && (!cause->witness.cause || cause->distance>=owner->final.count))) return XR_XIR_BAD_STRUCTURE;
        if (!fact || !cause->distance) continue;
        if (cause->next>=owner->final.count || cause->edge>=owner->final.edge_count ||
            owner->final.edges[cause->edge].latent || owner->final.edges[cause->edge].caller!=n ||
            owner->final.edges[cause->edge].target!=cause->next || owner->final.nodes[cause->next].root!=root ||
            owner->final.nodes[cause->next].causes[atom].distance+1!=cause->distance)
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_causes(EffectInvocationWorkspace *owner) {
    if (!owner || owner->pending || !owner->final.count || !owner->graph) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t root=0;root<owner->body.domain.function_count;++root) {
        if (!owner->final.root_spaces || owner->final.root_spaces[root]!=root) continue;
        EffectBodyFunctionDimensions head={0};XrXirStatus read_status=effect_invocation_dimensions(owner,root,&head);
        if (read_status!=XR_XIR_OK) return read_status;
        uint32_t parameters=head.parameter_count;
        uint64_t count=(uint64_t)parameters*2+3;
        if (count>UINT32_MAX) return XR_XIR_BUDGET;
        for (uint32_t atom=0;atom<(uint32_t)count;++atom) {
            XrXirStatus status=effect_invocation_cause_atom(owner,root,parameters,atom);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
