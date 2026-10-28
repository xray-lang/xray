/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_collector_cases.h - Fresh complete analysis and failure ownership
 */
#ifndef XIR_EFFECT_COLLECTOR_CASES_H
#define XIR_EFFECT_COLLECTOR_CASES_H
typedef enum CollectorAttack { COLLECTOR_NONE, COLLECTOR_PROMISE, COLLECTOR_CLEANUP, COLLECTOR_GO } CollectorAttack;
static XrXirArtifact *collector_bare_fixture(bool suspend) {
    XrXirInstruction ops[2]={{.op=XR_XIR_RETURN,.type=XR_XIR_UNIT},{.op=XR_XIR_RETURN,.type=XR_XIR_UNIT}};
    if (suspend) ops[0].op=XR_XIR_SUSPEND;
    XrXirBlock block={0,suspend?2u:1u,0,0};
    XrXirFunction body={"bare",4,NULL,0,XR_XIR_UNIT,&block,1,ops,block.count,NULL,0};
    XrXirModule module={XR_XIR_BUILT,&body,1,NULL,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *artifact=NULL;
    CHECK(xir_fixture_check(&effect_context,&module,&artifact,NULL)==XR_XIR_OK && artifact);
    return artifact;
}
static XrXirArtifact *collector_decl_fixture(bool suspend) {
    XrXirInstruction init={.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    XrXirInstruction entry[3]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={1}}};
    XrXirInstruction worker[2]={{.op=XR_XIR_RETURN,.type=XR_XIR_UNIT},{.op=XR_XIR_RETURN,.type=XR_XIR_UNIT}};
    if (suspend) worker[0].op=XR_XIR_SUSPEND;
    XrXirBlock blocks[3]={{0,1,0,0},{0,3,0,0},{0,suspend?2u:1u,0,0}};
    XrXirFunction functions[3]={{"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,&init,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&blocks[1],1,entry,3,NULL,0},
        {"worker",6,NULL,0,XR_XIR_UNIT,&blocks[2],1,worker,blocks[2].count,NULL,0}};
    XrXirSourceModule source={"collector",9,NULL,0,0, 0,XR_XIR_MODULE_FRESH_STORAGE};
    XrXirFunctionIdentity ids[3]={{0}};
    XrXirDeclarations declarations={&source,1,ids,NULL,0,NULL,0,0,1,NULL};
    XrXirTypeNode task={.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_UNIT};
    XrXirTypes types={&task,1,NULL,NULL};
    XrXirModule module={XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *artifact=NULL;
    CHECK(xir_fixture_check(&effect_context,&module,&artifact,NULL)==XR_XIR_OK && artifact);
    return artifact;
}
static XrXirArtifact *collector_go_fixture(void) {
    XrXirType cell=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_UNIT}};
    XrXirTypes types={nodes,2,NULL,NULL};
    XrXirInstruction init[4]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_CELL_NEW,.type=cell,.args={0}},
        {.op=XR_XIR_SLOT_INIT,.type=XR_XIR_UNIT,.args={1}},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT}};
    XrXirInstruction entry[3]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={1}}};
    XrXirInstruction worker[3]={{.op=XR_XIR_SLOT_LOAD,.type=cell},
        {.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={0}},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT}};
    XrXirBlock blocks[3]={{0,4,0,0},{0,3,0,0},{0,3,0,0}};
    XrXirFunction bodies[3]={{"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,4,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&blocks[1],1,entry,3,NULL,0},
        {"worker",6,NULL,0,XR_XIR_UNIT,&blocks[2],1,worker,3,NULL,0}};
    XrXirSourceModule source={"collector",9,NULL,0,0, 0,XR_XIR_MODULE_FRESH_STORAGE};XrXirFunctionIdentity ids[3]={{0}};
    XrXirSlot slot={0,cell,1};XrXirDeclarations declarations={&source,1,ids,&slot,1,NULL,0,0,1,NULL};
    XrXirModule module={XR_XIR_BUILT,bodies,3,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *artifact=NULL;
    CHECK(xir_fixture_check(&effect_context,&module,&artifact,NULL)==XR_XIR_OK && artifact);
    return artifact;
}
static void collector_attack(XrXirArtifact *artifact,CollectorAttack attack) {
    if (attack==COLLECTOR_NONE) return;
    XrXirFunctionIdentity *ids=(XrXirFunctionIdentity *)artifact->module.declarations->functions;
    if (attack==COLLECTOR_PROMISE) ids[2].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    else if (attack==COLLECTOR_CLEANUP) ids[2].cleanup_owner=2;
    else {
        CHECK(attack==COLLECTOR_GO);
        XrXirInstruction *ops=(XrXirInstruction *)artifact->module.functions[1].instructions;
        ops[0]=(XrXirInstruction){.op=XR_XIR_GO,.type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),.immediate=2};
    }
}
static void collector_faults(const XrXirArtifact *source,CollectorAttack attack,XrXirStatus expected) {
    EffectMark mark=effect_mark();XrXirCompileContext measured=effect_owner_new(effect_caps());
    uint64_t baseline=effect_stats(&measured).live_bytes;XrXirArtifact *copy=NULL;XrXirEffects *summary=NULL;
    CHECK(effect_reproduce(source,&measured,&copy)==XR_XIR_OK);collector_attack(copy,attack);
    attempts=0;fail_at=SIZE_MAX;injected=false;
    CHECK(xr_xir_compile_effects_analyze(copy,&summary)==expected);
    CHECK((expected==XR_XIR_OK)==(summary!=NULL));size_t sites=attempts;CHECK(sites);
    xr_xir_compile_effects_free(summary);xr_xir_compile_artifact_free(copy);
    effect_owner_free(&measured,baseline);effect_mark_check(mark);
    for (size_t point=0;point<sites;++point) {
        XrXirCompileContext context=effect_owner_new(effect_caps());baseline=effect_stats(&context).live_bytes;
        copy=NULL;summary=NULL;CHECK(effect_reproduce(source,&context,&copy)==XR_XIR_OK);collector_attack(copy,attack);
        EffectMark retained=effect_mark();attempts=0;fail_at=point;injected=false;
        CHECK(xr_xir_compile_effects_analyze(copy,&summary)==XR_XIR_OUT_OF_MEMORY && injected && !summary);
        effect_mark_check(retained);XrCompileResourceStats failed=effect_stats(&context);
        XrCompileResources *same=context.resources;fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_effects_analyze(copy,&summary)==expected && context.resources==same);
        CHECK((expected==XR_XIR_OK)==(summary!=NULL));XrCompileResourceStats retried=effect_stats(&context);
        CHECK(retried.allocated_bytes>=failed.allocated_bytes && retried.work>=failed.work);
        xr_xir_compile_effects_free(summary);xr_xir_compile_artifact_free(copy);
        effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    printf("Effect collector attack=%u expected=%u census=%zu everyOOM=1 sameOwnerRetry=1 physical=held\n",
        (unsigned)attack,(unsigned)expected,sites);
}
static void collector_occupied(const XrXirArtifact *artifact) {
    EffectMark mark=effect_mark();XrCompileResourceStats before=effect_stats(&effect_context);size_t calls=attempts;
    XrXirEffects *sentinel=(XrXirEffects *)(uintptr_t)1;
    CHECK(xr_xir_compile_effects_analyze(artifact,&sentinel)==XR_XIR_BAD_STRUCTURE && sentinel==(XrXirEffects *)(uintptr_t)1);
    CHECK(xr_xir_compile_verify_analyzed_effects_v2(&effect_context,&artifact->module,artifact->construction,
        &sentinel,NULL)==XR_XIR_BAD_STRUCTURE && sentinel==(XrXirEffects *)(uintptr_t)1);
    XrCompileResourceStats after=effect_stats(&effect_context);
    CHECK(before.work==after.work && before.allocation_count==after.allocation_count &&
        before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes && before.peak_bytes==after.peak_bytes);
    CHECK(attempts==calls);effect_mark_check(mark);
}
static void collector_bare_case(bool suspend) {
    XrXirArtifact *artifact=collector_bare_fixture(suspend);XrXirEffects *summary=NULL;XrXirDiagnostic diagnostic={0};
    EffectMark mark=effect_mark();
    CHECK(xr_xir_compile_verify_owned_effects_v2(&effect_context,&artifact->module,artifact->construction,
        &summary,&diagnostic)==XR_XIR_BAD_STRUCTURE && !summary);effect_mark_check(mark);
    collector_occupied(artifact);collector_faults(artifact,COLLECTOR_NONE,XR_XIR_OK);
    effect_budget_boundaries(artifact);
    XrXirArtifact *lowered=NULL;XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(artifact,&target, NULL,&lowered,NULL)==XR_XIR_OK && lowered);
    collector_occupied(lowered);collector_faults(lowered,COLLECTOR_NONE,XR_XIR_OK);
    effect_budget_boundaries(lowered);effect_artifact_free(lowered);
    CHECK(effect_analyze(artifact,&summary)==XR_XIR_OK && summary && summary->count==1);
    effect_artifact_free(artifact);effect_expect(summary,0,suspend?XR_XIR_EFFECT_MAY:XR_XIR_EFFECT_NONE,XR_XIR_EFFECT_NONE);
    effect_summary_free(summary);CHECK(effect_balanced());
}
static void collector_declaration_cases(void) {
    XrXirArtifact *positive=collector_decl_fixture(false);collector_attack(positive,COLLECTOR_PROMISE);
    collector_faults(positive,COLLECTOR_NONE,XR_XIR_OK);effect_budget_boundaries(positive);effect_artifact_free(positive);
    for (unsigned attack=COLLECTOR_PROMISE;attack<=COLLECTOR_GO;++attack) {
        XrXirArtifact *base=attack==COLLECTOR_GO?collector_go_fixture():collector_decl_fixture(true);
        XrXirArtifact *copy=NULL;
        CHECK(effect_reproduce(base,&effect_context,&copy)==XR_XIR_OK);collector_attack(copy,(CollectorAttack)attack);
        EffectMark mark=effect_mark();XrXirEffects *summary=NULL;XrXirDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_verify_analyzed_effects_v2(&effect_context,&copy->module,copy->construction,
            &summary,&diagnostic)==XR_XIR_BAD_TYPE && !summary);
        CHECK(diagnostic.reason==(attack==COLLECTOR_PROMISE?XR_XIR_DIAGNOSTIC_NO_SUSPEND:
            attack==COLLECTOR_CLEANUP?XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND:XR_XIR_DIAGNOSTIC_GO_ROOT_REQUIRED));
        effect_mark_check(mark);effect_artifact_free(copy);
        collector_faults(base,(CollectorAttack)attack,XR_XIR_BAD_TYPE);effect_artifact_free(base);
    }
}
#endif // XIR_EFFECT_COLLECTOR_CASES_H
