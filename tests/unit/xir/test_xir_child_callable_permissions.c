/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_child_callable_permissions.c - Same-flow callable execution by real VM children
 *
 * KEY CONCEPT:
 *   Actual GO children execute local closures; unknown advertisements keep their denial.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_output.h"
#include "xir/xxir_constraint_proof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "child callable FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_child_callable_permissions.h"

static CpWitness *cp_witness(const void *instance) {
    CHECK(cp_observed && instance);
    for (unsigned i = 0; i < XRCP_INSTANCES; ++i)
        if (cp_observed->witnesses[i].instance == instance) return &cp_observed->witnesses[i];
    CHECK(false);
    return NULL;
}

static void cp_resolve_expected(XrXirCallView *view, const XrXirValue *function,
                                XrXirCallStatus expected, uint32_t target) {
    uint32_t output = UINT32_C(0xfedcba98);
    const XrXirValue saved = *function;
    size_t before = runtime_attempts;
    XrXirCallStatus status = xr_xir_instance_resolve_function(view, function, &output);
    if (status != expected) fprintf(stderr, "child resolve actual=%u expected=%u target=%u output=%u\n",
        status, expected, target, output);
    CHECK(status == expected);
    CHECK(output == (expected == XR_XIR_CALL_READY ? target : UINT32_C(0xfedcba98)));
    CHECK(!memcmp(function, &saved, sizeof(saved)) && runtime_attempts == before);
}

