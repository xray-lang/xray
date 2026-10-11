/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effects.c - Bounded least-fixed-point control effects
 *
 * KEY CONCEPT:
 *   Direct edges propagate facts; indirect calls consume verified upper bounds.
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
#include "xxir_cell_provenance_internal.h"
#include "../base/xmalloc.h"

#include "xxir_effect_contract.inc.c"

typedef struct EffectContextOwner EffectContextOwner;
typedef struct EffectRefinementBounds EffectRefinementBounds;
typedef struct EffectInvocationCertificate EffectInvocationCertificate;
typedef struct EffectInvocationDeclaredBounds EffectInvocationDeclaredBounds;
typedef struct EffectOrdinaryContexts EffectOrdinaryContexts;
static void effect_invocation_certificate_free(EffectInvocationCertificate *certificate);
static void effect_invocation_bounds_free(EffectInvocationDeclaredBounds *bounds);
static void effect_refinement_free(EffectRefinementBounds *bounds);
static void effect_context_owner_free(EffectContextOwner *owner);
static XrXirStatus effect_context_owner_trace(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, XrXirRootCauseTrace **output);

typedef struct EffectErrorAtom { XrXirType type; uint32_t variant; } EffectErrorAtom;
struct XrXirEffects {
    /* Each charged allocation independently retains this ledger. */
    XrCompileResources *resources;
    uint32_t count, atom_count, atom_capacity, words, source_type_count;
    XrXirFunctionEffects *functions;
    XrXirEffectWitness *witnesses;
    EffectErrorAtom *atoms;
    uint64_t *errors;
    XrXirStatus *task_errors;
    XrXirStatus *go_safe;
    XrXirEffect *task_creation;
    XrXirRootEffects *root;
    XrXirRootEffectWitness *root_witnesses, *unresolved_witnesses;
    /* Two certified symbolic terminals per function, owned by this graph. */
    XrXirRootEffectWitness *formula_terminals;
    XrXirRootEffectWitness *constant_witnesses;
    XrXirFunctionEffectContract *contracts;
    XrXirCellProvenance *cells;
    XirEffectEntryRootView *entry_roots;
    EffectContextOwner *contexts;
    EffectRefinementBounds *refinement;
    /* One sealed ROOT owner; control and error analyses remain independent. */
    EffectInvocationCertificate *invocations;
    /* Construction-only owner; seal transfers this same generation. */
    struct EffectBodyMaterial *generation;
};

/* Ledger identity rejects foreign requests; it is not an admission proof. */
XR_FUNC bool xir_effects_context_matches(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t functions) {
    return xir_compile_context_valid(context) && effects && functions &&
        effects->resources==context->resources && effects->count==functions;
}
static bool effect_seed(const XrXirModule *module,const XrXirFunction *function,
    const XrXirInstruction *instruction,XrXirFunctionEffects *effect);
#include "xxir_effect_invocation_classification.inc.c"
#include "xxir_effect_terms.inc.c"
#include "xxir_effect_context_types.inc.c"
#include "xxir_effect_snapshot_types.inc.c"
#include "xxir_effect_snapshot_records.inc.c"
#include "xxir_effect_snapshot_request.inc.c"
#include "xxir_effect_snapshot_cell_read.inc.c"
/* Instruction fields and the actual callee SSA type belong to the current
 * admitted body. Metadata cannot supply or authorize an instruction. */
typedef struct EffectRootSeedRecord {
    const XirMetadataDomain *domain;
    const XrXirInstruction *instruction;
    uint32_t function,ordinal;
    XrXirType callable_type;
} EffectRootSeedRecord;

static XrXirStatus effect_root_seed_record(const EffectRootSeedRecord *request,
    XrXirEffects *effects,const XrXirCompileContext *work);
typedef struct EffectBodyAdvertisementBuilder EffectBodyAdvertisementBuilder;
static XrXirStatus effect_body_advertisements_begin(const XrXirCompileContext *work,
    const XrXirModule *source,EffectRefinementBounds *refinement,EffectBodyAdvertisementBuilder **output);
static XrXirStatus effect_body_advertisements_function(EffectBodyAdvertisementBuilder *builder,uint32_t function);
static XrXirStatus effect_body_advertisements_value(EffectBodyAdvertisementBuilder *builder,
    uint32_t function,uint32_t value,XrXirType actual);
static XrXirStatus effect_body_advertisements_finish(EffectBodyAdvertisementBuilder **builder,
    EffectInvocationDeclaredBounds **output);
static void effect_body_advertisements_free(EffectBodyAdvertisementBuilder *builder);
#include "xxir_effect_snapshot_storage.inc.c"

