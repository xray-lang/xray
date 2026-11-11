/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_conversion_cases.h - Conversion deltas retain one equation owner
 */
#ifndef XIR_EFFECT_CONVERSION_CASES_H
#define XIR_EFFECT_CONVERSION_CASES_H

typedef struct ConversionPreparation {
    EffectLifetimePreparation lifetime;
    XrXirInstruction *copy;
} ConversionPreparation;

static XrXirStatus conversion_observe(void *context,const XrXirModule *module,
    uint32_t function,uint32_t instruction,const XrXirInstruction *op) {
    ConversionPreparation *preparation = context;
    return effect_lifetime_observe(&preparation->lifetime,module,function,instruction,op);
}
static XrXirStatus conversion_points(void *context,const XirExecutionOwner *execution,
    XirStorageFlow *storage,bool final,bool *ready) {
    ConversionPreparation *preparation = context;
    return effect_lifetime_points(&preparation->lifetime,execution,storage,final,ready);
}
static XrXirStatus conversion_prepare(void *context,const XirExecutionOwner *execution,
    XirStorageFlow *storage,const XirRestrictionReceiver *receiver) {
    ConversionPreparation *preparation = context;
    return effect_lifetime_finish(&preparation->lifetime,execution,storage,receiver);
}
static XrXirStatus conversion_refine(void *context,const XrXirEffects *effects,bool *changed) {
    XrXirInstruction *copy = ((ConversionPreparation *)context)->copy;
    *changed = copy->op != XR_XIR_FUNCTION_WEAKEN;
    if (!*changed) return XR_XIR_OK;
    XrXirRootEffects before = {0};
    XrXirStatus status = xir_effects_construction_root(&effect_context,effects,1,&before);
    if (status != XR_XIR_OK) return status;
    CHECK(!before.requires_root && !before.unresolved);
    copy->op = XR_XIR_FUNCTION_WEAKEN;
    status = xir_effects_construction_weaken(&effect_context,effects,1,1,XR_XIR_CALLABLE_ROOT_UNRESOLVED,true);
    if (status == XR_XIR_OK) status = xir_effects_construction_close(&effect_context,effects,NULL);
    XrXirRootEffects after = {0};
    if (status == XR_XIR_OK) status = xir_effects_construction_root(&effect_context,effects,1,&after);
    if (status == XR_XIR_OK) CHECK(!after.requires_root && after.unresolved);
    if (status == XR_XIR_OK) {
        size_t allocations = attempts;
        status = xir_effects_construction_weaken(&effect_context,effects,1,1,XR_XIR_CALLABLE_ROOT_UNRESOLVED,true);
        if (status == XR_XIR_OK) status = xir_effects_construction_close(&effect_context,effects,NULL);
        CHECK(attempts == allocations);
    }
    return status;
}

static XrXirStatus conversion_owner_case(void) {
    XrXirTypeNode callable = {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,
        .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types = {.nodes=&callable,.count=1};
    XrXirType fn = XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirInstruction leaf[] = {{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={0}}};
    XrXirInstruction caller[] = {{.op=XR_XIR_FUNCTION_REF,.type=fn},
        {.op=XR_XIR_COPY,.type=fn,.args={0}},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_RETURN,.args={2}}};
    XrXirBlock blocks[] = {{.count=2},{.count=4}};
    XrXirFunction functions[] = {
        {.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,.instructions=leaf,.instruction_count=2,.blocks=blocks,.block_count=1},
        {.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,.instructions=caller,.instruction_count=4,.blocks=blocks+1,.block_count=1}};
    XrXirModule module = {.stage=XR_XIR_BUILT,.types=&types,.functions=functions,.function_count=2};
    XrXirDiagnostic diagnostic = {0};
    ConversionPreparation preparation = {.lifetime={.diagnostic=&diagnostic},.copy=&caller[1]};
    XrXirRootRefiner refiner = {.context=&preparation,.update=conversion_refine,.observe=conversion_observe,
        .points=conversion_points,.prepare=conversion_prepare};
    XrXirConstruction *construction = NULL;
    XrXirEffects *pending = NULL,*effects = NULL;
    XrXirStatus status = xir_fixture_module_construction(&effect_context,&module,&construction);
    if (status == XR_XIR_OK) status = xr_hir_escape_builder_new(&effect_context,&module,NULL,NULL,&preparation.lifetime.builder);
    if (status == XR_XIR_OK) status = xir_effects_prepare_verified_input(&effect_context,
        (XirEffectInput){&module,construction,NULL},&refiner,&pending);
    if (status != XR_XIR_OK && !injected) fprintf(stderr,"conversion prepare status%u\n",status);
    if (status == XR_XIR_OK) status = xir_effects_finish_prepared_input(&effect_context,
        (XirEffectInput){&module,construction,NULL},&refiner,&pending,&effects,&diagnostic);
    if (status != XR_XIR_OK && !injected) fprintf(stderr,"conversion finish status%u f%u i%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(!pending);
    xr_xir_compile_effects_free(effects);
    xr_hir_escape_builder_free(preparation.lifetime.builder);
    xr_xir_compile_construction_free(construction);
    return status;
}

static void conversion_owner_cases(void) {
    EffectMark baseline = effect_mark();
    attempts = 0; injected = false;
    CHECK(conversion_owner_case() == XR_XIR_OK);
    size_t sites = attempts;
    effect_mark_check(baseline);
    for (size_t site = 0; site < sites; ++site) {
        attempts = 0; injected = false; fail_at = site;
        XrXirStatus status = conversion_owner_case();
        if (status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"conversion allocation%zu/%zu status%u\n",site,sites,status);
        CHECK(status == XR_XIR_OUT_OF_MEMORY && injected);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    printf("conversion equations: in-owner closure and %zu allocation failures PASS\n",sites);
}

#endif // XIR_EFFECT_CONVERSION_CASES_H
