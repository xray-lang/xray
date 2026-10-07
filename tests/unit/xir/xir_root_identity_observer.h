/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_identity_observer.h - Adversarial probes on unchanged VM views
 *
 * KEY CONCEPT:
 *   Observers call existing APIs and check fixed outcomes. They neither
 *   reconstruct the runtime admission predicate nor change execution bindings.
 */
#ifndef XIR_ROOT_IDENTITY_OBSERVER_H
#define XIR_ROOT_IDENTITY_OBSERVER_H
static RiWitness *ri_witness(const void *instance) {
    CHECK(ri_observed && instance);
    for (unsigned i = 0; i < RI_INSTANCES; ++i)
        if (ri_observed->witnesses[i].instance == instance) return &ri_observed->witnesses[i];
    CHECK(false); return NULL;
}
static void ri_empty(const XrXirValue *value) {
    const XrXirValue empty = {0};
    CHECK(!memcmp(value, &empty, sizeof(empty)));
}
static void ri_denied_read(XrXirCallView *view) {
    XrXirValue output = {0};
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_slot_read(view, 0, &output) == XR_XIR_CALL_BAD_STATE);
    ri_empty(&output); CHECK(runtime_attempts == attempts);
}
static void ri_false_views(XrXirCallView *view, RiWitness *w) {
    XrXirCallView copy = *view;
    ri_denied_read(&copy); ++w->copied_view_denials;
    const XrXirCallView original = *view;
    CHECK(original.environment && original.state && original.arena);
    view->instance = w->peer; ri_denied_read(view); *view = original;
    view->environment = NULL; ri_denied_read(view); *view = original;
    view->state = NULL; ri_denied_read(view); *view = original;
    view->argument_count = original.argument_count + 1; ri_denied_read(view); *view = original;
    view->arena = NULL; ri_denied_read(view); *view = original;
    view->phase = original.phase == XR_XIR_CALL_NORMAL ? XR_XIR_CALL_EXIT : XR_XIR_CALL_NORMAL;
    ri_denied_read(view); *view = original;
    view->scope_exit = !original.scope_exit; ri_denied_read(view); *view = original;
    view->activation = NULL; ri_denied_read(view); *view = original;
    w->altered_view_denials += 8;
    CHECK(xr_xir_call_admission(view));
}
static void ri_local_cell(XrXirCallView *view, RiWitness *w) {
    if (w->child_local) return;
    XrXirValue initial = {XR_XIR_I64, 0, 31}, changed = {XR_XIR_I64, 0, 33};
    XrXirValue cell = {0}, observed = {0};
    CHECK(xr_xir_instance_cell(view, (XrXirType)RI_CELL_TYPE, &initial, &cell) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_cell_write(view, &cell, &changed) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_cell_read(view, &cell, &observed) == XR_XIR_CALL_READY);
    CHECK(observed.type == XR_XIR_I64 && !observed.reserved && observed.payload == 33);
    xr_xir_value_drop(&observed); xr_xir_value_drop(&cell); ++w->child_local;
}
static void ri_child_authority(XrXirCallView *view, RiWitness *w, unsigned function) {
    CHECK(w->root_call && view->activation != w->root_call);
    ri_denied_read(view);
    XrXirValue changed = {XR_XIR_I64, 0, 777}, marker = {0}, path_value = {0};
    CHECK(xr_xir_instance_slot_write(view, 0, &changed, false) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_slot_write(view, 0, &changed, true) == XR_XIR_CALL_BAD_STATE);
    XrXirValue group[2] = {{XR_XIR_I64, 0, 777}, {XR_XIR_I64, 0, 888}};
    CHECK(xr_xir_instance_slot_group_init(view, 0, 2, group, 2) == XR_XIR_CALL_BAD_STATE);
    const XrXirValueReceiver mutable_receiver = {.kind = XR_XIR_ROOT_SLOT, .type = XR_XIR_I64};
    const XrXirValueReceiver const_receiver = {.kind = XR_XIR_ROOT_SLOT, .type = XR_XIR_I64, .slot = 1};
    const XrXirValuePath path = {NULL, 0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_instance_path_read(view, &mutable_receiver, &path, &path_value, &fault) == XR_XIR_CALL_BAD_STATE);
    ri_empty(&path_value); CHECK(xr_xir_fault_empty(fault));
    CHECK(xr_xir_instance_path_write(view, &mutable_receiver, &path, &changed, &fault) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_fault_empty(fault));
    CHECK(xr_xir_instance_slot_read(view, 1, &marker) == XR_XIR_CALL_READY);
    CHECK(marker.type == XR_XIR_I64 && !marker.reserved && marker.payload == 21);
    CHECK(xr_xir_instance_path_read(view, &const_receiver, &path, &path_value, &fault) == XR_XIR_CALL_READY);
    CHECK(path_value.type == XR_XIR_I64 && !path_value.reserved && path_value.payload == 21);
    xr_xir_value_drop(&path_value); xr_xir_value_drop(&marker);
    uint32_t entry = UINT32_C(0xdeadbeef);
    CHECK(w->function.type == RI_FUNCTION_TYPE);
    CHECK(xr_xir_instance_resolve_function(view, &w->function, &entry) == XR_XIR_CALL_BAD_STATE);
    CHECK(entry == UINT32_C(0xdeadbeef));
    if (function == RI_CHILD) ri_local_cell(view, w);
    else ++w->child_cleanup_denials;
}
static void ri_root_authority(XrXirCallView *view, RiWitness *w, unsigned function) {
    XrXirValue counter = {0};
    CHECK(xr_xir_instance_slot_read(view, 0, &counter) == XR_XIR_CALL_READY);
    CHECK(counter.type == XR_XIR_I64 && !counter.reserved);
    CHECK(xr_xir_instance_slot_write(view, 0, &counter, false) == XR_XIR_CALL_READY);
    if (function == RI_ROOT_EXIT) {
        ++w->root_cleanup_reads; w->root_cleanup_slot = counter.payload;
    }
    xr_xir_value_drop(&counter);
    if (function == RI_ROOT && !w->function.type) {
        CHECK(xr_xir_instance_function(view, (XrXirType)RI_FUNCTION_TYPE, RI_READ,
            NULL, 0, &w->function) == XR_XIR_CALL_READY);
    }
    if (w->function.type) {
        uint32_t entry = UINT32_C(0xdeadbeef);
        CHECK(xr_xir_instance_resolve_function(view, &w->function, &entry) == XR_XIR_CALL_READY);
        CHECK(entry == RI_READ);
    }
}
static void ri_cleanup_no_spawn(XrXirCallView *view) {
    XrXirValue argument = {XR_XIR_I64, 0, 21}, task = {0};
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_task_go(view, (XrXirType)RI_TASK_TYPE, RI_CHILD, &argument, 1, &task) == XR_XIR_CALL_DEFER_ASYNC);
    ri_empty(&task); CHECK(runtime_attempts == attempts);
}
static XrXirAction ri_observe_resume(XrXirCallView *view) {
    const XrXirVmBinding *binding = view->environment;
    CHECK(binding && binding->function < RI_FUNCTIONS && binding == &ri_observed->bindings[binding->function]);
    const unsigned function = binding->function;
    RiWitness *w = ri_witness(view->instance);
    CHECK(!w->active_view && xr_xir_call_admission(view));
    w->active_view = view; ++w->calls[function];
    CHECK(xr_xir_instance_start(w->instance, RI_READ, NULL, 0) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_free(w->instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_poll_bounded(w->instance, 1).outcome.status == XR_XIR_CALL_BUSY);
    ri_false_views(view, w);
    if (function == RI_ROOT) {
        if (!w->root_call) w->root_call = view->activation;
        CHECK(w->root_call == view->activation);
        w->saved_root = *view; w->saved_root_live = true;
    }
    if (function == RI_CHILD || function == RI_CHILD_EXIT) ri_child_authority(view, w, function);
    else if (function != RI_INIT) ri_root_authority(view, w, function);
    if (function == RI_ROOT_EXIT || function == RI_CHILD_EXIT) ri_cleanup_no_spawn(view);
    /* The actual binding environment and frame remain unchanged for the VM. */
    XrXirAction action = ri_observed->original[function].resume(view);
    if (function == RI_ROOT && action.kind == XR_XIR_ACTION_AWAIT_TASK) ++w->await_count;
    if (function == RI_ROOT && action.kind == XR_XIR_ACTION_SUSPEND) ++w->host_suspend_count;
    w->active_view = NULL;
    /* Used only synchronously by OUTPUT action acceptance while the driver's
     * local view is still alive. No host code dereferences this after polling. */
    w->returned_view = view;
    return action;
}
static void ri_observe_release(XrXirCallView *view, XrXirCallStatus reason) {
    const XrXirVmBinding *binding = view->environment;
    CHECK(binding && binding->function < RI_FUNCTIONS);
    RiWitness *w = ri_witness(view->instance);
    CHECK(!xr_xir_call_admission(view));
    ri_denied_read(view); ++w->release_denials; ++w->releases[binding->function];
    if (ri_observed->original[binding->function].release)
        ri_observed->original[binding->function].release(view, reason);
}
static XrXirOutputStatus ri_output(void *context, const XrXirOutputGroup *group) {
    RiWitness *w = context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1);
    CHECK(group->values[0].type == XR_XIR_I64 && !group->values[0].reserved && w->output_count < 24);
    CHECK(!w->active_view && w->returned_view);
    CHECK(!xr_xir_call_admission(w->returned_view));
    ri_denied_read(w->returned_view); ++w->output_denials; w->returned_view = NULL;
    w->outputs[w->output_count++] = group->values[0].payload;
    return XR_XIR_OUTPUT_OK;
}
static void ri_lifecycle(void *context, XrXirLifecycleEvent event, uint32_t index) {
    RiWitness *w = context;
    CHECK(w->instance && w->lifecycle_count < 12);
    w->lifecycle[w->lifecycle_count++] = (RiTrace){event, index};
    CHECK(xr_xir_instance_free(w->instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(w->instance) == XR_XIR_CALL_BUSY);
    if (w->active_view) {
        ri_denied_read((XrXirCallView *)w->active_view); ++w->trace_denials;
    }
}
static void ri_instances(RiFixture *f) {
    ri_denied_read(NULL);
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        RiWitness *w = &f->witnesses[i];
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, ri_output, w};
        config.trace = ri_lifecycle; config.trace_context = w;
        CHECK(xr_xir_instance_new(f->program, &config, &w->instance) == XR_XIR_CALL_READY);
        XrXirValue argument = {XR_XIR_I64, 0, 21};
        CHECK(xr_xir_instance_start(w->instance, RI_CHILD, &argument, 1) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_NEW);
        CHECK(xr_xir_instance_poll_bounded(w->instance, 1).outcome.status == XR_XIR_CALL_BAD_STATE);
    }
    CHECK(f->witnesses[0].instance != f->witnesses[1].instance);
    f->witnesses[0].peer = f->witnesses[1].instance;
    f->witnesses[1].peer = f->witnesses[0].instance;
    xr_xir_compile_program_drop(f->program); f->program = NULL;
    CHECK(!f->code_releases);
}
#endif // XIR_ROOT_IDENTITY_OBSERVER_H
