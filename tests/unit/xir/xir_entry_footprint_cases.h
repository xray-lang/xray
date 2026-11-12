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

static XrXirStatus entry_suspend_parameter_step(void *context, const XirSuspendCauseStep *step) {
    uint32_t *count = context;
    CHECK(!step->function && step->callee == 1 && !step->parameter &&
        step->parameter_dependency && !step->terminal);
    ++*count;
    return XR_XIR_OK;
}

static void entry_suspend_parameter_ownership(void) {
    EffectMark baseline = effect_mark();
    uint64_t source[] = {4, 4}, target[] = {4, 4};
    EffectCheckFunction functions[] = {
        {.first=0,.parameters=1,.words=1,.root=1,.declaration=0,.suspend_bits=source},
        {.first=2,.parameters=1,.words=1,.root=3,.declaration=1,.suspend_bits=target}};
    EffectCheckNode nodes[4] = {0};
    for (uint32_t n = 0; n < 4; ++n) {
        nodes[n].function = n / 2;
        nodes[n].aliases = nodes[n].calls = UINT32_MAX;
    }
    nodes[1].calls = 0;
    uint32_t argument = 0;
    EffectCheckCall call = {.target=3,.count=1,.next=UINT32_MAX,.output=1,.protocol=2};
    EffectCheckedRoots roots = {.resources=effect_context.resources,.functions=2,.nodes=4,
        .function=functions,.node=nodes,.call=&call,.calls=1,.argument=&argument,.arguments=1};
    uint32_t count = 0;
    EffectSuspendParameterTrace walk = {entry_suspend_parameter_step, &count, 0, false};
    attempts = 0; injected = false;
    CHECK(effect_checked_suspend_parameter_trace(&roots,1,0,&walk) == XR_XIR_OK && count == 1 && walk.emitted);
    size_t sites = attempts;
    effect_mark_check(baseline);
    for (size_t site = 0; site < sites; ++site) {
        attempts = 0; injected = false; fail_at = site;
        walk.emitted = false;
        CHECK(effect_checked_suspend_parameter_trace(&roots,1,0,&walk) == XR_XIR_OUT_OF_MEMORY && injected);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    printf("symbolic suspension presentation: same-owner helper edge and %zu allocation failures PASS\n", sites);
}

static bool entry_footprint_map(const XrXirModule *, uint32_t, XirStorageRelocation *, XrXirStatus *);

typedef struct EntryCleanupPreparation {
    EffectLifetimePreparation lifetime;
    const XrXirModule *module;
    uint32_t parent, composed;
    bool panic_places;
} EntryCleanupPreparation;

static XrXirStatus entry_cleanup_observe(void *context, const XrXirModule *module,
    uint32_t function, uint32_t instruction, const XrXirInstruction *op) {
    EntryCleanupPreparation *prepare = context;
    return effect_lifetime_observe(&prepare->lifetime,module,function,instruction,op);
}

static XrXirStatus entry_cleanup_points(void *context, const XirExecutionOwner *execution,
    XirStorageFlow *flow, bool final, bool *ready) {
    EntryCleanupPreparation *prepare = context;
    uint32_t targets[] = {root_find(prepare->module,"forward_i64"),root_find(prepare->module,"write_i64")};
    while (prepare->composed < 2) {
        XirStorageFlowView view = {0};
        XrXirStatus status = xir_storage_flow_view(flow,&view);
        if (status != XR_XIR_OK) return status;
        uint32_t source = UINT32_MAX;
        for (uint32_t a = 0; a < view.application_count; ++a) {
            XirStorageApplication app = view.applications[a];
            if (app.context_only || app.kind != XIR_EXECUTION_CALL || app.protocol != 2 ||
                app.declaration != targets[prepare->composed] ||
                app.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT) continue;
            if (prepare->composed) {
                if (app.caller_instance == view.applications[prepare->parent].target) {source=a; break;}
            } else for (uint32_t b = 0; b < view.application_count; ++b) {
                XirStorageApplication caller = view.applications[b];
                if (caller.context_only || caller.kind != XIR_EXECUTION_CLEANUP_USE ||
                    caller.target != app.caller_instance) continue;
                source=a; prepare->parent=b; break;
            }
            if (source != UINT32_MAX) break;
        }
        if (source == UINT32_MAX) break;
        status = xir_storage_flow_compose_environment(flow,source,prepare->parent,&prepare->parent);
        if (status != XR_XIR_OK) return status;
        ++prepare->composed;
    }
    if (prepare->composed == 2 && !prepare->panic_places) {
        uint32_t callers[] = {root_find(prepare->module,"plainWriter"),root_find(prepare->module,"projectedWriter")};
        for (uint32_t c = 0; c < 2; ++c) {
            const XrXirFunction *body = &prepare->module->functions[callers[c]];
            uint32_t call = UINT32_MAX;
            for (uint32_t i = 0; i < body->instruction_count; ++i)
                if (body->instructions[i].op == XR_XIR_CALL && body->instructions[i].immediate == targets[1]) {call=i; break;}
            CHECK(call != UINT32_MAX);
            XrXirInstruction op = body->instructions[call];
            CHECK(op.args[1] == 1);
            uint32_t handle = body->operands[op.args[0]], value = UINT32_MAX;
            XrXirStatus status = xir_storage_flow_contents_at(flow,callers[c],call,handle,XIR_STORAGE_PANIC,&value);
            if (status != XR_XIR_OK) return status;
            XirStorageFlowView view = {0};
            CHECK(xir_storage_flow_view(flow,&view) == XR_XIR_OK);
            bool panic = false;
            for (uint32_t e = 0; e < view.crossflow_count; ++e) {
                XirStorageCrossFlow edge = view.crossflows[e];
                XirStorageApplication app = view.applications[edge.application];
                panic |= app.function == callers[c] && app.instruction == call &&
                    app.declaration == targets[1] && app.outcome == XIR_EXECUTION_PANIC;
            }
            CHECK(panic == (c != 0));
        }
        prepare->panic_places = true;
    }
    if (final) CHECK(prepare->composed == 2 && prepare->panic_places);
    return effect_lifetime_points(&prepare->lifetime,execution,flow,final,ready);
}

static XrXirStatus entry_cleanup_finish(void *context, const XirExecutionOwner *execution,
    XirStorageFlow *flow, const XirRestrictionReceiver *receiver) {
    EntryCleanupPreparation *prepare = context;
    return effect_lifetime_finish(&prepare->lifetime,execution,flow,receiver);
}

static XrXirStatus entry_cleanup_case(const XrXirArtifact *artifact, bool qualified) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    XrXirEffects *effects = NULL;
    XrXirDiagnostic diagnostic = {0};
    XirEffectInput input = {module,artifact->construction,NULL};
    EntryCleanupPreparation prepare = {.module=module,.parent=UINT32_MAX};
    prepare.lifetime.diagnostic = &diagnostic;
    XrXirStatus status = qualified ? xr_hir_escape_builder_new(&effect_context,module,NULL,NULL,
        &prepare.lifetime.builder) : XR_XIR_OK;
    if (status == XR_XIR_OK && qualified) {
        XrXirRootRefiner refiner = {.context=&prepare,.observe=entry_cleanup_observe,
            .points=entry_cleanup_points,.prepare=entry_cleanup_finish};
        status = effect_infer(&effect_context,input,&refiner,&effects,&diagnostic);
        if (status == XR_XIR_OK) CHECK(prepare.composed == 2);
    } else if (status == XR_XIR_OK)
        status = xir_effects_infer_lifetimes_input(&effect_context,input,&effects,&diagnostic);
    xr_hir_escape_builder_free(prepare.lifetime.builder);
    uint32_t cleanups = 0, projected = 0;
    for (uint32_t f = 0; status == XR_XIR_OK && f < module->function_count; ++f) {
        if (!module->declarations->functions[f].cleanup_owner) continue;
        ++cleanups;
        XrXirEffect task = XR_XIR_EFFECT_UNKNOWN;
        XrXirEffectFacts facts = xir_effects_declaration_read(&effect_context,effects,f,&task);
        CHECK(facts.status == XR_XIR_OK && facts.control.suspend == XR_XIR_EFFECT_NONE &&
            facts.control.throws == XR_XIR_EFFECT_NONE && task == XR_XIR_EFFECT_NONE);
        const EffectCheckedRoots *roots = effects->checked_roots;
        const EffectControlSummary *control = effect_roots_read(effects)->control_summary;
        uint32_t node = control->source_nodes[f];
        CHECK(node < roots->nodes && node != roots->function[f].root);
        uint32_t call = roots->node[node].cleanup_call;
        CHECK(call < roots->calls && roots->call[call].protocol == 4 &&
            roots->call[call].target == roots->function[f].root && roots->call[call].output == node);
        CHECK(roots->call[call].suspend_projected);
        for (uint32_t a = roots->call[call].suspend_actuals; a != UINT32_MAX; a = roots->suspend_actual[a].next) {
            CHECK(a < roots->suspend_actuals && roots->suspend_actual[a].source < roots->nodes);
            ++projected;
        }
    }
    if (status == XR_XIR_OK) CHECK(cleanups == (qualified ? 2u : 1u) && projected);
    XirStorageRelocation map = {0};
    if (status == XR_XIR_OK && qualified) {
        XirStorageFlowView before = {0};
        CHECK(xir_storage_flow_view(effects->checked_roots->storage,&before) == XR_XIR_OK);
        uint32_t contexts = 0, sample = UINT32_MAX;
        for (uint32_t a = 0; a < before.application_count; ++a) {
            XirStorageApplication app = before.applications[a];
            if (!app.context_only) continue;
            uint32_t found = UINT32_MAX;
            CHECK(xir_storage_flow_environment(effects->checked_roots->storage,app.source_application,app.environment,&found) == XR_XIR_OK && found == a);
            sample = a;
            ++contexts;
        }
        CHECK(contexts && sample != UINT32_MAX);
        XirStorageApplication context = before.applications[sample];
        entry_footprint_map(module,context.function,&map,&status);
        if (status == XR_XIR_OK) status = xir_storage_flow_relocate(effects->checked_roots->storage,&map,1,NULL);
        if (status == XR_XIR_OK) {
            XirStorageFlowView after = {0};
            CHECK(xir_storage_flow_view(effects->checked_roots->storage,&after) == XR_XIR_OK);
            uint32_t found = UINT32_MAX;
            CHECK(xir_storage_flow_environment(effects->checked_roots->storage,context.source_application,context.environment,&found) == XR_XIR_OK && found == sample);
            CHECK(after.applications[sample].instruction == context.instruction + 1 &&
                after.applications[sample].cleanup_handler == context.cleanup_handler &&
                after.applications[sample].after_panic == context.after_panic);
        }
    }
    xr_compile_resources_free((void *)map.values);
    xr_compile_resources_free((void *)map.instructions);
    xr_xir_compile_effects_free(effects);
    return status;
}

