/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_owner.inc.c - Actual dense contexts in one summary owner
 *
 * KEY CONCEPT:
 *   Authentic source bodies choose ordinary implementations and physical-use
 *   roles before complete actual types enter private execution equations.
 */
struct EffectContextOwner {
    EffectOrdinaryContexts *ordinary, *dense;
    EffectContextForest *forest;
    EffectInvocationDeclaredBounds *declared;
    EffectRefinementBounds *refinement;
    uint32_t borrows;
};

/* Publication removes the ordinary classifier's real allocations and its
 * construction identity. Surviving descriptors belong to this Final owner. */

static void effect_context_owner_free(EffectContextOwner *owner) {
    if (!owner) return;
    /* Retire summary views while the forest still owns their Result.
     * Its retained Material keeps all immutable backbone payloads live. */
    effect_invocation_bounds_free(owner->declared);
    effect_ordinary_free(owner->dense);effect_ordinary_free(owner->ordinary);
    effect_context_forest_free(owner->forest);
    xr_compile_resources_free(owner);
}

/* The original COPY is an inferred SSA value, not a declared barrier. Its
 * complete operand/target/type-argument edge must survive normalization;
 * both current operand weakening and all final semantic gates still run. */
static XrXirStatus effect_refinement_copy_transition(
    const EffectRefinementFunction *original,uint32_t instruction,const XrXirInstruction *current) {
    if (!original || !current || instruction>=original->instruction_count ||
        !original->instructions || original->copy_count>original->instruction_count ||
        !!original->copies!=!!original->copy_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t low=0,high=original->copy_count;
    while (low<high) {

        uint32_t at=low+(high-low)/2;
        if (original->copies[at].instruction<instruction) low=at+1;else high=at;
    }
    if (low>=original->copy_count || original->copies[low].instruction!=instruction) return XR_XIR_UNRESOLVED;
    const XrXirInstruction *saved=&original->copies[low].operation;

    return saved->op==XR_XIR_COPY && saved->type==original->instructions[instruction] &&
        saved->args[0]==current->args[0] && saved->args[1]==current->args[1] &&
        saved->targets[0]==current->targets[0] && saved->targets[1]==current->targets[1] &&
        saved->immediate==current->immediate && saved->type_arguments[0]==current->type_arguments[0] &&
        saved->type_arguments[1]==current->type_arguments[1]?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus effect_invocation_source_fixed(const XrXirCompileContext *work,
    const XrXirModule *source,uint32_t function,const XrXirRootValueIdentity *identity,
    const EffectRefinementFunction *original) {
    const XrXirFunction *body=&source->functions[function];
    const XrXirInstruction *op=&body->instructions[identity->instruction];

    if (!original || identity->instruction>=original->instruction_count ||
        identity->mode!=XR_XIR_EFFECT_VALUE_FIXED || op->op!=XR_XIR_FUNCTION_WEAKEN ||
        op->type!=identity->declared_type ||
        (uint64_t)op->args[0]>=(uint64_t)body->parameter_count+body->instruction_count)
        return XR_XIR_BAD_TYPE;
    XrXirStatus status=xr_xir_compile_callable_weakening(work,source->types,
        xr_xir_operand_type(body,op->args[0]),identity->declared_type);
    if (status!=XR_XIR_OK) return status;
    status=effect_refinement_copy_transition(original,identity->instruction,op);
    if (status!=XR_XIR_UNRESOLVED) return status;
    return xr_xir_compile_callable_weakening(work,source->types,
        original->instructions[identity->instruction],identity->declared_type);
}

/* CALL_BIND keeps its consuming declaration. A real fixed WEAKEN keeps its
 * normalized bound instead of resurrecting a discarded pre-bottom qualifier. */
static XrXirStatus effect_invocation_source_declarations(const XrXirCompileContext *work,
    const XrXirModule *source,uint32_t function,const EffectRefinementFunction *original,XrXirType **output) {
    const XrXirType *prebottom=original?original->instructions:NULL;
    if (!source || !source->functions || function>=source->function_count || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    if (original && (original->instruction_count!=source->functions[function].instruction_count ||
        (original->instruction_count && !prebottom))) return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *provenance=source->provenance;
    if (!provenance || provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_OK;
    /* Internal ordinary views carry a marker without stored source records.
     * Their actual types still undergo complete bounds and equation proof. */
    if (!provenance->contracts && !provenance->contract_count) return XR_XIR_OK;
    if (!provenance->contracts || provenance->contract_count!=source->function_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&source->functions[function];
    const XrXirFunctionEffectContract *contract=&provenance->contracts[function];
    XrXirStatus status=effect_contract_values(source,function,contract);
    bool present=false;
    for (uint32_t v=0;v<contract->value_count && status==XR_XIR_OK;++v) {

        const XrXirRootValueIdentity *identity=&contract->values[v];
        if (identity->mode==XR_XIR_EFFECT_VALUE_CALL_BIND ||
            (prebottom && identity->mode==XR_XIR_EFFECT_VALUE_FIXED &&
                prebottom[identity->instruction]!=identity->declared_type)) present=true;
    }
    if (status!=XR_XIR_OK || !present) return status;
    uint64_t bytes=(uint64_t)body->instruction_count*sizeof(XrXirType);
    if (bytes>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirType *types=xir_compile_alloc(work,(size_t)bytes,&status);
    if (!types) return status;
    for (uint32_t i=0;i<body->instruction_count && status==XR_XIR_OK;++i) {

        types[i]=prebottom?prebottom[i]:body->instructions[i].type;
    }
    for (uint32_t v=0;v<contract->value_count && status==XR_XIR_OK;++v) {

        const XrXirRootValueIdentity *identity=&contract->values[v];
        if (identity->mode==XR_XIR_EFFECT_VALUE_CALL_BIND)
            status=xr_xir_compile_callable_weakening(work,source->types,
                body->instructions[identity->instruction].type,identity->declared_type);
        else if (prebottom && identity->mode==XR_XIR_EFFECT_VALUE_FIXED &&
            prebottom[identity->instruction]!=identity->declared_type)
            status=effect_invocation_source_fixed(work,source,function,identity,original);
        else continue;
        if (status==XR_XIR_OK) types[identity->instruction]=identity->declared_type;
    }
    if (status!=XR_XIR_OK) { xr_compile_resources_free(types);return status; }
    *output=types;return XR_XIR_OK;
}

/* A nongeneric authentic reference may infer its returned callable's outer
 * bound while Source keeps the original descriptor prefix immutable. Prove
 * that current result against the old result, then retain the old reference
 * advertisement on this exact current physical shape. Fixed receivers never
 * enter this path; all nested children still use the shared exact matcher. */

/* Refinement matched the immutable source type prefix and copied declaration
 * domain before the dense pool was seeded. These original IDs therefore name
 * that verified prefix. Actual ordinary binders still undergo full substitution. */

/* Ordinary views carry actual typed initial-role products for physical-use derivation,
 * not stored source records. Capture each authentic source advertisement in
 * this exact ordinary pool before the common queue receives that view. */

/* A sequential expansion pass owns this temporary workspace. Only small
 * capacities survive between definitions; no actual type or proof is reused.
 * Large workspaces end with their definition and every workspace ends before
 * solve/stable. Physical vectors and interned descriptors remain in terms. */
typedef struct EffectDenseScratch {
    const XrXirCompileContext *remaining;
    void *memory;
    size_t capacity;
    XrXirStatus status;
    bool claimed;
} EffectDenseScratch;

typedef struct EffectDenseArrays {
    XrXirType *declared, *values, *before;
    uint8_t *fixed;
} EffectDenseArrays;
