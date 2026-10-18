/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_final_runtime_cases.h - Literal runtime roles after producer death
 */
#ifndef XIR_CELL_FINAL_RUNTIME_CASES_H
#define XIR_CELL_FINAL_RUNTIME_CASES_H
#include "xir/xxir_effect_runtime_internal.h"
#include "xir/xxir_effects.h"

static void cell_final_runtime_cases(void) {
    CellProofFixture fixture;cell_proof_fixture(&fixture);
    XrXirCompileContext context=cell_proof_context(33554432);
    uint64_t baseline=value_compile_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCC,sizeof(fixture));
    XrCompileResourceStats before=value_compile_stats(&context);
    static const XrXirCellProofRole expected[]={XR_XIR_CELL_PROOF_SCOPED_REF,
        XR_XIR_CELL_PROOF_OWNED_CAPTURE,XR_XIR_CELL_PROOF_LEXICAL_CLEANUP,XR_XIR_CELL_PROOF_UNKNOWN};
    for (uint32_t index=0;index<4;++index) {
        XrXirCellProofRole role=XR_XIR_CELL_PROOF_UNKNOWN;
        CHECK(xir_effects_runtime_cell_role(effects,index+2,0,&role)==XR_XIR_OK && role==expected[index]);
    }
    XirEffectEntryRootView root={0};
    CHECK(xir_effects_runtime_entry_root(effects,2,&root)==XR_XIR_OK);
    CHECK(!root.intrinsic_mask && root.parameter_count==1 && root.parameters && root.parameters[0]==0);
    XrXirCellProofRole role=XR_XIR_CELL_PROOF_UNKNOWN;
    CHECK(xir_effects_runtime_cell_role(NULL,2,0,&role)==XR_XIR_BAD_STRUCTURE && role==XR_XIR_CELL_PROOF_UNKNOWN);
    CHECK(xir_effects_runtime_cell_role(effects,6,0,&role)==XR_XIR_BAD_STRUCTURE && role==XR_XIR_CELL_PROOF_UNKNOWN);
    CHECK(xir_effects_runtime_cell_role(effects,2,1,&role)==XR_XIR_BAD_STRUCTURE && role==XR_XIR_CELL_PROOF_UNKNOWN);
    CHECK(xir_effects_runtime_cell_role(effects,0,0,&role)==XR_XIR_BAD_STRUCTURE && role==XR_XIR_CELL_PROOF_UNKNOWN);
    CHECK(xir_effects_runtime_cell_role(effects,2,0,NULL)==XR_XIR_BAD_STRUCTURE);
    role=XR_XIR_CELL_PROOF_LEXICAL_CLEANUP;
    CHECK(xir_effects_runtime_cell_role(effects,2,0,&role)==XR_XIR_BAD_STRUCTURE && role==XR_XIR_CELL_PROOF_LEXICAL_CLEANUP);
    XirEffectEntryRootView empty={0},sentinel=root;
    CHECK(xir_effects_runtime_entry_root(effects,2,&root)==XR_XIR_BAD_STRUCTURE &&
        root.intrinsic_mask==sentinel.intrinsic_mask && root.parameters==sentinel.parameters &&
        root.parameter_count==sentinel.parameter_count);
    CHECK(xir_effects_runtime_entry_root(NULL,2,&empty)==XR_XIR_BAD_STRUCTURE &&
        !empty.intrinsic_mask && !empty.parameters && !empty.parameter_count);
    CHECK(xir_effects_runtime_entry_root(effects,6,&empty)==XR_XIR_BAD_STRUCTURE &&
        !empty.intrinsic_mask && !empty.parameters && !empty.parameter_count);
    CHECK(xir_effects_runtime_entry_root(effects,2,NULL)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=value_compile_stats(&context);
    CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes && before.peak_bytes==after.peak_bytes && before.work==after.work);
    xr_xir_compile_effects_free(effects);cell_proof_close(&context,baseline);
    puts("Final runtime roles:scope/owned/cleanup/unknown,conditional Cell root,producer death,sentinels,no compile ledger use,physical0");
}
#endif // XIR_CELL_FINAL_RUNTIME_CASES_H
