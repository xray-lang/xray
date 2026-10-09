/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_owned_string_source_resources.c - Owned Source results after code destruction
 *
 * KEY CONCEPT:
 *   Observe real compiler and runtime costs; numerical oracles arrive independently.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "C2 FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_owned_string_source_resource_observer.h"
#include "xir_owned_string_source_resources.h"

static void cs_runtime_step(CsMetrics *m, unsigned i, unsigned phase, size_t begin) {
    CHECK(i < CS_INSTANCES && phase < CS_RUNTIME_PHASES && runtime_attempts >= begin);
    m->sites[i][phase] = runtime_attempts - begin;
}

static void cs_new_pair(CsFixture *f, CsMetrics *m, XrXirInstance *instances[CS_INSTANCES],
                        XrXirDomain *domains[CS_INSTANCES], CsObservation w[CS_INSTANCES]) {
    for (unsigned i = 0; i < CS_INSTANCES; ++i) {
        CsObservation *o = &w[i];
        o->module = f->module;
        o->sink = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, cs_output_write, o, sizeof(o->bytes)};
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, cs_typed_output, o};
        config.trace = cs_lifecycle;
        config.trace_context = o;
        size_t begin = runtime_attempts;
        CHECK(xr_xir_instance_new(f->program, &config, &instances[i]) == XR_XIR_CALL_READY);
        cs_runtime_step(m, i, CS_CREATE, begin);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_NEW);
        domains[i] = instances[i]->domain;
        begin = runtime_attempts;
        CHECK(xr_xir_domain_retain(domains[i]));
        CHECK(runtime_attempts == begin);
        CHECK(instances[i]->program == f->program && instances[i]->budget.work_domain == domains[i]);
        CHECK(!o->begins && !o->ready && !o->groups);
    }
    CHECK(instances[0] != instances[1] && domains[0] != domains[1]);
    CHECK(&instances[0]->budget != &instances[1]->budget);
    CHECK(instances[0]->program == instances[1]->program);
}

static XrXirInstanceResult cs_poll_returned(XrXirInstance *instance, const char *phase) {
    XrXirInstanceResult result = {0};
    unsigned polls = 0;
    do {
        result = xr_xir_instance_poll_bounded(instance, 1);
        CHECK(++polls < 32768);
    } while (result.outcome.status == XR_XIR_CALL_READY);
    if (result.outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr, "C2 phase=%s status=%u state=%u epoch=%llu\n", phase,
            result.outcome.status, xr_xir_instance_state(instance), (unsigned long long)result.epoch);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && xr_xir_call_result_valid(&result.outcome));
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY && result.epoch);
    printf("C2_POLLS phase=%s polls=%u epoch=%llu status=%u\n", phase, polls,
        (unsigned long long)result.epoch, result.outcome.status);
    return result;
}

static void cs_neutral(CsFixture *f, CsMetrics *m, unsigned i, XrXirInstance *instance,
                       CsObservation *w) {
    size_t begin = runtime_attempts;
    CHECK(xr_xir_instance_start(instance, f->main_entry, NULL, 0) == XR_XIR_CALL_READY);
    cs_runtime_step(m, i, CS_MAIN_START, begin);
    begin = runtime_attempts;
    XrXirInstanceResult result = cs_poll_returned(instance, "declared_entry");
    cs_runtime_step(m, i, CS_MAIN_BODY, begin);
    CHECK(result.outcome.value.type == XR_XIR_I64 && !result.outcome.value.reserved && !result.outcome.value.payload);
    CHECK(w->begins == 1 && w->ready == 1 && !w->groups && !w->writes);
    XrXirValue value = {0};
    begin = runtime_attempts;
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(value.type == XR_XIR_I64 && !value.reserved && !value.payload);
    xr_xir_value_drop(&value);
    cs_runtime_step(m, i, CS_MAIN_TAKE, begin);
    CHECK(!m->sites[i][CS_MAIN_TAKE] && !value.type && !value.reserved && !value.payload);
}

