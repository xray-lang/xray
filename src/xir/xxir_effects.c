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
#include "xxir_effect_scc_internal.h"
#include "xxir_read_scratch.h"
#include "xxir_metadata_domain_internal.h"
#include "xxir_defaults_internal.h"
#include "xxir_internal.h"
#include "xxir_source_replay_internal.h"
#include "xxir_types.h"
#include "xxir_native_callable.h"
#include "xxir_class_hierarchy_internal.h"
#include "xxir_class_method_read_internal.h"
#include "xxir_interface.h"
#include "xxir_constraint_proof.h"
#include "xxir_operand_roles.h"
#include "xxir_cell_provenance_internal.h"
#include "../base/xmalloc.h"
#include <stdatomic.h>

/* Private execution contexts share the existing equation-ID bound. Repeated
 * environments do not add declarations to the admitted source function table. */

#include "xxir_effect_contract.inc.c"

#include "xxir_effect_input_facts.inc.c"

static XrXirStatus effect_source_body_current(const XrXirCompileContext *context,
    const XrXirModule *source, const XrXirEffects *effects);

static XrXirStatus effect_source_input_terminal(const XrXirCompileContext *context,
    const XrXirModule *source,const XrXirEffects *effects);

typedef struct EffectCheckedRoots EffectCheckedRoots;
static void effect_checked_roots_free(EffectCheckedRoots *roots);
static XrXirStatus effect_checked_reference_read(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,uint32_t instruction,uint32_t *output);
static XrXirStatus effect_checked_trace_copy(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,XrXirRootCauseTrace **output);
typedef struct EffectFinalOutcomes {
    XrCompileResources *resources;
    bool owns_formulas;
    uint32_t count;
    XrXirFunctionEffects *functions;
    XrXirEffectWitness *witnesses;
    EffectControlSummary *control_summary;
    XrXirStatus *go_safe;
    XrXirEffect *task_creation;
    XrXirRootEffects *root;
    XrXirRootEffectWitness *root_witnesses, *unresolved_witnesses;
    XrXirFunctionEffectContract *contracts;
} EffectFinalOutcomes;

/* Formula storage belongs to the canonical summary after ROOT publication.
 * Partial constructors alone own their provisional formula allocations. */
static void effect_contracts_clear(XrXirFunctionEffectContract *contracts,uint32_t count,bool formulas) {
    if (!contracts) return;
    for (uint32_t f=0;f<count;++f) {
        xr_compile_resources_free((void *)contracts[f].parameters);
        if (formulas) xr_compile_resources_free((void *)contracts[f].formula.terms);
        xr_compile_resources_free((void *)contracts[f].values);
        xr_compile_resources_free((void *)contracts[f].bindings);
    }
    xr_compile_resources_free(contracts);
}

static void effect_outcomes_clear(EffectFinalOutcomes *outcomes,uint32_t count) {
    xir_effect_control_free(outcomes->control_summary);
    effect_contracts_clear(outcomes->contracts,count,outcomes->owns_formulas);
    xr_compile_resources_free(outcomes->go_safe);
    xr_compile_resources_free(outcomes->root);xr_compile_resources_free(outcomes->root_witnesses);
    xr_compile_resources_free(outcomes->unresolved_witnesses);
    *outcomes=(EffectFinalOutcomes){0};
}

static bool effect_outcomes_empty(const EffectFinalOutcomes *outcomes);

static const EffectFinalOutcomes *effect_final_outcomes(const XrXirEffects *effects);

struct XrXirEffects {
    atomic_uint_fast64_t references;
    /* Each charged allocation independently retains this ledger. */
    XrCompileResources *resources;
    EffectInputFacts *input_facts;
    uint32_t count;
    /* Only construction owns these mutable records. Successful publication
     * moves every allocation into the actual Final and empties this staging. */
    EffectFinalOutcomes outcomes_work;
    EffectFinalOutcomes *outcomes_owner;
    EffectFinalOutcomes *final_outcomes;
    EffectCheckedRoots *checked_roots;

};

/* Public queries require the complete Final. Private construction and the
 * request-owned Source replay have explicit, disjoint read inputs. */
static const EffectFinalOutcomes *effect_roots_read(const XrXirEffects *effects) {
    if (!effects) return NULL;
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    if (final) return final;
    return &effects->outcomes_work;
}

static const XrXirFunctionEffectContract *effect_contracts_read(const XrXirEffects *effects) {
    const EffectFinalOutcomes *records=effect_roots_read(effects);
    return records?records->contracts:NULL;
}

