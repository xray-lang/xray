/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_entry_footprint_cases.h - Current environments of hidden Cell inputs
 *
 * Entry environment inputs have their own restriction and loan dependencies.
 * Receiver allocation failures and relocation preserve their owned namespace.
 */
#ifndef XIR_ENTRY_FOOTPRINT_CASES_H
#define XIR_ENTRY_FOOTPRINT_CASES_H

static XrXirStatus entry_footprint_case(const XrXirArtifact *artifact, bool relocate) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    XrXirEffects *effects = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xir_effects_infer_lifetimes_input(&effect_context,
        (XirEffectInput){module,artifact->construction,NULL},&effects,&diagnostic);
    XirViewLoanFacts facts = {0};
    if (status == XR_XIR_OK) status = xir_effects_view_loan_facts(&effect_context,effects,&facts);
    XirStorageInput observed = {0};
    uint32_t found = 0;
    for (uint32_t i = 0; status == XR_XIR_OK && i < facts.storage.input_count; ++i) {
        XirStorageInput input = facts.storage.inputs[i];
        if (!input.entry_environment) continue;
        CHECK(input.bank == XIR_STORAGE_FOOTPRINT);
        const XrXirFunction *function = &module->functions[input.function];
        CHECK(input.handle < function->parameter_count &&
            xr_xir_type_is_cell(module->types,function->parameters[input.handle]));
        XirRestrictionFunction row = facts.restrictions.functions[input.function];
        CHECK(input.value >= row.physical_values && input.value < row.value_count);
        XirRestrictionFormulaRow formula = facts.restrictions.formulas[row.intrinsic[input.value]];
        bool own = false;
        for (uint32_t t = 0; t < formula.count; ++t)
            own |= facts.restrictions.inputs[formula.first + t] == input.value;
        CHECK(!formula.constant && own);
        observed = input; ++found;
    }
    if (status == XR_XIR_OK) CHECK(found);
    uint32_t *values = NULL, *instructions = NULL;
    if (status == XR_XIR_OK && relocate) {
        const XrXirFunction *body = &module->functions[observed.function];
        uint32_t count = body->parameter_count + body->instruction_count;
        values = xir_compile_alloc(&effect_context,(size_t)count*sizeof(*values),&status);
        if (status == XR_XIR_OK) instructions = xir_compile_alloc(&effect_context,
            (size_t)body->instruction_count*sizeof(*instructions),&status);
        if (status == XR_XIR_OK) {
            for (uint32_t v = 0; v < count; ++v) values[v] = v < body->parameter_count ? v : v+1;
            for (uint32_t i = 0; i < body->instruction_count; ++i) instructions[i] = i+1;
            XirStorageRelocation map = {observed.function,count+1,body->instruction_count+1,values,instructions};
            const XirStorageInput *before = facts.storage.inputs;
            status = xir_storage_flow_relocate(effects->checked_roots->storage,&map,1,NULL);
            XirStorageFlowView view = {0};
            CHECK(xir_storage_flow_view(effects->checked_roots->storage,&view) == XR_XIR_OK);
            if (status != XR_XIR_OK) CHECK(view.inputs == before);
            else {
                bool moved = false;
                for (uint32_t i = 0; i < view.input_count; ++i) {
                    XirStorageInput input = view.inputs[i];
                    moved |= input.function == observed.function && input.value == observed.value+1 &&
                        input.handle == observed.handle && input.entry_environment;
                }
                CHECK(moved);
            }
        }
    }
    xr_compile_resources_free(values); xr_compile_resources_free(instructions);
    xr_xir_compile_effects_free(effects);
    return status;
}

static void entry_footprint_ownership(void) {
    EffectMark baseline = effect_mark();
    XrXirArtifact *artifact = root_source("entry_cell_footprints.xr",XR_XIR_OK);
    EffectMark retained = effect_mark();
    size_t sites[2] = {0};
    for (uint32_t relocated = 0; relocated < 2; ++relocated) {
        attempts = 0; injected = false;
        CHECK(entry_footprint_case(artifact,relocated != 0) == XR_XIR_OK);
        sites[relocated] = attempts;
        effect_mark_check(retained);
        for (size_t site = 0; site < sites[relocated]; ++site) {
            attempts = 0; injected = false; fail_at = site;
            CHECK(entry_footprint_case(artifact,relocated != 0) == XR_XIR_OUT_OF_MEMORY && injected);
            fail_at = SIZE_MAX; effect_mark_check(retained);
        }
    }
    XrXirCheckedPacket packet = {0};
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_checked_write(artifact,&packet,&diagnostic) == XR_XIR_OK);
    XrXirArtifact *received = NULL;
    CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&received,&diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(received); xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(artifact); effect_mark_check(baseline);
    printf("hidden Cell entry environments: symbolic restriction, loan transport and %zu/%zu relocated allocation failures PASS\n",
        sites[0],sites[1]);
}
#endif