static void cs_take_owned(CsMetrics *m, unsigned i, XrXirInstance *instance,
                          const XrXirInstanceResult *result, XrXirValue *held) {
    CsLedger before = cs_ledger(instance);
    XrXirValue occupied = {XR_XIR_I64, 0, INT64_C(778899)};
    const XrXirValue saved = occupied;
    size_t begin = runtime_attempts;
    CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied, &saved, sizeof(occupied)));
    CHECK(xr_xir_instance_take_result(instance, held) == XR_XIR_CALL_RETURNED);
    CHECK(held->payload == result->outcome.value.payload);
    cs_expect_string(held);
    XrXirValue empty = {0};
    CHECK(xr_xir_instance_take_result(instance, &empty) == XR_XIR_CALL_BAD_STATE);
    CHECK(!empty.type && !empty.reserved && !empty.payload);
    cs_runtime_step(m, i, CS_OWNED_TAKE, begin);
    CHECK(!m->sites[i][CS_OWNED_TAKE]);
    cs_unchanged_other(instance, &before); /* Move and rejected outputs spend no fee. */
}

static void cs_owned(CsFixture *f, CsMetrics *m, unsigned i, XrXirInstance *instance,
                     CsObservation *w, XrXirValue *held, XrXirValue *alias) {
    size_t begin = runtime_attempts;
    CHECK(xr_xir_instance_start(instance, f->owned_entry, NULL, 0) == XR_XIR_CALL_READY);
    cs_runtime_step(m, i, CS_OWNED_START, begin);
    begin = runtime_attempts;
    XrXirInstanceResult result = cs_poll_returned(instance, "owned_string");
    cs_runtime_step(m, i, CS_OWNED_BODY, begin);
    cs_expect_string(&result.outcome.value);
    CHECK(w->groups == 1 && w->writes == 1 && w->length == sizeof(cs_output_bytes) - 1);
    CHECK(!memcmp(w->bytes, cs_output_bytes, w->length) && w->begins == 1 && w->ready == 1);
    CHECK(!w->published && !w->released);
    cs_take_owned(m, i, instance, &result, held);
    CsLedger before = cs_ledger(instance);
    begin = runtime_attempts;
    CHECK(xr_xir_value_copy(held, alias) == XR_XIR_VALUE_OK);
    cs_runtime_step(m, i, CS_COPY, begin);
    CHECK(!m->sites[i][CS_COPY] && alias->payload == held->payload);
    cs_expect_string(alias);
    CsLedger after = cs_ledger(instance);
    CHECK(after.domain.work == before.domain.work + 1);
    CHECK(after.domain.requested_bytes == before.domain.requested_bytes);
    CHECK(after.domain.requested_call_bytes == before.domain.requested_call_bytes);
    CHECK(after.values.live_bytes == before.values.live_bytes && after.values.allocations == before.values.allocations);
    CHECK(after.domain.metadata_live == before.domain.metadata_live && after.domain.call_live == before.domain.call_live);
    cs_record_boundary(m, i, &after);
    cs_print_ledger(i, "after_take_copy", instance->domain, &after.domain, &after.values);
    printf("C2_OWNER instance=%u instance_ptr=%p domain=%p call_budget=%p work_domain=%p program=%p compiler=%p\n",
        i, (void *)instance, (void *)instance->domain, (void *)&instance->budget,
        (void *)instance->budget.work_domain, (void *)f->program, (void *)f->compiler.resources);
}

static void cs_close_pair(CsFixture *f, CsMetrics *m, XrXirInstance *instances[CS_INSTANCES],
                          XrXirDomain *domains[CS_INSTANCES], const CsObservation w[CS_INSTANCES]) {
    for (unsigned i = 0; i < CS_INSTANCES; ++i) {
        CsLedger before = cs_ledger(instances[i]);
        size_t begin = runtime_attempts;
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        instances[i] = NULL;
        cs_runtime_step(m, i, CS_CLOSE, begin);
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(domains[i]);
        XrXirDomainStats values = xr_xir_domain_stats(domains[i]);
        CHECK(!after.metadata_live && !after.call_live);
        CHECK(after.metadata_allocations == after.metadata_frees && after.call_allocations == after.call_frees);
        CHECK(values.live_bytes && values.allocations > values.frees);
        CHECK(after.requested_bytes == before.domain.requested_bytes);
        CHECK(after.requested_call_bytes == before.domain.requested_call_bytes && after.work >= before.domain.work);
        m->fees_after_close[i][0] = after.requested_bytes;
        m->fees_after_close[i][1] = after.requested_call_bytes;
        m->fees_after_close[i][2] = after.work;
        m->boundary[i][0] = after.requested_bytes;
        m->boundary[i][1] = after.requested_call_bytes;
        m->boundary[i][2] = values.peak_bytes;
        m->boundary[i][3] = after.metadata_peak;
        m->boundary[i][4] = after.call_peak;
        m->boundary[i][5] = after.work;
        cs_print_ledger(i, "instance_released_string_alive", domains[i], &after, &values);
        CHECK(w[i].groups == 1 && w[i].writes == 1 && !w[i].released && !f->code_releases);
    }
    CHECK(!f->code_releases);
    xr_xir_compile_program_drop(f->program);
    f->program = NULL;
    CHECK(f->code_releases == 1 && !f->lowered);
    xr_compile_resources_release(f->compiler.resources);
    f->compiler.resources = NULL;
    instance_compile_zero();
    printf("C2_CODE releases=1 lowered=0 compiler_blocks=0 compiler_bytes=0 strings_alive=2\n");
}