typedef struct EffectEdge { uint32_t caller, next, instruction; bool cleanup; } EffectEdge;
typedef struct EffectReference { uint32_t caller, next; } EffectReference;
typedef struct EffectGraph {
    uint32_t *heads, *queue;
    uint8_t *queued;
    EffectEdge *edges;
    /* Supplemental GO dependencies are read only by escaping-error flow. */
    uint32_t *error_heads;
    EffectEdge *error_edges;
    uint32_t edge_count, error_edge_count;
    /* Latent callable dependencies never enter execution or GO error flow. */
    uint32_t *reference_heads;
    EffectReference *references;
    /* One inference-local workspace; every function starts with cleared rows. */
    uint64_t *flow_rows;
    size_t flow_capacity;
    /* Fresh construction owns this domain. After publication this is only a
     * borrow from the latest certificate, valid until the next refresh. */
    EffectPhysicalDomain *physical;
    EffectBodyMaterial *material;
    bool physical_owned;
    /* Only the actual ordinary constructor supplies this operation-local input. */
    EffectBodyOrdinaryInput ordinary;
} EffectGraph;

static XrXirStatus effect_graph_physical_refresh(const XrXirCompileContext *work,
    const XrXirModule *module,EffectGraph *graph,EffectRefinementBounds *refinement) {
    if (!graph || (graph->physical_owned && (!graph->physical || !graph->material)))
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyMaterial *fresh=NULL;
    if (!!graph->ordinary.contexts!=!!graph->ordinary.source) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=graph->ordinary.contexts ?
        effect_body_material_generation_input(work,module,refinement,&graph->ordinary,&fresh) :
        effect_body_material_generation(work,module,refinement,&fresh);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,3)) { effect_body_material_free(fresh);return XR_XIR_BUDGET; }
    if (graph->physical_owned) effect_body_material_free(graph->material);
    graph->material=fresh;graph->physical=fresh->physical;graph->physical_owned=true;return XR_XIR_OK;
}

/* This view contains ordinal membership, never a permission. A sealed owner
 * still performs the complete independent physical/source correspondence. */
static XrXirStatus effect_graph_owned_physical_function(const XrXirCompileContext *work,
    const EffectGraph *graph,uint32_t function,
    const EffectPhysicalFunction **output) {
    if (!xir_compile_context_valid(work) || !graph || !output || *output ||
        !effect_body_material_shape(graph->material) || graph->material->resources!=work->resources ||
        graph->physical!=graph->material->physical || !graph->physical ||
        graph->physical->resources!=work->resources || graph->physical->count!=effect_body_count(graph->material) ||
        function>=effect_body_count(graph->material) || !graph->physical->functions)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(work,graph->material);
    EffectBodyFunctionDimensions actual={0};
    XrXirStatus status=effect_body_request_dimensions(&request,function,&actual);
    if (status!=XR_XIR_OK) return status;
    const EffectPhysicalFunction *body=&graph->physical->functions[function];
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    if (body->parameters!=actual.parameter_count || body->instructions!=actual.instruction_count ||
        body->values!=(uint64_t)actual.parameter_count+actual.instruction_count ||
        body->count>body->capacity || body->capacity>body->values ||
        !!body->records!=!!body->capacity) return XR_XIR_BAD_STRUCTURE;
    *output=body;return XR_XIR_OK;
}

