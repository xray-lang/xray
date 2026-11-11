/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_callback_entry_cases.h - Opaque callback guards and numeric ownership
 */
typedef struct CallbackEntryOracle { const XirExecutionOwner *owner; uint32_t count; bool unbound; } CallbackEntryOracle;

static XrXirStatus callback_entry_oracle(void *context, const XirExecutionSite *site) {
    CallbackEntryOracle *oracle = context;
    CHECK(site->kind == XIR_EXECUTION_CALLBACK_ENTRY && site->parent_kind == XIR_EXECUTION_PENDING);
    CHECK(site->function == 1 && site->instruction == 3 && !site->callback_parameter);
    CHECK(!site->unresolved && site->declaration == 2 && site->argument_count == (oracle->unbound ? 2u : 1u) && site->capture_count == 1);
    CHECK(site->remainder == (XIR_EXECUTION_REMAINDER_OPAQUE |
        (oracle->unbound ? XIR_EXECUTION_REMAINDER_PARAMETER : 0u)));
    XirExecutionArgument actual = {0};
    CHECK(xir_execution_argument(oracle->owner, site, 0, &actual) == XR_XIR_OK);
    CHECK(actual.function == 1 && actual.value == 2 && actual.cell);
    actual.value = 91;
    CHECK(xir_execution_argument(oracle->owner, site, 1, &actual) ==
        (oracle->unbound ? XR_XIR_UNRESOLVED : XR_XIR_BAD_STRUCTURE) && actual.value == 91);
    CHECK(xir_execution_argument(oracle->owner, site, 2, &actual) == XR_XIR_BAD_STRUCTURE && actual.value == 91);
    ++oracle->count;
    return XR_XIR_OK;
}

static uint32_t callback_entry_application(const XirStorageFlowView *view, uint32_t instruction) {
    uint32_t found = UINT32_MAX;
    for (uint32_t a = 0; a < view->application_count; ++a) {
        XirStorageApplication app = view->applications[a];
        if (app.kind != XIR_EXECUTION_CALLBACK_ENTRY) continue;
        CHECK(found == UINT32_MAX && app.function == 1 && app.declaration == 2);
        CHECK(app.instruction == instruction && app.parent_kind == XIR_EXECUTION_PENDING && !app.callback_parameter);
        CHECK(app.outcome == XIR_EXECUTION_NORMAL && app.reference != UINT32_MAX);
        bool identity = false;
        for (uint32_t b = 0; b < app.count; ++b) {
            XirStorageBinding binding = view->bindings[app.first + b];
            if (binding.formal_value) continue;
            CHECK(binding.formal_function == 2 && binding.actual_function == 1);
            CHECK(binding.actual_value == instruction - 1 && binding.actual_environment == XIR_EXECUTION_CURRENT_ENVIRONMENT);
            identity = true;
        }
        CHECK(identity);
        for (uint32_t e = 0; e < view->crossflow_count; ++e) CHECK(view->crossflows[e].application != a);
        found = a;
    }
    CHECK(found != UINT32_MAX);
    return found;
}