static void cs_release_strings(CsMetrics *m, XrXirDomain *domains[CS_INSTANCES],
                               XrXirValue held[CS_INSTANCES], XrXirValue aliases[CS_INSTANCES]) {
    size_t attempts = runtime_attempts;
    runtime_fail_at = runtime_attempts; /* Retained readers and physical release must not invoke malloc. */
    for (unsigned i = 0; i < CS_INSTANCES; ++i) {
        cs_expect_string(&held[i]);
        cs_expect_string(&aliases[i]);
        XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domains[i]);
        XrXirDomainStats owned = xr_xir_domain_stats(domains[i]);
        xr_xir_value_drop(&held[i]);
        CHECK(!held[i].type && !held[i].reserved && !held[i].payload);
        cs_expect_string(&aliases[i]);
        XrXirDomainStats one = xr_xir_domain_stats(domains[i]);
        CHECK(one.live_bytes == owned.live_bytes && one.frees == owned.frees);
        size_t begin = runtime_attempts;
        xr_xir_value_drop(&aliases[i]);
        CHECK(!aliases[i].type && !aliases[i].reserved && !aliases[i].payload);
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(domains[i]);
        XrXirDomainStats empty = xr_xir_domain_stats(domains[i]);
        CHECK(empty.live_bytes == sizeof(*domains[i]) && empty.allocations == empty.frees + 1);
        CHECK(!after.metadata_live && !after.call_live);
        CHECK(after.requested_bytes == before.requested_bytes && after.requested_call_bytes == before.requested_call_bytes);
        CHECK(after.work == before.work);
        cs_print_ledger(i, "last_string_alias_released", domains[i], &after, &empty);
        xr_xir_domain_drop(domains[i]);
        domains[i] = NULL;
        cs_runtime_step(m, i, CS_LAST_ALIAS, begin);
        CHECK(!m->sites[i][CS_LAST_ALIAS]);
    }
    CHECK(runtime_attempts == attempts && !runtime_live && !runtime_bytes);
    runtime_fail_at = SIZE_MAX;
    printf("C2_PHYSICAL domains=2 last_alias_malloc=0 runtime_blocks=0 runtime_bytes=0 compiler_blocks=0 compiler_bytes=0\n");
}

static void cs_run(CsMetrics *m) {
    CHECK(!runtime_live && !runtime_bytes);
    CsFixture fixture;
    cs_build(&fixture, m);
    XrXirInstance *instances[CS_INSTANCES] = {0};
    XrXirDomain *domains[CS_INSTANCES] = {0};
    CsObservation witnesses[CS_INSTANCES] = {0};
    XrXirValue held[CS_INSTANCES] = {0}, aliases[CS_INSTANCES] = {0};
    cs_new_pair(&fixture, m, instances, domains, witnesses);
    for (unsigned i = 0; i < CS_INSTANCES; ++i) {
        unsigned other = 1 - i;
        CsLedger before = cs_ledger(instances[other]);
        cs_neutral(&fixture, m, i, instances[i], &witnesses[i]);
        cs_owned(&fixture, m, i, instances[i], &witnesses[i], &held[i], &aliases[i]);
        cs_unchanged_other(instances[other], &before);
        CHECK(witnesses[other].groups == (i ? 1u : 0u));
    }
    CHECK(held[0].payload != held[1].payload && aliases[0].payload != aliases[1].payload);
    CHECK(object_pointer(&held[0])->domain == domains[0] && object_pointer(&held[1])->domain == domains[1]);
    cs_close_pair(&fixture, m, instances, domains, witnesses);
    cs_expect_string(&held[0]);
    cs_expect_string(&held[1]);
    cs_release_strings(m, domains, held, aliases);
    CHECK(fixture.code_releases == 1);
}

