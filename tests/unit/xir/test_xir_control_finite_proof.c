/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_control_finite_proof.c - Actual finite control evidence counterfacts
 */
#define main pending_control_fixture_entry
#include "test_xir_root_conditional_helpers.c"
#undef main

static void control_no_callback(void *memory) {
    size_t bytes=pc_final_physical_size(memory);
    (void)memory;(void)bytes;CHECK(false);
}
static void control_verify(const XrXirCompileContext *context,const XrXirEffects *effects,XrXirStatus expected) {
    size_t before=rp_attempts;uint64_t physical=rp_live_bytes;
    CHECK(!rp_allocation_observer && !rp_release_observer);rp_allocation_observer=control_no_callback;rp_release_observer=control_no_callback;
    XrXirStatus status=effect_final_control_verify(context,effects);
    rp_allocation_observer=NULL;rp_release_observer=NULL;
    if(status!=expected) fprintf(stderr,"control status%u expected%u\n",status,expected);
    CHECK(status==expected && rp_attempts==before && physical==rp_live_bytes);
}
static void control_counterfacts(void) {
    XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
    PcControlFixture fixture;pc_control_fixture(&fixture,true);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    EffectFinalOutcomes *final=effects->invocations->final->outcomes;EffectControlProof *proof=final->control_proof;
    uint64_t bytes=effect_control_bytes(proof->functions,proof->edges);unsigned char saved[512];
    CHECK(bytes<=sizeof(saved) && proof->functions==3 && proof->edges==2);memcpy(saved,proof,(size_t)bytes);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCC,sizeof(fixture));control_verify(&context,effects,XR_XIR_OK);
    for(uint32_t attack=0;attack<14;++attack) {
        EffectControlPositions *positions=(EffectControlPositions *)(proof+1);
        uint32_t *order=(uint32_t *)effect_control_order(proof,0),*offsets=effect_control_offsets(proof);
        EffectControlEdgeId *edges=effect_control_edges(proof);
        switch(attack) {
        case 0:proof->counts[0]=4;break;
        case 1:--proof->counts[0];break;
        case 2:positions[0].position[0]=UINT32_MAX;break;
        case 3:order[0]=3;break;
        case 4:order[0]=2;order[1]=1;positions[1].position[0]=1;positions[2].position[0]=0;break;
        case 5:offsets[0]=1;break;
        case 6:offsets[1]=3;break;
        case 7:edges[0].caller=3;break;
        case 8:edges[0].instruction=2;break;
        case 9:edges[1]=edges[0];break;
        case 10:--proof->edges;break;
        case 11:proof->resources=NULL;break;
        case 12:proof->material=(const EffectBodyMaterial *)(uintptr_t)1;break;
        default:proof->counts[2]=1;positions[0].position[2]=0;
            ((uint32_t *)effect_control_order(proof,2))[0]=0;final->go_safe[0]=XR_XIR_BAD_TYPE;break;
        }
        control_verify(&context,effects,XR_XIR_BAD_STRUCTURE);memcpy(proof,saved,(size_t)bytes);final->go_safe[0]=XR_XIR_OK;
        control_verify(&context,effects,XR_XIR_OK);
    }
    XrXirCompileContext foreign=context;foreign.resources=NULL;XrCompileResourceLimits caps=rp_caps();
    CHECK(xr_compile_resources_new(&caps,&foreign.resources)==XR_COMPILE_RESOURCE_OK);
    control_verify(&foreign,effects,XR_XIR_BAD_STRUCTURE);xr_compile_resources_release(foreign.resources);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);
    puts("finite Control: 14 malformed/joint counterfacts, foreign owner, producer death, no callbacks");
}
static void control_unseeded_cycle(void) {
    XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
    PcControlFixture fixture;pc_control_fixture(&fixture,false);XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    EffectFinalOutcomes *final=effects->invocations->final->outcomes;EffectControlProof *proof=final->control_proof;
    EffectControlPositions *positions=(EffectControlPositions *)(proof+1);
    for(uint32_t mode=0;mode<3;++mode) {
        proof->counts[mode]=2;uint32_t *order=(uint32_t *)effect_control_order(proof,mode);
        for(uint32_t f=0;f<2;++f) {
            order[f]=f;positions[f].position[mode]=f;
            if(!mode) {
                final->functions[f].suspend=XR_XIR_EFFECT_MAY;
                final->witnesses[f]=(XrXirEffectWitness){XR_XIR_EFFECT_CAUSE_CALL,0,1-f,1};
            } else if(mode==1) final->task_creation[f]=XR_XIR_EFFECT_MAY;
            else final->go_safe[f]=XR_XIR_BAD_TYPE;
        }
        control_verify(&context,effects,XR_XIR_BAD_STRUCTURE);
        proof->counts[mode]=0;
        for(uint32_t f=0;f<2;++f) {
            order[f]=0;positions[f].position[mode]=UINT32_MAX;final->functions[f].suspend=XR_XIR_EFFECT_NONE;
            final->witnesses[f]=(XrXirEffectWitness){0};final->task_creation[f]=XR_XIR_EFFECT_NONE;final->go_safe[f]=XR_XIR_OK;
        }
        control_verify(&context,effects,XR_XIR_OK);
    }
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);
    puts("finite Control: jointly invented suspend/Task/shape cycles rejected");
}
static XrCompileResourceStats control_boundary(XrCompileResourceLimits caps,XrXirStatus expected) {
    XrXirCompileContext context=rp_owner(caps);context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
    PcControlFixture fixture;pc_control_fixture(&fixture,true);XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xDD,sizeof(fixture));
    control_verify(&context,effects,expected);XrCompileResourceStats measured=rp_stats(&context);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);return measured;
}
static XrXirStatus control_whole(const XrXirCompileContext *context) {
    PcControlFixture fixture;pc_control_fixture(&fixture,true);XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if(status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if(status==XR_XIR_OK) {
        size_t before=rp_attempts;uint64_t physical=rp_live_bytes;
        rp_allocation_observer=control_no_callback;rp_release_observer=control_no_callback;
        status=effect_final_control_verify(context,effects);rp_allocation_observer=NULL;rp_release_observer=NULL;
        CHECK(rp_attempts==before && rp_live_bytes==physical);
    }
    xr_xir_compile_effects_free(effects);return status;
}
static XrCompileResourceStats control_measured(XrCompileResourceLimits caps,XrXirStatus expected) {
    XrXirCompileContext context=rp_owner(caps);context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
    CHECK(control_whole(&context)==expected);XrCompileResourceStats measured=rp_stats(&context);
    rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);return measured;
}
static void control_resources(void) {
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
        rp_attempts=0;rp_injected=false;rp_fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=control_whole(&context);size_t actual=rp_attempts;rp_fail_at=SIZE_MAX;
        if(!pass) { CHECK(status==XR_XIR_OK && actual && actual<2048);sites=actual; }
        else CHECK(rp_injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(rp_stats(&context).held_bytes==baseline && control_whole(&context)==XR_XIR_OK);
        rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);
    }
    XrCompileResourceStats measured=control_measured(rp_caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={XR_COMPILE_MEMORY_FINITE,measured.peak_bytes,true};
    (void)control_measured(exact,XR_XIR_OK);
    XrCompileResourceLimits less=exact;CHECK(less.memory_limit>1);--less.memory_limit;
    (void)control_measured(less,XR_XIR_MEMORY_LIMIT);
    printf("finite Control whole OOM sites%zu; same-owner retry; held-memory exact/minus-one boundary\n",sites);
}
#include "xir_control_input_terminal_cases.h"
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"input-terminals")) { control_current_input_cases();return 0; }
    if(argc==2 && !strcmp(argv[1],"resources")) { control_resources();return 0; }
    CHECK(argc==1);
    pc_control_proof_literals();control_counterfacts();control_unseeded_cycle();
    (void)control_boundary(rp_caps(),XR_XIR_OK);
    puts("finite Control: producer death, exact receiving evidence and no physical allocation");return 0;
}
