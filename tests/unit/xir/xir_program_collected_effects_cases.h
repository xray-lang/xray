/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_program_collected_effects_cases.h - Fresh coupled decoding ownership
 *
 * KEY CONCEPT:
 *   Complete byte admission yields independent owners without stored permission flags.
 */
#ifndef XIR_PROGRAM_COLLECTED_EFFECTS_CASES_H
#define XIR_PROGRAM_COLLECTED_EFFECTS_CASES_H
static void program_collected_effects_reject(const XrXirCompileContext *context,
    const XrXirProgramProof *proof, const XrXirTarget *target) {
    for (unsigned part=0;part<2;++part) {
        XirLoweredEffects output={0};
        if (part) output.effects=(XrXirEffects *)(uintptr_t)1;
        else output.artifact=(XrXirArtifact *)(uintptr_t)1;
        XirLoweredEffects sentinel=output;
        XrCompileResourceStats before=consumer_context_stats(context);
        size_t attempts_before=source_program_compile_attempts;
        CHECK(xr_xir_compile_checked_read_lower_effects(context,(const void *)(uintptr_t)1,
            SIZE_MAX,target,&output,NULL)==XR_XIR_BAD_STRUCTURE);
        XrCompileResourceStats after=consumer_context_stats(context);
        CHECK(!memcmp(&before,&after,sizeof(before)) && source_program_compile_attempts==attempts_before);
        CHECK(!memcmp(&output,&sentinel,sizeof(output)));
    }
    XirLoweredEffects output={0};
    XrCompileResourceStats before=consumer_context_stats(context);
    CHECK(xr_xir_compile_checked_read_lower_effects(context,proof->bytes,32,target,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!output.artifact && !output.effects && consumer_context_stats(context).live_bytes==before.live_bytes);
    XrXirTarget bad=*target;bad.abi_version=0;
    CHECK(xr_xir_compile_checked_read_lower_effects(context,proof->bytes,proof->length,
        &bad,&output,NULL)==XR_XIR_BAD_LAYOUT);
    CHECK(!output.artifact && !output.effects && consumer_context_stats(context).live_bytes==before.live_bytes);
}
static void program_collected_effects_cases(void) {
    XrXirCompileContext context=consumer_context_default();
    uint64_t baseline=consumer_context_stats(&context).live_bytes;
    XrXirArtifact *source=program_fixture(&context,0);
    XrXirProgramProof proof=xr_xir_compile_program_proof(source);
    XrXirTarget target=*xr_xir_compile_artifact_target(source);
    program_collected_effects_reject(&context,&proof,&target);
    XrCompileResourceStats retained=consumer_context_stats(&context);
    size_t blocks_before=source_program_compile_live,bytes_before=source_program_compile_bytes;
    size_t ordinal=source_program_compile_attempts;
    source_program_compile_fail_at=ordinal;source_program_compile_injected=false;
    XirLoweredEffects failed={0};
    CHECK(xr_xir_compile_checked_read_lower_effects(&context,proof.bytes,proof.length,
        &target,&failed,NULL)==XR_XIR_OUT_OF_MEMORY);
    CHECK(source_program_compile_injected && !failed.artifact && !failed.effects);
    CHECK(consumer_context_stats(&context).live_bytes==retained.live_bytes);
    CHECK(source_program_compile_live==blocks_before && source_program_compile_bytes==bytes_before);
    source_program_compile_fail_at=SIZE_MAX;source_program_compile_injected=false;
    XrCompileResourceStats failed_stats=consumer_context_stats(&context);
    XirLoweredEffects actual={0};
    CHECK(xr_xir_compile_checked_read_lower_effects(&context,proof.bytes,proof.length,
        &target,&actual,NULL)==XR_XIR_OK && actual.artifact && actual.effects);
    CHECK(consumer_context_stats(&context).work>failed_stats.work && context.resources==actual.artifact->context.resources);
    const XrXirModule *module=xr_xir_compile_artifact_module(actual.artifact);
    CHECK(module->function_count==8 && module->stage==XR_XIR_LOWERED);
    XrXirEffects *independent=NULL;
    CHECK(xr_xir_compile_effects_infer_verified(&context,module,&independent)==XR_XIR_OK);
    for (uint32_t f=0;f<8;++f) {
        const XrXirFunctionEffects *a=xr_xir_effects_function(actual.effects,f),*b=xr_xir_effects_function(independent,f);
        const XrXirRootEffects *x=xr_xir_effects_root(actual.effects,f),*y=xr_xir_effects_root(independent,f);
        CHECK(a && b && a->throws==b->throws && a->suspend==b->suspend);
        CHECK(x && y && x->requires_root==y->requires_root && x->unresolved==y->unresolved);
    }
    xr_xir_compile_effects_free(independent);
    xr_xir_compile_artifact_free(source);source=NULL;
    xr_xir_compile_artifact_free(actual.artifact);actual.artifact=NULL;
    CHECK(xir_effects_context_matches(&context,actual.effects,8));
    const XrXirRootEffects *root=xr_xir_effects_root(actual.effects,3);
    CHECK(root && root->requires_root && !root->unresolved);
    CHECK(xr_xir_effects_root(actual.effects,6)->requires_root);
    for (uint32_t f=0;f<8;++f) {
        XirEffectEntryRootView entry={0};
        CHECK(xir_effects_entry_root(&context,actual.effects,f,&entry)==XR_XIR_OK && !entry.parameter_count);
    }
    xr_xir_compile_effects_free(actual.effects);actual.effects=NULL;
    CHECK(consumer_context_stats(&context).live_bytes==baseline);
    puts("Fresh Program byte reader: occupied outputs, target and packet refusal, same-ledger OOM retry, post-artifact ROOT ownership PASS");
}
#endif // XIR_PROGRAM_COLLECTED_EFFECTS_CASES_H