/* Published readers borrow the actual Final. Only a live constructor has a
 * mutable context slot; request-only ROOT borrows never own context storage. */

/* Ledger identity rejects foreign requests; it is not an admission proof. */
XR_FUNC bool xir_effects_context_matches(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t functions) {
    return xir_compile_context_valid(context) && effects && functions &&
        effects->resources==context->resources && effects->count==functions;
}

#include "xxir_effect_terms.inc.c"
#include "xxir_effect_context_types.inc.c"
typedef struct EffectRootSeedRecord {
    const XirMetadataDomain *domain;
    const XrXirInstruction *instruction;
    uint32_t function,ordinal;
    XrXirType callable_type;
} EffectRootSeedRecord;
#include "xxir_local_errors.inc.c"
#include "xxir_effect_parameters.inc.c"

void xr_xir_compile_effects_free(XrXirEffects *effects) {
    if (effects) {
        uint_fast64_t previous=atomic_fetch_sub_explicit(&effects->references,1,memory_order_acq_rel);
        XR_CHECK(previous,"A published effect owner must be released exactly once");
        if(previous!=1)return;
        if (effects->final_outcomes) {
            effect_outcomes_clear(effects->final_outcomes,effects->count);
            xr_compile_resources_free(effects->final_outcomes);
        }
        effect_checked_roots_free(effects->checked_roots);
        effect_outcomes_clear(&effects->outcomes_work,effects->count);
        xr_compile_resources_free(effects->outcomes_owner);
        effect_input_facts_free(effects->input_facts);
        xr_compile_resources_free(effects);
    }
}
XR_FUNC XrXirStatus xir_effects_retain(const XrXirCompileContext *context,
    XrXirEffects *effects,XrXirEffects **output,XrCompileRequestControl *control) {
    if(!xir_compile_context_valid(context)||!effects||!output||*output||
        effects->resources!=context->resources)return XR_XIR_BAD_STRUCTURE;
    uint_fast64_t count=atomic_load_explicit(&effects->references,memory_order_relaxed);
    uint32_t progress=0;
    for(;;) {
        if(!count||count==UINT_FAST64_MAX)return XR_XIR_OVERFLOW;
        XrXirStatus status=xir_receive_step(control,&progress);
        if(status!=XR_XIR_OK)return status;
        if(atomic_compare_exchange_weak_explicit(&effects->references,&count,count+1,
            memory_order_relaxed,memory_order_relaxed))break;
    }
    *output=effects;
    return XR_XIR_OK;
}
XR_FUNC XrXirEffectFacts xr_xir_effects_read(const XrXirEffects *effects,uint32_t function) {
    XrXirEffectFacts read={.status=XR_XIR_BAD_STRUCTURE};
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    if (!final || function>=final->count) return read;
    read.status=XR_XIR_OK;read.control=final->functions[function];read.root=final->root[function];
    read.suspend_witness=final->witnesses[function];read.root_witness=final->root_witnesses[function];
    read.unresolved_witness=final->unresolved_witnesses[function];return read;
}
/* Source refinement reads only its genuine unpublished constructor. This
 * capability cannot recover provisional values through a public query. */