static void entry_cleanup_fixture(const char *file, bool qualified) {
    EffectMark baseline = effect_mark();
    XrXirArtifact *artifact = root_source(file,XR_XIR_OK);
    EffectMark retained = effect_mark();
    attempts = 0; injected = false;
    XrXirStatus initial = entry_cleanup_case(artifact,qualified);
    if (initial != XR_XIR_OK) fprintf(stderr,"cleanup fixture %s initial status%u\n",file,initial);
    CHECK(initial == XR_XIR_OK);
    size_t sites = attempts;
    effect_mark_check(retained);
    for (size_t site = 0; site < sites; ++site) {
        attempts = 0; injected = false; fail_at = site;
        CHECK(entry_cleanup_case(artifact,qualified) == XR_XIR_OUT_OF_MEMORY && injected);
        fail_at = SIZE_MAX;
        effect_mark_check(retained);
    }
    XrXirCheckedPacket packet = {0};
    XrXirArtifact *received = NULL;
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);
    XrXirDiagnostic cold = {0};
    XrXirStatus cold_status = xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&received,&cold);
    if (cold_status != XR_XIR_OK) fprintf(stderr,"cleanup cold %s status%u f%u b%u i%u reason%u\n",
        file,cold_status,cold.function,cold.block,cold.instruction,cold.reason);
    CHECK(cold_status == XR_XIR_OK);
    CHECK(entry_cleanup_case(received,qualified) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(received);
    effect_mark_check(baseline);
    printf("cleanup %s: actual suspension, cold Checked receipt and %zu allocation failures PASS\n", file, sites);
}

