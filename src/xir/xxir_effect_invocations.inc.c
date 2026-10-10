/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocations.inc.c - Owned finite physical invocation equations
 *
 * KEY CONCEPT:
 *   Complete input environments use a fixed root basis. Repeated recursive
 *   keys connect existing equations instead of copying a binding path.
 */
typedef struct EffectInvocationDeferred {
    uint32_t node,instruction,parameter,count;
    uint64_t *arguments;
} EffectInvocationDeferred;
typedef struct EffectInvocationCause {
    XrXirRootEffectWitness witness;
    uint32_t next,edge,distance;
} EffectInvocationCause;
/* Two cause rows lead the shared payload; every following word row keeps
 * its natural alignment without padding or another allocation owner. */
_Static_assert((2*sizeof(EffectInvocationCause))%_Alignof(uint64_t)==0,
    "Invocation cause rows preserve word alignment");
/* A fact identifies one genuine zero-capture site at a real SSA or return
 * vertex. Descending causes cannot manufacture a producer in an SCC. */
typedef struct EffectInvocationProducer {
    uint32_t value,site,next_node,next_value,edge,instruction;
    uint64_t distance;
} EffectInvocationProducer;
typedef struct EffectInvocationNode {
    uint32_t root,body,parameter_count;
    uint32_t intrinsic_mask,local_mask,cause_count;
    EffectInvocationCause *causes,*local_causes;
    uint64_t *input,*outputs,*cells,*parameters,*contexts;
    uint64_t *local_cells,*local_parameters,*local_contexts;
    EffectInvocationProducer *producers;
    uint32_t producer_count,producer_capacity;
    uint32_t edge_head;
    bool deferred,local_deferred,queued,expanded;
} EffectInvocationNode;

typedef struct EffectInvocationEdge {
    uint32_t caller,target,instruction,producer,next,consumer_next;
    bool latent;
} EffectInvocationEdge;

typedef struct EffectInvocationBasis {
    uint32_t parameters,origin_words,fn_words,cell_words,row_words;
} EffectInvocationBasis;

typedef struct EffectInvocationSite {
    uint32_t function,instruction,target,captures,binding;
} EffectInvocationSite;

typedef struct EffectInvocationOwner {
    const XrXirCompileContext *work;
    const XrXirModule *module;
    XrXirEffects *effects;
    EffectGraph *graph;
    const EffectInvocationDeclaredBounds *declared;
    EffectInvocationNode *nodes;
    EffectInvocationEdge *edges;
    EffectInvocationSite *sites;
    EffectInvocationDeferred *deferred_calls;
    uint32_t *consumers;
    uint32_t *roots,*root_spaces;
    XrXirRootEffects *local_roots;
    XrXirRootEffectWitness *local_witnesses;
    uint32_t site_count,binding_count;
    uint32_t deferred_count,deferred_capacity;
    uint32_t count,capacity,edge_count,edge_capacity,maximum;
    uint32_t front,back,pending;
    bool producer_enabled;
} EffectInvocationOwner;

static XrXirStatus effect_root_seed(const XrXirModule *module,XrXirEffects *effects,
    uint32_t function,uint32_t instruction,const XrXirCompileContext *work);

#include "xxir_effect_invocation_nodes.inc.c"
#include "xxir_effect_invocation_causes.inc.c"
#include "xxir_effect_invocation_flow.inc.c"
#include "xxir_effect_invocation_deferred.inc.c"
#include "xxir_effect_invocation_calls.inc.c"
#include "xxir_effect_invocation_producers.inc.c"
#include "xxir_effect_invocation_solve.inc.c"
#include "xxir_effect_invocation_solution.inc.c"
#include "xxir_effect_invocation_deferred_verify.inc.c"
#include "xxir_effect_invocation_seal.inc.c"
#include "xxir_effect_invocation_query.inc.c"
#include "xxir_effect_invocation_projection.inc.c"
#include "xxir_effect_invocation_trace.inc.c"
#include "xxir_effect_invocation_driver.inc.c"