XR_FUNC XrXirStatus xir_effects_construction_root(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,XrXirRootEffects *output) {
    if (!output || !xir_effects_context_matches(context,effects,effects?effects->count:0) ||
        !effects->outcomes_owner || !effects->outcomes_work.root || function>=effects->count)
        return XR_XIR_BAD_STRUCTURE;
    *output=effects->outcomes_work.root[function];return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_effects_go_safe(const XrXirEffects *effects, uint32_t function) {
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    return final && function<final->count?final->go_safe[function]:XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirEffect xr_xir_effects_task_creation(const XrXirEffects *effects, uint32_t function) {
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    return final && function<final->count?final->task_creation[function]:XR_XIR_EFFECT_UNKNOWN;
}
/* An unclassified opcode must not acquire an accidental no-effect proof. */
typedef enum EffectSeedProjection {
    EFFECT_SEED_INVALID,EFFECT_SEED_LOCAL,EFFECT_SEED_FIELDS
} EffectSeedProjection;

/* Opcode-only control terminals are shared by construction and receiving.
 * Metadata-dependent calls require their actual operand/signature fields. */
static EffectSeedProjection effect_seed_opcode(XrXirOp op,XrXirFunctionEffects *effect) {
    switch (op) {
    case XR_XIR_SCOPE_BEGIN: case XR_XIR_SCOPE_JOIN: case XR_XIR_ITERATOR_HAS_NEXT: case XR_XIR_ITERATOR_NEXT: case XR_XIR_ITERATOR_NTH: case XR_XIR_SUSPEND: case XR_XIR_TIMER_AFTER_MS: case XR_XIR_TASK_AWAIT: case XR_XIR_TASK_AWAIT_RESULT: case XR_XIR_GENERATOR_YIELD:
    case XR_XIR_CHANNEL_SEND: case XR_XIR_CHANNEL_RECV: case XR_XIR_CHANNEL_RECV_OR:
    case XR_XIR_CHANNEL_SEND_TIMEOUT: case XR_XIR_CHANNEL_RECV_TIMEOUT: case XR_XIR_CHANNEL_SELECT:
        effect->suspend = XR_XIR_EFFECT_MAY; return EFFECT_SEED_LOCAL;
    /* Lifecycle drain performs scheduler work without a language suspension. */
    case XR_XIR_ITERATOR_CLOSE: case XR_XIR_GO: case XR_XIR_SCOPED_GO: case XR_XIR_GENERATOR_NEW: return EFFECT_SEED_LOCAL;
    case XR_XIR_THROW: case XR_XIR_CLEANUP_REGISTER:
    case XR_XIR_CLEANUP_LEAVE: case XR_XIR_CLEANUP_ERROR: return EFFECT_SEED_LOCAL;
    case XR_XIR_CALL_REQUIREMENT: case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT:
        return EFFECT_SEED_FIELDS;
    case XR_XIR_INVOKE_DEFAULT: case XR_XIR_CALL_DEFAULT: case XR_XIR_CALL: case XR_XIR_INVOKE:
    /* View faults use the panic protocol without language suspension. */
    case XR_XIR_VIEW_NEW: case XR_XIR_VIEW_REBORROW: case XR_XIR_VIEW_LEN:
    case XR_XIR_VIEW_GET: case XR_XIR_VIEW_SET: case XR_XIR_VIEW_COPY:
    case XR_XIR_VIEW_END: case XR_XIR_VIEW_CLEANUP:
    case XR_XIR_PTR_NULL: case XR_XIR_PTR_READONLY: case XR_XIR_PTR_OFFSET:
    case XR_XIR_PTR_LOAD: case XR_XIR_PTR_STORE: case XR_XIR_PTR_COPY: case XR_XIR_PTR_EQ:
    case XR_XIR_F64_TRY_PARSE: case XR_XIR_STRING_COPY_BYTES: case XR_XIR_STRING_REPLACE:
    case XR_XIR_CONST_RUNE: case XR_XIR_RUNE_IS_WHITESPACE: case XR_XIR_RUNE_TO_INTEGER: case XR_XIR_INTEGER_TO_RUNE:
    case XR_XIR_LT_STRING: case XR_XIR_LE_STRING: case XR_XIR_GT_STRING: case XR_XIR_GE_STRING:
    case XR_XIR_TASK_CANCEL: case XR_XIR_TASK_POLL:
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_CONST_STRING:
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_STORE: case XR_XIR_SLOT_GROUP_INIT:
    case XR_XIR_CHANNEL_TRY_SEND: case XR_XIR_CHANNEL_TRY_RECV:
    case XR_XIR_CHANNEL_NEW: case XR_XIR_CHANNEL_CAPACITY: case XR_XIR_CHANNEL_IS_CLOSED: case XR_XIR_CHANNEL_CLOSE:
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
    case XR_XIR_CELL_LOCAL_WRITE: case XR_XIR_FUNCTION_REF: case XR_XIR_CLASS_METHOD_REF: case XR_XIR_CELL_TAKE: case XR_XIR_VALUE_TAKE: case XR_XIR_CELL_NEW_TAKE: case XR_XIR_CELL_WRITE_TAKE: case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ:
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_STRING_APPEND: case XR_XIR_CELL_PROJECT:
    case XR_XIR_CONVERT_NUMBER: case XR_XIR_CONST_FLOAT:
    case XR_XIR_NEG_FLOAT: case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT:
    case XR_XIR_LT_FLOAT: case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT:
    case XR_XIR_GE_FLOAT: case XR_XIR_CELL_PLACE: case XR_XIR_SLOT_PLACE: case XR_XIR_OBJECT_PLACE:
    case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_PLACE_READ: case XR_XIR_PLACE_WRITE:
    case XR_XIR_MAP_NEW: case XR_XIR_SET_NEW: case XR_XIR_MAP_GET: case XR_XIR_MAP_INDEX: case XR_XIR_HASH_KEYS: case XR_XIR_HASH_VALUES: case XR_XIR_MAP_ENTRIES: case XR_XIR_HASH_CONTAINS:
    case XR_XIR_HASH_KEY_AT: case XR_XIR_MAP_VALUE_AT:
    case XR_XIR_MAP_CONTAINS_VALUE:
    case XR_XIR_SET_UNION: case XR_XIR_SET_INTERSECTION: case XR_XIR_SET_DIFFERENCE:
    case XR_XIR_SET_SYMMETRIC_DIFFERENCE: case XR_XIR_SET_IS_SUBSET: case XR_XIR_SET_IS_SUPERSET:
    case XR_XIR_HASH_LEN: case XR_XIR_MAP_SET: case XR_XIR_SET_ADD: case XR_XIR_HASH_DELETE: case XR_XIR_HASH_CLEAR:
    case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_GET: case XR_XIR_ARRAY_SET:
    case XR_XIR_ARRAY_PUSH: case XR_XIR_ARRAY_LEN: case XR_XIR_ARRAY_CAPACITY:
    case XR_XIR_ARRAY_WITH_CAPACITY: case XR_XIR_BYTE_ARRAY_APPEND: case XR_XIR_BYTE_ARRAY_REPEAT: case XR_XIR_ARRAY_RESERVE: case XR_XIR_ADD_FLOAT:
    case XR_XIR_SUB_FLOAT: case XR_XIR_MUL_FLOAT: case XR_XIR_DIV_FLOAT:
    case XR_XIR_STRUCT_NEW: case XR_XIR_STRUCT_GET: case XR_XIR_STRUCT_SET:
    case XR_XIR_CLASS_NEW: case XR_XIR_CLASS_GET: case XR_XIR_CLASS_SET:
    case XR_XIR_LOCAL_UNINIT: case XR_XIR_STRING_LEN: case XR_XIR_EQ_STRING:
    case XR_XIR_NE_STRING: case XR_XIR_CALL_INTRINSIC:
    case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF:
    case XR_XIR_ENUM_NEW: case XR_XIR_ENUM_TAG: case XR_XIR_ENUM_GET:
    case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME:
    case XR_XIR_RANGE_CHECK: case XR_XIR_INDEX_CHECK: case XR_XIR_EQUAL: case XR_XIR_ASSERT_CONDITION: case XR_XIR_PANIC_ERROR: case XR_XIR_MATCH_FAIL: case XR_XIR_ERROR_ERASE: case XR_XIR_INVOKE_RESULT:
    case XR_XIR_INVOKE_ERROR: case XR_XIR_INVOKE_DISCARD: case XR_XIR_ERROR_IS: case XR_XIR_ERROR_NARROW:
    case XR_XIR_PANIC_CATCH: case XR_XIR_PANIC_CODE: case XR_XIR_PANIC_MESSAGE: case XR_XIR_PANIC_REPLACED_ERROR:
    case XR_XIR_CLOCK_NANOS: case XR_XIR_UTC_OFFSET_AT:
    case XR_XIR_FIXED_ARRAY_NEW: case XR_XIR_FIXED_ARRAY_GET: case XR_XIR_FIXED_ARRAY_LEN:
    case XR_XIR_TUPLE_NEW: case XR_XIR_TUPLE_FIELD:
        return EFFECT_SEED_LOCAL;
    default: return EFFECT_SEED_INVALID;
    }
}

static bool effect_seed_type(const XrXirTypes *types,XrXirType callable,
    const XrXirInstruction *instruction,XrXirFunctionEffects *effect) {
    XrXirOp op=instruction->op;EffectSeedProjection projection=effect_seed_opcode(op,effect);
    if (projection==EFFECT_SEED_LOCAL) return true;
    if (projection==EFFECT_SEED_INVALID) return false;
    switch (op) {
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
    default: return false;
    }
}
#include "xxir_effect_summary.inc.c"
#include "xxir_effects_root.inc.c"

#include "xxir_effect_outcomes.inc.c"
#include "xxir_effect_source_terminal.inc.c"
#include "xxir_effect_check_access.inc.c"

#include "xxir_effect_check.inc.c"
#include "xxir_effect_check_pipeline.inc.c"
#include "xxir_effect_access_query.inc.c"

static XrXirStatus effect_infer_construct(const XrXirCompileContext *context,
    XirEffectInput input, const XrXirRootRefiner *refiner, EffectControlSummary *control,
    XrXirEffects **output) {
    return effect_checked_construct(context, input, refiner, control, output);
}
/* The constructor has retired its graph before the only public ownership
 * move. Its staging can be observed only by private construction consumers. */
static XrXirStatus effect_infer(const XrXirCompileContext *context,
    XirEffectInput input,const XrXirRootRefiner *refiner,EffectControlSummary *control,XrXirEffects **output) {
    const XrXirModule *module=input.module;
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirEffects *effects=NULL;
    XrXirStatus status=effect_infer_construct(context,input,refiner,control,&effects);
    if (status==XR_XIR_OK) status=effect_outcomes_publish(context,module,effects);
    if (status!=XR_XIR_OK) { xr_xir_compile_effects_free(effects);return status; }
    *output=effects;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_effects_infer_verified_input(const XrXirCompileContext *context,
    XirEffectInput input, XrXirEffects **output) {
    return effect_infer(context,input,NULL,NULL,output);
}
XR_FUNC XrXirStatus xir_effects_lower_verified_input(const XrXirCompileContext *context,
    XirEffectInput input, EffectControlSummary *control, XrXirEffects **output) {
    if (!input.module || input.module->stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    return effect_infer(context,input,NULL,control,output);
}
XR_FUNC EffectControlSummary *xir_effects_control_summary(const XrXirEffects *effects) {
    const EffectFinalOutcomes *final = effect_final_outcomes(effects);
    return final ? final->control_summary : NULL;
}
XR_FUNC XrXirStatus xr_xir_compile_effects_refine_verified_input(const XrXirCompileContext *context,
    XirEffectInput input, const XrXirRootRefiner *refiner, XrXirEffects **output) {
    if (!refiner || !refiner->update || !refiner->context) return XR_XIR_BAD_STRUCTURE;
    return effect_infer(context,input,refiner,NULL,output);
}
/* The constructor checks local errors before publishing. Trusted readers use
 * that result; the paranoid build independently checks the current body. */
static XrXirStatus effect_local_errors_current(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirEffects *effects,XrXirDiagnostic *diagnostic) {
    (void)diagnostic;
    if (!module || !effect_final_outcomes(effects) ||
        !xir_effects_context_matches(context,effects,module->function_count)) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_PARANOID ? xir_compile_local_errors_verify(context,module,diagnostic) : XR_XIR_OK;
}

static XrXirStatus effect_source_body_current(const XrXirCompileContext *context,
    const XrXirModule *source, const XrXirEffects *effects) {
    if (!source || !effect_final_outcomes(effects) ||
        !xir_effects_context_matches(context,effects,source->function_count)) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots = effects->checked_roots;
    if (roots->templates != source->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *provenance = source->provenance;
    if (!provenance || provenance->kind != XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_OK;
    if (!provenance->contracts || provenance->contract_count != source->function_count)
        return XR_XIR_BAD_STRUCTURE;
    /* Published callback bindings retain their actual SSA producer. The owned
     * alias edge checks that identity without retaining an instruction copy. */
    for (uint32_t f = 0; f < source->function_count; ++f) {
        const XrXirFunctionEffectContract *contract = &provenance->contracts[f];
        const XrXirFunction *body = &source->functions[f];
        const EffectCheckFunction *row = &roots->function[f];
        if (contract->value_count > body->instruction_count ||
            (contract->value_count && !contract->values)) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t v = 0; v < contract->value_count; ++v) {
            const XrXirRootValueIdentity *identity = &contract->values[v];
            if (identity->mode != XR_XIR_EFFECT_VALUE_CALL_BIND) continue;
            if (identity->instruction >= body->instruction_count ||
                identity->instruction >= row->values - row->parameters) return XR_XIR_BAD_STRUCTURE;
            const XrXirInstruction *op = &body->instructions[identity->instruction];
            uint32_t node = row->first + row->parameters + identity->instruction;
            uint32_t alias = roots->node[node].aliases;
            if (op->op != XR_XIR_COPY || op->args[0] >= row->values || alias >= roots->aliases ||
                roots->alias[alias].source != row->first + op->args[0] ||
                roots->alias[alias].next != UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
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

#include "xxir_effect_input_entries.inc.c"