static void entry_cleanup_ownership(void) {
    entry_cleanup_fixture("cleanup_receiver_footprints.xr",false);
    entry_cleanup_fixture("cleanup_mutable_footprints.xr",true);
    EffectMark baseline = effect_mark();
    XrXirArtifact *artifact = root_source("cleanup_receiver_footprints.xr",XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    bool checked = false;
    for (uint32_t f = 0; f < module->function_count && !checked; ++f) {
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i) {
            XrXirInstruction *op = (XrXirInstruction *)module->functions[f].instructions + i;
            if (op->op != XR_XIR_CLEANUP_REGISTER) continue;
            XrXirInstruction original = *op;
            op->op = XR_XIR_OP_COUNT;
            CHECK(xr_xir_compile_verify_v2(&effect_context,module,artifact->construction,NULL) == XR_XIR_BAD_STRUCTURE);
            *op = original;
            op->immediate = module->function_count;
            CHECK(xr_xir_compile_verify_v2(&effect_context,module,artifact->construction,NULL) == XR_XIR_BAD_STRUCTURE);
            *op = original;
            checked = true;
            break;
        }
    }
    CHECK(checked);
    xr_xir_compile_artifact_free(artifact);
    effect_mark_check(baseline);
}

static XrXirStatus entry_module_stored_site(void *context, const XirExecutionSite *site) {
    CHECK(site->kind == XIR_EXECUTION_STORABLE);
    ++*(uint32_t *)context;
    return XR_XIR_OK;
}

