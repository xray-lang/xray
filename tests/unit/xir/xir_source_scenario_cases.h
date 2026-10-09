/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_scenario_cases.h - Finite source scenarios with lifetime budgets
 *
 * KEY CONCEPT:
 *   Each fixed scenario owns two fully initialized, independently metered instances.
 */
#ifndef XIR_SOURCE_SCENARIO_CASES_H
#define XIR_SOURCE_SCENARIO_CASES_H

typedef struct SourceScenario {
    SourceOutput outputs[2];
    XrXirOutputSink sinks[2];
    XrXirInstanceConfig config;
    XrXirInstance *instances[2];
    XrXirDomain *domains[2];
    size_t physical_live, physical_bytes;
} SourceScenario;

static void source_scenario_open(SourceScenario *pair, XrXirProgram *program, uint32_t entry) {
    *pair = (SourceScenario){0};
    pair->physical_live = runtime_live; pair->physical_bytes = runtime_bytes;
    pair->outputs[1].reject_write = true;
    CHECK(xr_xir_instance_config_init(&pair->config, sizeof(pair->config)) == XR_XIR_CALL_READY);
    pair->config.metadata_limit = 65536; pair->config.value_limit = 65536; pair->config.call_limit = 65536;
    pair->config.poll_limit = 8000; pair->config.depth_limit = 96;
    for (uint32_t i = 0; i < 2; ++i) {
        pair->sinks[i] = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, source_bytes, &pair->outputs[i], 65536};
        pair->config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, source_output, &pair->sinks[i]};
        CHECK(xr_xir_instance_new(program, &pair->config, &pair->instances[i]) == XR_XIR_CALL_READY);
        pair->domains[i] = pair->instances[i]->domain;
        CHECK(xr_xir_domain_retain(pair->domains[i]));
        CHECK(pair->instances[i]->budget.work_domain == pair->domains[i]);
        CHECK(xr_xir_instance_start(pair->instances[i], entry, NULL, 0) == XR_XIR_CALL_READY);
    }
    CHECK(pair->instances[0] != pair->instances[1] && pair->domains[0] != pair->domains[1]);
    CHECK(&pair->instances[0]->budget != &pair->instances[1]->budget);
    for (unsigned step = 0; step < 14; ++step) for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(pair->instances[i], UINT64_MAX);
        if (step < 13) {
            uint32_t expected_calls = step >= 12 ? 53u : step >= 11 ? 50u : step >= 10 ? 49u : step >= 9 ? 48u : step >= 8 ? 47u : step >= 4 ? 40u : step >= 2 ? 25u + step : 0u;
            if (xr_xir_instance_state(pair->instances[i]) != XR_XIR_INSTANCE_INITIALIZING || pair->outputs[i].calls != expected_calls)
                fprintf(stderr,"Source init boundary step=%u instance=%u state=%u status=%u calls=%u expected=%u resumes=%llu exhausted=%u\n",
                    step,i,(unsigned)xr_xir_instance_state(pair->instances[i]),(unsigned)result.outcome.status,
                    pair->outputs[i].calls,expected_calls,(unsigned long long)pair->instances[i]->budget.resumes,
                    (unsigned)pair->instances[i]->budget.exhausted);
            CHECK(xr_xir_instance_state(pair->instances[i]) == XR_XIR_INSTANCE_INITIALIZING && pair->outputs[i].calls == expected_calls);
            source_resume_once(pair->instances[i], result);
        } else {
            if (result.outcome.status != XR_XIR_CALL_RETURNED)
                fprintf(stderr, "Source initialization status=%u outputs=%u\n", result.outcome.status, pair->outputs[i].calls);
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && result.outcome.value.type == XR_XIR_I64 && !result.outcome.value.payload);
            CHECK(pair->outputs[i].calls == 59 && xr_xir_instance_state(pair->instances[i]) == XR_XIR_INSTANCE_READY);
        }
    }
}

/* All four physical-allocation test TUs include the actual private owner.
 * These observations add no public API, substitute structure, or fresh ledger. */