static void cs_census(const CsMetrics *m) {
    for (unsigned p = 0; p < CS_COMPILER_PHASES; ++p)
        printf("C2_COMPILER phase=%s actual_allocations=%zu\n", cs_compiler_names[p], m->compiler_sites[p]);
    printf("C2_COMPILER_BUDGET allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)m->compiler.allocated_bytes, (unsigned long long)m->compiler.peak_bytes,
        (unsigned long long)m->compiler.work);
    for (unsigned i = 0; i < CS_INSTANCES; ++i) {
        for (unsigned p = 0; p < CS_RUNTIME_PHASES; ++p)
            printf("C2_RUNTIME instance=%u phase=%s actual_allocations=%zu\n", i, cs_runtime_names[p], m->sites[i][p]);
        printf("C2_SELECTOR instance=%u requested_value=%llu requested_call=%llu value_peak=%llu "
            "metadata_peak=%llu call_peak=%llu work=%llu\n", i,
            (unsigned long long)m->boundary[i][0], (unsigned long long)m->boundary[i][1],
            (unsigned long long)m->boundary[i][2], (unsigned long long)m->boundary[i][3],
            (unsigned long long)m->boundary[i][4], (unsigned long long)m->boundary[i][5]);
    }
    puts("C2_RESOURCE_CLASSIFICATION OOM=OPEN six_selector_exact_minus1=OPEN retry=OPEN cancellation=OPEN");
}

static void cs_token(FILE *file, const char *expected) {
    char token[64] = {0};
    CHECK(fscanf(file, "%63s", token) == 1 && !strcmp(token, expected));
}

static uint64_t cs_decimal(FILE *file) {
    char token[32] = {0};
    CHECK(fscanf(file, "%31s", token) == 1 && token[0]);
    uint64_t result = 0;
    for (const char *p = token; *p; ++p) {
        CHECK(*p >= '0' && *p <= '9');
        uint64_t digit = (uint64_t)(*p - '0');
        CHECK(result <= (UINT64_MAX - digit) / 10);
        result = result * 10 + digit;
    }
    return result;
}

static void cs_strict(const CsMetrics *m, const char *path) {
    FILE *file = fopen(path, "rb");
    CHECK(file);
    cs_token(file, "C2_STRING_SOURCE_R1");
    CHECK(cs_decimal(file) == 70);
    cs_token(file, "compiler_sites");
    for (unsigned p = 0; p < CS_COMPILER_PHASES; ++p) CHECK(cs_decimal(file) == m->compiler_sites[p]);
    cs_token(file, "compiler_budget");
    CHECK(cs_decimal(file) == m->compiler.allocated_bytes);
    CHECK(cs_decimal(file) == m->compiler.peak_bytes);
    CHECK(cs_decimal(file) == m->compiler.work);
    for (unsigned i = 0; i < CS_INSTANCES; ++i) {
        cs_token(file, "instance");
        CHECK(cs_decimal(file) == i);
        cs_token(file, "sites");
        for (unsigned p = 0; p < CS_RUNTIME_PHASES; ++p) CHECK(cs_decimal(file) == m->sites[i][p]);
        cs_token(file, "boundary");
        for (unsigned p = 0; p < CS_SELECTORS; ++p) CHECK(cs_decimal(file) == m->boundary[i][p]);
        cs_token(file, "fees_after_close");
        for (unsigned p = 0; p < 3; ++p) CHECK(cs_decimal(file) == m->fees_after_close[i][p]);
    }
    cs_token(file, "code_releases");
    CHECK(cs_decimal(file) == 1);
    cs_token(file, "END");
    int byte;
    while ((byte = fgetc(file)) != EOF) CHECK(isspace((unsigned char)byte));
    CHECK(!ferror(file) && !fclose(file));
    puts("C2 strict independently frozen numeric oracle PASS");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    _Static_assert(XR_XIR_CHECKED_CONTRACT == 70u, "Explicit root bounds are required.");
    CHECK(argc == 2 || argc == 3);
    bool normal = !strcmp(argv[1], "normal");
    bool census = !strcmp(argv[1], "census");
    bool strict = !strcmp(argv[1], "strict");
    CHECK((normal || census) ? argc == 2 : strict && argc == 3);
    CsMetrics metrics;
    cs_run(&metrics);
    if (census) cs_census(&metrics);
    if (strict) cs_strict(&metrics, argv[2]);
    puts("C2 Source generic STRING/typed output/two Instances/result take/last alias physical release PASS");
    return 0;
}
