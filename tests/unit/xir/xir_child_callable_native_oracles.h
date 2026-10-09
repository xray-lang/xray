/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_child_callable_native_oracles.h - Independent native closure outcomes
 *
 * KEY CONCEPT:
 *   Local calls need both actual body facts and the advertised root bound.
 */
#ifndef XIR_CHILD_CALLABLE_NATIVE_ORACLES_H
#define XIR_CHILD_CALLABLE_NATIVE_ORACLES_H

enum { CN_INIT, CN_ENTRY, CN_ROOT, CN_CHILD, CN_PURE, CN_RELAY, CN_ROOT_LEAF,
    CN_UNKNOWN, CN_MIXED, CN_FACTORY, CN_FUNCTIONS, CN_INSTANCES = 2 };
enum { CN_TASK = 256, CN_NONE, CN_REQUIRED, CN_UNRESOLVED, CN_BOTH };
enum { CN_PURE_NONE, CN_PURE_ROOT, CN_PURE_UNKNOWN, CN_PURE_MIXED,
    CN_ROOT_VALUE, CN_CAPTURED, CN_CARRIERS };

typedef struct CnBinding { uint32_t function; const XrXirCallEntry *original; } CnBinding;
typedef struct CnWitness {
    XrXirInstance *instance;
    XrXirDomain *domain;
    XrXirCall *root_call;
    XrXirCallView saved_root;
    XrXirValue values[CN_CARRIERS];
    unsigned calls[CN_FUNCTIONS], releases[CN_FUNCTIONS];
    unsigned probes, child_callbacks, typed_outputs, byte_outputs, await_actions;
    XrXirOutputSink sink;
} CnWitness;
typedef struct CnFixture {
    const XrXirCompileContext *context;
    XrXirProgram *program;
    XrXirCallEntry entries[CN_FUNCTIONS];
    CnBinding bindings[CN_FUNCTIONS];
    CnWitness witnesses[CN_INSTANCES];
    unsigned code_releases;
} CnFixture;

static CnFixture *cn_observed;

static CnWitness *cn_witness(const void *instance) {
    CHECK(cn_observed && instance);
    for (unsigned i = 0; i < CN_INSTANCES; ++i)
        if (cn_observed->witnesses[i].instance == instance) return &cn_observed->witnesses[i];
    CHECK(false);
    return NULL;
}

static void cn_resolve_expected(XrXirCallView *view, CnWitness *w, const XrXirValue *function,
    XrXirCallStatus expected, uint32_t target, bool free_entry, XrXirValueAdmission *active_admission) {
    uint32_t output = UINT32_C(0xfedcba98);
    const XrXirValue saved = *function;
    const size_t attempts = runtime_attempts;
    const uint64_t domain_work = xr_xir_domain_budget_stats(w->domain).work;
    const uint64_t work = active_admission ? active_admission->work : 0;
    XrXirCallStatus status = xr_xir_instance_resolve_function(view, function, &output);
    if (status != expected) fprintf(stderr, "native resolve actual=%u expected=%u target=%u output=%u\n",
        status, expected, target, output);
    CHECK(status == expected);
    CHECK(output == (expected == XR_XIR_CALL_READY ? target : UINT32_C(0xfedcba98)));
    CHECK(!memcmp(function, &saved, sizeof(saved)) && runtime_attempts == attempts);
    if (free_entry) {
        CHECK(xr_xir_domain_budget_stats(w->domain).work == domain_work);
        CHECK(!active_admission || active_admission->work == work);
    }
}

