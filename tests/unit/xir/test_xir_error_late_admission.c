/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_late_admission.c - Admission after actual marker callbacks
 */
#define main pending_fixture_entry
#include "test_xir_error_cfg_pending.c"
#undef main

static EffectErrorFinite *late_proof;
static EffectErrorFiniteRow late_saved_row;
static uint32_t late_mode,late_calls,late_count,late_words,late_rank;

/* The fixture allocator exposes its actual payload behind the ledger header.
 * No ledger call, allocation or free occurs inside the adversarial callback. */
static void late_allocate(void *memory,size_t bytes) {
    CHECK(memory && bytes);
    if(!late_proof) {
        CHECK(bytes==sizeof(CompileAllocation)+sizeof(EffectErrorFinite));
        late_proof=(EffectErrorFinite *)((CompileAllocation *)memory+1);return;
    }
    if(bytes!=sizeof(CompileAllocation)+sizeof(EffectErrorMarkers)) return;
    pc_allocate_hook=NULL;++late_calls;
    CHECK(late_proof->functions==2 && late_proof->words==1 && late_proof->count);
    late_saved_row=late_proof->rows[1];late_count=late_proof->count;late_words=late_proof->words;
    late_rank=late_proof->facts[late_count-1].rank;
    if(late_mode==1) late_proof->rows[1].end+=64;
    if(late_mode==2) ++late_proof->words;
    if(late_mode==3) late_proof->facts[late_count-1].rank=UINT32_MAX;
    if(late_mode==4) --late_proof->count;
}
static void late_forbid(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }

static void late_once(uint32_t mode) {
    EdgeFixture fixture;ee_fixture(&fixture,EE_INVOKE,false);
    fixture.functions[1]=fixture.functions[2];fixture.identities[1]=fixture.identities[2];fixture.module.function_count=2;
    fixture.module.stage=XR_XIR_BUILT;
    XrXirFunctionEffectContract contracts[2]={{.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED}},{0}};
    const XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=2};
    fixture.module.provenance=&evidence;
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    const EffectFinalOutcomes *published=effect_final_outcomes(effects);CHECK(published);
    const EffectBodyMaterial *material=effects->invocations->body;
    EffectErrorFinite *candidate=NULL;late_proof=NULL;late_mode=mode;late_calls=0;
    CHECK(!pc_allocate_hook && !pc_free_hook);
    pc_allocate_hook=late_allocate;
    XrXirStatus status=error_finite_construct(&context,effects->invocations,published,&candidate);
    pc_allocate_hook=NULL;
    CHECK(status==XR_XIR_OK && candidate==late_proof && late_calls==1);
    size_t allocations=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=late_forbid;pc_free_hook=late_forbid;
    status=error_finite_check(&context,candidate);
    pc_allocate_hook=NULL;pc_free_hook=NULL;
    CHECK(status==(mode?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
    CHECK(attempts==allocations && live_bytes==physical && effect_final_outcomes(effects)==published);
    candidate->rows[1]=late_saved_row;candidate->count=late_count;candidate->words=late_words;
    candidate->facts[late_count-1].rank=late_rank;
    pc_allocate_hook=late_forbid;pc_free_hook=late_forbid;
    CHECK(error_finite_check(&context,candidate)==XR_XIR_OK);
    CHECK(error_finite_outcomes_match(&context,material,published)==XR_XIR_OK);
    pc_allocate_hook=NULL;pc_free_hook=NULL;
    CHECK(attempts==allocations && live_bytes==physical);
    error_finite_release(candidate);late_proof=NULL;
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xDC,sizeof(fixture));
    pc_allocate_hook=late_forbid;pc_free_hook=late_forbid;
    CHECK(error_finite_outcomes_match(&context,material,published)==XR_XIR_OK);
    pc_allocate_hook=NULL;pc_free_hook=NULL;
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    printf("late Error marker callback mode%u actual%u restoration producer-death no-callback physical0 PASS\n",mode,status);
}
int main(void) { for(uint32_t mode=0;mode<5;++mode) late_once(mode);return 0; }