static void cp_materialize(XrXirCallView *view, CpWitness *w) {
    if (w->values[XRCP_PURE_NONE].type) return;
    const XrXirType types[XRCP_CARRIERS] = {(XrXirType)XRCP_NONE, (XrXirType)XRCP_REQUIRED,
        (XrXirType)XRCP_UNRESOLVED, (XrXirType)XRCP_BOTH, (XrXirType)XRCP_REQUIRED, (XrXirType)XRCP_NONE};
    const uint32_t targets[XRCP_CARRIERS] = {XRCP_PURE, XRCP_PURE, XRCP_PURE, XRCP_PURE, XRCP_ROOT_LEAF, XRCP_RELAY};
    for (unsigned i = 0; i < XRCP_CARRIERS; ++i) {
        bool captured = i == XRCP_CAPTURED;
        CHECK(xr_xir_instance_function(view, types[i], targets[i],
            captured ? &w->values[XRCP_PURE_NONE] : NULL, captured ? 1u : 0u, &w->values[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_value_valid(&w->values[i]));
        CHECK(xr_xir_function_binding(&w->values[i])->entry == targets[i]);
    }
}

static void cp_child_probes(XrXirCallView *view, CpWitness *w) {
    if (w->probes) return;
    CHECK(w->root_call && view->activation != w->root_call);
    cp_resolve_expected(view, &w->values[XRCP_PURE_NONE], XR_XIR_CALL_READY, XRCP_PURE);
    cp_resolve_expected(view, &w->values[XRCP_CAPTURED], XR_XIR_CALL_READY, XRCP_RELAY);
    cp_resolve_expected(view, &w->values[XRCP_PURE_ROOT], XR_XIR_CALL_BAD_STATE, 0);
    cp_resolve_expected(view, &w->values[XRCP_PURE_UNKNOWN], XR_XIR_CALL_BAD_STATE, 0);
    cp_resolve_expected(view, &w->values[XRCP_PURE_MIXED], XR_XIR_CALL_BAD_STATE, 0);
    cp_resolve_expected(view, &w->values[XRCP_ROOT_VALUE], XR_XIR_CALL_BAD_STATE, 0);
    for (unsigned i = 0; i < XRCP_INSTANCES; ++i)
        if (&cp_observed->witnesses[i] != w)
            cp_resolve_expected(view, &cp_observed->witnesses[i].values[XRCP_PURE_NONE], XR_XIR_CALL_BAD_ARGUMENT, 0);
    XrXirFunctionBinding *pure = (XrXirFunctionBinding *)xr_xir_function_binding(&w->values[XRCP_PURE_NONE]);
    CHECK(pure && pure->entry == XRCP_PURE);
    pure->entry = XRCP_ROOT_LEAF;
    cp_resolve_expected(view, &w->values[XRCP_PURE_NONE], XR_XIR_CALL_BAD_ARGUMENT, 0);
    pure->entry = XRCP_PURE;
    XrXirFunctionBinding *captured = (XrXirFunctionBinding *)xr_xir_function_binding(&w->values[XRCP_CAPTURED]);
    CHECK(captured && captured->entry == XRCP_RELAY && captured->capture_count == 1);
    captured->entry = XRCP_UNKNOWN;
    cp_resolve_expected(view, &w->values[XRCP_CAPTURED], XR_XIR_CALL_BAD_ARGUMENT, 0);
    captured->entry = XRCP_MIXED;
    cp_resolve_expected(view, &w->values[XRCP_CAPTURED], XR_XIR_CALL_BAD_ARGUMENT, 0);
    captured->entry = XRCP_RELAY;
    XrXirValue invalid = {XR_XIR_I64, 0, 123};
    cp_resolve_expected(view, &invalid, XR_XIR_CALL_BAD_ARGUMENT, 0);
    XrXirCallView copy = *view;
    cp_resolve_expected(&copy, &w->values[XRCP_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0);
    cp_resolve_expected(NULL, &w->values[XRCP_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0);
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    CHECK(admission);
    uint64_t work = admission->work;
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_resolve_function(view, &w->values[XRCP_PURE_NONE], NULL) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(admission->work == work && runtime_attempts == attempts);
    XrXirValue task = {0};
    CHECK(xr_xir_task_go(view, (XrXirType)XRCP_TASK, XRCP_RELAY, &w->values[XRCP_PURE_NONE], 1, &task) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!task.type && !task.reserved && !task.payload && runtime_attempts == attempts);
    ++w->probes;
}

static XrXirAction cp_resume(XrXirCallView *view) {
    const XrXirVmBinding *binding = view->environment;
    CHECK(binding && binding->function < XRCP_FUNCTIONS);
    CpWitness *w = cp_witness(view->instance);
    CHECK(xr_xir_call_admission(view));
    bool child = false;
    CHECK(xr_xir_task_executor_view_member(w->instance->executor, view, &child));
    uint32_t function = binding->function;
    ++w->calls[function];
    if (function == XRCP_FACTORY) { CHECK(!child); cp_materialize(view, w); }
    if (function == XRCP_ROOT) {
        CHECK(!child);
        if (!w->root_call) w->root_call = view->activation;
        CHECK(w->root_call == view->activation);
        w->saved_root = *view;
    }
    if (function == XRCP_CHILD || function == XRCP_RELAY || function == XRCP_PURE) {
        CHECK(child && w->root_call && view->activation != w->root_call);
        ++w->child_callbacks;
        if (function == XRCP_CHILD) cp_child_probes(view, w);
    }
    XrXirAction action = cp_observed->original[function].resume(view);
    if (function == XRCP_ROOT && action.kind == XR_XIR_ACTION_AWAIT_TASK) ++w->await_actions;
    return action;
}

static void cp_release(XrXirCallView *view, XrXirCallStatus reason) {
    const XrXirVmBinding *binding = view->environment;
    CHECK(binding && binding->function < XRCP_FUNCTIONS);
    CpWitness *w = cp_witness(view->instance);
    CHECK(!xr_xir_call_admission(view));
    cp_resolve_expected(view, &w->values[XRCP_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0);
    ++w->releases[binding->function];
    cp_observed->original[binding->function].release(view, reason);
}

static XrXirOutputStatus cp_bytes(void *opaque, XrXirOutputStream stream, const char *bytes, size_t length) {
    CpWitness *w = opaque;
    CHECK(stream == XR_XIR_STDOUT && length == 3 && !memcmp(bytes, "42\n", 3));
    CHECK(++w->byte_outputs == 1);
    return XR_XIR_OUTPUT_OK;
}

static XrXirOutputStatus cp_output(void *opaque, const XrXirOutputGroup *group) {
    CpWitness *w = opaque;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && group->values);
    CHECK(group->values[0].type == XR_XIR_I64 && !group->values[0].reserved && group->values[0].payload == 42);
    CHECK(++w->typed_outputs == 1);
    return xr_xir_output_render(&w->sink, group);
}

static XrXirInstanceResult cp_returned(CpWitness *w) {
    XrXirInstanceResult result = {0};
    unsigned polls = 0;
    do { result = xr_xir_instance_poll_bounded(w->instance, 1); CHECK(++polls < 32768); }
    while (result.outcome.status == XR_XIR_CALL_READY);
    if (result.outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr, "child root status=%u state=%u epoch=%llu\n", result.outcome.status,
            xr_xir_instance_state(w->instance), (unsigned long long)result.epoch);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
    return result;
}

static void cp_new_pair(CpFixture *f) {
    for (unsigned i = 0; i < XRCP_INSTANCES; ++i) {
        CpWitness *w = &f->witnesses[i];
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        w->sink = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, cp_bytes, w, 32};
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, cp_output, w};
        CHECK(xr_xir_instance_new(f->program, &config, &w->instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(w->instance, XRCP_FACTORY, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = cp_returned(w);
        CHECK(!result.outcome.value.type && !result.outcome.value.reserved && !result.outcome.value.payload);
        XrXirValue unit = {0};
        CHECK(xr_xir_instance_take_result(w->instance, &unit) == XR_XIR_CALL_RETURNED);
        CHECK(!unit.type && !unit.reserved && !unit.payload);
    }
    CHECK(f->witnesses[0].instance->domain != f->witnesses[1].instance->domain);
    CHECK(f->witnesses[0].instance != f->witnesses[1].instance);
}

static void cp_run_pair(CpFixture *f) {
    for (unsigned i = 0; i < XRCP_INSTANCES; ++i) {
        CpWitness *w = &f->witnesses[i];
        CpWitness *peer = &f->witnesses[1 - i];
        unsigned before = peer->child_callbacks;
        CHECK(xr_xir_instance_start(w->instance, XRCP_ROOT, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = cp_returned(w);
        CHECK(result.outcome.value.type == XR_XIR_I64 && !result.outcome.value.reserved && result.outcome.value.payload == 42);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(w->instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && !value.reserved && value.payload == 42);
        xr_xir_value_drop(&value);
        CHECK(w->probes == 1 && w->child_callbacks && w->typed_outputs == 1 && w->byte_outputs == 1);
        CHECK(w->calls[XRCP_CHILD] && w->calls[XRCP_RELAY] && w->calls[XRCP_PURE] && w->await_actions);
        CHECK(w->releases[XRCP_CHILD] == 1 && w->releases[XRCP_RELAY] == 1 && w->releases[XRCP_PURE] == 1);
        CHECK(w->root_call == w->instance->call && peer->child_callbacks == before);
        /* The Instance still owns this actual Call. Inactive admission rejects
         * before inspecting any saved frame or borrowed callback storage. */
        cp_resolve_expected(&w->saved_root, &w->values[XRCP_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0);
        printf("child permissions instance=%u real_child_callbacks=%u relay_worker=BAD_TYPE result=42 typed_output=42\n",
            i, w->child_callbacks);
    }
}

static void cp_close(CpFixture *f) {
    XrXirDomain *domains[XRCP_INSTANCES] = {0};
    for (unsigned i = 0; i < XRCP_INSTANCES; ++i) {
        CpWitness *w = &f->witnesses[i];
        domains[i] = w->instance->domain;
        CHECK(xr_xir_domain_retain(domains[i]));
        CHECK(xr_xir_instance_stop(w->instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start_function(w->instance, &w->values[XRCP_PURE_NONE], NULL, 0) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_free(w->instance) == XR_XIR_CALL_READY);
        w->instance = NULL;
        XrXirDomainBudgetStats stats = xr_xir_domain_budget_stats(domains[i]);
        CHECK(!stats.call_live && stats.call_allocations == stats.call_frees);
        CHECK(stats.metadata_live == sizeof(FunctionGate) && stats.metadata_allocations == stats.metadata_frees + 1);
    }
    xr_xir_compile_program_drop(f->program);
    f->program = NULL;
    CHECK(!f->code_releases);
    for (unsigned i = 0; i < XRCP_INSTANCES; ++i) {
        CpWitness *w = &f->witnesses[i];
        for (unsigned c = 0; c < XRCP_CARRIERS; ++c) {
            CHECK(xr_xir_value_valid(&w->values[c]));
            xr_xir_value_drop(&w->values[c]);
        }
        XrXirDomainStats values = xr_xir_domain_stats(domains[i]);
        XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(domains[i]);
        CHECK(values.live_bytes == sizeof(*domains[i]) && values.allocations == values.frees + 1);
        CHECK(!budget.metadata_live && budget.metadata_allocations == budget.metadata_frees);
        CHECK(!budget.call_live && budget.call_allocations == budget.call_frees);
        xr_xir_domain_drop(domains[i]);
    }
    CHECK(f->code_releases == 1 && !f->lowered && !runtime_live && !runtime_bytes);
    cp_observed = NULL;
    effects_source_owners_free();
    puts("child permissions two Domains/code final aliases compiler/runtime physical=0/0; resource matrix OPEN");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(XR_XIR_CHECKED_CONTRACT == 70u && !runtime_live && !runtime_bytes);
    CpFixture fixture;
    cp_build(&fixture);
    cp_new_pair(&fixture);
    cp_run_pair(&fixture);
    cp_close(&fixture);
    puts("real VM GO child ordinary NONE callable/capture42, closed advertisements and stale gates PASS");
    return 0;
}