static void source_scenario_owner_report(const SourceScenario *pair, const char *name,
    unsigned begin, unsigned end, unsigned i) {
    const XrXirInstance *instance = pair->instances[i];
    const XrXirCallBudget *budget = &instance->budget;
    const XrXirDomainBudgetStats stats = xr_xir_domain_budget_stats(pair->domains[i]);
    CHECK(instance->domain == pair->domains[i] && budget->work_domain == pair->domains[i]);
    CHECK(instance->config.metadata_limit == 65536 && instance->config.value_limit == 65536 &&
        instance->config.call_limit == 65536 && instance->config.poll_limit == 8000 && instance->config.depth_limit == 96);
    CHECK(budget->resume_limit == 8000 && budget->resumes <= 8000 && !budget->exhausted);
    CHECK(pair->outputs[i].calls == 59 && !instance->stopping && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    printf("Source scenario %s [%u,%u) instance%u resumes=%llu/8000 epoch=%llu callRequested=%llu valueRequested=%llu work=%llu metadata=%llu callLive=%llu\n",
        name, begin, end, i, (unsigned long long)budget->resumes, (unsigned long long)instance->epoch,
        (unsigned long long)stats.requested_call_bytes, (unsigned long long)stats.requested_bytes,
        (unsigned long long)stats.work, (unsigned long long)stats.metadata_live, (unsigned long long)stats.call_live);
}
static void source_scenario_report(const SourceScenario *pair, const char *name, unsigned begin, unsigned end) {
    for (unsigned i = 0; i < 2; ++i) source_scenario_owner_report(pair, name, begin, end, i);
}
static void source_scenario_domain_only(XrXirDomain *domain) {
    const XrXirDomainBudgetStats ledger = xr_xir_domain_budget_stats(domain);
    const XrXirDomainStats physical = xr_xir_domain_stats(domain);
    CHECK(!ledger.metadata_live && !ledger.call_live);
    CHECK(physical.live_bytes == sizeof(XrXirDomain) && physical.allocations == physical.frees + 1);
}
static void source_scenario_close(SourceScenario *pair, const char *name, unsigned begin, unsigned end) {
    source_scenario_report(pair, name, begin, end);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_stop(pair->instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(pair->instances[i]) == XR_XIR_CALL_READY);
        source_scenario_domain_only(pair->domains[i]);
        xr_xir_domain_drop(pair->domains[i]);
    }
    CHECK(runtime_live == pair->physical_live && runtime_bytes == pair->physical_bytes);
}

