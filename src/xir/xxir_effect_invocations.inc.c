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
/* One fact identifies a genuine site or one bit of its real Cell capture
 * relation at an SSA or return vertex. Descending authentic causes cannot
 * manufacture either identity or an environment in an unrooted SCC. */
typedef struct EffectInvocationProducer {
    uint32_t value,site,next_node,next_value,edge,instruction;
    uint64_t distance;
    uint32_t atom;
} EffectInvocationProducer;
typedef struct EffectInvocationProducerSet {
    EffectInvocationProducer *facts;
    uint32_t count,capacity;
} EffectInvocationProducerSet;
enum { EFFECT_CLASS_KNOWN=1u, EFFECT_CLASS_ROOT=2u, EFFECT_CLASS_UNKNOWN=4u,
    EFFECT_CLASS_VALID=7u };
/* Masks preserve residual uncertainty. Only a complete local relation has a
 * positive descending rank; a type or a temporary lattice bottom has none. */
typedef struct EffectInvocationClassFact {
    uint32_t value,mask;
    uint64_t rank;
} EffectInvocationClassFact;
typedef struct EffectInvocationClassDiscovery {
    EffectInvocationClassFact *facts;
    uint32_t count,return_mask;
    uint64_t return_rank;
} EffectInvocationClassDiscovery;

typedef struct EffectInvocationNode {
    uint32_t root,body,parameter_count,instruction_count;
    uint32_t intrinsic_mask,local_mask,cause_count;
    EffectInvocationCause *causes;
    uint64_t *input,*cells,*local_cells;
    uint32_t edge_head;
    bool deferred,local_deferred;
} EffectInvocationNode;

/* Construction history belongs only to the solver's finite scratch owner.
 * It cannot qualify any receiving Final fact or survive publication. */
typedef struct EffectInvocationState {
    EffectInvocationProducerSet producers;
    EffectInvocationClassDiscovery classes;
    bool queued,expanded;
} EffectInvocationState;
static uint64_t effect_invocation_state_offset(uint32_t capacity) {
    uint64_t bytes=(uint64_t)capacity*sizeof(uint32_t),alignment=_Alignof(EffectInvocationState);
    return (bytes+alignment-1)/alignment*alignment;
}
static uint64_t effect_invocation_state_bytes(uint32_t capacity) {
    return effect_invocation_state_offset(capacity)+(uint64_t)capacity*sizeof(EffectInvocationState);
}

/* The two output copies and cause copies share one payload lifetime. These
 * views store no permission facts and are checked against the real root and
 * body dimensions before the closed owner can be sealed or consumed. */
typedef enum EffectInvocationRow {
    EFFECT_INVOCATION_CELLS,
    EFFECT_INVOCATION_PARAMETERS,
    EFFECT_INVOCATION_CONTEXTS
} EffectInvocationRow;

static uint64_t *effect_invocation_node_row(const EffectInvocationNode *node,
    EffectInvocationRow row,bool local) {
    uint64_t *base=local?node->local_cells:node->cells;
    if (!base || node->cause_count<3 || !(node->cause_count&1)) return NULL;
    uint32_t parameters=(node->cause_count-3)/2;
    size_t dependencies=(size_t)(((uint64_t)parameters+63)/64);
    return base+(size_t)row*dependencies;
}

static EffectInvocationCause *effect_invocation_local_causes(const EffectInvocationNode *node) {
    return node->causes?node->causes+(size_t)node->cause_count:NULL;
}

static bool effect_invocation_node_shape(const EffectInvocationNode *node,
    uint32_t parameters,uint32_t instructions) {
    uint64_t dependencies=((uint64_t)parameters+63)/64;
    uint64_t contexts=((uint64_t)instructions+63)/64;
    uint64_t words=dependencies*2+contexts;
    return node->instruction_count==instructions &&
        node->cause_count==(uint64_t)parameters*2+3 && node->causes &&
        !!node->cells==!!words && !!node->local_cells==!!words;
}

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

typedef struct EffectFinalFunction {
    XrXirRootFormula formula;
    XrXirRootTerm *storage;
    uint32_t capacity;
} EffectFinalFunction;

struct EffectFinalFunctions {
    XrCompileResources *resources;
    uint32_t count,capacity,term_count;
    EffectFinalFunction *functions;
    XrXirRootEffectWitness *witnesses;
};