static void entry_module_cell_contract(void) {
    EffectMark baseline = effect_mark();
    XrXirArtifact *artifact = root_source("module_callback_cells.xr",XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);
    artifact = NULL;
    CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&artifact,NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    XrXirEffects *effects = NULL;
    CHECK(xir_effects_infer_lifetimes_input(&effect_context,
        (XirEffectInput){module,artifact->construction,NULL},&effects,NULL) == XR_XIR_OK);
    uint32_t reads = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *body = &module->functions[f];
        EffectCheckFunction row = effects->checked_roots->function[f];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            XrXirInstruction op = body->instructions[i];
            if (op.op != XR_XIR_CELL_READ || !xr_xir_type_is_callable(module->types,op.type)) continue;
            uint32_t value = body->parameter_count + i;
            CHECK(effects->checked_roots->node[row.first+op.args[0]].module_cell &&
                effects->checked_roots->node[row.first+value].storable_value);
            XirExecutionValue reference = {f,value,XIR_EXECUTION_CURRENT_ENVIRONMENT};
            uint32_t seen = 0, remainder = 0;
            bool complete = false;
            CHECK(xir_execution_visit_value_footprint_in(effects->checked_roots,&reference,
                entry_module_stored_site,&seen,&complete,&remainder) == XR_XIR_OK);
            CHECK(seen && !complete && remainder == XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT);
            ++reads;
        }
    }
    CHECK(reads == 1);
    xr_xir_compile_effects_free(effects);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(artifact);
    effect_mark_check(baseline);
    puts("module callable Cells: stored contract retains opaque environment after cold Checked receipt PASS");
}

