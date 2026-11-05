/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_input_seed_run_cases.h - Current Input primitive and scalar transfer probes
 */
#ifndef XIR_INPUT_SEED_RUN_CASES_H
#define XIR_INPUT_SEED_RUN_CASES_H

static void input_seed_run_roots(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;
    CellIndexFixture fixture;ci_fixture(&fixture,CI_CALL,false,true);
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL)==XR_XIR_OK);
    EffectBodyMaterial *material=NULL;
    CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    XrXirRootEffects facts={true,true};
    CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_OK &&
        !facts.requires_root && !facts.unresolved);
    EffectBodyRead read=effect_body_compile_read(&context);const EffectBodyRun *borrowed=NULL;
    CHECK(effect_body_record_ref(&read,&material->functions[1],4,&borrowed)==XR_XIR_OK);
    EffectBodyRun *run=(EffectBodyRun *)borrowed;
    XrXirOp opcode=run->base.op;uint32_t mask=run->mask;
    CHECK(opcode==XR_XIR_CONST_INT && !(mask&UINT32_C(1)));
    for (uint32_t damage=0;damage<3;++damage) {
        if (!damage) run->base.op=XR_XIR_SLOT_LOAD;
        else if (damage==1) run->base.op=XR_XIR_INVALID;
        else {
            run->mask=mask|UINT32_C(1);
            run->delta[0]=(uint32_t)XR_XIR_SLOT_LOAD-(uint32_t)opcode;
        }
        facts=(XrXirRootEffects){true,true};
        CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_BAD_STRUCTURE &&
            facts.requires_root && facts.unresolved);
        run->base.op=opcode;run->mask=mask;
        CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_OK &&
            !facts.requires_root && !facts.unresolved);
    }
    uint32_t unused=run->base.targets[1];run->base.targets[1]^=UINT32_C(0x12345678);
    CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_OK &&
        !facts.requires_root && !facts.unresolved);
    CHECK(effect_body_source_match(&context,material,&fixture.module)==XR_XIR_BAD_STRUCTURE);
    run->base.targets[1]=unused;
    CHECK(effect_body_source_match(&context,material,&fixture.module)==XR_XIR_OK);
    memset(&fixture,0xCE,sizeof(fixture));
    CHECK(effect_invocation_input_seed(&context,material,1,&facts)==XR_XIR_OK &&
        !facts.requires_root && !facts.unresolved);
    effect_body_material_free(material);rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void input_seed_run_literals(void) {
    input_seed_run_roots();
    puts("current Input ROOT seeds passed");
}
#endif // XIR_INPUT_SEED_RUN_CASES_H