static XrXirStatus callback_entry_owner_case(bool unbound) {
    XrXirType callable = XR_XIR_CONSTRUCTED_TYPE_BASE, executor = callable + 1, cell = callable + 2;
    XrXirCallableParameter callback = {.type = callable};
    XrXirCallableParameter argument = {.type = XR_XIR_I64};
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_UNIT, .element = XR_XIR_NEVER,
            .parameters = &argument, .parameter_count = 1, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_UNIT, .element = XR_XIR_NEVER,
            .parameters = &callback, .parameter_count = 1, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_I64}
    };
    XrXirTypes types = {.nodes = nodes, .count = 3};
    if (!unbound) { nodes[0].parameters = NULL; nodes[0].parameter_count = 0; }
    XrXirInstruction done = {.op = XR_XIR_RETURN};
    XrXirInstruction caller[] = {
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64},
        {.op = XR_XIR_CELL_NEW, .type = cell, .args = {1}},
        {.op = XR_XIR_FUNCTION_REF, .type = callable, .immediate = 2, .args = {0, 1}},
        {.op = XR_XIR_CALL_INDIRECT, .immediate = 0, .args = {1, 1}},
        {.op = XR_XIR_RETURN}
    };
    XrXirInstruction entry[] = {{.op = XR_XIR_CONST_INT, .type = XR_XIR_I64}, {.op = XR_XIR_RETURN}};
    uint32_t operands[] = {2, 3};
    XrXirType captured_parameters[] = {cell, XR_XIR_I64};
    XrXirBlock blocks[] = {{.count = 1}, {.count = 5}, {.count = 1}, {.count = 2}};
    XrXirFunction functions[] = {
        {.instructions = &done, .instruction_count = 1, .blocks = blocks, .block_count = 1, .error_type = XR_XIR_NEVER},
        {.parameters = &executor, .parameter_count = 1, .instructions = caller, .instruction_count = 5,
            .blocks = blocks + 1, .block_count = 1, .operands = operands, .operand_count = 2, .error_type = XR_XIR_NEVER},
        {.parameters = captured_parameters, .parameter_count = 2, .instructions = &done, .instruction_count = 1,
            .blocks = blocks + 2, .block_count = 1, .error_type = XR_XIR_NEVER},
        {.result = XR_XIR_I64, .instructions = entry, .instruction_count = 2,
            .blocks = blocks + 3, .block_count = 1, .error_type = XR_XIR_NEVER}
    };
    if (!unbound) functions[2].parameter_count = 1;
    XrXirFunctionIdentity identities[] = {{0}, {0}, {.capture_owner = 2, .capture_count = 1}, {0}};
    XrXirSourceModule source = {.initializer = 0};
    XrXirDeclarations declarations = {.modules = &source, .module_count = 1, .functions = identities, .entry_function = 3};
    XrXirModule module = {.stage = XR_XIR_CHECKED, .types = &types, .functions = functions,
        .function_count = 4, .declarations = &declarations};
    EffectCheckedRoots *roots = NULL;
    XrXirStatus status = effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, false);
    CallbackEntryOracle oracle = {.owner = roots, .unbound = unbound};
    if (status == XR_XIR_OK) status = xir_execution_visit_callback_entries(roots, callback_entry_oracle, &oracle);
    if (status == XR_XIR_OK) CHECK(oracle.count == 1);
    XirStorageFlowView view = {0};
    if (status == XR_XIR_OK) status = xir_storage_flow_seal(roots->storage, &view);
    if (unbound && status == XR_XIR_UNRESOLVED) CHECK(!view.functions && !view.applications);
    uint32_t application = status == XR_XIR_OK ? callback_entry_application(&view, 3) : UINT32_MAX;
    uint32_t values[] = {0, 2, 3, 4, 5, 6}, instructions[] = {1, 2, 3, 4, 5};
    XirStorageRelocation relocation = {1, 7, 6, values, instructions};
    if (status == XR_XIR_OK) {
        const XirStorageApplication *previous = view.applications;
        status = xir_storage_flow_relocate(roots->storage, &relocation, 1, NULL);
        CHECK(xir_storage_flow_view(roots->storage, &view) == XR_XIR_OK);
        if (status != XR_XIR_OK) CHECK(view.applications == previous && view.applications[application].instruction == 3);
        else CHECK(callback_entry_application(&view, 4) == application);
    }
    effect_checked_roots_free(roots);
    if (status != (unbound ? XR_XIR_UNRESOLVED : XR_XIR_OK) && fail_at == SIZE_MAX)
        fprintf(stderr, "callback entry owner status %u\n", status);
    return status;
}

