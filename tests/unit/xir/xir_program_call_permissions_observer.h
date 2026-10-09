/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_program_call_permissions_observer.h - Native action requests on authentic VM children
 *
 * KEY CONCEPT:
 *   Entry and typed-output witnesses expose execution before a later slot denial.
 */
#ifndef XIR_PROGRAM_CALL_PERMISSIONS_OBSERVER_H
#define XIR_PROGRAM_CALL_PERMISSIONS_OBSERVER_H

static PceWitness *pce_witness(const XrXirInstance *instance) {
    CHECK(pce_current && instance);
    for (unsigned i = 0; i < PCE_INSTANCES; ++i)
        if (pce_current->witnesses[i].instance == instance) return &pce_current->witnesses[i];
    CHECK(false);
    return NULL;
}

static PceLedger pce_ledger(const PceWitness *w) {
    CHECK(w->instance && w->instance->domain == w->domain && w->instance->budget.work_domain == w->domain);
    const XrXirCallBudget *b = &w->instance->budget;
    return (PceLedger){xr_xir_domain_stats(w->domain), xr_xir_domain_budget_stats(w->domain),
        b->requested_bytes, b->live_bytes, b->allocations, b->frees, b->transitions, b->resumes, runtime_attempts};
}

static bool pce_same_fee(const PceLedger *a, const PceLedger *b) {
    return a->runtime_attempts == b->runtime_attempts && a->call_requested == b->call_requested &&
        a->call_live == b->call_live && a->call_allocations == b->call_allocations &&
        a->call_frees == b->call_frees && a->transitions == b->transitions && a->resumes == b->resumes &&
        a->domain.requested_bytes == b->domain.requested_bytes &&
        a->domain.requested_call_bytes == b->domain.requested_call_bytes && a->domain.work == b->domain.work &&
        a->domain.metadata_live == b->domain.metadata_live &&
        a->domain.metadata_allocations == b->domain.metadata_allocations &&
        a->domain.metadata_frees == b->domain.metadata_frees &&
        a->domain.call_live == b->domain.call_live && a->domain.call_allocations == b->domain.call_allocations &&
        a->domain.call_frees == b->domain.call_frees &&
        a->values.live_bytes == b->values.live_bytes && a->values.allocations == b->values.allocations &&
        a->values.frees == b->values.frees && a->values.reallocations == b->values.reallocations;
}

static void pce_print_ledger(const PceWitness *w, const char *phase, const PceLedger *s) {
    printf("PCE_LEDGER instance=%u phase=%s domain=%p attempts=%zu requested_value=%llu "
        "requested_call=%llu work=%llu value_live=%llu metadata_live=%llu call_live=%llu "
        "call_budget_request=%llu call_allocations=%llu call_frees=%llu transitions=%llu resumes=%llu\n",
        w->index, phase, (void *)w->domain, s->runtime_attempts,
        (unsigned long long)s->domain.requested_bytes, (unsigned long long)s->domain.requested_call_bytes,
        (unsigned long long)s->domain.work, (unsigned long long)s->values.live_bytes,
        (unsigned long long)s->domain.metadata_live, (unsigned long long)s->domain.call_live,
        (unsigned long long)s->call_requested, (unsigned long long)s->call_allocations,
        (unsigned long long)s->call_frees, (unsigned long long)s->transitions, (unsigned long long)s->resumes);
}