static void source_scenario_b(XrXirProgram *program, uint32_t entry, SourceFunctions functions) {
    SourceScenario pair; source_scenario_open(&pair, program, entry);
    for (unsigned i = 0; i < 2; ++i) source_default_fault(pair.instances[i], functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_constructor_fault(pair.instances[i], functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_compound_fault(pair.instances[i], functions.calculate);
    source_scenario_close(&pair, "B-default-ctor-compound", 0, 0);
}
static void source_scenario_c(XrXirProgram *program, uint32_t entry, SourceFunctions functions) {
    SourceScenario pair; source_scenario_open(&pair, program, entry);
    for (unsigned i = 0; i < 2; ++i) source_float_fault(pair.instances[i], functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_updates(pair.instances[i], functions.update);
    source_scenario_close(&pair, "C-float-updates", 0, 0);
}
static void source_scenario_matrices(XrXirProgram *program, uint32_t entry, uint32_t calculate) {
    const unsigned totals[] = {174, (unsigned)(sizeof(bitwise_cases) / sizeof(bitwise_cases[0])) + 6};
    for (unsigned family = 0; family < 2; ++family) for (unsigned begin = 0; begin < totals[family]; begin += 16) {
        const unsigned end = totals[family] - begin < 16 ? totals[family] : begin + 16;
        SourceScenario pair; source_scenario_open(&pair, program, entry);
        for (unsigned i = 0; i < 2; ++i) {
            if (!family) source_numeric_range(pair.instances[i], calculate, begin, end);
            else source_bitwise_range(pair.instances[i], calculate, begin, end);
        }
        source_scenario_close(&pair, family ? "bitwise" : "numeric", begin, end);
    }
}

/* A finite stress prefix deliberately mixes two legal families in the same
 * owner. Success always has an independent literal value; the only permitted
 * failure is cumulative LIMIT at exactly 8000 actual resumes. */
static void source_scenario_exhaust(SourceScenario *pair, unsigned i, uint32_t calculate) {
    XrXirInstance *instance = pair->instances[i], *peer = pair->instances[1 - i];
    bool limited = false;
    for (unsigned attempt = 0; attempt < 8001; ++attempt) {
        const XrXirCallBudget peer_before = peer->budget;
        const XrXirDomainBudgetStats peer_ledger = xr_xir_domain_budget_stats(peer->domain);
        const bool bitwise = attempt % 2 == 0;
        XrXirValue args[] = {{XR_XIR_I64, 0, bitwise ? 12 : 0},
            {XR_XIR_I64, 0, bitwise ? INT64_MIN : INT64_MAX}, {XR_XIR_I64, 0, bitwise ? -1 : 1}};
        CHECK(xr_xir_instance_start(instance, calculate, args, 3) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome;
        CHECK(peer->budget.resumes == peer_before.resumes && peer->budget.resume_limit == peer_before.resume_limit &&
            peer->budget.byte_limit == peer_before.byte_limit && peer->budget.requested_limit == peer_before.requested_limit &&
            peer->budget.requested_bytes == peer_before.requested_bytes && peer->budget.live_bytes == peer_before.live_bytes &&
            peer->budget.peak_bytes == peer_before.peak_bytes && peer->budget.allocations == peer_before.allocations &&
            peer->budget.frees == peer_before.frees && peer->budget.transitions == peer_before.transitions &&
            peer->budget.release_tickets == peer_before.release_tickets && peer->budget.released_frames == peer_before.released_frames &&
            peer->budget.exhausted == peer_before.exhausted && peer->budget.work_domain == peer_before.work_domain);
        const XrXirDomainBudgetStats peer_after = xr_xir_domain_budget_stats(peer->domain);
        CHECK(peer_after.requested_bytes == peer_ledger.requested_bytes &&
            peer_after.requested_call_bytes == peer_ledger.requested_call_bytes && peer_after.work == peer_ledger.work &&
            peer_after.metadata_live == peer_ledger.metadata_live && peer_after.call_live == peer_ledger.call_live);
        CHECK(args[1].payload == (bitwise ? INT64_MIN : INT64_MAX) && args[2].payload == (bitwise ? -1 : 1));
        if (result.status == XR_XIR_CALL_RETURNED) {
            CHECK(result.value.type == XR_XIR_I64 && !result.value.reserved && result.value.payload == INT64_MIN);
            CHECK(instance->budget.resumes <= 8000 && !instance->budget.exhausted);
            continue;
        }
        CHECK(result.status == XR_XIR_CALL_LIMIT && result.value.type == XR_XIR_UNIT &&
            !result.value.reserved && !result.value.payload);
        CHECK(instance->budget.resumes == 8000 && instance->budget.resume_limit == 8000 && instance->budget.exhausted);
        CHECK(instance->state == XR_XIR_INSTANCE_READY && pair->outputs[i].calls == 59);
        limited = true; break;
    }
    CHECK(limited);
}
static void source_scenario_retry_exhausted(SourceScenario *pair, unsigned i, uint32_t calculate) {
    XrXirInstance *instance = pair->instances[i];
    XrXirDomain *domain = pair->domains[i]; XrXirCallBudget *owner = &instance->budget;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const XrXirCallBudget before = *owner;
        const XrXirDomainBudgetStats ledger = xr_xir_domain_budget_stats(domain);
        const XrXirDomainStats physical = xr_xir_domain_stats(domain);
        const uint64_t epoch = instance->epoch; XrXirCall *old_call = instance->call;
        XrXirValue args[] = {{XR_XIR_I64, 0, 12}, {XR_XIR_I64, 0, INT64_MIN}, {XR_XIR_I64, 0, -1}};
        CHECK(xr_xir_instance_start(instance, calculate, args, 3) == XR_XIR_CALL_BAD_STATE);
        CHECK(&instance->budget == owner && instance->domain == domain && owner->work_domain == domain);
        CHECK(instance->epoch == epoch && instance->call == old_call && owner->resumes == 8000 && owner->exhausted);
        CHECK(owner->resume_limit == before.resume_limit && owner->byte_limit == before.byte_limit &&
            owner->requested_limit == before.requested_limit && owner->requested_bytes >= before.requested_bytes);
        CHECK(owner->transitions >= before.transitions && owner->allocations >= before.allocations &&
            owner->frees >= before.frees && owner->live_bytes == before.live_bytes);
        const XrXirDomainBudgetStats retry = xr_xir_domain_budget_stats(domain);
        CHECK(retry.requested_bytes >= ledger.requested_bytes && retry.requested_call_bytes >= ledger.requested_call_bytes &&
            retry.work >= ledger.work && retry.work_limit == ledger.work_limit && retry.requested_limit == ledger.requested_limit &&
            retry.requested_call_limit == ledger.requested_call_limit && retry.call_live == ledger.call_live && retry.metadata_live == ledger.metadata_live);
        CHECK(xr_xir_domain_stats(domain).live_bytes == physical.live_bytes);
        printf("Source cumulative limit instance%u retry%u resumes=8000/8000 epoch=%llu retry=BAD_STATE callRequested=%llu->%llu work=%llu->%llu same-owner\n",
            i, attempt, (unsigned long long)epoch, (unsigned long long)ledger.requested_call_bytes,
            (unsigned long long)retry.requested_call_bytes, (unsigned long long)ledger.work, (unsigned long long)retry.work);
    }
    XrXirValue output = {0}, sentinel = output;
    CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_LIMIT && !memcmp(&output, &sentinel, sizeof(output)));
    output = (XrXirValue){XR_XIR_I64, 0, 77}; sentinel = output;
    CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_LIMIT && !memcmp(&output, &sentinel, sizeof(output)));
    XrXirCallResult failure = {0}, failure_before = failure;
    CHECK(xr_xir_instance_copy_failure(instance, &failure) == XR_XIR_CALL_BAD_STATE && !memcmp(&failure, &failure_before, sizeof(failure)));
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_LIMIT);

}
static void source_scenario_cumulative(XrXirProgram *program, uint32_t entry, SourceFunctions functions) {
    SourceScenario pair; source_scenario_open(&pair, program, entry);
    XrXirValue held[2] = {{0}, {0}}, alias[2] = {{0}, {0}};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(pair.instances[i], functions.result, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(source_drive(pair.instances[i], 0).status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(pair.instances[i], &held[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_value_copy(&held[i], &alias[i]) == XR_XIR_VALUE_OK);
    }
    for (unsigned i = 0; i < 2; ++i) {
        source_scenario_exhaust(&pair, i, functions.calculate);
        source_scenario_retry_exhausted(&pair, i, functions.calculate);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(pair.domains[i]);
        CHECK(xr_xir_instance_stop(pair.instances[i]) == XR_XIR_CALL_LIMIT);
        CHECK(xr_xir_instance_start(pair.instances[i], entry, NULL, 0) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_free(pair.instances[i]) == XR_XIR_CALL_LIMIT);
        const XrXirDomainBudgetStats released = xr_xir_domain_budget_stats(pair.domains[i]);
        CHECK(!released.metadata_live && !released.call_live && released.requested_bytes == spent.requested_bytes &&
            released.requested_call_bytes == spent.requested_call_bytes && released.work >= spent.work);
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&held[i], &bytes, &length) && length == 2 && !memcmp(bytes, "a!", 2));
        xr_xir_value_drop(&held[i]);
        CHECK(xr_xir_string_view(&alias[i], &bytes, &length) && length == 2 && !memcmp(bytes, "a!", 2));
        xr_xir_value_drop(&alias[i]);
        source_scenario_domain_only(pair.domains[i]); xr_xir_domain_drop(pair.domains[i]);
    }
    CHECK(runtime_live == pair.physical_live && runtime_bytes == pair.physical_bytes);
    printf("Source cumulative dual-instance limit stop/free aliases released to captured physical baseline\n");
}