/* The complete semantic inventory survives construction without a solver,
 * queue, compile callback, mutable receiver or edge lookup workspace. */
#include "xxir_effect_role_types.inc.c"

/* Value and initial-key coordinates share a finite positive cause order.
 * The complete relation remains in the sole owning Final. */
typedef struct EffectCaptureFact {
    uint32_t node,value,word,bit;
    uint64_t rank;
} EffectCaptureFact;
typedef struct EffectCaptureProof {
    EffectCaptureFact *facts;
    uint32_t count,capacity;
} EffectCaptureProof;

typedef struct EffectInvocationFinal {
    EffectCaptureProof capture;
    EffectRoleProof role_proof;
    EffectFinalFunctions summary;
    EffectFinalOutcomes *outcomes;
    EffectPhysicalDomain *physical;
    EffectBodyMaterial *material;
    const XrXirCellProvenance *cells;
    EffectInvocationNode *nodes;
    EffectInvocationEdge *edges;
    EffectInvocationSite *sites;
    EffectInvocationDeferred *deferred_calls;
    uint32_t *roots,*root_spaces;
    const XrXirRootEffects *local_roots;
    const XrXirRootEffectWitness *local_witnesses;
    uint32_t site_count,binding_count;
    uint32_t deferred_count,deferred_capacity;
    uint32_t count,capacity,edge_count,edge_capacity;
} EffectInvocationFinal;

/* A verification request borrows the sole Final and immutable Input. Its
 * incoming index is fresh request scratch; no solver state or semantic copy
 * exists in this view. The builder supplies the same view of its live Final. */
typedef struct EffectInvocationRoles {
    const XrXirEffects *construction;
    const EffectInvocationCertificate *sealed;
} EffectInvocationRoles;

typedef struct EffectInvocationRead {
    const XrXirCompileContext *work;
    const EffectInvocationFinal *final;
    EffectInvocationRoles roles;
    const EffectInvocationDeclaredBounds *declared;
    EffectBodyRequest body;
    uint32_t *consumers;
    const struct EffectInvocationWorkspace *discovery;
    bool producer_enabled,class_enabled;
} EffectInvocationRead;

static bool effect_invocation_roles_shape(const EffectInvocationRead *read);
static XrXirStatus effect_invocation_role(const EffectInvocationRead *read,
    uint32_t function,uint32_t parameter,uint32_t *output);

typedef struct EffectInvocationWorkspace {
    EffectInvocationFinal final;
    EffectInvocationRead read;
    XrXirEffects *effects;
    EffectGraph *graph;
    EffectFinalFunctions *prepared_result;
    uint32_t *edge_index;
    size_t edge_index_capacity;
    EffectInvocationState *states;
    uint32_t maximum;
    uint32_t front,back,pending;
} EffectInvocationWorkspace;

/* Discovery rows have only the workspace lifetime. A sealed reader has no
 * workspace pointer and cannot recover this constructor capability. */
static const EffectInvocationProducerSet *effect_invocation_discovery_read(
    const EffectInvocationRead *read,uint32_t node) {
    const EffectInvocationWorkspace *owner=read?read->discovery:NULL;
    if(!owner || read->roles.sealed || !read->roles.construction ||
        read->final!=&owner->final || !owner->states || node>=owner->final.count ||
        !read->work || read->work->resources!=owner->read.work->resources) return NULL;
    return &owner->states[node].producers;
}

static const EffectInvocationClassDiscovery *effect_invocation_class_discovery(
    const EffectInvocationRead *read,uint32_t node) {
    const EffectInvocationWorkspace *owner=read?read->discovery:NULL;
    if(!owner || read->roles.sealed || !read->roles.construction ||
        read->final!=&owner->final || !owner->states || node>=owner->final.count ||
        !read->work || read->work->resources!=owner->read.work->resources) return NULL;
    return &owner->states[node].classes;
}

static XrXirStatus effect_final_class_mask(const XrXirCompileContext *work,
    const EffectInvocationFinal *final,uint32_t node,uint32_t value,uint32_t *output);

static XrXirStatus effect_root_seed(const XrXirModule *module,XrXirEffects *effects,
    uint32_t function,uint32_t instruction,const XrXirCompileContext *work);

