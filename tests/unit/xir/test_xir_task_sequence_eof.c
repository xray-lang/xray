/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_sequence_eof.c - Complete root seed scans reject malformed tails
 */
#define main sequence_retained_original_main
#include "test_xir_root_conditional_helpers.c"
#undef main
int main(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    fixture.init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
    fixture.init[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2};
    fixture.init[2]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture.init_block.count=fixture.functions[3].instruction_count=3;
    CHECK(fixture.source.initializer==3);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    EffectBodyMaterial *body=effects->invocations->body;
    CHECK(effect_body_material_shape(body) && body->domain.function_count==4);
    XrXirRootEffects root={0};
    CHECK(effect_invocation_input_seed(&context,body,3,&root)==XR_XIR_OK);
    CHECK(root.requires_root && !root.unresolved);
    EffectBodyFunction *function=&body->functions[3];
    CHECK(function->run_count && function->runs);
    EffectBodyRun *tail=&function->runs[function->run_count-1];
    uint32_t count=tail->count;XrXirOp opcode=tail->base.op;
    CHECK(count<UINT32_MAX);
    for (unsigned damage=0;damage<3;++damage) {
        if (damage==0) tail->count=count+1;
        else if (damage==1) ++function->instruction_count;
        else tail->base.op=XR_XIR_INVALID;
        XrXirRootEffects output={false,true};
        CHECK(effect_invocation_input_seed(&context,body,3,&output)==XR_XIR_BAD_STRUCTURE);
        CHECK(!output.requires_root && output.unresolved);
        tail->count=count;tail->base.op=opcode;
        if (damage==1) --function->instruction_count;
        CHECK(effect_invocation_input_seed(&context,body,3,&root)==XR_XIR_OK && root.requires_root && !root.unresolved);
    }
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
    puts("SEQUENCE_ROOT_SEED owned producer dead, malformed tails and opcode, unchanged failed output, physical0 PASS");
    return 0;
}