static void source_scenario_a(XrXirProgram *program, uint32_t entry, SourceFunctions functions, XrXirValue *results) {
    SourceScenario pair; source_scenario_open(&pair, program, entry);
    SourceOutput *outputs = pair.outputs; XrXirInstance **instances = pair.instances;
    XrXirInstanceConfig config = pair.config;
    XrXirValue errors[2]={{0},{0}};
    for (unsigned i=0;i<2;++i) errors[i]=source_error(instances[i],functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_match_faults(instances[i],functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_search_bounds(instances[i], functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_pattern_calls(instances[i], functions.calculate);
    source_cancel_cases(program, entry, functions.resume_text);
    source_numeric_pair(instances, functions.numeric_pause);
    XrXirValue resumed[2] = {{0}, {0}};
    source_resume_pair(instances, functions.resume_text, resumed);
    XrXirValue deep[2] = {{0}, {0}};
    source_deep_pair(instances, functions.stack_depth, deep);
    source_failed_init(program, entry, config);
    for (int64_t expected = 15; expected < 17; ++expected) for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], functions.advance, NULL, 0) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == expected && outputs[i].calls == 59);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], functions.witness_result, NULL, 0) == XR_XIR_CALL_READY);
        XrXirCallResult witness = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome;
        CHECK(witness.status == XR_XIR_CALL_RETURNED && witness.value.payload == 41);
    }
    uint32_t enum_entries[] = {functions.enum_witness_result,functions.enum_generic_witness_result};
    for (uint32_t f = 0; f < 2; ++f) for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i],enum_entries[f],NULL,0) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_I64 && result.value.payload == 41);
    }
    XrXirValue generic_results[2][3] = {{{0}}};
    uint32_t generic_entries[] = {functions.generic_method_number,functions.generic_method_text,functions.generic_method_array};
    for (uint32_t f = 0; f < 3; ++f) for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i],generic_entries[f],NULL,0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i],&generic_results[i][f]) == XR_XIR_CALL_RETURNED);
    }
    XrXirValue bound[2] = {{0}, {0}}, bound_text[2] = {{0}, {0}};
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], functions.bound_result, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &bound[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_start_function(instances[1 - i], &bound[i], NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start_function(instances[i], &bound[i], NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &bound_text[i]) == XR_XIR_CALL_RETURNED);
    }
    source_scenario_report(&pair, "A", 0, 0);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], functions.result, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &results[i]) == XR_XIR_CALL_RETURNED);
        source_scenario_owner_report(&pair, "A-final", 0, 0, i);
        CHECK(xr_xir_instance_stop(instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start_function(instances[i], &bound[i], NULL, 0) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        for (uint32_t f = 0; f < 3; ++f) source_generic_result_drop(&generic_results[i][f],f);
        source_error_drop(&errors[i]);
        XrXirValue copy = {0};
        CHECK(xr_xir_value_copy(&bound[i], &copy) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&bound[i]); xr_xir_value_drop(&copy);
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&bound_text[i], &bytes, &length) && length == 5 && !memcmp(bytes, "bound", 5));
        xr_xir_value_drop(&bound_text[i]);
        CHECK(xr_xir_string_view(&resumed[i], &bytes, &length) && length == 3 && !memcmp(bytes, i ? "ry!" : "rn!", 3));
        xr_xir_value_drop(&resumed[i]);
        CHECK(xr_xir_string_view(&deep[i], &bytes, &length) && length == 130 + i * 2);
        for (size_t at = 0; at < length; ++at) CHECK(bytes[at] == (at % 2 ? 'r' : 'f'));
        xr_xir_value_drop(&deep[i]);
        XrXirDomainBudgetStats released = xr_xir_domain_budget_stats(pair.domains[i]);
        CHECK(!released.metadata_live && !released.call_live);
        xr_xir_domain_drop(pair.domains[i]);
    }
}

static void source_pair(XrXirProgram *program, uint32_t entry, SourceFunctions functions, XrXirValue *results) {
    source_scenario_a(program, entry, functions, results);
    source_scenario_b(program, entry, functions);
    source_scenario_c(program, entry, functions);
    source_scenario_matrices(program, entry, functions.calculate);
    source_scenario_cumulative(program, entry, functions);
}
#endif // XIR_SOURCE_SCENARIO_CASES_H