static void cn_materialize(XrXirCallView *view, CnWitness *w) {
    if (w->values[CN_PURE_NONE].type) return;
    const XrXirType types[CN_CARRIERS] = {(XrXirType)CN_NONE, (XrXirType)CN_REQUIRED,
        (XrXirType)CN_UNRESOLVED, (XrXirType)CN_BOTH, (XrXirType)CN_REQUIRED, (XrXirType)CN_NONE};
    const uint32_t targets[CN_CARRIERS] = {CN_PURE, CN_PURE, CN_PURE, CN_PURE, CN_ROOT_LEAF, CN_RELAY};
    for (unsigned i = 0; i < CN_CARRIERS; ++i) {
        bool captured = i == CN_CAPTURED;
        CHECK(xr_xir_instance_function(view, types[i], targets[i],
            captured ? &w->values[CN_PURE_NONE] : NULL, captured ? 1u : 0u, &w->values[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_value_valid(&w->values[i]));
        const XrXirFunctionBinding *binding = xr_xir_function_binding(&w->values[i]);
        CHECK(binding && binding->entry == targets[i]);
        CHECK(binding->capture_count == (captured ? 1u : 0u));
        if (captured) CHECK(binding->captures && !memcmp(&binding->captures[0], &w->values[CN_PURE_NONE], sizeof(XrXirValue)));
    }
}

static void cn_child_probes(XrXirCallView *view, CnWitness *w) {
    if (w->probes) return;
    CHECK(w->root_call && view->activation != w->root_call);
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    CHECK(admission);
    cn_resolve_expected(view, w, &w->values[CN_PURE_NONE], XR_XIR_CALL_READY, CN_PURE, false, admission);
    cn_resolve_expected(view, w, &w->values[CN_CAPTURED], XR_XIR_CALL_READY, CN_RELAY, false, admission);
    cn_resolve_expected(view, w, &w->values[CN_PURE_ROOT], XR_XIR_CALL_BAD_STATE, 0, false, admission);
    cn_resolve_expected(view, w, &w->values[CN_PURE_UNKNOWN], XR_XIR_CALL_BAD_STATE, 0, false, admission);
    cn_resolve_expected(view, w, &w->values[CN_PURE_MIXED], XR_XIR_CALL_BAD_STATE, 0, false, admission);
    cn_resolve_expected(view, w, &w->values[CN_ROOT_VALUE], XR_XIR_CALL_BAD_STATE, 0, false, admission);
    for (unsigned i = 0; i < CN_INSTANCES; ++i)
        if (&cn_observed->witnesses[i] != w)
            cn_resolve_expected(view, w, &cn_observed->witnesses[i].values[CN_PURE_NONE], XR_XIR_CALL_BAD_ARGUMENT, 0, true, admission);
    XrXirFunctionBinding *pure = (XrXirFunctionBinding *)xr_xir_function_binding(&w->values[CN_PURE_NONE]);
    CHECK(pure && pure->entry == CN_PURE);
    unsigned char saved_pure[sizeof(*pure)];
    memcpy(saved_pure, pure, sizeof(*pure));
    pure->entry = CN_ROOT_LEAF;
    cn_resolve_expected(view, w, &w->values[CN_PURE_NONE], XR_XIR_CALL_BAD_ARGUMENT, 0, false, admission);
    pure->entry = CN_PURE;
    CHECK(!memcmp(pure, saved_pure, sizeof(*pure)));
    XrXirFunctionBinding *captured = (XrXirFunctionBinding *)xr_xir_function_binding(&w->values[CN_CAPTURED]);
    CHECK(captured && captured->entry == CN_RELAY && captured->capture_count == 1);
    unsigned char saved_captured[sizeof(*captured)];
    memcpy(saved_captured, captured, sizeof(*captured));
    captured->entry = CN_UNKNOWN;
    cn_resolve_expected(view, w, &w->values[CN_CAPTURED], XR_XIR_CALL_BAD_ARGUMENT, 0, false, admission);
    captured->entry = CN_MIXED;
    cn_resolve_expected(view, w, &w->values[CN_CAPTURED], XR_XIR_CALL_BAD_ARGUMENT, 0, false, admission);
    captured->entry = CN_RELAY;
    CHECK(!memcmp(captured, saved_captured, sizeof(*captured)));
    const XrXirValue invalid = {XR_XIR_I64, 0, 123};
    cn_resolve_expected(view, w, &invalid, XR_XIR_CALL_BAD_ARGUMENT, 0, true, admission);
    XrXirCallView copy = *view;
    cn_resolve_expected(&copy, w, &w->values[CN_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0, true, admission);
    cn_resolve_expected(NULL, w, &w->values[CN_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0, true, admission);
    uint64_t work = admission->work;
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_resolve_function(view, &w->values[CN_PURE_NONE], NULL) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(admission->work == work && runtime_attempts == attempts);
    XrXirValue task = {0};
    CHECK(xr_xir_task_go(view, (XrXirType)CN_TASK, CN_RELAY, &w->values[CN_PURE_NONE], 1, &task) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!task.type && !task.reserved && !task.payload && runtime_attempts == attempts);
    ++w->probes;
}

static XrXirAction cn_resume(XrXirCallView *view) {
    const CnBinding *binding = view->environment;
    CHECK(binding && binding->function < CN_FUNCTIONS && binding->original == &child_callable_native_entries[binding->function]);
    CnWitness *w = cn_witness(view->instance);
    CHECK(xr_xir_call_admission(view));
    bool child = false;
    CHECK(xr_xir_task_executor_view_member(w->instance->executor, view, &child));
    uint32_t function = binding->function;
    ++w->calls[function];
    if (function == CN_FACTORY) { CHECK(!child); cn_materialize(view, w); }
    if (function == CN_ROOT) {
        CHECK(!child);
        if (!w->root_call) w->root_call = view->activation;
        CHECK(w->root_call == view->activation);
        w->saved_root = *view;
    }
    if (function == CN_CHILD || function == CN_RELAY || function == CN_PURE) {
        CHECK(child && w->root_call && view->activation != w->root_call);
        ++w->child_callbacks;
        if (function == CN_CHILD) cn_child_probes(view, w);
    }
    XrXirAction action = binding->original->resume(view);
    if (function == CN_ROOT && action.kind == XR_XIR_ACTION_AWAIT_TASK) ++w->await_actions;
    return action;
}

static void cn_release(XrXirCallView *view, XrXirCallStatus reason) {
    const CnBinding *binding = view->environment;
    CHECK(binding && binding->function < CN_FUNCTIONS && binding->original == &child_callable_native_entries[binding->function]);
    CnWitness *w = cn_witness(view->instance);
    CHECK(!xr_xir_call_admission(view));
    cn_resolve_expected(view, w, &w->values[CN_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0, true, NULL);
    ++w->releases[binding->function];
    binding->original->release(view, reason);
}

static void cn_code_drop(void *owner) {
    CnFixture *f = owner;
    CHECK(f == cn_observed && !f->code_releases);
    ++f->code_releases;
}

static void cn_build(CnFixture *f) {
    CHECK(!cn_observed);
    cn_observed = f;
    f->context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    CHECK(child_callable_native_program.entry_count == CN_FUNCTIONS);
    CHECK(child_callable_native_program.entries == child_callable_native_entries);
    CHECK(child_callable_native_program.proof.bytes && child_callable_native_program.proof.length);
    CHECK(child_callable_native_program.proof.identity && child_callable_native_program.proof.layouts);
    CHECK(child_callable_native_program.declarations->entry_function == CN_ENTRY);
    CHECK(child_callable_native_program.types && child_callable_native_program.types->count == 5);
    const uint32_t advertised[4] = {2u, 4u, 8u, 12u};
    for (unsigned i = 0; i < 4; ++i) {
        const XrXirTypeNode *node = &child_callable_native_program.types->nodes[i + 1];
        CHECK(node->kind == XR_XIR_TYPE_CALLABLE && node->flags == advertised[i]);
    }
    for (unsigned i = 0; i < CN_FUNCTIONS; ++i) {
        CHECK(child_callable_native_entries[i].resume && child_callable_native_entries[i].release);
        CHECK(!child_callable_native_entries[i].environment);
        f->bindings[i] = (CnBinding){i, &child_callable_native_entries[i]};
        f->entries[i] = child_callable_native_entries[i];
        f->entries[i].resume = cn_resume;
        f->entries[i].release = cn_release;
        f->entries[i].environment = &f->bindings[i];
    }
    XrXirProgramSpec spec = child_callable_native_program;
    spec.entries = f->entries;
    spec.code = (XrXirCodeLease){f, cn_code_drop};
    CHECK(xr_xir_compile_program_seal(f->context, &spec, &f->program) == XR_XIR_OK);
    const XrXirProgramPermissions *permissions = f->program->permissions;
    CHECK(permissions && permissions->function_count == CN_FUNCTIONS);
    CHECK(!permissions->entries[CN_PURE].requires_root && !permissions->entries[CN_PURE].unresolved);
    CHECK(!permissions->entries[CN_RELAY].requires_root && !permissions->entries[CN_RELAY].unresolved);
    CHECK(permissions->entries[CN_RELAY].worker == XR_XIR_BAD_TYPE);
    CHECK(permissions->entries[CN_CHILD].worker == XR_XIR_OK);
    CHECK(permissions->entries[CN_ROOT_LEAF].requires_root && !permissions->entries[CN_ROOT_LEAF].unresolved);
    CHECK(!permissions->entries[CN_UNKNOWN].requires_root && permissions->entries[CN_UNKNOWN].unresolved);
    CHECK(permissions->entries[CN_MIXED].requires_root && permissions->entries[CN_MIXED].unresolved);
}

static XrXirOutputStatus cn_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    CnWitness *w = context;
    CHECK(stream == XR_XIR_STDOUT && length == 3 && !memcmp(bytes, "42\n", 3));
    CHECK(++w->byte_outputs == 1);
    return XR_XIR_OUTPUT_OK;
}

static XrXirOutputStatus cn_output(void *context, const XrXirOutputGroup *group) {
    CnWitness *w = context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && group->values);
    CHECK(group->values[0].type == XR_XIR_I64 && !group->values[0].reserved && group->values[0].payload == 42);
    CHECK(++w->typed_outputs == 1);
    return xr_xir_output_render(&w->sink, group);
}

static XrXirInstanceResult cn_returned(CnWitness *w) {
    for (unsigned polls = 0; polls < 32768; ++polls) {
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(w->instance, 1);
        if (result.outcome.status == XR_XIR_CALL_READY) continue;
        if (result.outcome.status != XR_XIR_CALL_RETURNED) fprintf(stderr,
            "native child root status=%u state=%u epoch=%llu\n", result.outcome.status,
            xr_xir_instance_state(w->instance), (unsigned long long)result.epoch);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
        return result;
    }
    CHECK(false);
    return (XrXirInstanceResult){0};
}

static void cn_new_pair(CnFixture *f) {
    for (unsigned i = 0; i < CN_INSTANCES; ++i) {
        CnWitness *w = &f->witnesses[i];
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        w->sink = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, cn_bytes, w, 32};
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, cn_output, w};
        CHECK(xr_xir_instance_new(f->program, &config, &w->instance) == XR_XIR_CALL_READY);
        w->domain = w->instance->domain;
        CHECK(xr_xir_instance_start(w->instance, CN_FACTORY, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = cn_returned(w);
        CHECK(!result.outcome.value.type && !result.outcome.value.reserved && !result.outcome.value.payload);
        XrXirValue unit = {0};
        CHECK(xr_xir_instance_take_result(w->instance, &unit) == XR_XIR_CALL_RETURNED);
        CHECK(!unit.type && !unit.reserved && !unit.payload);
    }
    CHECK(f->witnesses[0].domain != f->witnesses[1].domain);
    CHECK(f->witnesses[0].instance != f->witnesses[1].instance);
}

static void cn_run_pair(CnFixture *f) {
    for (unsigned i = 0; i < CN_INSTANCES; ++i) {
        CnWitness *w = &f->witnesses[i];
        CnWitness *peer = &f->witnesses[1 - i];
        const unsigned before = peer->child_callbacks;
        CHECK(xr_xir_instance_start(w->instance, CN_ROOT, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = cn_returned(w);
        CHECK(result.outcome.value.type == XR_XIR_I64 && !result.outcome.value.reserved && result.outcome.value.payload == 42);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(w->instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && !value.reserved && value.payload == 42);
        xr_xir_value_drop(&value);
        CHECK(w->probes == 1 && w->child_callbacks && w->typed_outputs == 1 && w->byte_outputs == 1);
        CHECK(w->calls[CN_CHILD] && w->calls[CN_RELAY] && w->calls[CN_PURE] && w->await_actions);
        CHECK(w->releases[CN_CHILD] == 1 && w->releases[CN_RELAY] == 1 && w->releases[CN_PURE] == 1);
        CHECK(w->root_call == w->instance->call && peer->child_callbacks == before);
        cn_resolve_expected(&w->saved_root, w, &w->values[CN_PURE_NONE], XR_XIR_CALL_BAD_STATE, 0, true, NULL);
    }
}

static void cn_close(CnFixture *f) {
    XrXirDomain *domains[CN_INSTANCES] = {0};
    for (unsigned i = 0; i < CN_INSTANCES; ++i) {
        CnWitness *w = &f->witnesses[i];
        domains[i] = w->domain;
        CHECK(xr_xir_domain_retain(domains[i]));
        CHECK(xr_xir_instance_stop(w->instance) == XR_XIR_CALL_READY);
        const uint64_t work = xr_xir_domain_budget_stats(domains[i]).work;
        const size_t attempts = runtime_attempts;
        CHECK(xr_xir_instance_start_function(w->instance, &w->values[CN_PURE_NONE], NULL, 0) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_domain_budget_stats(domains[i]).work == work && runtime_attempts == attempts);
        CHECK(xr_xir_instance_free(w->instance) == XR_XIR_CALL_READY);
        w->instance = NULL;
        XrXirDomainBudgetStats stats = xr_xir_domain_budget_stats(domains[i]);
        CHECK(!stats.call_live && stats.call_allocations == stats.call_frees);
        CHECK(stats.metadata_live == sizeof(FunctionGate) && stats.metadata_allocations == stats.metadata_frees + 1);
    }
    xr_xir_compile_program_drop(f->program);
    f->program = NULL;
    CHECK(!f->code_releases);
    for (unsigned i = 0; i < CN_INSTANCES; ++i) {
        CnWitness *w = &f->witnesses[i];
        for (unsigned c = 0; c < CN_CARRIERS; ++c) {
            CHECK(xr_xir_value_valid(&w->values[c]));
            xr_xir_value_drop(&w->values[c]);
        }
        XrXirDomainStats values = xr_xir_domain_stats(domains[i]);
        XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(domains[i]);
        CHECK(values.live_bytes == sizeof(*domains[i]) && values.allocations == values.frees + 1);
        CHECK(!budget.metadata_live && budget.metadata_allocations == budget.metadata_frees);
        CHECK(!budget.call_live && budget.call_allocations == budget.call_frees);
        xr_xir_domain_drop(domains[i]);
        w->domain = NULL;
    }
    CHECK(f->code_releases == 1 && !runtime_live && !runtime_bytes);
    cn_observed = NULL;
    effects_source_owners_free();
}
#endif // XIR_CHILD_CALLABLE_NATIVE_ORACLES_H
