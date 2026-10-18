/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_substitution_cases.h - Current generic Error identity rejection
 */
#ifndef XIR_ERROR_SUBSTITUTION_CASES_H
#define XIR_ERROR_SUBSTITUTION_CASES_H
static void ed_substitution_check(const XrXirCompileContext *work,EffectErrorFinite *proof,XrXirStatus expected) {
    size_t allocations=attempts;uint64_t physical=live_bytes;
    fail_at=attempts;injected=false;
    XrXirStatus status=error_finite_check(work,proof);fail_at=SIZE_MAX;
    CHECK(status==expected && !injected && attempts==allocations && live_bytes==physical);
}
static void ed_substitution_controls(const XrXirCompileContext *work,XrXirEffects *effects) {
    const EffectFinalOutcomes *outcomes=effect_final_outcomes(effects);CHECK(outcomes && outcomes->error_proof);
    EffectErrorFinite *proof=outcomes->error_proof;
    CHECK(proof->atom_count==63 && proof->words==2 && proof->material->domain.types->count==1);
    CHECK(proof->terms.types.count==2 && proof->bindings && proof->bindings->count==1);
    CHECK(effect_terms_scratch_retired(&proof->terms));
    EffectErrorFinite saved=*proof;
    ed_substitution_check(work,proof,XR_XIR_OK);
    CHECK(error_finite_outcomes_match(work,proof->material,outcomes)==XR_XIR_OK);
    EffectErrorAtom *atoms=(EffectErrorAtom *)proof->atoms;EffectErrorAtom tail=atoms[62];
    atoms[62].variant=61;ed_substitution_check(work,proof,XR_XIR_BAD_STRUCTURE);atoms[62]=tail;
    atoms[62]=atoms[0];ed_substitution_check(work,proof,XR_XIR_BAD_STRUCTURE);atoms[62]=tail;
    CHECK(outcomes->atom_capacity>63);EffectErrorAtom spare=atoms[63];atoms[63]=tail;++proof->atom_count;
    ed_substitution_check(work,proof,XR_XIR_BAD_STRUCTURE);--proof->atom_count;atoms[63]=spare;
    XrXirType *argument=(XrXirType *)proof->terms.types.nodes[1].nominal.arguments;
    CHECK(argument && argument[0]==XR_XIR_I64);argument[0]=XR_XIR_BOOL;
    ed_substitution_check(work,proof,XR_XIR_BAD_STRUCTURE);argument[0]=XR_XIR_I64;
    XrXirType *actual=(XrXirType *)proof->material->generics[1].arguments;
    CHECK(actual && actual[0]==XR_XIR_I64);actual[0]=XR_XIR_BOOL;
    ed_substitution_check(work,proof,XR_XIR_BAD_STRUCTURE);actual[0]=XR_XIR_I64;
    ErrorTypeBinding *binding=&proof->bindings->items[0];XrXirType type=binding->actual;
    binding->actual=(XrXirType)256;ed_substitution_check(work,proof,XR_XIR_BAD_STRUCTURE);binding->actual=type;
    ErrorMarkerNode *pair=&proof->bindings->pairs->nodes[binding->first];uint32_t value=pair->value;
    pair->value^=1;ed_substitution_check(work,proof,XR_XIR_BAD_STRUCTURE);pair->value=value;
    const XrXirTypeNode *nodes=proof->terms.types.nodes;
    proof->terms.types.nodes=(XrXirTypeNode *)(uintptr_t)1;
    ed_substitution_check(work,proof,XR_XIR_BAD_STRUCTURE);proof->terms.types.nodes=nodes;
    EffectFinalOutcomes *mutable=(EffectFinalOutcomes *)outcomes;++mutable->source_type_count;
    CHECK(error_finite_outcomes_match(work,proof->material,outcomes)==XR_XIR_BAD_STRUCTURE);
    --mutable->source_type_count;ed_substitution_check(work,proof,XR_XIR_OK);
    CHECK(error_finite_outcomes_match(work,proof->material,outcomes)==XR_XIR_OK);
    CHECK(!memcmp(&saved,proof,sizeof(saved)));
    puts("generic Error restart: retired constructor; current atoms/types/binders; no allocation; producer dead");
}
#endif // XIR_ERROR_SUBSTITUTION_CASES_H
