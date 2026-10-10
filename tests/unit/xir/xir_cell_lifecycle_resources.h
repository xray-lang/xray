/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_lifecycle_resources.h - Actual runtime failure-prefix closure and budgets
 */
#ifndef XIR_CELL_LIFECYCLE_RESOURCES_H
#define XIR_CELL_LIFECYCLE_RESOURCES_H
enum { CELL_RT_CREATE, CELL_RT_START, CELL_RT_POLL, CELL_RT_CONTROL,
    CELL_RT_RESULT, CELL_RT_GETTER, CELL_RT_NEXT, CELL_RT_RETRY, CELL_RT_STICKY,
    CELL_RT_CLOSE, CELL_RT_DROP, CELL_RT_PHASES };
typedef struct CellRuntimeCost {
    size_t sites, hits, prepare_begin, prepare_end;
    XrXirCallStatus failure;
    XrXirDomainBudgetStats budgets[2], prepare_before, prepare_failed, prepare_after;
    XrXirDomainStats values[2];
    bool recovered, complete;
    uint32_t outputs, reentered;
    int64_t lines[3];
} CellRuntimeCost;
typedef struct CellRuntimeRun {
    unsigned mode;
    const uint32_t *entries;
    XrXirInstance *instances[2];
    XrXirDomain *domains[2];
    XrXirValue result, other_result, other_counter;
    bool result_valid, other_result_valid, other_counter_valid, allow_retry, other_done;
    CellRuntimeCost cost;
} CellRuntimeRun;
static XrXirInstance **cell_runtime_instances;
static void cell_runtime_event_role(CellRuntimeAllocationEvent *event) {
    if (!cell_runtime_instances || event->instance >= 2) return;
    XrXirInstance *instance = cell_runtime_instances[event->instance];
    if (!instance || !instance->executor) return;
    XrXirTaskExecutor *executor = instance->executor;
    XrXirCall *call = executor->current ? executor->current->call : instance->call;
    if (!call) return;
    event->role = executor->current && executor->current->task ? 2u : 1u;
    if (call->top) {
        uintptr_t first = (uintptr_t)call->config.entries, entry = (uintptr_t)call->top->entry;
        uint64_t bytes = (uint64_t)call->config.entry_count * sizeof(*call->config.entries);
        if (entry >= first && (uint64_t)(entry - first) < bytes &&
            (entry - first) % sizeof(*call->config.entries) == 0)
            event->entry = (unsigned)((entry - first) / sizeof(*call->config.entries));
    }
}
static void cell_runtime_phase_set(unsigned phase, unsigned instance) {
    cell_runtime_phase = phase; cell_runtime_instance = instance; cell_runtime_poll = 0;
}
static bool cell_runtime_resource_status(XrXirCallStatus status) {
    return status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT;
}
static void cell_runtime_failure(CellRuntimeRun *run, unsigned pass, XrXirCallStatus status) {
    CHECK(cell_runtime_resource_status(status));
    if (run->cost.failure == XR_XIR_CALL_READY) run->cost.failure = status;
    XrXirInstance *instance = run->instances[pass];
    if (!instance) return;
    unsigned failed_phase = cell_runtime_phase;
    cell_runtime_phase_set(CELL_RT_STICKY, pass);
    XrXirValue output = {0};
    if (xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED) {
        XrXirCallResult first = {0}, second = {0};
        CHECK(xr_xir_instance_copy_failure(instance, &first) == status);
        CHECK(xr_xir_instance_copy_failure(instance, &second) == status);
        CHECK(!memcmp(&first, &second, sizeof(first)));
        CHECK(xr_xir_instance_start(instance, run->entries[CELL_ENTRY_COUNTER], NULL, 0) == status);
        CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == status);
        CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_BAD_STATE);
        xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second);
    } else if (instance->call && (failed_phase == CELL_RT_POLL || failed_phase == CELL_RT_CONTROL)) {
        CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == status);
        XrXirCallStatus completed = xr_xir_task_executor_completion_status(instance->executor);
        XrXirCallStatus expected = completed == XR_XIR_CALL_READY ? XR_XIR_CALL_BAD_STATE : completed;
        CHECK(xr_xir_instance_take_result(instance, &output) == expected);
    }
    CHECK(!output.type && !output.reserved && !output.payload);
}
static XrXirCallStatus cell_runtime_start(CellRuntimeRun *run, unsigned pass, uint32_t entry, unsigned phase) {
    XrXirInstance *instance = run->instances[pass];
    XrXirDomain *domain = instance->domain;
    XrXirInstanceState state = xr_xir_instance_state(instance);
    uint64_t epoch = instance->epoch;
    XrXirInstanceConfig config = instance->config;
    size_t physical_blocks = runtime_live, physical_bytes = runtime_bytes;
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
    bool preparation = pass == 0 && !run->cost.prepare_end;
    if (preparation) {
        run->cost.prepare_begin = runtime_attempts;
        run->cost.prepare_before = before;
    }
    cell_runtime_phase_set(phase, pass);
    XrXirCallStatus status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (cell_runtime_resource_status(status)) {
        CHECK(instance->domain == domain && instance->budget.work_domain == domain);
        CHECK(xr_xir_instance_state(instance) == state && instance->epoch == epoch);
        CHECK(!memcmp(&config, &instance->config, sizeof(config)));
        CHECK(runtime_live == physical_blocks && runtime_bytes == physical_bytes);
        XrXirDomainBudgetStats failed = xr_xir_domain_budget_stats(domain);
        CHECK(failed.requested_call_bytes >= before.requested_call_bytes && failed.work >= before.work);
        CHECK(failed.call_live == before.call_live && failed.metadata_live == before.metadata_live);
        if (preparation) run->cost.prepare_failed = failed;
        if (status == XR_XIR_CALL_OOM && preparation && run->allow_retry && !run->cost.recovered) {
            CHECK(state == XR_XIR_INSTANCE_NEW && !epoch && cell_runtime_hits == 1);
            run->cost.recovered = true;
            cell_runtime_phase_set(CELL_RT_RETRY, pass);
            status = xr_xir_instance_start(instance, entry, NULL, 0);
            CHECK(instance->domain == domain && instance->budget.work_domain == domain);
            CHECK(!memcmp(&config, &instance->config, sizeof(config)));
            XrXirDomainBudgetStats retried = xr_xir_domain_budget_stats(domain);
            CHECK(retried.requested_call_bytes >= failed.requested_call_bytes && retried.work >= failed.work);
            if (cell_runtime_resource_status(status)) {
                CHECK(xr_xir_instance_state(instance) == state && instance->epoch == epoch);
                CHECK(runtime_live == physical_blocks && runtime_bytes == physical_bytes);
            }
        }
    }
    if (preparation) {
        run->cost.prepare_end = runtime_attempts;
        run->cost.prepare_after = xr_xir_domain_budget_stats(domain);
    }
    return status;
}
static XrXirCallStatus cell_runtime_run_entry(CellRuntimeRun *run, unsigned pass, uint32_t entry,
    XrXirValue *output, unsigned phase) {
    CHECK(output && !output->type && !output->reserved && !output->payload);
    XrXirCallStatus status = cell_runtime_start(run, pass, entry, phase);
    if (status == XR_XIR_CALL_READY) {
        cell_runtime_phase_set(CELL_RT_POLL, pass);
        status = xr_xir_instance_poll_bounded(run->instances[pass], UINT64_MAX).outcome.status;
    }
    if (status == XR_XIR_CALL_RETURNED || status == XR_XIR_CALL_THROWN) {
        cell_runtime_phase_set(CELL_RT_RESULT, pass);
        CHECK(xr_xir_instance_take_result(run->instances[pass], output) == status);
        CHECK(xr_xir_value_valid(output));
    } else if (cell_runtime_resource_status(status)) cell_runtime_failure(run, pass, status);
    return status;
}
static bool cell_runtime_other(CellRuntimeRun *run) {
    CHECK(!run->other_done);
    XrXirCallStatus status = cell_runtime_run_entry(run, 1, run->entries[CELL_ENTRY_RUN],
        &run->other_result, CELL_RT_START);
    if (cell_runtime_resource_status(status)) return false;
    CHECK(status == XR_XIR_CALL_RETURNED); run->other_result_valid = true;
    cell_life_number(&run->other_result, 2);
    status = cell_runtime_run_entry(run, 1, run->entries[CELL_ENTRY_COUNTER],
        &run->other_counter, CELL_RT_GETTER);
    if (cell_runtime_resource_status(status)) return false;
    CHECK(status == XR_XIR_CALL_RETURNED); run->other_counter_valid = true;
    cell_life_number(&run->other_counter, 2); run->other_done = true;
    return true;
}
static XrXirOutputStatus cell_runtime_output(void *context, const XrXirOutputGroup *group) {
    CellRuntimeRun *run = context;
    CHECK(group && group->line && group->stream == XR_XIR_STDOUT && group->count == 1);
    CHECK(xr_xir_value_valid(&group->values[0]) && group->values[0].type == XR_XIR_I64);
    CHECK(!group->values[0].reserved && run->cost.outputs < 3);
    run->cost.lines[run->cost.outputs++] = group->values[0].payload;
    if (run->mode == CELL_LIFE_REENTRANT && !run->cost.reentered) {
        ++run->cost.reentered;
        XrXirInstance *active = run->instances[0];
        CHECK(active && run->instances[1]);
        CHECK(xr_xir_instance_start(active, run->entries[CELL_ENTRY_COUNTER], NULL, 0) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_instance_poll_bounded(active, UINT64_MAX).outcome.status == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_instance_cancel_current(active) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_instance_free(active) == XR_XIR_CALL_BUSY);
        if (!cell_runtime_other(run)) {
            cell_runtime_phase_set(CELL_RT_POLL, 0);
            return run->cost.failure == XR_XIR_CALL_OOM ? XR_XIR_OUTPUT_OOM : XR_XIR_OUTPUT_LIMIT;
        }
        cell_runtime_phase_set(CELL_RT_POLL, 0);
    }
    return XR_XIR_OUTPUT_OK;
}
static void cell_runtime_output_oracle(const CellRuntimeRun *run) {
    const int64_t thrown[] = {11}, resumed[] = {1,13,117}, cancelled[] = {1,11,111}, early[] = {100};
    if (run->cost.failure == XR_XIR_CALL_READY) {
        unsigned count = run->mode == CELL_LIFE_RETURN ? 0 : run->mode < CELL_LIFE_RESUME ? 1 : 3;
        const int64_t *expected = run->mode < CELL_LIFE_RESUME ? thrown :
            run->mode == CELL_LIFE_RESUME || run->mode == CELL_LIFE_REENTRANT ? resumed : cancelled;
        CHECK(run->cost.outputs == count);
        for (unsigned i=0;i<count;++i) CHECK(run->cost.lines[i] == expected[i]);
        CHECK(run->cost.reentered == (unsigned)(run->mode == CELL_LIFE_REENTRANT));
        return;
    }
    if (run->mode == CELL_LIFE_RETURN) CHECK(!run->cost.outputs);
    else if (run->mode < CELL_LIFE_RESUME) {
        CHECK(run->cost.outputs <= 1);
        if (run->cost.outputs) CHECK(run->cost.lines[0] == 11);
    } else {
        /* Failure before paused() enters can execute only bridge's +100 EXIT.
         * Otherwise resource EXIT follows a prefix of cancel or normal resume. */
        bool cancel = run->cost.outputs <= 3, resume = run->cost.outputs <= 3;
        for (unsigned i=0;i<run->cost.outputs;++i) {
            if (run->cost.lines[i] != cancelled[i]) cancel = false;
            if (run->cost.lines[i] != resumed[i]) resume = false;
        }
        bool before_paused = run->cost.outputs == 1 && run->cost.lines[0] == early[0];
        CHECK(cancel || before_paused || ((run->mode == CELL_LIFE_RESUME || run->mode == CELL_LIFE_REENTRANT) && resume));
    }
}
static bool cell_runtime_number_entry(CellRuntimeRun *run, uint32_t entry, int64_t number, unsigned phase) {
    XrXirValue output = {0};
    XrXirCallStatus status = cell_runtime_run_entry(run, 0, entry, &output, phase);
    if (cell_runtime_resource_status(status)) { CHECK(!output.type && !output.payload); return false; }
    CHECK(status == XR_XIR_CALL_RETURNED); cell_life_number(&output, number); xr_xir_value_drop(&output);
    return true;
}
static CellRuntimeCost cell_runtime_operation(XrXirProgram *program, const uint32_t *entries,
    unsigned mode, const XrXirInstanceConfig *limits, unsigned limit_pass, bool retry) {
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
    size_t compiler_blocks = instance_compile_live, compiler_bytes = instance_compile_bytes;
    CellRuntimeRun run = {0}; run.mode = mode; run.entries = entries; run.allow_retry = retry;
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    cell_runtime_fault_at = cell_runtime_hits = cell_runtime_event_count = 0;
    cell_runtime_instances = run.instances; cell_runtime_recording = true;
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned pass=0;pass<2;++pass) {
        XrXirInstanceConfig selected = limits && pass == limit_pass ? *limits : config;
        if (!pass) selected.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,cell_runtime_output,&run};
        cell_runtime_phase_set(CELL_RT_CREATE, pass);
        XrXirCallStatus status = xr_xir_instance_new(program, &selected, &run.instances[pass]);
        if (cell_runtime_resource_status(status)) { CHECK(!run.instances[pass]); cell_runtime_failure(&run, pass, status); goto finish; }
        CHECK(status == XR_XIR_CALL_READY && run.instances[pass]);
        run.domains[pass] = run.instances[pass]->domain;
        CHECK(xr_xir_domain_retain(run.domains[pass]));
    }
    CHECK(run.domains[0] != run.domains[1] && run.instances[0]->program == program && run.instances[1]->program == program);
    if (mode != CELL_LIFE_REENTRANT && !cell_runtime_other(&run)) goto finish;
    if (mode < CELL_LIFE_RESUME) {
        uint32_t entry = mode == CELL_LIFE_RETURN ? CELL_ENTRY_RUN : mode == CELL_LIFE_THROW ? CELL_ENTRY_THROW :
            mode == CELL_LIFE_PANIC ? CELL_ENTRY_PANIC : CELL_ENTRY_UNCAUGHT;
        XrXirCallStatus status = cell_runtime_run_entry(&run, 0, entries[entry], &run.result, CELL_RT_START);
        if (cell_runtime_resource_status(status)) goto finish;
        CHECK(status == (mode == CELL_LIFE_UNCAUGHT ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED));
        run.result_valid = true;
        if (!cell_runtime_number_entry(&run, entries[CELL_ENTRY_COUNTER],
            mode == CELL_LIFE_RETURN ? 2 : mode == CELL_LIFE_UNCAUGHT ? 11 : 12, CELL_RT_GETTER)) goto finish;
    } else {
        XrXirCallStatus status = cell_runtime_start(&run, 0, entries[CELL_ENTRY_SUSPENDED], CELL_RT_START);
        if (cell_runtime_resource_status(status)) { cell_runtime_failure(&run, 0, status); goto finish; }
        CHECK(status == XR_XIR_CALL_READY);
        cell_runtime_phase_set(CELL_RT_POLL, 0);
        XrXirInstanceResult step = xr_xir_instance_poll_bounded(run.instances[0], UINT64_MAX);
        if (cell_runtime_resource_status(step.outcome.status)) { cell_runtime_failure(&run, 0, step.outcome.status); goto finish; }
        CHECK(step.outcome.status == XR_XIR_CALL_SUSPENDED);
        CHECK(xr_xir_instance_start(run.instances[0], entries[CELL_ENTRY_COUNTER], NULL, 0) == XR_XIR_CALL_BUSY);
        cell_runtime_phase_set(CELL_RT_CONTROL, 0);
        if (mode == CELL_LIFE_CLOSE) {
            cell_runtime_phase_set(CELL_RT_CLOSE, 0);
            status = xr_xir_instance_free(run.instances[0]); run.instances[0] = NULL;
            if (cell_runtime_resource_status(status)) { cell_runtime_failure(&run, 0, status); goto finish; }
            CHECK(status == XR_XIR_CALL_READY);
        } else if (mode == CELL_LIFE_STOP) {
            status = xr_xir_instance_stop(run.instances[0]);
            if (cell_runtime_resource_status(status)) { cell_runtime_failure(&run, 0, status); goto finish; }
            CHECK(status == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(run.instances[0], UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
            size_t attempts = runtime_attempts;
            CHECK(xr_xir_instance_start(run.instances[0], entries[CELL_ENTRY_COUNTER], NULL, 0) == XR_XIR_CALL_BAD_STATE);
            CHECK(runtime_attempts == attempts);
        } else {
            bool cancel = mode == CELL_LIFE_CANCEL;
            size_t attempts = runtime_attempts;
            status = cancel ? xr_xir_instance_cancel_current(run.instances[0]) :
                xr_xir_instance_resume(run.instances[0], step.epoch, step.outcome.wake);
            if (cell_runtime_resource_status(status)) { cell_runtime_failure(&run, 0, status); goto finish; }
            CHECK(status == (cancel ? XR_XIR_CALL_CANCEL_REQUESTED : XR_XIR_CALL_READY));
            if (cancel) CHECK(runtime_attempts == attempts);
            cell_runtime_phase_set(CELL_RT_POLL, 0);
            status = xr_xir_instance_poll_bounded(run.instances[0], UINT64_MAX).outcome.status;
            if (cell_runtime_resource_status(status)) { cell_runtime_failure(&run, 0, status); goto finish; }
            CHECK(status == (cancel ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
            if (!cancel) {
                cell_runtime_phase_set(CELL_RT_RESULT, 0);
                CHECK(xr_xir_instance_take_result(run.instances[0], &run.result) == XR_XIR_CALL_RETURNED);
                run.result_valid = true;
            }
            if (!cell_runtime_number_entry(&run, entries[CELL_ENTRY_COUNTER], cancel ? 111 : 117, CELL_RT_GETTER)) goto finish;
            if (!cell_runtime_number_entry(&run, entries[CELL_ENTRY_RUN], cancel ? 113 : 119, CELL_RT_NEXT)) goto finish;
        }
    }
    run.cost.complete = true;
 finish:
    for (unsigned pass=0;pass<2;++pass) if (run.instances[pass]) {
        cell_runtime_phase_set(CELL_RT_CLOSE, pass);
        XrXirCallStatus status = xr_xir_instance_free(run.instances[pass]); run.instances[pass] = NULL;
        if (cell_runtime_resource_status(status)) cell_runtime_failure(&run, pass, status);
        else CHECK(status == XR_XIR_CALL_READY);
    }
    /* The original ordinary gate separately drops the last Program before close.
     * This suite keeps one immutable Program lease for all actual FI traversals. */
    if (run.result_valid) {
        if (mode == CELL_LIFE_UNCAUGHT) cell_life_problem(&run.result);
        else cell_life_number(&run.result, mode == CELL_LIFE_RETURN ? 2 : mode < CELL_LIFE_RESUME ? 12 : 3);
    } else CHECK(!run.result.type && !run.result.reserved && !run.result.payload);
    if (run.other_result_valid) cell_life_number(&run.other_result, 2);
    if (run.other_counter_valid) cell_life_number(&run.other_counter, 2);
    cell_runtime_output_oracle(&run);
    cell_runtime_phase_set(CELL_RT_DROP, 0);
    xr_xir_value_drop(&run.result); xr_xir_value_drop(&run.other_result); xr_xir_value_drop(&run.other_counter);
    for (unsigned pass=0;pass<2;++pass) if (run.domains[pass]) {
        run.cost.budgets[pass] = xr_xir_domain_budget_stats(run.domains[pass]);
        run.cost.values[pass] = xr_xir_domain_stats(run.domains[pass]);
        CHECK(run.cost.budgets[pass].bound && run.cost.budgets[pass].work <= run.cost.budgets[pass].work_limit);
        CHECK(!run.cost.budgets[pass].call_live && !run.cost.budgets[pass].metadata_live);
        CHECK(run.cost.values[pass].live_bytes == sizeof(XrXirDomain));
        xr_xir_domain_drop(run.domains[pass]); run.domains[pass] = NULL;
    }
    run.cost.sites = runtime_attempts; run.cost.hits = cell_runtime_hits;
    CHECK(cell_runtime_event_count == run.cost.sites);
    cell_runtime_recording = false; cell_runtime_instances = NULL;
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
    CHECK(instance_compile_live == compiler_blocks && instance_compile_bytes == compiler_bytes);
    return run.cost;
}
static bool cell_runtime_same_event(const CellRuntimeAllocationEvent *a, const CellRuntimeAllocationEvent *b) {
    return a->ordinal == b->ordinal && a->bytes == b->bytes && a->line == b->line && a->kind == b->kind &&
        a->phase == b->phase && a->instance == b->instance && a->role == b->role && a->entry == b->entry &&
        !strcmp(a->file, b->file);
}
static uint64_t cell_runtime_fi_runs;
static void cell_runtime_fault_tree(XrXirProgram *program, const uint32_t *entries, unsigned mode,
    size_t depth, bool retry, const CellRuntimeAllocationEvent *expected) {
    cell_runtime_fault_count = depth;
    CellRuntimeCost cost = cell_runtime_operation(program, entries, mode, NULL, 0, retry);
    CHECK(cost.hits == depth);
    if (depth) {
        CHECK(cell_runtime_same_event(&cell_runtime_events[cell_runtime_faults[depth-1]], expected));
        CHECK(cost.failure == XR_XIR_CALL_OOM || (depth == 1 && retry && cost.recovered && cost.complete && cost.failure == XR_XIR_CALL_READY));
        ++cell_runtime_fi_runs;
    } else CHECK(cost.complete && cost.failure == XR_XIR_CALL_READY);
    CellRuntimeAllocationEvent *events = cell_runtime_observer_new(cost.sites * sizeof(*events));
    memcpy(events, cell_runtime_events, cost.sites * sizeof(*events));
    printf("CELL_RUNTIME_BRANCH mode=%u depth=%zu sites=%zu hits=%zu failure=%u recovered=%u physical=0/0\n",
        mode, depth, cost.sites, cost.hits, cost.failure, (unsigned)cost.recovered);
    size_t start = depth ? cell_runtime_faults[depth-1] + 1 : 0;
    for (size_t ordinal=start;ordinal<cost.sites;++ordinal) {
        CHECK(depth < 32768); cell_runtime_faults[depth] = ordinal;
        printf("CELL_RUNTIME_SITE mode=%u depth=%zu ordinal=%zu bytes=%zu kind=%u phase=%u instance=%u role=%u entry=%u file=%s line=%u\n",
            mode, depth+1, ordinal, events[ordinal].bytes, events[ordinal].kind, events[ordinal].phase,
            events[ordinal].instance, events[ordinal].role, events[ordinal].entry, events[ordinal].file, events[ordinal].line);
        cell_runtime_fault_tree(program, entries, mode, depth+1, retry, &events[ordinal]);
    }
    cell_runtime_observer_free(events);
}
static void cell_runtime_call_other_limits(const CellRuntimeCost *cost,
    const XrXirInstanceConfig *config) {
    for (unsigned pass=0;pass<2;++pass) {
        const XrXirDomainBudgetStats *b=&cost->budgets[pass];
        CHECK(b->bound && b->requested_bytes < b->requested_limit &&
            b->requested_call_bytes < b->requested_call_limit && b->work < b->work_limit);
        CHECK(b->metadata_peak < b->metadata_limit && cost->values[pass].peak_bytes < config->value_limit);
    }
}
/* A frame segment may use less than its normal reserve when a live cap is
 * tight. Measure complete executions until exact and minus-one separate. */
static uint64_t cell_runtime_call_boundary(XrXirProgram *program, const uint32_t *entries,
    unsigned mode, unsigned pass, const CellRuntimeCost *baseline) {
    uint64_t exact = baseline->budgets[pass].call_peak;
    CHECK(exact > 1);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.call_limit = exact;
    cell_runtime_fault_count = 0;
    CellRuntimeCost measured = cell_runtime_operation(program, entries, mode, &config, pass, false);
    CHECK(measured.complete && measured.failure == XR_XIR_CALL_READY && !measured.hits);
    cell_runtime_call_other_limits(&measured, &config);
    CHECK(measured.budgets[pass].call_limit == exact && measured.budgets[pass].call_peak <= exact);
    exact = measured.budgets[pass].call_peak;
    CHECK((baseline->budgets[pass].call_peak - exact) % XR_XIR_CALL_STATE_ALIGNMENT == 0);
    /* These complete fixtures keep their Call table size. Every smaller
     * frame segment changes the observed peak by at least one alignment. */
    uint64_t remaining = baseline->budgets[pass].call_peak / XR_XIR_CALL_STATE_ALIGNMENT + 1;
    for (uint64_t probes = 1; remaining; ++probes, --remaining) {
        CHECK(exact > 1);
        config.call_limit = exact - 1;
        cell_runtime_fault_count = 0;
        measured = cell_runtime_operation(program, entries, mode, &config, pass, false);
        CHECK(!measured.hits && measured.budgets[pass].call_peak <= config.call_limit);
        cell_runtime_call_other_limits(&measured, &config);
        if (measured.failure == XR_XIR_CALL_LIMIT) {
            printf("CELL_RUNTIME_CALL_BOUNDARY mode=%u pass=%u original=%llu exact=%llu probes=%llu minus1=LIMIT fullRun=1 physical=0/0\n",
                mode,pass,(unsigned long long)baseline->budgets[pass].call_peak,
                (unsigned long long)exact,(unsigned long long)probes);
            return exact;
        }
        CHECK(measured.complete && measured.failure == XR_XIR_CALL_READY);
        CHECK(measured.budgets[pass].call_limit == config.call_limit &&
            measured.budgets[pass].call_peak > 1 && measured.budgets[pass].call_peak < exact);
        CHECK((exact - measured.budgets[pass].call_peak) % XR_XIR_CALL_STATE_ALIGNMENT == 0);
        exact = measured.budgets[pass].call_peak;
    }
    CHECK(false); return 0;
}
static void cell_runtime_axes(XrXirProgram *program, const uint32_t *entries, unsigned mode,
    const CellRuntimeCost *baseline) {
    /* The first three axes cover cumulative value/call/work. The remaining
     * controls independently enforce the original live value/call/metadata caps. */
    for (unsigned pass=0;pass<2;++pass) for (unsigned axis=0;axis<6;++axis) {
        uint64_t exact = axis == 0 ? baseline->budgets[pass].requested_bytes :
            axis == 1 ? baseline->budgets[pass].requested_call_bytes : axis == 2 ? baseline->budgets[pass].work :
            axis == 3 ? baseline->values[pass].peak_bytes : axis == 4 ?
                cell_runtime_call_boundary(program,entries,mode,pass,baseline) : baseline->budgets[pass].metadata_peak;
        CHECK(exact > 1);
        for (unsigned minus=0;minus<2;++minus) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        if (axis == 0) config.requested_value_limit = exact - minus;
        else if (axis == 1) config.requested_call_limit = exact - minus;
        else if (axis == 2) config.work_limit = exact - minus;
        else if (axis == 3) config.value_limit = exact - minus;
        else if (axis == 4) config.call_limit = exact - minus;
        else config.metadata_limit = exact - minus;
        cell_runtime_fault_count = 0;
        CellRuntimeCost cost = cell_runtime_operation(program, entries, mode, &config, pass, false);
        if (cost.failure != (minus ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY)) {
            fprintf(stderr,"CELL_AXIS_FIRST mode=%u pass=%u axis=%u minus=%u exact=%llu actual=%u complete=%u sites=%zu hits=%zu outputs=%u\n",
                mode,pass,axis,minus,(unsigned long long)exact,(unsigned)cost.failure,
                (unsigned)cost.complete,cost.sites,cost.hits,cost.outputs);
            for (unsigned owner=0;owner<2;++owner) {
                const XrXirDomainBudgetStats *b=&cost.budgets[owner];
                fprintf(stderr,"CELL_AXIS_OWNER pass=%u call_limit=%llu call_peak=%llu baseline_peak=%llu call_live=%llu requested=%llu work=%llu metadata_peak=%llu value_peak=%llu\n",
                    owner,(unsigned long long)b->call_limit,(unsigned long long)b->call_peak,
                    (unsigned long long)baseline->budgets[owner].call_peak,(unsigned long long)b->call_live,
                    (unsigned long long)b->requested_call_bytes,(unsigned long long)b->work,
                    (unsigned long long)b->metadata_peak,(unsigned long long)cost.values[owner].peak_bytes);
            }
            for (size_t i=0;i<cell_runtime_event_count;++i) {
                const CellRuntimeAllocationEvent *e=&cell_runtime_events[i];
                fprintf(stderr,"CELL_AXIS_ALLOC ordinal=%zu bytes=%zu kind=%u phase=%u instance=%u role=%u entry=%u injected=%u file=%s line=%u\n",
                    e->ordinal,e->bytes,e->kind,e->phase,e->instance,e->role,e->entry,
                    (unsigned)e->injected,e->file,e->line);
            }
        }
        CHECK(cost.failure == (minus ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
        CHECK(!cost.hits);
        printf("CELL_RUNTIME_AXIS mode=%u pass=%u axis=%u minus=%u exact=%llu failure=%u physical=0/0\n",
            mode, pass, axis, minus, (unsigned long long)exact, cost.failure);
        }
    }
}
static void cell_runtime_preparation(XrXirProgram *program, const uint32_t *entries, unsigned mode,
    const CellRuntimeCost *baseline, const CellRuntimeAllocationEvent *events) {
    CHECK(baseline->prepare_end > baseline->prepare_begin);
    for (size_t ordinal=baseline->prepare_begin;ordinal<baseline->prepare_end;++ordinal) {
        CHECK(events[ordinal].phase == CELL_RT_START && events[ordinal].instance == 0);
        cell_runtime_faults[0] = ordinal;
        cell_runtime_fault_tree(program, entries, mode, 1, true, &events[ordinal]);
    }
    /* Failed requests are not refunded; retry on the same finite requested-call
     * quota must fail before another physical allocator attempt. */
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.requested_call_limit = baseline->prepare_after.requested_call_bytes;
    CHECK(config.requested_call_limit && config.requested_call_limit <= UINT64_C(67108864));
    cell_runtime_faults[0] = baseline->prepare_end - 1; cell_runtime_fault_count = 1;
    CellRuntimeCost cost = cell_runtime_operation(program, entries, mode, &config, 0, true);
    CHECK(cost.hits == 1 && cost.recovered && cost.failure == XR_XIR_CALL_LIMIT);
    CHECK(cost.prepare_failed.requested_call_bytes == config.requested_call_limit);
    CHECK(cost.prepare_after.requested_call_bytes == cost.prepare_failed.requested_call_bytes);
    printf("CELL_RUNTIME_SAMEQUOTA mode=%u first=OOM retry=LIMIT owner=same quota=%llu no_refund=1 physical=0/0\n",
        mode, (unsigned long long)config.requested_call_limit);
}
static void cell_life_runtime_resources(const char *const *fixtures, size_t fixture_count, const char *operation) {
    for (size_t f=0;f<fixture_count;++f) for (unsigned mode=0;mode<CELL_LIFE_MODES;++mode) {
        cell_life_zero(); CHECK(!cell_runtime_observer_live);
        SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
        uint32_t entries[CELL_LIFE_ENTRIES];
        XrXirProgram *program = cell_life_program(&owner.context, fixtures[f], entries);
        cell_runtime_fault_count = 0;
        CellRuntimeCost baseline = cell_runtime_operation(program, entries, mode, NULL, 0, false);
        CHECK(baseline.complete && baseline.failure == XR_XIR_CALL_READY && baseline.sites && !baseline.hits);
        CellRuntimeAllocationEvent *events = cell_runtime_observer_new(baseline.sites * sizeof(*events));
        memcpy(events, cell_runtime_events, baseline.sites * sizeof(*events));
        printf("CELL_RUNTIME_BASE fixture=%s mode=%u sites=%zu prepare=%zu..%zu physical=0/0\n",
            fixtures[f], mode, baseline.sites, baseline.prepare_begin, baseline.prepare_end);
        for (unsigned pass=0;pass<2;++pass)
            printf("CELL_RUNTIME_COST fixture=%s mode=%u pass=%u value=%llu call=%llu work=%llu value_peak=%llu call_peak=%llu metadata_peak=%llu\n",
                fixtures[f], mode, pass, (unsigned long long)baseline.budgets[pass].requested_bytes,
                (unsigned long long)baseline.budgets[pass].requested_call_bytes, (unsigned long long)baseline.budgets[pass].work,
                (unsigned long long)baseline.values[pass].peak_bytes, (unsigned long long)baseline.budgets[pass].call_peak,
                (unsigned long long)baseline.budgets[pass].metadata_peak);
        if (!strcmp(operation,"--runtime-fi")) {
            cell_runtime_fi_runs = 0; cell_runtime_fault_tree(program, entries, mode, 0, false, NULL);
            printf("CELL_RUNTIME_CLOSURE fixture=%s mode=%u actual_runs=%llu every_failure_suffix=1\n",
                fixtures[f], mode, (unsigned long long)cell_runtime_fi_runs);
        } else if (!strcmp(operation,"--runtime-axes")) cell_runtime_axes(program, entries, mode, &baseline);
        else if (!strcmp(operation,"--runtime-prepare")) cell_runtime_preparation(program, entries, mode, &baseline, events);
        else CHECK(!strcmp(operation,"--runtime-normal"));
        cell_runtime_fault_count = 0; cell_runtime_observer_free(events);
        xr_xir_compile_program_drop(program); source_fixture_owner_free(&owner);
        cell_life_zero(); CHECK(!cell_runtime_observer_live);
    }
}
#endif // XIR_CELL_LIFECYCLE_RESOURCES_H
