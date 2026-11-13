/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void closure_placement_source_expectations(const XrXirArtifact *artifact) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XirClosurePlacement *rows = NULL;
    uint32_t count = 0;
    CHECK(xr_xir_compile_closure_placements(artifact, &rows, &count) == XR_XIR_OK && count && rows);
    const char *names[] = {"placementLocal", "placementReturn", "placementNested", "placementChoose",
        "placementStore", "placementLoop", "placementLoopLocal"};
    const bool owned[] = {false, true, false, false, true, true, false};
    for (unsigned n = 0; n < 7; ++n) {
        uint32_t function = root_find(module, names[n]), constructors = 0;
        const XrXirFunction *body = &module->functions[function];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            if (body->instructions[i].op != XR_XIR_FUNCTION_REF || !body->instructions[i].args[1]) continue;
            XirClosureStorage storage = XIR_CLOSURE_UNRESOLVED;
            CHECK(xr_xir_compile_closure_placement(artifact, function, body->parameter_count + i, &storage) == XR_XIR_OK);
            if (storage != (owned[n] ? XIR_CLOSURE_OWNED : XIR_CLOSURE_FRAME))
                fprintf(stderr, "placement %s i%u storage%u\n", names[n], i, (unsigned)storage);
            CHECK(storage == (owned[n] ? XIR_CLOSURE_OWNED : XIR_CLOSURE_FRAME));
            if (n == 5) {
                bool historical = false;
                for (uint32_t p = 0; p < count; ++p)
                    if (rows[p].function == function && rows[p].value == body->parameter_count + i)
                        historical |= (rows[p].ages & 2u) != 0;
                CHECK(historical);
            }
            ++constructors;
        }
        CHECK(constructors);
    }
}

static XrXirStatus closure_placement_receive_case(const XrXirArtifact *artifact) {
    XrXirEffects *effects = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xir_effects_infer_lifetimes_input(&effect_context,
        (XirEffectInput){&artifact->module, artifact->construction, NULL}, &effects, &diagnostic);
    if (status == XR_XIR_OK) {
        XrXirArtifact borrowed = *artifact;
        borrowed.checked_effects = effects;
        borrowed.program_final = NULL;
        closure_placement_source_expectations(&borrowed);
    }
    xr_xir_compile_effects_free(effects);
    return status;
}

static void closure_placement_receiving_boundaries(const XrXirArtifact *artifact) {
    const XrXirEffects *effects = artifact->program_final ? artifact->program_final : artifact->checked_effects;
    const EffectCheckedRoots *roots = effects->checked_roots;
    XirRestrictionView input = roots->restrictions.view;
    CHECK(input.placement_count);
    uint32_t observed = 0;
    while (observed < input.placement_count && input.placements[observed].constructor_call == UINT32_MAX) ++observed;
    CHECK(observed < input.placement_count);
    XirClosurePlacement row = input.placements[observed], original = row;
    input.placements = &row;
    input.placement_count = 1;
    EffectMark retained = effect_mark();
    EffectCheckRestrictions received = {0};
    CHECK(effect_check_restrictions_copy(&effect_context, roots, &input, &received) == XR_XIR_OK);
    row.storage = XIR_CLOSURE_UNRESOLVED;
    CHECK(received.view.placements[0].storage == original.storage);
    effect_check_restrictions_clear(&received);
    effect_mark_check(retained);
    XirStorageFlowView storage = {0};
    CHECK(xir_storage_flow_view(roots->storage, &storage) == XR_XIR_OK);
    uint32_t unrelated = 0;
    while (unrelated < storage.application_count &&
        storage.applications[unrelated].target == original.caller_instance &&
        storage.applications[unrelated].declaration == original.function) ++unrelated;
    CHECK(unrelated < storage.application_count);
    for (unsigned variant = 0; variant < 13; ++variant) {
        row = original;
        switch (variant) {
        case 0: row.function = roots->templates; break;
        case 1: row.value = roots->function[row.function].values; break;
        case 2: row.ages = 4; break;
        case 3: row.remainder = 16; break;
        case 4: row.storage = (XirClosureStorage)3; break;
        case 5: row.storage = XIR_CLOSURE_FRAME; row.ages = 2; break;
        case 6: row.storage = XIR_CLOSURE_FRAME; row.environment = XIR_EXECUTION_UNKNOWN_ENVIRONMENT; break;
        case 7: row.constructor_call = roots->calls; break;
        case 8: row.caller_instance = roots->functions; break;
        case 9: row.entry_context = storage.application_count; break;
        case 10: row.environment = unrelated; break;
        case 11: row.constructor_call = UINT32_MAX; break;
        default: row.caller_instance = (row.caller_instance + 1) % roots->functions; break;
        }
        CHECK(effect_check_restrictions_copy(&effect_context, roots, &input, &received) == XR_XIR_BAD_STRUCTURE);
        effect_check_restrictions_clear(&received);
        effect_mark_check(retained);
    }
}

static void closure_placement_ownership(void) {
    EffectMark baseline = effect_mark();
    XrXirArtifact *artifact = root_source("closure_placements.xr", XR_XIR_OK);
    closure_placement_source_expectations(artifact);
    closure_placement_receiving_boundaries(artifact);
    EffectMark retained = effect_mark();
    attempts = 0; injected = false;
    CHECK(closure_placement_receive_case(artifact) == XR_XIR_OK);
    size_t sites = attempts;
    effect_mark_check(retained);
    for (size_t point = 0; point < sites; ++point) {
        attempts = 0; injected = false; fail_at = point;
        CHECK(closure_placement_receive_case(artifact) == XR_XIR_OUT_OF_MEMORY && injected);
        fail_at = SIZE_MAX;
        effect_mark_check(retained);
    }
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(artifact, &packet, NULL) == XR_XIR_OK);
    XrXirArtifact *specialized = NULL;
    CHECK(xr_xir_compile_specialize(artifact, &specialized, NULL, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);
    artifact = NULL;
    const XrXirModule *closed = xr_xir_compile_artifact_module(specialized);
    uint32_t constructors = 0;
    for (uint32_t f = 0; f < closed->function_count; ++f) {
        const XrXirFunction *body = &closed->functions[f];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            if (body->instructions[i].op != XR_XIR_FUNCTION_REF) continue;
            XirClosureStorage storage = XIR_CLOSURE_UNRESOLVED;
            CHECK(xr_xir_compile_closure_placement(specialized, f, body->parameter_count + i, &storage) == XR_XIR_OK);
            ++constructors;
        }
    }
    CHECK(constructors);
    xr_xir_compile_artifact_free(specialized);
    CHECK(xr_xir_compile_checked_read(&effect_context, packet.bytes, packet.length, &artifact, NULL) == XR_XIR_OK);
    closure_placement_source_expectations(artifact);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(artifact);
    effect_mark_check(baseline);
    printf("Closure placement: publication, local/conditional/nested contexts, cold Checked and %zu failures PASS\n", sites);
}