static void callback_entry_owner_cases(void) {
    EffectMark baseline = effect_mark();
    attempts = 0;
    CHECK(callback_entry_owner_case(false) == XR_XIR_OK);
    size_t sites = attempts;
    effect_mark_check(baseline);
    for (size_t site = 0; site < sites; ++site) {
        attempts = 0; injected = false; fail_at = site;
        CHECK(callback_entry_owner_case(false) == XR_XIR_OUT_OF_MEMORY && injected);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    attempts = 0;
    CHECK(callback_entry_owner_case(true) == XR_XIR_UNRESOLVED);
    size_t pending_sites = attempts;
    effect_mark_check(baseline);
    for (size_t site = 0; site < pending_sites; ++site) {
        attempts = 0; injected = false; fail_at = site;
        CHECK(callback_entry_owner_case(true) == XR_XIR_OUT_OF_MEMORY && injected);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    printf("callback entries: opaque guards, relocation %zu and pending %zu allocation failures PASS\n", sites, pending_sites);
}

typedef struct CallbackReceiverOracle { uint32_t count; bool opaque; } CallbackReceiverOracle;

static XrXirStatus callback_receiver_oracle(void *context, const XirExecutionSite *site) {
    CallbackReceiverOracle *oracle = context;
    CHECK(site->kind == XIR_EXECUTION_CALLBACK_ENTRY && site->parent_kind == XIR_EXECUTION_CALL);
    CHECK(site->function == 0 && site->instruction == 3 && site->declaration == 1);
    CHECK(site->callback_parameter == UINT32_MAX && site->entry_context != UINT32_MAX);
    CHECK(!site->unresolved && site->remainder == (oracle->opaque ? XIR_EXECUTION_REMAINDER_OPAQUE : 0u));
    ++oracle->count;
    return XR_XIR_OK;
}

static uint32_t callback_receiver_application(const XirStorageFlowView *view, uint32_t instruction) {
    uint32_t found = UINT32_MAX;
    for (uint32_t a = 0; a < view->application_count; ++a) {
        XirStorageApplication app = view->applications[a];
        if (app.kind != XIR_EXECUTION_CALLBACK_ENTRY) continue;
        CHECK(found == UINT32_MAX && app.function == 0 && app.instruction == instruction && app.declaration == 1);
        CHECK(app.callback_parameter == UINT32_MAX && app.entry_context < view->application_count);
        XirStorageApplication parent = view->applications[app.entry_context];
        CHECK(parent.kind == XIR_EXECUTION_CALL && parent.function == 0 && parent.declaration == 2 &&
            parent.instruction == instruction && parent.outcome == XIR_EXECUTION_NORMAL);
        for (uint32_t e = 0; e < view->crossflow_count; ++e) CHECK(view->crossflows[e].application != a);
        found = a;
    }
    CHECK(found != UINT32_MAX);
    return found;
}

typedef struct CallbackGrowOracle { EffectCheckedRoots *roots; uint32_t pending, count; } CallbackGrowOracle;

static XrXirStatus callback_grow_application_bank(void *context, const XirExecutionSite *site) {
    CallbackGrowOracle *oracle = context;
    CHECK(site->kind == XIR_EXECUTION_CALL && site->protocol == 1);
    ++oracle->count;
    XirStorageFlowView before = {0}, after = {0};
    XrXirStatus status = xir_storage_flow_view(oracle->roots->storage, &before);
    oracle->roots->pending_count = oracle->pending;
    bool changed = false;
    if (status == XR_XIR_OK) status = xir_storage_flow_link(oracle->roots->storage, oracle->roots, &changed);
    if (status == XR_XIR_OK) status = xir_storage_flow_view(oracle->roots->storage, &after);
    if (status == XR_XIR_OK) CHECK(changed && after.application_count == before.application_count + oracle->pending &&
        before.applications != after.applications);
    return status;
}

static XrXirStatus callback_origin_rebind(EffectCheckedRoots *roots) {
    uint32_t pending = roots->pending_count;
    roots->pending_count = 0;
    bool changed = false;
    XrXirStatus status = xir_storage_flow_link(roots->storage, roots, &changed);
    XirStorageFlowView view = {0};
    if (status == XR_XIR_OK) status = xir_storage_flow_view(roots->storage, &view);
    uint32_t application = UINT32_MAX;
    for (uint32_t a = 0; status == XR_XIR_OK && a < view.application_count; ++a)
        if (view.applications[a].function == 0 && view.applications[a].declaration == 2 &&
            view.applications[a].outcome == XIR_EXECUTION_NORMAL) application = a;
    if (status == XR_XIR_OK) CHECK(application != UINT32_MAX);
    /* A grounded numeric cycle retains an application edge on the constructor
     * value while its target visitor extends the application bank. */
    XirStorageCrossFlow edge = {2, 0, 0, 2, application, XIR_STORAGE_CONTENTS};
    view.crossflows = &edge; view.crossflow_count = 1;
    if (status == XR_XIR_OK) status = execution_origins_receive(&effect_context, roots, &view);
    CallbackGrowOracle oracle = {roots, pending, 0};
    bool complete = false;
    if (status == XR_XIR_OK) status = xir_execution_visit_value_captures(roots, 0, 2,
        callback_grow_application_bank, &oracle, &complete);
    if (status == XR_XIR_OK) CHECK(complete && oracle.count == 1);
    roots->pending_count = pending;
    return status;
}

static XrXirStatus callback_receiver_owner_case(bool callable_contents, bool grow) {
    XrXirType callable = XR_XIR_CONSTRUCTED_TYPE_BASE, cell = callable + 1;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_UNIT, .element = XR_XIR_NEVER,
            .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_I64}
    };
    XrXirTypes types = {.nodes = nodes, .count = 2};
    if (callable_contents) nodes[1].element = callable;
    XrXirInstruction done = {.op = XR_XIR_RETURN};
    XrXirInstruction caller[] = {
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64},
        {.op = XR_XIR_CELL_NEW, .type = cell, .args = {0}},
        {.op = XR_XIR_FUNCTION_REF, .type = callable, .immediate = 1, .args = {0, 1}},
        {.op = XR_XIR_CALL, .immediate = 2, .args = {1, 1}},
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64},
        {.op = XR_XIR_RETURN, .args = {4}}
    };
    XrXirInstruction invoke[] = {{.op = XR_XIR_CALL_INDIRECT}, {.op = XR_XIR_RETURN}};
    XrXirInstruction repeated[30] = {{0}};
    for (uint32_t i = 0; i < 29; ++i) repeated[i].op = XR_XIR_CALL_INDIRECT;
    repeated[29].op = XR_XIR_RETURN;
    if (callable_contents) caller[0] = (XrXirInstruction){.op = XR_XIR_FUNCTION_REF, .type = callable, .immediate = 4};
    uint32_t operands[] = {1, 2};
    XrXirBlock blocks[] = {{.count = 6}, {.count = 1}, {.count = 2}, {.count = 1}, {.count = 1}};
    XrXirFunction functions[] = {
        {.result = XR_XIR_I64, .instructions = caller, .instruction_count = 6, .blocks = blocks,
            .block_count = 1, .operands = operands, .operand_count = 2, .error_type = XR_XIR_NEVER},
        {.parameters = &cell, .parameter_count = 1, .instructions = &done, .instruction_count = 1,
            .blocks = blocks + 1, .block_count = 1, .error_type = XR_XIR_NEVER},
        {.parameters = &callable, .parameter_count = 1, .instructions = invoke, .instruction_count = 2,
            .blocks = blocks + 2, .block_count = 1, .error_type = XR_XIR_NEVER},
        {.instructions = &done, .instruction_count = 1, .blocks = blocks + 3, .block_count = 1, .error_type = XR_XIR_NEVER},
        {.instructions = &done, .instruction_count = 1, .blocks = blocks + 4, .block_count = 1, .error_type = XR_XIR_NEVER}
    };
    XrXirFunctionIdentity identities[] = {{0}, {.capture_owner = 1, .capture_count = 1}, {0}, {0}, {0}};
    if (grow) {
        functions[2].instructions = repeated; functions[2].instruction_count = 30; blocks[2].count = 30;
    }
    XrXirSourceModule source = {.initializer = 3};
    XrXirDeclarations declarations = {.modules = &source, .module_count = 1, .functions = identities, .entry_function = 0};
    XrXirModule module = {.stage = XR_XIR_CHECKED, .types = &types, .functions = functions,
        .function_count = 5, .declarations = &declarations};
    EffectCheckedRoots *roots = NULL;
    XrXirStatus status = grow ? effect_checked_roots_collect(&effect_context, &module, NULL, NULL, NULL, &roots, true) :
        effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, false);
    if (grow) {
        if (status == XR_XIR_OK) status = callback_origin_rebind(roots);
        effect_checked_roots_free(roots);
        return status;
    }
    CallbackReceiverOracle oracle = {.opaque = callable_contents};
    if (status == XR_XIR_OK) status = xir_execution_visit_callback_entries(roots, callback_receiver_oracle, &oracle);
    if (status == XR_XIR_OK) CHECK(oracle.count == 1);
    XirStorageFlowView view = {0};
    if (status == XR_XIR_OK) status = xir_storage_flow_seal(roots->storage, &view);
    uint32_t application = status == XR_XIR_OK ? callback_receiver_application(&view, 3) : UINT32_MAX;
    uint32_t values[] = {1, 2, 3, 4, 5, 6}, instructions[] = {1, 2, 3, 4, 5, 6};
    XirStorageRelocation relocation = {0, 7, 7, values, instructions};
    if (status == XR_XIR_OK) {
        const XirStorageApplication *previous = view.applications;
        status = xir_storage_flow_relocate(roots->storage, &relocation, 1, NULL);
        CHECK(xir_storage_flow_view(roots->storage, &view) == XR_XIR_OK);
        if (status != XR_XIR_OK) CHECK(view.applications == previous && view.applications[application].instruction == 3);
        else CHECK(callback_receiver_application(&view, 4) == application);
    }
    effect_checked_roots_free(roots);
    return status;
}

static void callback_receiver_owner_cases(void) {
    EffectMark baseline = effect_mark();
    for (uint32_t payload = 0; payload < 2; ++payload) {
        attempts = 0;
        CHECK(callback_receiver_owner_case(payload != 0, false) == XR_XIR_OK);
        size_t sites = attempts;
        effect_mark_check(baseline);
        for (size_t site = 0; site < sites; ++site) {
            attempts = 0; injected = false; fail_at = site;
            CHECK(callback_receiver_owner_case(payload != 0, false) == XR_XIR_OUT_OF_MEMORY && injected);
            fail_at = SIZE_MAX;
            effect_mark_check(baseline);
        }
        printf("callback receivers: payload %u, application substitution, relocation and %zu allocation failures PASS\n", payload, sites);
    }
    CHECK(callback_receiver_owner_case(false, true) == XR_XIR_OK);
    effect_mark_check(baseline);
    printf("callback origins: application bank growth within a grounded origin cursor PASS\n");
}