static inline XrXirStatus effect_invocation_producer_column(const EffectInvocationWorkspace *owner,
    const EffectInvocationBasis *basis,uint32_t site,uint32_t atom,uint32_t *word,uint32_t *bit);

static XrXirStatus effect_invocation_class_verify(EffectInvocationWorkspace *owner);
static XrXirStatus effect_invocation_return_supported(const XrXirCompileContext *work,
    const XrXirTypes *types,XrXirType type,bool *output);
static XrXirStatus effect_final_callable_complete(const XrXirCompileContext *work,
    const EffectInvocationFinal *final,uint32_t node,uint32_t value,bool *complete);

typedef struct EffectInvocationColumn { uint32_t word,bit; } EffectInvocationColumn;
static XrXirStatus effect_invocation_producer_column_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,const EffectInvocationBasis *basis,
    uint32_t index,uint32_t atom,EffectInvocationColumn *output);
static XrXirStatus effect_invocation_cell_terminals_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request);
static XrXirStatus effect_invocation_edge_shape_read(const XrXirCompileContext *work,
    const EffectInvocationFinal *equations,const EffectBodyRequest *body,
    const XrXirTypes *types,uint32_t caller,uint32_t edge_index);
/* Each request charges the current real context and reads the sole immutable
 * material. There is no construction-time raw instruction fallback. */
static inline EffectBodyRequest effect_invocation_body(const EffectInvocationRead *owner) {
    return effect_body_owned_request(owner->work,owner->final->material);
}
static XrXirStatus effect_invocation_head(const EffectInvocationRead *owner,uint32_t function,
    EffectBodyFunctionHead *output) {
    if (!owner || !owner->work || !effect_body_material_shape(owner->final->material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_invocation_body(owner);
    return effect_body_request_head(&request,function,output);
}
static XrXirStatus effect_invocation_dimensions(const EffectInvocationRead *owner,uint32_t function,
    EffectBodyFunctionDimensions *output) {
    if (!owner || !owner->work || !effect_body_material_shape(owner->final->material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_invocation_body(owner);
    return effect_body_request_dimensions(&request,function,output);
}
static XrXirStatus effect_invocation_operation(const EffectInvocationRead *owner,uint32_t function,
    uint32_t instruction,XrXirInstruction *storage,const XrXirInstruction **output) {
    if (!owner || !owner->work || !effect_body_material_shape(owner->final->material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_invocation_body(owner);
    return effect_body_request_instruction(&request,function,instruction,storage,output);
}

static XrXirStatus effect_invocation_shape(const EffectInvocationRead *owner,uint32_t function,
    uint32_t instruction,EffectBodyInstructionShape *output) {
    if (!owner || !owner->work || !effect_body_material_shape(owner->final->material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_invocation_body(owner);
    return effect_body_request_shape(&request,function,instruction,output);
}

static XrXirStatus effect_capture_build(const XrXirCompileContext *work,EffectInvocationFinal *final,
    const EffectInvocationWorkspace *discovery);
static void effect_invocation_result_clear(EffectFinalFunctions *result);
static void effect_invocation_result_free(EffectFinalFunctions *result);
static bool effect_invocation_witness_equal(const XrXirRootEffectWitness *a,
    const XrXirRootEffectWitness *b);

#include "xxir_effect_invocation_class_nodes.inc.c"
#include "xxir_effect_invocation_nodes.inc.c"
#include "xxir_effect_invocation_root_seeds.inc.c"
#include "xxir_effect_invocation_causes.inc.c"
#include "xxir_effect_invocation_flow.inc.c"
#include "xxir_effect_invocation_class_flow.inc.c"
#include "xxir_effect_invocation_deferred.inc.c"
#include "xxir_effect_invocation_calls.inc.c"
#include "xxir_effect_invocation_producers.inc.c"
#include "xxir_effect_invocation_solve.inc.c"
#include "xxir_effect_invocation_solution.inc.c"
#include "xxir_effect_invocation_deferred_verify.inc.c"
#include "xxir_effect_invocation_seal.inc.c"
#include "xxir_effect_invocation_query.inc.c"
#include "xxir_effect_invocation_input_match.inc.c"
#include "xxir_effect_invocation_class_verify.inc.c"
#include "xxir_effect_invocation_projection.inc.c"
#include "xxir_effect_invocation_trace.inc.c"
#include "xxir_effect_invocation_driver.inc.c"
