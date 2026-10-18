/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_finite_source.c - Current Error proof at the final Source boundary
 */
#define main original_pending_entry
#include "test_xir_error_cfg_pending.c"
#undef main
static EffectFinalOutcomes *source_outcomes;
static uint32_t source_frees,source_attacks,source_mode,source_cut;
static bool source_marker_unsafe,source_marker_mode,source_attack_enabled;
static void source_error_attack(void);
static void source_marker_attack(void);
static void source_callback(void *memory,size_t bytes) {
    CHECK(memory && bytes);++source_frees;
    if(source_attack_enabled && source_cut==source_frees) {
        pc_free_hook=NULL;
        if(source_marker_mode) source_marker_attack();else source_error_attack();
    }
}
static void source_error_attack(void) {
    CHECK(source_outcomes);++source_attacks;
    if(source_mode>=2) {
        EffectControlProof *proof=source_outcomes->control_proof;
        EffectControlPositions *positions=(EffectControlPositions *)(proof+1);
        uint32_t mode=source_mode-2;CHECK(mode<3);
        proof->counts[mode]=1;positions[1].position[mode]=0;
        ((uint32_t *)effect_control_order(proof,mode))[0]=1;
        if(!mode) {
            source_outcomes->functions[1].suspend=XR_XIR_EFFECT_MAY;
            source_outcomes->witnesses[1]=(XrXirEffectWitness){XR_XIR_EFFECT_CAUSE_SUSPEND,0,UINT32_MAX,0};
        } else if(mode==1) source_outcomes->task_creation[1]=XR_XIR_EFFECT_MAY;
        else source_outcomes->go_safe[1]=XR_XIR_BAD_TYPE;
        return;
    }
    CHECK(source_outcomes->words==1);
    source_outcomes->errors[1]|=UINT64_C(2);
    source_outcomes->functions[1].throws=XR_XIR_EFFECT_MAY;
    source_outcomes->task_errors[1]=XR_XIR_BAD_TYPE;
    if(source_mode) {
        EffectErrorFinite *proof=source_outcomes->error_proof;uint64_t coordinate=proof->rows[1].end-64+1;
        CHECK(proof->count<proof->capacity);uint32_t at=0;
        /* Adversarial writes never re-enter the ledger from its free callback. */
        while(at<proof->count && proof->facts[at].coordinate<coordinate) ++at;
        CHECK(at==proof->count || proof->facts[at].coordinate!=coordinate);
        memmove(proof->facts+at+1,proof->facts+at,(size_t)(proof->count-at)*sizeof(*proof->facts));
        proof->facts[at]=(EffectErrorFiniteFact){coordinate,3};++proof->count;
    }
}
static uint32_t source_error_once(uint32_t cut,bool attack,uint32_t mode) {
    XrXirInstruction ops[2][2]={
        {{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},{.op=XR_XIR_RETURN,.args={0}}},
        {{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},{.op=XR_XIR_RETURN,.args={0}}}};
    XrXirBlock blocks[2]={{.count=2},{.count=2}};
    XrXirFunction functions[2]={{.result=XR_XIR_I64,.instructions=ops[0],.instruction_count=2,.blocks=&blocks[0],.block_count=1},
        {.result=XR_XIR_I64,.instructions=ops[1],.instruction_count=2,.blocks=&blocks[1],.block_count=1}};
    const XrXirInstruction init={.op=XR_XIR_RETURN};
    blocks[0].count=1;functions[0].instructions=&init;functions[0].instruction_count=1;
    functions[0].result=XR_XIR_UNIT;functions[0].name="init";functions[0].name_length=4;
    functions[1].name="main";functions[1].name_length=4;
    const XrXirSourceModule source={"root",4,NULL,0,0};
    const XrXirFunctionIdentity identities[2]={{.method_kind=XR_XIR_NON_MEMBER},{.method_kind=XR_XIR_NON_MEMBER}};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=1};
    XrXirFunctionEffectContract contracts[2]={{.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED}},{0}};
    const XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=2};
    XrXirModule module={.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_PROGRAM,.functions=functions,.function_count=2,
        .declarations=&declarations,.provenance=&evidence};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus admitted=xir_fixture_check(&context,&module,&checked,&diagnostic);
    if(admitted!=XR_XIR_OK) fprintf(stderr,"Source fixture admission%u f%u b%u i%u reason%u\n",admitted,
        diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
    CHECK(admitted==XR_XIR_OK);
    XrXirStatus status=xr_xir_compile_effects_analyze(checked,&effects);
    if(status!=XR_XIR_OK) fprintf(stderr,"Source infer status%u work%llu\n",status,(unsigned long long)pc_stats(&context).work);
    CHECK(status==XR_XIR_OK && effects->invocations->final->outcomes->error_proof);
    EffectFinalOutcomes *outcomes=effects->invocations->final->outcomes;EffectErrorFinite *proof=outcomes->error_proof;
    CHECK(outcomes->atom_count==0 && outcomes->error_rows==2 && outcomes->words==1 && !outcomes->errors[1]);
    /* The exact empty domain has no positive records. Reserve owned attack
     * storage before adversarial writes; verification must invoke no callbacks. */
    if(!proof->capacity) {
        CHECK(!proof->facts && !proof->count);XrXirStatus reserve=XR_XIR_OK;
        proof->facts=xir_compile_calloc(&context,32,sizeof(*proof->facts),&reserve);
        CHECK(proof->facts && reserve==XR_XIR_OK);proof->capacity=32;
    }
    EffectErrorFiniteFact facts[32];CHECK(proof->capacity==32);uint32_t count=proof->count;
    memcpy(facts,proof->facts,(size_t)count*sizeof(*facts));
    EffectControlProof *control=outcomes->control_proof;unsigned char control_saved[512];
    uint64_t control_bytes=effect_control_bytes(control->functions,control->edges);CHECK(control_bytes<=sizeof(control_saved));
    memcpy(control_saved,control,(size_t)control_bytes);
    XrXirFunctionEffects function_saved=outcomes->functions[1];XrXirEffectWitness witness_saved=outcomes->witnesses[1];
    XrXirEffect task_saved=outcomes->task_creation[1];XrXirStatus go_saved=outcomes->go_safe[1];
    source_outcomes=outcomes;source_frees=0;source_attacks=0;source_mode=mode;
    source_cut=cut;source_marker_mode=false;source_attack_enabled=attack;
    if(attack && !cut) source_error_attack();
    CHECK(!pc_free_hook && !pc_allocate_hook);pc_free_hook=source_callback;
    status=xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects);
    pc_free_hook=NULL;pc_allocate_hook=NULL;source_outcomes=NULL;
    CHECK(source_attacks==(attack?1u:0u));
    fprintf(stderr,"actual Source Error attack%u mode%u callbacks%u status%u expected%u work%llu\n",
        attack,mode,source_frees,status,attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK,(unsigned long long)pc_stats(&context).work);
    CHECK(status==(attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));uint32_t frees=source_frees;
    outcomes->errors[1]=0;outcomes->functions[1].throws=XR_XIR_EFFECT_NONE;outcomes->task_errors[1]=XR_XIR_OK;
    memcpy(control,control_saved,(size_t)control_bytes);outcomes->functions[1]=function_saved;
    outcomes->witnesses[1]=witness_saved;outcomes->task_creation[1]=task_saved;outcomes->go_safe[1]=go_saved;
    memcpy(proof->facts,facts,(size_t)count*sizeof(*facts));proof->count=count;
    CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(ops,0xAC,sizeof(ops));memset(functions,0xAC,sizeof(functions));
    size_t allocations=attempts;uint64_t physical=live_bytes;
    CHECK(error_finite_outcomes_match(&context,effects->invocations->body,outcomes)==XR_XIR_OK);
    CHECK(attempts==allocations && live_bytes==physical);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);return frees;
}
static void source_nonempty(void) {
    EdgeFixture fixture;ee_fixture(&fixture,EE_INVOKE,false);
    fixture.functions[1]=fixture.functions[2];fixture.identities[1]=fixture.identities[2];fixture.module.function_count=2;
    fixture.module.stage=XR_XIR_BUILT;
    XrXirFunctionEffectContract contracts[2]={{.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED}},{0}};
    const XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=2};
    fixture.module.provenance=&evidence;
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    XrXirStatus status=xr_xir_compile_effects_analyze(checked,&effects);
    if(status!=XR_XIR_OK) fprintf(stderr,"nonempty Source infer status%u\n",status);
    CHECK(status==XR_XIR_OK);
    EffectFinalOutcomes *outcomes=effects->invocations->final->outcomes;
    CHECK(outcomes->errors[1]==8 && outcomes->functions[1].throws==XR_XIR_EFFECT_MAY && outcomes->task_errors[1]==XR_XIR_OK);
    CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects)==XR_XIR_OK);
    uint64_t row=outcomes->errors[1];outcomes->errors[1]=0;outcomes->functions[1].throws=XR_XIR_EFFECT_NONE;
    CHECK(error_finite_outcomes_match(&context,effects->invocations->body,outcomes)==XR_XIR_BAD_STRUCTURE);
    outcomes->errors[1]=row;outcomes->functions[1].throws=XR_XIR_EFFECT_MAY;
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xDC,sizeof(fixture));
    CHECK(error_finite_outcomes_match(&context,effects->invocations->body,outcomes)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    puts("actual nonempty Source Error: precise enum variant, missing contribution rejected, producer dead, physical0");
}
static void source_marker_attack(void) {
    if(source_mode>=2) { source_error_attack();return; }
    CHECK(source_outcomes);++source_attacks;
    CHECK(source_outcomes->errors[1]==(source_marker_unsafe?4u:8u) && source_outcomes->functions[1].throws==XR_XIR_EFFECT_MAY);
    source_outcomes->task_errors[1]=source_marker_unsafe?XR_XIR_OK:XR_XIR_BAD_TYPE;
    if(source_mode) {
        EffectErrorMarkers *markers=source_outcomes->error_proof->markers;
        for(uint32_t i=0;i<markers->count;++i) markers->nodes[i].value=1;
    }
}
static uint32_t source_marker_once(uint32_t cut,bool attack,bool unsafe,uint32_t mode) {
    EdgeFixture fixture;ee_fixture(&fixture,EE_INVOKE,false);
    fixture.functions[1]=fixture.functions[2];fixture.identities[1]=fixture.identities[2];fixture.module.function_count=2;
    XrXirNominalDeclaration nominal[2]={fixture.nominal,{.module={"edges",5},.name={"Local",5},.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL}};
    XrXirNominalField hidden={{"hidden",6},(XrXirType)257,0};XrXirType projected=(XrXirType)257;
    if(unsafe) {
        fixture.variants[1].field_count=1;nominal[0].fields=&hidden;nominal[0].field_count=1;
        fixture.nominals.declarations=nominal;fixture.nominals.count=2;
        fixture.nodes[0].nominal.fields=&projected;fixture.nodes[0].nominal.field_count=1;
        fixture.nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1}};
        fixture.thrower[0].immediate=0;
    }
    fixture.module.stage=XR_XIR_BUILT;
    XrXirFunctionEffectContract contracts[2]={{.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED}},{0}};
    const XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=2};
    fixture.module.provenance=&evidence;
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    EffectFinalOutcomes *outcomes=effects->invocations->final->outcomes;
    XrXirStatus permission=unsafe?XR_XIR_BAD_TYPE:XR_XIR_OK;
    CHECK(outcomes->errors[1]==(unsafe?4u:8u) && outcomes->functions[1].throws==XR_XIR_EFFECT_MAY && outcomes->task_errors[1]==permission);
    EffectErrorMarkers *markers=outcomes->error_proof->markers;ErrorMarkerNode saved[64];CHECK(markers->count<=64);
    memcpy(saved,markers->nodes,markers->count*sizeof(*saved));
    EffectControlProof *control=outcomes->control_proof;unsigned char control_saved[512];
    uint64_t control_bytes=effect_control_bytes(control->functions,control->edges);CHECK(control_bytes<=sizeof(control_saved));
    memcpy(control_saved,control,(size_t)control_bytes);
    XrXirFunctionEffects function_saved=outcomes->functions[1];XrXirEffectWitness witness_saved=outcomes->witnesses[1];
    XrXirEffect task_saved=outcomes->task_creation[1];XrXirStatus go_saved=outcomes->go_safe[1];
    source_outcomes=outcomes;source_frees=0;source_attacks=0;source_mode=mode;source_marker_unsafe=unsafe;
    source_cut=cut;source_marker_mode=true;source_attack_enabled=attack;
    if(attack && !cut) source_marker_attack();
    CHECK(!pc_free_hook && !pc_allocate_hook);pc_free_hook=source_callback;
    XrXirStatus status=xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects);
    pc_free_hook=NULL;pc_allocate_hook=NULL;source_outcomes=NULL;uint32_t frees=source_frees;
    CHECK(source_attacks==(attack?1u:0u));
    fprintf(stderr,"actual Source known-atom marker attack%u callbacks%u status%u expected%u row%llu permission%u\n",
        attack,frees,status,attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK,(unsigned long long)outcomes->errors[1],outcomes->task_errors[1]);
    outcomes->task_errors[1]=permission;memcpy(markers->nodes,saved,markers->count*sizeof(*saved));
    memcpy(control,control_saved,(size_t)control_bytes);outcomes->functions[1]=function_saved;
    outcomes->witnesses[1]=witness_saved;outcomes->task_creation[1]=task_saved;outcomes->go_safe[1]=go_saved;
    CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xDE,sizeof(fixture));
    CHECK(error_finite_outcomes_match(&context,effects->invocations->body,outcomes)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    CHECK(status==(attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));return frees;
}
static void source_marker_literals(void) {
    uint32_t frees=source_marker_once(UINT32_MAX,false,false,0);
    CHECK(source_marker_once(frees,true,false,0)==frees);
    frees=source_marker_once(UINT32_MAX,false,true,0);
    CHECK(source_marker_once(frees,true,true,0)==frees);
    CHECK(source_marker_once(frees,true,true,1)==frees);
    CHECK(frees);
    for(uint32_t mode=2;mode<5;++mode) CHECK(source_marker_once(frees,true,true,mode)==frees);
}

int main(void) {
    uint32_t frees=source_error_once(UINT32_MAX,false,0);
    CHECK(source_error_once(frees,true,0)==frees);CHECK(source_error_once(frees,true,1)==frees);
    for(uint32_t mode=2;mode<5;++mode) CHECK(source_error_once(frees,true,mode)==frees);
    source_nonempty();source_marker_literals();
    puts("actual Source finite Error: joint forgery rejected after the last available free, physical0 finite reader; partial scope");return 0;
}