static XrXirStatus effect_graph_physical_function(const XrXirCompileContext *work,
    const XrXirModule *module,const EffectGraph *graph,uint32_t function,
    const EffectPhysicalFunction **output) {
    if (!module || !graph || !graph->physical || graph->physical->count!=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    return effect_graph_owned_physical_function(work,graph,function,output);
}

#ifdef XR_XIR_EFFECT_CONTEXT_TESTS
/* Literal whole-owner counterfactuals only. Production transfer is performed
 * by effect_parameters_allocate and effect_invocation_seal. */
/* Only the complete owner can move. An old certificate borrow is never
 * converted into independently owned classification or permission storage. */
static inline XrXirStatus effect_graph_material_take(const XrXirCompileContext *work,
    const XrXirModule *module,EffectGraph *graph,EffectBodyMaterial **output) {
    if (!xir_compile_context_valid(work) || !module || !graph || !output || *output ||
        !graph->physical_owned || !effect_body_material_shape(graph->material) ||
        graph->material->resources!=work->resources || graph->physical!=graph->material->physical ||
        effect_body_count(graph->material)!=module->function_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
    *output=graph->material;graph->material=NULL;graph->physical=NULL;graph->physical_owned=false;
    return XR_XIR_OK;
}
#endif // XR_XIR_EFFECT_CONTEXT_TESTS

static bool effect_root_cause_before(const XrXirRootEffectWitness *a, const XrXirRootEffectWitness *b);

static XrXirStatus effect_invocation_publish(const XrXirCompileContext *work,
    const XrXirModule *module,XrXirEffects *effects,EffectGraph *graph,
    const EffectInvocationDeclaredBounds *declared,const EffectOrdinaryContexts *dense);
static XrXirStatus effect_invocation_effect_edge_mask(const XrXirCompileContext *work,
    const XrXirEffects *effects,uint32_t caller,uint32_t instruction,uint32_t target,uint32_t *output);
static XrXirStatus effect_invocation_public_gate(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t limit);
static XrXirStatus effect_invocation_public_trace(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t limit,uint32_t function,
    const XrXirRootEffects *facts,XrXirRootCauseTrace **output);
#include "xxir_effect_parameters.inc.c"

static void effect_graph_free(EffectGraph *graph) {
    if (graph->physical_owned) effect_body_material_free(graph->material);
    graph->material=NULL;graph->physical=NULL;graph->physical_owned=false;
    xr_compile_resources_free(graph->flow_rows);
    xr_compile_resources_free(graph->reference_heads); xr_compile_resources_free(graph->references);
    xr_compile_resources_free(graph->error_heads); xr_compile_resources_free(graph->error_edges);
    xr_compile_resources_free(graph->heads); xr_compile_resources_free(graph->queue); xr_compile_resources_free(graph->queued); xr_compile_resources_free(graph->edges);
}
void xr_xir_compile_effects_free(XrXirEffects *effects) {
    if (effects) {
        if (effects->entry_roots) {
            for (uint32_t f=0;f<effects->count;++f)
                xr_compile_resources_free((void *)effects->entry_roots[f].parameters);
            xr_compile_resources_free(effects->entry_roots);
        }
        effect_context_owner_free(effects->contexts);
        effect_refinement_free(effects->refinement);
        effect_parameters_free(effects);
        xr_compile_resources_free(effects->errors); xr_compile_resources_free(effects->atoms); xr_compile_resources_free(effects->functions);
        xr_compile_resources_free(effects->witnesses); xr_compile_resources_free(effects->task_errors);
        xr_compile_resources_free(effects->go_safe); xr_compile_resources_free(effects->task_creation);
        xr_compile_resources_free(effects->root); xr_compile_resources_free(effects->root_witnesses);
        xr_compile_resources_free(effects->unresolved_witnesses);
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
static bool effect_seed_type(const XrXirTypes *types,XrXirType callable,
    const XrXirInstruction *instruction,XrXirFunctionEffects *effect) {
    XrXirOp op = instruction->op;
    switch (op) {
    case XR_XIR_SUSPEND: case XR_XIR_TIMER_AFTER_MS: case XR_XIR_TASK_AWAIT:
        effect->suspend = XR_XIR_EFFECT_MAY; return true;
    case XR_XIR_GO: return true;
    case XR_XIR_THROW: case XR_XIR_CLEANUP_REGISTER:
    case XR_XIR_CLEANUP_LEAVE: case XR_XIR_CLEANUP_ERROR: return true;
    case XR_XIR_CALL_REQUIREMENT: {
        const XrXirInterfaceTable *table = types ? types->interfaces : NULL;
        if (!table || instruction->targets[0] >= table->count) return false;
        const XrXirInterfaceDeclaration *declaration = &table->declarations[instruction->targets[0]];
        if (instruction->targets[1] >= declaration->method_count) return false;
        const XrXirTypeNode *signature = xr_xir_callable_signature(types,
            declaration->methods[instruction->targets[1]].signature);
        if (!signature) return false;
        if (!(signature->flags & XR_XIR_CALLABLE_NO_SUSPEND) && effect->suspend < XR_XIR_EFFECT_UNKNOWN)
            effect->suspend = XR_XIR_EFFECT_UNKNOWN;
        return true;
    }
    case XR_XIR_CALL_INDIRECT:
    case XR_XIR_INVOKE_INDIRECT: {
        const XrXirTypeNode *signature=xr_xir_callable_signature(types,callable);
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
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_PROJECT:
    case XR_XIR_CONVERT_NUMBER: case XR_XIR_CONST_FLOAT:
    case XR_XIR_NEG_FLOAT: case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT:
    case XR_XIR_LT_FLOAT: case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT:
    case XR_XIR_GE_FLOAT: case XR_XIR_CELL_PLACE: case XR_XIR_SLOT_PLACE: case XR_XIR_OBJECT_PLACE:
    case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_PLACE_READ: case XR_XIR_PLACE_WRITE:
    case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_GET: case XR_XIR_ARRAY_SET:
    case XR_XIR_ARRAY_PUSH: case XR_XIR_ARRAY_LEN: case XR_XIR_ARRAY_CAPACITY:
    case XR_XIR_ARRAY_WITH_CAPACITY: case XR_XIR_ARRAY_RESERVE: case XR_XIR_ADD_FLOAT:
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
static bool effect_seed(const XrXirModule *module,const XrXirFunction *function,
    const XrXirInstruction *instruction,XrXirFunctionEffects *effect) {
    XrXirType callable=XR_XIR_UNIT;
    if (instruction->op==XR_XIR_CALL_INDIRECT || instruction->op==XR_XIR_INVOKE_INDIRECT)
        callable=xr_xir_operand_type(function,(uint32_t)instruction->immediate);
    return effect_seed_type(module->types,callable,instruction,effect);
}
#include "xxir_effects_root.inc.c"

static XrXirStatus effect_reference_graph(const XrXirModule *module,
    EffectGraph *graph, uint32_t count, XrXirCompileContext *work) {
    if (!count) return XR_XIR_OK;
    uint64_t bytes = (uint64_t)module->function_count * sizeof(*graph->reference_heads) +
        (uint64_t)count * sizeof(*graph->references);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    graph->reference_heads = xir_compile_alloc(work,
        (size_t)module->function_count * sizeof(*graph->reference_heads), &status);
    if (!graph->reference_heads) return status;
    graph->references = xir_compile_alloc(work, (size_t)count * sizeof(*graph->references), &status);
    if (!graph->references) return status;
    if (!xir_compile_work(work, module->function_count)) return XR_XIR_BUDGET;
    for (uint32_t f = 0; f < module->function_count; ++f) graph->reference_heads[f] = UINT32_MAX;
    uint32_t at = 0;
    /* Prepending in reverse source order preserves the original ascending
     * caller queue order, including multiple references to the same callee. */
    for (uint32_t f = module->function_count; f-- > 0;) {
        EffectBodyRequest request=effect_body_owned_request(work,graph->material);
        EffectBodyFunctionDimensions head={0};XrXirStatus read_status=effect_body_request_dimensions(&request,f,&head);
        if (read_status!=XR_XIR_OK) return read_status;
        const EffectBodyFunctionDimensions *function=&head;
        const EffectPhysicalFunction *physical=NULL;
        XrXirStatus classified=effect_graph_physical_function(work,module,graph,f,&physical);
        if (classified!=XR_XIR_OK) return classified;
        for (uint32_t r=physical->count;r-- > 0;) {
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=physical->records[r];
            if (!(record.mask&EFFECT_PHYSICAL_REFERENCE)) continue;
            if (record.value<function->parameter_count || record.value>=physical->values)
                return XR_XIR_BAD_STRUCTURE;
            uint32_t i=record.value-function->parameter_count;
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            read_status=effect_body_request_instruction(&request,f,i,&storage,&op);
            if (read_status!=XR_XIR_OK) return read_status;
            if (op->op != XR_XIR_FUNCTION_REF) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(work, 5)) return XR_XIR_BUDGET;
            if (op->immediate < 0 || (uint64_t)op->immediate >= module->function_count || at >= count)
                return XR_XIR_BAD_STRUCTURE;
            uint32_t callee = (uint32_t)op->immediate;
            graph->references[at] = (EffectReference){f, graph->reference_heads[callee]};
            graph->reference_heads[callee] = at++;
        }
    }
    return at == count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}

/* Control dependencies consume this operation's complete owned input. A
 * certificate borrow cannot restart construction or admit another raw input. */
typedef struct EffectGraphDependencyCounts {
    uint32_t edges,references;
} EffectGraphDependencyCounts;

/* Every dependency replay starts at the current owning body and reaches its
 * exact canonical EOF. A constant physical opcode avoids unused row copies;
 * each real dependency still enters the original semantic handler. */
static XrXirStatus effect_graph_dependencies_count(const XrXirCompileContext *remaining,
    XrXirEffects *effects,const EffectGraph *graph,uint32_t f,bool symbolic,
    EffectGraphDependencyCounts *counts) {
    EffectBodyRequest request=effect_body_owned_request(remaining,graph->material);
    EffectBodyFunctionDimensions head={0};XrXirStatus status=effect_body_request_dimensions(&request,f,&head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionDimensions *function=&head;
    const EffectPhysicalFunction *physical=NULL;
    status=effect_graph_owned_physical_function(remaining,graph,f,&physical);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunction *owned=&graph->material->functions[f];
    EffectBodyRecordCursor cursor={0,0,0,UINT32_MAX};uint32_t r=0;
    if (!xir_compile_work(remaining,sizeof(cursor)+sizeof(r))) return XR_XIR_BUDGET;
    while (r<physical->count && physical->records[r].value<function->parameter_count) {
        if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
        if (physical->records[r++].mask&(EFFECT_PHYSICAL_CONTROL|EFFECT_PHYSICAL_DIRECT|
            EFFECT_PHYSICAL_GO|EFFECT_PHYSICAL_REFERENCE)) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t run=0;run<owned->run_count;++run) {
        EffectBodyRunRef ref;uint32_t opcode=XR_XIR_INVALID;
        status=effect_body_range_ref(&request.read,owned,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        if (!(ref.mask&UINT32_C(1))) {
            status=effect_body_run_word(&request.read,&ref,ref.first,0,&opcode);
            if (status!=XR_XIR_OK) return status;
            if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        }
        while (r<physical->count && physical->records[r].value-function->parameter_count<ref.first+ref.count) {
            if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=physical->records[r++];
            if (record.value<function->parameter_count || record.value>=physical->values ||
                record.value-function->parameter_count<ref.first) return XR_XIR_BAD_STRUCTURE;
            if (!(record.mask&(EFFECT_PHYSICAL_CONTROL|EFFECT_PHYSICAL_DIRECT|
                EFFECT_PHYSICAL_GO|EFFECT_PHYSICAL_REFERENCE))) continue;
            if (ref.mask&UINT32_C(1)) {
                status=effect_body_run_word(&request.read,&ref,record.value-function->parameter_count,0,&opcode);
                if (status!=XR_XIR_OK) return status;
                if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            }
            XrXirOp op=(XrXirOp)opcode;
            if (op==XR_XIR_GO) effects->task_creation[f]=XR_XIR_EFFECT_MAY;
            if (symbolic && op==XR_XIR_FUNCTION_REF) {
                if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
                if (counts->references==UINT32_MAX) return XR_XIR_BUDGET;
                ++counts->references;
            }
            if (op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_CALL_DEFAULT || op==XR_XIR_CALL ||
                op==XR_XIR_INVOKE || op==XR_XIR_CLEANUP_REGISTER) {
                if (counts->edges==UINT32_MAX) return XR_XIR_BUDGET;
                ++counts->edges;
            }
        }
    }
    if (r!=physical->count || cursor.first!=owned->instruction_count ||
        cursor.offset!=owned->stream_words) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus effect_graph_build_material(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, XrXirCompileContext *remaining) {
    if (!xir_compile_context_valid(remaining) || !module || !effects || !graph ||
        effects->resources!=remaining->resources || effects->count!=module->function_count ||
        !effects->functions || !effects->task_creation || !effect_body_material_shape(graph->material) ||
        graph->material->resources!=remaining->resources || graph->physical!=graph->material->physical ||
        !graph->physical || effect_body_count(graph->material)!=effects->count ||
        (!graph->physical_owned && graph->material!=effects->generation) || graph->heads || graph->queue ||
        graph->queued || graph->edges || graph->references || graph->reference_heads)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(remaining,14)) return XR_XIR_BUDGET;
    XrXirStatus allocation_status = XR_XIR_OK;
    EffectGraphDependencyCounts counts={0};
    if (!xir_compile_work(remaining,2*sizeof(counts))) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    if (!graph->material->control) return XR_XIR_BAD_STRUCTURE;
    uint64_t control_bytes=(uint64_t)module->function_count*sizeof(*effects->functions);
    if (!xir_compile_work(remaining,control_bytes)) return XR_XIR_BUDGET;
    memcpy(effects->functions,graph->material->control,(size_t)control_bytes);
    bool symbolic=graph->material->ordinary_identities || graph->material->evidence_kind==XR_XIR_EVIDENCE_TEMPLATE;
    for (uint32_t t=0;t<graph->material->domain.types->count && !symbolic;++t) {
        if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
        if (graph->material->domain.types->nodes[t].kind==XR_XIR_TYPE_CELL) symbolic=true;
    }
    for (uint32_t f=0;f<module->function_count;++f) {
        status=effect_graph_dependencies_count(remaining,effects,graph,f,symbolic,&counts);
        if (status!=XR_XIR_OK) return status;
    }
    uint32_t edges=counts.edges,references=counts.references;
    uint64_t bytes = (uint64_t) module->function_count * (2 * sizeof(uint32_t) + sizeof(uint8_t)) +
        (uint64_t) edges * sizeof(EffectEdge);
    if ((bytes > SIZE_MAX) || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
    graph->edge_count = edges;
    graph->heads = xir_compile_calloc(remaining, module->function_count, sizeof(*graph->heads), &allocation_status);
    graph->queue = xir_compile_calloc(remaining, module->function_count, sizeof(*graph->queue), &allocation_status);
    graph->queued = xir_compile_calloc(remaining, module->function_count, sizeof(*graph->queued), &allocation_status);
    if (edges) graph->edges = xir_compile_calloc(remaining, edges, sizeof(*graph->edges), &allocation_status);
    if (!graph->heads || !graph->queue || !graph->queued || (edges && !graph->edges))
        return allocation_status;
    for (uint32_t f = 0; f < module->function_count; ++f) graph->heads[f] = UINT32_MAX;
    uint32_t at = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        EffectBodyRequest request=effect_body_owned_request(remaining,graph->material);
        EffectBodyFunctionDimensions head={0};status=effect_body_request_dimensions(&request,f,&head);
        if (status!=XR_XIR_OK) return status;
        const EffectBodyFunctionDimensions *function=&head;
        const EffectPhysicalFunction *physical=NULL;
        status=effect_graph_physical_function(remaining,module,graph,f,&physical);
        if (status!=XR_XIR_OK) return status;
        const EffectBodyFunction *owned=&graph->material->functions[f];
        EffectBodyRecordCursor cursor={0,0,0,UINT32_MAX};uint32_t r=0;
        if (!xir_compile_work(remaining,sizeof(cursor)+sizeof(r))) return XR_XIR_BUDGET;
        while (r<physical->count && physical->records[r].value<function->parameter_count) {
            if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
            if (physical->records[r++].mask&EFFECT_PHYSICAL_DIRECT) return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t run=0;run<owned->run_count;++run) {
            EffectBodyRunRef ref;XrXirInstruction constant={0};bool decoded=false;
            if (!xir_compile_work(remaining,sizeof(constant)+sizeof(decoded))) return XR_XIR_BUDGET;
            status=effect_body_range_ref(&request.read,owned,&cursor,&ref);
            if (status!=XR_XIR_OK) return status;
            while (r<physical->count && physical->records[r].value-function->parameter_count<ref.first+ref.count) {
                if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
                EffectPhysicalRecord record=physical->records[r++];
                if (record.value<function->parameter_count || record.value>=physical->values ||
                    record.value-function->parameter_count<ref.first) return XR_XIR_BAD_STRUCTURE;
                if (!(record.mask&EFFECT_PHYSICAL_DIRECT)) continue;
                uint32_t i=record.value-function->parameter_count,words[EFFECT_BODY_WORDS];
                XrXirInstruction storage;const XrXirInstruction *op=NULL;
                if (ref.mask || !decoded) {
                    status=effect_body_instruction_ref_words(&request.read,&ref,i,words);
                    if (status==XR_XIR_OK) status=effect_body_instruction_words_publish(&request.read,words,
                        ref.mask?&storage:&constant);
                    if (status!=XR_XIR_OK) return status;
                    if (!ref.mask) {
                        if (!xir_compile_work(remaining,sizeof(decoded))) return XR_XIR_BUDGET;
                        decoded=true;
                    }
                }
                if (!xir_compile_work(remaining,sizeof(op))) return XR_XIR_BUDGET;
                op=ref.mask?&storage:&constant;
                if (op->op!=XR_XIR_INVOKE_DEFAULT && op->op!=XR_XIR_CALL_DEFAULT && op->op!=XR_XIR_CALL &&
                    op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_CLEANUP_REGISTER) return XR_XIR_BAD_STRUCTURE;
                uint32_t callee=(uint32_t)op->immediate;
                if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
                    const XrXirDefaultBinding *binding=NULL;
                    const uint32_t *identity=xr_xir_default_identity(op);
                    XrXirStatus looked_up=xir_metadata_default_lookup(remaining,&request.domain,identity[0],identity[1],&binding);
                    if (looked_up!=XR_XIR_OK) return looked_up;
                    if (!binding) return XR_XIR_BAD_STRUCTURE;
                    callee=binding->function;
                }
                if (callee>=module->function_count || at>=edges) return XR_XIR_BAD_STRUCTURE;
                graph->edges[at]=(EffectEdge){f,graph->heads[callee],i,op->op==XR_XIR_CLEANUP_REGISTER};
                graph->heads[callee]=at++;
            }
        }
        if (r!=physical->count || cursor.first!=owned->instruction_count ||
            cursor.offset!=owned->stream_words) return XR_XIR_BAD_STRUCTURE;
    }
    if (at!=edges) return XR_XIR_BAD_STRUCTURE;
    return effect_reference_graph(module, graph, references, remaining);
}
static XrXirStatus effect_graph_build(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, XrXirCompileContext *remaining) {
    XrXirStatus status=effect_graph_physical_refresh(remaining,module,graph,effects->refinement);
    return status==XR_XIR_OK?effect_graph_build_material(module,effects,graph,remaining):status;
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
            XrXirFunctionEffects *to = &effects->functions[link.caller];
            bool changed = false;
            if (!link.cleanup && from.suspend > to->suspend) { to->suspend = from.suspend; changed = true; }
            if (!link.cleanup && effects->task_creation[callee] > effects->task_creation[link.caller]) {
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
#include "xxir_effect_errors.inc.c"
#include "xxir_go_safe.inc.c"
#include "xxir_effect_context.inc.c"
#include "xxir_effect_context_parameters.inc.c"
#include "xxir_effect_snapshot_initial_ordinary.inc.c"
#include "xxir_effect_context_roots.inc.c"
#include "xxir_effect_context_forest.inc.c"
#include "xxir_effect_context_scalar.inc.c"
#include "xxir_effect_context_dense.inc.c"
#include "xxir_effect_refinement_bounds.inc.c"
#include "xxir_effect_invocation_bounds.inc.c"
#include "xxir_effect_generation_advertisements.inc.c"
#include "xxir_effect_context_owner.inc.c"
#include "xxir_effect_context_query.inc.c"
#include "xxir_effect_invocations.inc.c"
#include "xxir_effect_invocation_public_trace.inc.c"
#include "xxir_effect_invocation_publish.inc.c"
#include "xxir_effect_error_invocations.inc.c"

/* A breadth-first forest over final facts cannot inherit a cyclic cause chain
 * from recursive fixed-point updates. Each function enters the queue once. */
static XrXirStatus effect_witnesses(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, const XrXirCompileContext *work) {
    uint32_t front = 0, back = 0;
    for (uint32_t f = 0; f < effects->count; ++f) {
        EffectBodyRequest body=effect_body_owned_request(work,graph->material);
        EffectBodyFunctionDimensions dimensions={0};
        XrXirStatus head_status=effect_body_request_dimensions(&body,f,&dimensions);
        if (head_status!=XR_XIR_OK) return head_status;
        const EffectBodyFunctionDimensions *function=&dimensions;
        XrXirEffect fact = effects->functions[f].suspend;
        /* The pre-existing NONE precharge is retained in this slice. No
         * accounting reduction is attributed to a walk that never occurred. */
        if (fact == XR_XIR_EFFECT_NONE) {
            if (!xir_compile_work(work,(uint64_t)function->instruction_count+1)) return XR_XIR_BUDGET;
            continue;
        }
        const EffectPhysicalFunction *physical=NULL;
        XrXirStatus status=effect_graph_physical_function(work,module,graph,f,&physical);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t r=0;r<physical->count;++r) {
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=physical->records[r];
            if (!(record.mask&EFFECT_PHYSICAL_CONTROL)) continue;
            if (record.value<function->parameter_count || record.value>=physical->values)
                return XR_XIR_BAD_STRUCTURE;
            uint32_t i=record.value-function->parameter_count;
            XrXirInstruction storage;const XrXirInstruction *instruction=NULL;
            status=effect_body_request_instruction(&body,f,i,&storage,&instruction);
            if (status!=XR_XIR_OK) return status;
            XrXirOp op=instruction->op;XrXirType callable=XR_XIR_UNIT;
            if (op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT) {
                if (instruction->immediate<0 || (uint64_t)instruction->immediate>=physical->values) return XR_XIR_BAD_STRUCTURE;
                status=effect_body_request_type(&body,f,(uint32_t)instruction->immediate,&callable);
                if (status!=XR_XIR_OK) return status;
            }
            XrXirFunctionEffects local={0};
            if (!effect_seed_type(body.domain.types,callable,instruction,&local)) return XR_XIR_BAD_STRUCTURE;
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

/* Seal only after the common formula owner and all other analyses agree.
 * The publication owns a second, compact copy rather than borrowing formulas. */
static XrXirStatus effect_entry_roots_seal(const XrXirModule *module,
    XrXirEffects *effects, const XrXirCompileContext *work) {
    if (effects->entry_roots || effects->count!=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!effects->invocations || !effect_body_material_shape(effects->invocations->body) ||
        effects->invocations->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest body=effect_body_owned_request(work,effects->invocations->body);
    XrXirStatus status=XR_XIR_OK;
    effects->entry_roots=xir_compile_calloc(work,effects->count,
        sizeof(*effects->entry_roots),&status);
    if (!effects->entry_roots) return status;
    for (uint32_t f=0;f<effects->count;++f) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        EffectBodyFunctionHead function={0};status=effect_body_request_head(&body,f,&function);
        if (status!=XR_XIR_OK) return status;
        XirEffectEntryRootView *view=&effects->entry_roots[f];
        if (!effects->contracts) {
            view->intrinsic_mask=(effects->root[f].requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
                (effects->root[f].unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
            continue;
        }
        const XrXirRootFormula *formula=&effects->contracts[f].formula;
        view->intrinsic_mask=formula->constant_mask;
        for (uint32_t t=0;t<formula->term_count;++t) {
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            const XrXirRootTerm *term=&formula->terms[t];
            if (term->kind==XR_XIR_ROOT_TERM_CELL_PARAMETER) {
                if (term->index>=function.parameter_count ||
                    !xr_xir_type_is_cell(body.domain.types,function.parameters[term->index]))
                    return XR_XIR_BAD_STRUCTURE;
                ++view->parameter_count;
            } else if (term->kind==XR_XIR_ROOT_TERM_CONTEXT_CALL) {
                view->intrinsic_mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            } else if (term->kind==XR_XIR_ROOT_TERM_PARAMETER) {
                if (term->index>=function.parameter_count ||
                    effects->contracts[f].parameters[term->index].kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
                    return XR_XIR_BAD_STRUCTURE;
                const XrXirTypeNode *signature=xr_xir_callable_signature(body.domain.types,
                    function.parameters[term->index]);
                if (!signature) return XR_XIR_BAD_TYPE;
                view->intrinsic_mask|=signature->flags&
                    (XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED);
            } else return XR_XIR_BAD_STRUCTURE;
        }
        if (view->parameter_count) {
            uint32_t *parameters=xir_compile_calloc(work,view->parameter_count,sizeof(*parameters),&status);
            if (!parameters) return status;
            view->parameters=parameters;
            uint32_t at=0;
            for (uint32_t t=0;t<formula->term_count;++t) {
                if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
                if (formula->terms[t].kind!=XR_XIR_ROOT_TERM_CELL_PARAMETER) continue;
                uint32_t parameter=formula->terms[t].index;
                if (at && parameters[at-1]>=parameter) return XR_XIR_BAD_STRUCTURE;
                parameters[at++]=parameter;
            }
            if (at!=view->parameter_count) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}

#include "xxir_effect_invocation_producer.inc.c"
#include "xxir_effect_invocation_runtime.inc.c"

XR_FUNC XrXirStatus xir_effects_entry_root(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, XirEffectEntryRootView *output) {
    if (!output || !xir_effects_context_matches(context,effects,effects?effects->count:0) ||
        !effects->entry_roots || function>=effects->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    *output=effects->entry_roots[function];return XR_XIR_OK;
}

static XrXirStatus effect_infer(const XrXirCompileContext *compile_context,
    const XrXirModule *module, const XrXirRootRefiner *refiner, XrXirEffects **output) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context) || !module || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;

    uint64_t bytes = sizeof(XrXirEffects) + (uint64_t) module->function_count *
        (sizeof(XrXirFunctionEffects) + sizeof(XrXirEffectWitness) + sizeof(XrXirRootEffects) +
            2 * sizeof(XrXirRootEffectWitness));
    if ((bytes > SIZE_MAX) || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirEffects *effects = xir_compile_calloc(compile_context, 1, sizeof(*effects), &allocation_status);
    if (!effects) return allocation_status;
    effects->resources = compile_context->resources;
    effects->count = module->function_count;
    effects->functions = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->functions), &allocation_status);
    effects->witnesses = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->witnesses), &allocation_status);
    effects->task_errors = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->task_errors), &allocation_status);
    effects->go_safe = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->go_safe), &allocation_status);
    effects->task_creation = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->task_creation), &allocation_status);
    effects->root = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->root), &allocation_status);
    effects->root_witnesses = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->root_witnesses), &allocation_status);
    effects->unresolved_witnesses = xir_compile_calloc(compile_context, effects->count, sizeof(*effects->unresolved_witnesses), &allocation_status);
    if (!effects->functions || !effects->witnesses || !effects->task_errors || !effects->go_safe ||
        !effects->task_creation || !effects->root || !effects->root_witnesses ||
        !effects->unresolved_witnesses) { xr_xir_compile_effects_free(effects); return allocation_status; }
    EffectGraph graph = {0};
    bool changed = false;
    XrXirStatus status = refiner ? refiner->update(refiner->context,effects,&changed) : XR_XIR_OK;
    if (status == XR_XIR_OK) status = effect_graph_build(module, effects, &graph, remaining);
    if (status == XR_XIR_OK) status = effect_parameters_derive(module, effects, &graph, remaining);
    if (status == XR_XIR_OK) status = effect_invocation_publish(remaining,module,effects,&graph,NULL,NULL);
    if (status == XR_XIR_OK) status = effect_propagate(effects, &graph, remaining);
    if (status == XR_XIR_OK) status = effect_context_owner_refresh(remaining,module,effects,effects->invocations->body);
    while (status == XR_XIR_OK && refiner) {
        changed = false;
        status = refiner->update(refiner->context,effects,&changed);
        if (status != XR_XIR_OK) break;
        if (!changed && effects->refinement) {
            /* Discard temporary pre-bottom SSA storage and rebuild from the
             * final graph. Real CALL_BIND declarations remain source bounds. */
            effect_refinement_free(effects->refinement);effects->refinement=NULL;
            /* Final equation input has changed even when Source's body is
             * stable. Seal it before Source copies the canonical contracts. */
            status=effect_graph_physical_refresh(remaining,module,&graph,effects->refinement);
            if (status==XR_XIR_OK) status=effect_invocation_publish(remaining,module,effects,&graph,NULL,NULL);
            if (status==XR_XIR_OK) status=effect_context_owner_refresh(remaining,module,effects,effects->invocations->body);
            if (status!=XR_XIR_OK) break;
            status=refiner->update(refiner->context,effects,&changed);
        }
        if (status != XR_XIR_OK || !changed) break;
        status = effect_graph_physical_refresh(remaining,module,&graph,effects->refinement);
        if (status == XR_XIR_OK) status = effect_parameters_derive(module, effects, &graph, remaining);
        if (status == XR_XIR_OK) status = effect_invocation_publish(remaining,module,effects,&graph,NULL,NULL);
        if (status == XR_XIR_OK) status = effect_propagate(effects,&graph,remaining);
        if (status == XR_XIR_OK) status = effect_context_owner_refresh(remaining,module,effects,effects->invocations->body);
    }
    if (status == XR_XIR_OK) status = effect_go_safe(module, effects, &graph, remaining);
    if (status == XR_XIR_OK) status = effect_witnesses(module, effects, &graph, remaining);
    if (status==XR_XIR_OK && effects->contexts)
        status=effect_context_owner_project(remaining,module,effects,effects->contexts);
    if (status == XR_XIR_OK) status = effect_errors_analyze(effects, &graph, remaining);
    if (status == XR_XIR_OK && !effects->contexts)
        status=effect_invocation_public_gate(remaining,effects->invocations,effects->count);
    if (status == XR_XIR_OK) status = effect_entry_roots_seal(module,effects,remaining);
    effect_graph_free(&graph);
    if (status != XR_XIR_OK) { xr_xir_compile_effects_free(effects); return status; }
    *output = effects; return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_effects_infer_verified(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirEffects **output) {
    return effect_infer(context,module,NULL,output);
}
XR_FUNC XrXirStatus xr_xir_compile_effects_refine_verified(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirRootRefiner *refiner, XrXirEffects **output) {
    if (!refiner || !refiner->update || !refiner->context) return XR_XIR_BAD_STRUCTURE;
    return effect_infer(context,module,refiner,output);
}
XrXirStatus xr_xir_compile_effects_analyze(const XrXirArtifact *artifact, XrXirEffects **output) {
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = artifact->context;
    XrXirCompileContext *budget = &compile_state;
    if (!output) return XR_XIR_BAD_STRUCTURE;

    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    if (!module || (module->stage != XR_XIR_CHECKED && module->stage != XR_XIR_LOWERED)) return XR_XIR_BAD_STAGE;
    if (*output) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext remaining = *budget;
    return xr_xir_compile_verify_analyzed_effects_v2(&remaining, module, artifact->construction, output, NULL);
}