static XrXirAction pce_resume(XrXirCallView *view) {
    PceFixture *f = pce_current;
    const XrXirVmBinding *binding = view->environment;
    CHECK(f && binding && binding->function < f->functions);
    uint32_t function = binding->function;
    CHECK(binding == &f->bindings[function] && binding->artifact == f->lowered);
    PceWitness *w = pce_witness(view->instance);
    bool child = false;
    const XrXirValueAdmission *admission = xr_xir_call_admission(view);
    CHECK(admission && admission->domain == w->domain && view->activation->budget == &w->instance->budget);
    CHECK(xr_xir_task_executor_view_member(w->instance->executor, view, &child));
    if (function == f->initializer) { CHECK(!child); ++w->init_entered; }
    if (function == f->entries[PCE_RUN]) {
        CHECK(!child && view->activation == w->instance->call);
        w->root_call = view->activation;
        if (f->mode == PCE_ADVERTISED_UNKNOWN && !w->carrier_created) {
            CHECK(!w->advertised.type && !w->advertised.reserved && !w->advertised.payload);
            CHECK(xr_xir_instance_function(view, f->advertised_type, f->entries[PCE_LEAF],
                NULL, 0, &w->advertised) == XR_XIR_CALL_READY);
            CHECK(xr_xir_value_valid(&w->advertised) && w->advertised.type == (uint32_t)f->advertised_type);
            const XrXirFunctionBinding *carrier = xr_xir_function_binding(&w->advertised);
            CHECK(carrier && carrier->owner == w->instance->function_gate && carrier->release &&
                carrier->entry == f->entries[PCE_LEAF] && !carrier->capture_count);
            const XrXirTypeNode *signature = xr_xir_callable_signature(f->program->types, f->advertised_type);
            CHECK(signature && !signature->parameter_count && signature->result == XR_XIR_I64 &&
                signature->flags == XR_XIR_CALLABLE_ROOT_UNRESOLVED);
            ++w->carrier_created;
            printf("PCE_CARRIER instance=%u phase=create actual_root=1 same_instance=1 type=%u "
                "advertised_root=8 actual_body_root=0 actual_body_unknown=0 target=%u\n",
                w->index, (unsigned)f->advertised_type, carrier->entry);
        }
    }
    if (child) { CHECK(w->root_call && view->activation != w->root_call); ++w->child_callbacks; }
    if (function == f->target) { CHECK(child); ++w->target_entered; }
    if (function == f->entries[PCE_LEAF]) { CHECK(child); ++w->leaf_entered; }
    XrXirAction action = f->original[function].resume(view);
    if (function == f->entries[PCE_WORKER] && action.kind == XR_XIR_ACTION_CALL) {
        CHECK(child && !w->call_requests && action.callee == f->entries[PCE_LEAF]);
        /* A real zero-argument VM CALL still carries its outgoing buffer. */
        CHECK(!action.argument_count && !action.value.type &&
            !action.value.reserved && !action.value.payload && !action.flags);
        ++w->call_requests;
        if (f->mode == PCE_ADVERTISED_UNKNOWN) {
            const XrXirFunctionBinding *carrier = xr_xir_function_binding(&w->advertised);
            CHECK(w->carrier_created == 1 && carrier && carrier->entry == action.callee &&
                carrier->owner == w->instance->function_gate);
            /* Keep the real child CALL target, arguments and frame edge. Only
             * the advertised function value is replaced by the root-created loan. */
            action.value = w->advertised;
        }
        w->before_call = pce_ledger(w);
        w->slot_before = w->instance->slots[0];
        w->boundary_captured = true;
        action.callee = f->target;
        printf("PCE_REQUEST instance=%u caller=%u target=%u original=%u actual_child=1 call=%p root_call=%p\n",
            w->index, function, action.callee, f->entries[PCE_LEAF], (void *)view->activation, (void *)w->root_call);
    }
    return action;
}

static void pce_release(XrXirCallView *view, XrXirCallStatus reason) {
    PceFixture *f = pce_current;
    const XrXirVmBinding *binding = view->environment;
    CHECK(f && binding && binding->function < f->functions && !xr_xir_call_admission(view));
    PceWitness *w = pce_witness(view->instance);
    ++w->entry_releases[binding->function];
    if (f->original[binding->function].release) f->original[binding->function].release(view, reason);
}

static XrXirOutputStatus pce_write(void *opaque, XrXirOutputStream stream, const char *bytes, size_t length) {
    PceWitness *w = opaque;
    CHECK(stream == XR_XIR_STDOUT && length <= sizeof(w->bytes) - w->length);
    memcpy(w->bytes + w->length, bytes, length);
    w->length += length;
    ++w->byte_writes;
    return XR_XIR_OUTPUT_OK;
}

static XrXirOutputStatus pce_output(void *opaque, const XrXirOutputGroup *group) {
    PceWitness *w = opaque;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && group->values);
    CHECK(group->values[0].type == XR_XIR_I64 && !group->values[0].reserved);
    int64_t value = group->values[0].payload;
    CHECK(value == 42 || value == 701 || value == 702 || value == 703);
    ++w->typed_groups;
    printf("PCE_TYPED instance=%u value=%lld groups=%u\n", w->index, (long long)value, w->typed_groups);
    return xr_xir_output_render(&w->sink, group);
}

static void pce_trace(void *opaque, XrXirLifecycleEvent event, uint32_t id) {
    PceWitness *w = opaque;
    if (event == XR_XIR_MODULE_BEGIN) { CHECK(id == pce_current->module); ++w->module_begins; }
    else if (event == XR_XIR_MODULE_READY) { CHECK(id == pce_current->module); ++w->module_ready; }
    else if (event == XR_XIR_SLOT_PUBLISHED) { CHECK(!id); ++w->published; }
    else { CHECK(event == XR_XIR_SLOT_RELEASED && !id); ++w->released; }
}

static void pce_new_pair(PceFixture *f) {
    for (unsigned i = 0; i < PCE_INSTANCES; ++i) {
        PceWitness *w = &f->witnesses[i];
        w->index = i;
        w->sink = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, pce_write, w, sizeof(w->bytes)};
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, pce_output, w};
        config.trace = pce_trace; config.trace_context = w;
        CHECK(xr_xir_instance_new(f->program, &config, &w->instance) == XR_XIR_CALL_READY);
        CHECK(w->instance->program == f->program);
        w->domain = w->instance->domain;
        CHECK(xr_xir_domain_retain(w->domain));
    }
    CHECK(f->witnesses[0].instance != f->witnesses[1].instance);
    CHECK(f->witnesses[0].domain != f->witnesses[1].domain);
    CHECK(&f->witnesses[0].instance->budget != &f->witnesses[1].instance->budget);
}
#endif // XIR_PROGRAM_CALL_PERMISSIONS_OBSERVER_H