static bool entry_footprint_map(const XrXirModule *module, uint32_t function,
    XirStorageRelocation *map, XrXirStatus *status) {
    const XrXirFunction *body = &module->functions[function];
    uint32_t count = body->parameter_count + body->instruction_count;
    uint32_t *values = xir_compile_alloc(&effect_context,(size_t)count*sizeof(*values),status);
    *map = (XirStorageRelocation){.function=function,.physical_values=count+1,
        .instruction_count=body->instruction_count+1,.values=values};
    if (!values) return false;
    uint32_t *instructions = xir_compile_alloc(&effect_context,(size_t)body->instruction_count*sizeof(*instructions),status);
    map->instructions = instructions;
    if (!instructions) return false;
    for (uint32_t v = 0; v < count; ++v) values[v] = v < body->parameter_count ? v : v+1;
    for (uint32_t i = 0; i < body->instruction_count; ++i) instructions[i] = i+1;
    return true;
}

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
    bool unused = false;
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
        bool called = false, captured = false;
        for (uint32_t a = 0; a < facts.storage.application_count; ++a) {
            XirStorageApplication app = facts.storage.applications[a];
            if (app.declaration != input.function) continue;
            bool creation = app.kind == XIR_EXECUTION_CALL && (app.protocol == 1 || app.protocol == 4);
            captured |= creation;
            called |= !creation;
            if (creation) for (uint32_t b = 0; b < app.count; ++b)
                CHECK(facts.storage.bindings[app.first+b].formal_value != input.value);
        }
        unused |= captured && !called;
        observed = input; ++found;
    }
    if (status == XR_XIR_OK) CHECK(found && unused && facts.storage.bottom_count);
    XirStorageBottom bottom = status == XR_XIR_OK ? facts.storage.bottoms[0] : (XirStorageBottom){0};
    XirStorageRelocation maps[2] = {0};
    if (status == XR_XIR_OK && relocate) {
        uint32_t count = bottom.function == observed.function ? 1u : 2u;
        entry_footprint_map(module,observed.function,&maps[0],&status);
        if (status == XR_XIR_OK && count == 2) entry_footprint_map(module,bottom.function,&maps[1],&status);
        if (status == XR_XIR_OK) {
            const XirStorageInput *before = facts.storage.inputs;
            const XirStorageBottom *before_bottom = facts.storage.bottoms;
            status = xir_storage_flow_relocate(effects->checked_roots->storage,maps,count,NULL);
            XirStorageFlowView view = {0};
            CHECK(xir_storage_flow_view(effects->checked_roots->storage,&view) == XR_XIR_OK);
            if (status != XR_XIR_OK) CHECK(view.inputs == before && view.bottoms == before_bottom);
            else {
                bool moved = false;
                for (uint32_t i = 0; i < view.input_count; ++i) {
                    XirStorageInput input = view.inputs[i];
                    moved |= input.function == observed.function && input.value == observed.value+1 &&
                        input.handle == observed.handle && input.entry_environment;
                }
                CHECK(moved);
                bool moved_bottom = false;
                for (uint32_t b = 0; b < view.bottom_count; ++b)
                    moved_bottom |= view.bottoms[b].function == bottom.function && view.bottoms[b].value == bottom.value+1;
                CHECK(moved_bottom);
            }
        }
    }
    for (uint32_t m = 0; m < 2; ++m) {
        xr_compile_resources_free((void *)maps[m].values);
        xr_compile_resources_free((void *)maps[m].instructions);
    }
    xr_xir_compile_effects_free(effects);
    return status;
}

static void entry_footprint_ownership(void) {
    entry_suspend_parameter_ownership();
    entry_cleanup_ownership();
    entry_module_cell_contract();
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
