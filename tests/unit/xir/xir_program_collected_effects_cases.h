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
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_program_internal.h"
#ifndef XIR_PROGRAM_COLLECTED_EFFECTS_CASES_H
#define XIR_PROGRAM_COLLECTED_EFFECTS_CASES_H
static void program_collected_effects_reject(const XrXirCompileContext *context,
    const XrXirProgramPacket *proof, const XrXirTarget *target) {
    for (unsigned part=0;part<2;++part) {
        XrCompileResourceStats before=consumer_context_stats(context);
        size_t attempts_before=source_program_compile_attempts;
        if (!part) {
            XrXirProgramInput *output=(XrXirProgramInput *)(uintptr_t)1;
            XrXirProgramPacket poison={(const uint8_t *)(uintptr_t)1,SIZE_MAX,(const uint8_t *)(uintptr_t)1};
            CHECK(xir_program_input_read(context,&poison,target,&output)==XR_XIR_BAD_STRUCTURE);
            CHECK(output==(XrXirProgramInput *)(uintptr_t)1);
        } else {
            XrXirProgramPermissions *output=(XrXirProgramPermissions *)(uintptr_t)1;
            XrXirProgramSpec spec={0};
            CHECK(xir_program_input_prepare(context,&spec,(const XrXirProgramInput *)(uintptr_t)1,
                &output)==XR_XIR_BAD_STRUCTURE);
            CHECK(output==(XrXirProgramPermissions *)(uintptr_t)1);
        }
        XrCompileResourceStats after=consumer_context_stats(context);
        CHECK(!memcmp(&before,&after,sizeof(before)) && source_program_compile_attempts==attempts_before);
    }
    XrXirProgramInput *output=NULL;
    XrXirProgramPacket short_packet=*proof;short_packet.length=32;
    XrCompileResourceStats before=consumer_context_stats(context);
    CHECK(xir_program_input_read(context,&short_packet,target,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(!output && consumer_context_stats(context).live_bytes==before.live_bytes);
    XrXirTarget bad=*target;bad.abi_version=0;
    CHECK(xir_program_input_read(context,proof,&bad,&output)==XR_XIR_BAD_LAYOUT);
    CHECK(!output && consumer_context_stats(context).live_bytes==before.live_bytes);
}
static void program_collected_effects_cases(void) {
    XrXirCompileContext context=consumer_context_default();
    uint64_t baseline=consumer_context_stats(&context).live_bytes;
    XrXirArtifact *source=program_fixture(&context,0);
    XrXirProgramPacket proof=xir_program_packet_borrow(source);
    XrXirTarget target=*xr_xir_compile_artifact_target(source);
    program_collected_effects_reject(&context,&proof,&target);
    XrCompileResourceStats retained=consumer_context_stats(&context);
    size_t blocks_before=source_program_compile_live,bytes_before=source_program_compile_bytes;
    size_t ordinal=source_program_compile_attempts;
    source_program_compile_fail_at=ordinal;source_program_compile_injected=false;
    XrXirProgramInput *failed=NULL;
    CHECK(xir_program_input_read(&context,&proof,&target,&failed)==XR_XIR_OUT_OF_MEMORY);
    CHECK(source_program_compile_injected && !failed);
    CHECK(consumer_context_stats(&context).live_bytes==retained.live_bytes);
    CHECK(source_program_compile_live==blocks_before && source_program_compile_bytes==bytes_before);
    source_program_compile_fail_at=SIZE_MAX;source_program_compile_injected=false;
    XrCompileResourceStats failed_stats=consumer_context_stats(&context);
    XrXirProgramInput *actual=NULL;
    CHECK(xir_program_input_read(&context,&proof,&target,&actual)==XR_XIR_OK && actual);
    CHECK(consumer_context_stats(&context).work>failed_stats.work && context.resources==actual->context.resources);
    const XrXirModule *module=xr_xir_compile_artifact_module(actual->artifact);
    const XrXirEffects *actual_effects=actual->artifact->program_final;
    CHECK(actual_effects && xir_program_input_verify(&context,actual)==XR_XIR_OK);
    CHECK(module->function_count==8 && module->stage==XR_XIR_LOWERED);
    XrXirEffects *independent=NULL;
    CHECK(xr_xir_compile_effects_infer_verified(&context,module,&independent)==XR_XIR_OK);
    for (uint32_t f=0;f<8;++f) {
        XrXirEffectFacts a_read=xr_xir_effects_read(actual_effects,f);
        const XrXirFunctionEffects *a=(a_read.status==XR_XIR_OK?&a_read.control:NULL);XrXirEffectFacts b_read=xr_xir_effects_read(independent,f);
        const XrXirFunctionEffects *b=(b_read.status==XR_XIR_OK?&b_read.control:NULL);
        XrXirEffectFacts x_read=xr_xir_effects_read(actual_effects,f);
        const XrXirRootEffects *x=(x_read.status==XR_XIR_OK?&x_read.root:NULL);XrXirEffectFacts y_read=xr_xir_effects_read(independent,f);
        const XrXirRootEffects *y=(y_read.status==XR_XIR_OK?&y_read.root:NULL);
        CHECK(a && b && a->throws==b->throws && a->suspend==b->suspend);
        CHECK(x && y && x->requires_root==y->requires_root && x->unresolved==y->unresolved);
    }
    xr_xir_compile_effects_free(independent);
    xr_xir_compile_artifact_free(source);source=NULL;
    /* Producer death cannot retire the independently received Input or its Final. */
    CHECK(xir_program_input_verify(&context,actual)==XR_XIR_OK);
    CHECK(xir_effects_context_matches(&context,actual_effects,8));
    XrXirEffectFacts root_read=xr_xir_effects_read(actual_effects,3);
    const XrXirRootEffects *root=(root_read.status==XR_XIR_OK?&root_read.root:NULL);
    CHECK(root && root->requires_root && !root->unresolved);
    CHECK(xr_xir_effects_read(actual_effects,6).root.requires_root);
    for (uint32_t f=0;f<8;++f) {
        XirEffectEntryRootView entry={0};
        CHECK(xir_effects_entry_root(&context,actual_effects,f,&entry)==XR_XIR_OK && !entry.parameter_count);
    }
    xir_program_input_free(actual);actual=NULL;
    CHECK(consumer_context_stats(&context).live_bytes==baseline);
    puts("Fresh Program Input: occupied outputs, target and packet refusal, same-ledger OOM retry, producer-death Final ROOT ownership PASS");
}
#endif // XIR_PROGRAM_COLLECTED_EFFECTS_CASES_H
