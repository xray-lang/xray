/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_call_observation.c - Authenticated mixed frame and dispatch events
 *
 * KEY CONCEPT:
 *   Observe real generated C and VM bodies across root and child activations.
 */
#include "xir/xxir_call.h"
static XrXirAction observation_resume(XrXirCallView *view);
static void observation_release(XrXirCallView *view, XrXirCallStatus status);
#define CM_RESUME_OBSERVER observation_resume
#define CM_RELEASE_OBSERVER observation_release
#define main mixed_reference_main
#include "test_xir_child_callable_mixed.c"
#undef main

enum { OBS_FRAMES = 32 };
typedef struct ObservedFrame {
    XrXirCallEvent first;
    unsigned begins, ends, codes;
    XrXirCallCodeKind code;
    XrXirCallStatus leave;
    bool left, dispatch;
} ObservedFrame;
typedef struct ObservedInstance {
    XrXirInstance *instance;
    XrXirCallObserver observer;
    const XrXirCallView *view;
    ObservedFrame frames[OBS_FRAMES];
    unsigned count, events, outputs, installations, bodies[CN_FUNCTIONS];
    uint64_t bind_before, bind_work;
    XrXirCallStatus bind_status;
    bool installed;
} ObservedInstance;
static ObservedInstance observed[CN_INSTANCES];
static bool observe_late, observe_cancel, observe_bind_probe;
static ObservedInstance *observation_instance(const void *instance) {
    for (unsigned i = 0; i < CN_INSTANCES; ++i)
        if (observed[i].instance == instance) return &observed[i];
    CHECK(false); return NULL;
}
static bool observation_key(const XrXirCallEvent *left, const XrXirCallEvent *right) {
    return left->binding.owner == right->binding.owner && left->binding.activation == right->binding.activation &&
        left->binding.generation == right->binding.generation && left->binding.ticket == right->binding.ticket &&
        left->frame == right->frame;
}
static void observation_reentry(ObservedInstance *record, const XrXirCallEvent *event) {
    XrXirInstance *instance = record->instance;
    CHECK(instance->budget.notifying && !instance->observing);
    const size_t allocations = runtime_attempts;
    const uint64_t work = xr_xir_domain_budget_stats(instance->domain).work;
    const bool stopping = instance->stopping, executor_stopping = instance->executor->stopping;
    XrXirCall *call = ((XirTaskActivation *)event->binding.activation)->call;
    CHECK(call && xr_xir_call_executor_member(call, &event->binding));
    XrXirExecutorBinding forged = event->binding; ++forged.ticket;
    CHECK(!xr_xir_call_executor_member(call, &forged));
    CHECK(xr_xir_call_poll_bounded(call, 1).status == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_request_cancel(call) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_resume(call, 1) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_bind_executor(call, &event->binding) == XR_XIR_CALL_BUSY);
    XrXirValue value = {0}; XrXirCallResult outcome = {0};
    CHECK(xr_xir_call_take_result(call, &value) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_take_outcome(call, &outcome) == XR_XIR_CALL_BUSY);
    /* Reject every other live Call in the same budget, including a parked root. */
    for (XrXirCall *member = instance->budget.observed_calls; member; member = member->observed_next) {
        CHECK(xr_xir_call_free(member) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_call_request_cancel(member) == XR_XIR_CALL_BUSY);
    }
    CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_start(instance, CN_ROOT, NULL, 0) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_task_executor_stop(instance->executor) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_task_executor_free(instance->executor, NULL) == XR_XIR_CALL_BUSY);
    bool visited=false;
    CHECK(xir_call_vm_visit_ready(call,&event->binding,event->frame)==XR_XIR_CALL_BUSY);
    CHECK(xir_task_executor_visit_vm(instance->executor,&event->binding,event->frame,&visited)==XR_XIR_CALL_BUSY && !visited);
    CHECK(xir_instance_visit_vm(instance,&event->binding,event->frame,&visited)==XR_XIR_CALL_BUSY && !visited);
    if (record->view) {
        CHECK(!xr_xir_call_admission(record->view));
        CHECK(xr_xir_call_observer_bind(record->view, &record->observer) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_observer_vm(record->view) == XR_XIR_CALL_BAD_STATE);
    }
    CHECK(instance->stopping == stopping && instance->executor->stopping == executor_stopping);
    CHECK(!value.type && !value.reserved && !value.payload && xr_xir_call_result_empty(&outcome));
    CHECK(runtime_attempts == allocations && xr_xir_domain_budget_stats(instance->domain).work == work);
}
static void observation_event(void *owner, const XrXirCallEvent *event) {
    ObservedInstance *record = owner;
    CHECK(record && event && event->instance == record->instance);
    CHECK(event->binding.owner == record->instance->executor && event->binding.generation && event->binding.ticket);
    CHECK(event->frame && event->depth && event->entry <= CN_FUNCTIONS);
    observation_reentry(record, event); ++record->events;
    ObservedFrame *frame = NULL;
    for (unsigned i = 0; i < record->count; ++i)
        if (observation_key(&record->frames[i].first, event)) frame = &record->frames[i];
    if (event->kind == XR_XIR_CALL_FRAME_ENTER || event->kind == XR_XIR_CALL_FRAME_PRESENT) {
        CHECK(!frame && record->count < OBS_FRAMES);
        frame = &record->frames[record->count++]; frame->first = *event; frame->code = event->code;
        if (event->kind == XR_XIR_CALL_FRAME_PRESENT && record->view &&
            record->view->activation == ((XirTaskActivation *)event->binding.activation)->call &&
            record->view->activation->top->epoch == event->frame) frame->dispatch = true;
        return;
    }
    CHECK(frame && !frame->left && frame->first.entry == event->entry && frame->first.parent == event->parent &&
        frame->first.depth == event->depth);
    if (event->kind == XR_XIR_CALL_FRAME_CODE) {
        CHECK(frame->code == XR_XIR_CALL_CODE_UNKNOWN && event->code != XR_XIR_CALL_CODE_UNKNOWN && !frame->codes++);
        frame->code = event->code;
    } else if (event->kind == XR_XIR_CALL_DISPATCH_BEGIN) {
        CHECK(!frame->dispatch && event->code == frame->code && event->status == XR_XIR_CALL_READY);
        frame->dispatch = true; ++frame->begins;
    } else if (event->kind == XR_XIR_CALL_DISPATCH_END) {
        CHECK(frame->dispatch && event->code == frame->code && event->status == XR_XIR_CALL_READY);
        frame->dispatch = false; ++frame->ends;
    } else {
        CHECK(event->kind == XR_XIR_CALL_FRAME_LEAVE && !frame->dispatch && event->code == frame->code);
        for (unsigned i = 0; i < record->count; ++i) {
            ObservedFrame *child = &record->frames[i]; XrXirCallEvent key = child->first;
            key.frame = event->frame;
            if (child->first.parent == event->frame && observation_key(&key, event)) CHECK(child->left);
        }
        frame->left = true; frame->leave = event->status;
    }
}
static XrXirAction observation_resume(XrXirCallView *view) {
    uint32_t function = cm_function(view); ObservedInstance *record = observation_instance(view->instance);
    ++record->bodies[function]; CHECK(!record->view && xr_xir_call_admission(view)); record->view = view;
    XrXirCallView copy = *view;
    CHECK(xr_xir_call_observer_bind(&copy, &record->observer) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_observer_vm(&copy) == XR_XIR_CALL_BAD_STATE);
    if (!observe_late || function == CN_RELAY) {
        const uint64_t before = xr_xir_domain_budget_stats(record->instance->domain).work;
        XrXirCallStatus status = xr_xir_call_observer_bind(view, &record->observer);
        if (!record->installed) {
            record->bind_before = before; record->bind_status = status;
            record->bind_work = xr_xir_domain_budget_stats(record->instance->domain).work - before;
        }
        if (status != XR_XIR_CALL_READY) {
            CHECK(status == XR_XIR_CALL_LIMIT && !record->instance->budget.observer && !record->events);
            const uint64_t spent = xr_xir_domain_budget_stats(record->instance->domain).work;
            const size_t allocations = runtime_attempts;
            CHECK(xr_xir_call_observer_bind(view, &record->observer) == XR_XIR_CALL_LIMIT);
            CHECK(!record->instance->budget.observer && !record->events && runtime_attempts == allocations &&
                xr_xir_domain_budget_stats(record->instance->domain).work == spent);
            record->view = NULL;
            return xr_xir_call_fault(XR_XIR_RUN_FRAME_LIMIT);
        }
        if (!record->installed) { record->installed = true; ++record->installations; }
        const size_t allocations = runtime_attempts;
        const uint64_t work = xr_xir_domain_budget_stats(record->instance->domain).work;
        CHECK(xr_xir_call_observer_bind(view, &record->observer) == XR_XIR_CALL_READY);
        XrXirCallObserver foreign = record->observer;
        CHECK(xr_xir_call_observer_bind(view, &foreign) == XR_XIR_CALL_BAD_STATE);
        CHECK(runtime_attempts == allocations && xr_xir_domain_budget_stats(record->instance->domain).work == work);
    }
    if (observe_bind_probe) {
        record->view = NULL; return xr_xir_call_fault(XR_XIR_RUN_FRAME_LIMIT);
    }
    XrXirAction action = cm_observed->selected[function]->resume(view);
    CHECK(xr_xir_call_admission(view)); record->view = NULL; return action;
}
static void observation_release(XrXirCallView *view, XrXirCallStatus status) {
    CHECK(!xr_xir_call_admission(view));
    cm_observed->selected[cm_function(view)]->release(view, status);
}
static XrXirOutputStatus observation_output(void *owner, const XrXirOutputGroup *group) {
    ObservedInstance *record = owner;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1);
    CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == 42);
    ++record->outputs; return XR_XIR_OUTPUT_OK;
}
static void observation_verify(ObservedInstance *record, bool complete, unsigned runs) {
    unsigned vm = 0, native = 0, missing_begin = 0, child = 0, roots = 0;
    for (unsigned i = 0; i < record->count; ++i) {
        ObservedFrame *frame = &record->frames[i]; const XrXirCallEvent *event = &frame->first;
        CHECK(frame->left && !frame->dispatch && frame->ends >= frame->begins);
        missing_begin += frame->ends - frame->begins;
        if (event->parent) {
            const ObservedFrame *parent = NULL;
            for (unsigned p = 0; p < record->count; ++p) {
                XrXirCallEvent key = *event; key.frame = event->parent;
                if (observation_key(&record->frames[p].first, &key)) parent = &record->frames[p];
            }
            CHECK(parent && parent->first.depth + 1 == event->depth);
        } else CHECK(event->depth == 1);
        if (event->entry == CN_CHILD) { CHECK(event->depth == 1); ++child; }
        if (event->entry == CN_RELAY) CHECK(event->depth == 2);
        if (event->entry == CN_PURE) CHECK(event->depth == 3);
        if (event->entry == CN_ROOT) { CHECK(event->depth == 2); ++roots; }
        if (frame->code == XR_XIR_CALL_CODE_VM) ++vm;
        if (frame->code == XR_XIR_CALL_CODE_NATIVE) ++native;
        if (complete) {
            CHECK(frame->leave == XR_XIR_CALL_RETURNED);
            if (event->entry < CN_FUNCTIONS) CHECK(frame->code ==
                (cm_observed->providers[event->entry] == CM_VM ? XR_XIR_CALL_CODE_VM : XR_XIR_CALL_CODE_NATIVE));
        }
    }
    CHECK(missing_begin <= 1);
    if (complete) {
        CHECK(record->installed && record->installations == 1 && vm && native && child == runs && roots == runs);
        CHECK(record->bodies[CN_ROOT] == 3 * runs && record->bodies[CN_CHILD] == 5 * runs &&
            record->bodies[CN_RELAY] == 2 * runs && record->bodies[CN_PURE] == 2 * runs && record->outputs == runs);
        CHECK(record->count == (observe_late ? 5u : 6u) + (runs - 1) * 5u);
        /* Frame epochs may restart; scheduling tickets distinguish replacement roots. */
        if (runs == 2) {
            uint64_t tickets[2] = {0}; unsigned found = 0;
            for (unsigned i = 0; i < record->count; ++i) if (record->frames[i].first.entry == CN_ROOT)
                tickets[found++] = record->frames[i].first.binding.ticket;
            CHECK(found == 2 && tickets[0] < tickets[1]);
        }
    }
}
static bool observation_operation(CmFixture *fixture, bool consume_program,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost, unsigned runs) {
    memset(observed, 0, sizeof(observed)); bool complete = true;
    XrXirDomain *domains[CN_INSTANCES] = {0};
    for (unsigned i = 0; i < CN_INSTANCES; ++i) {
        ObservedInstance *record = &observed[i]; record->observer = (XrXirCallObserver){record, observation_event};
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        if (limits) config = *limits;
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, observation_output, record};
        XrXirCallStatus status = xr_xir_instance_new(fixture->base.program, &config, &record->instance);
        if (status != XR_XIR_CALL_READY) { CHECK(status == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM)); complete = false; }
        else { domains[i] = record->instance->domain; CHECK(xr_xir_domain_retain(domains[i])); }
    }
    if (consume_program) { xr_xir_compile_program_drop(fixture->base.program); fixture->base.program = NULL; }
    for (unsigned i = 0; i < CN_INSTANCES; ++i) if (observed[i].instance) {
        ObservedInstance *record = &observed[i]; bool done = true;
        for (unsigned run = 0; run < runs && done; ++run) {
            XrXirCallStatus status = xr_xir_instance_start(record->instance, CN_ROOT, NULL, 0);
            if (status == XR_XIR_CALL_READY) {
                XrXirInstanceResult result = {0}; unsigned polls = 0; bool cancelled = false;
                do {
                    result = xr_xir_instance_poll_bounded(record->instance, 1); CHECK(++polls < 32768);
                    for (unsigned f=0;f<record->count;++f) {
                        const ObservedFrame *frame=&record->frames[f];
                        if (frame->left || frame->code==XR_XIR_CALL_CODE_VM) continue;
                        XrXirCall *call=((const XirTaskActivation *)frame->first.binding.activation)->call;
                        if (call && call->top && call->top->epoch==frame->first.frame)
                            CHECK(xir_call_vm_visit_ready(call,&frame->first.binding,frame->first.frame)==XR_XIR_CALL_BAD_STATE);
                    }
                    if (observe_cancel && !cancelled && record->bodies[CN_CHILD]) {
                        CHECK(xr_xir_instance_cancel_current(record->instance) == XR_XIR_CALL_CANCEL_REQUESTED); cancelled = true;
                    }
                } while (result.outcome.status == XR_XIR_CALL_READY);
                if (result.outcome.status == XR_XIR_CALL_RETURNED) {
                    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(record->instance, &value) == XR_XIR_CALL_RETURNED);
                    CHECK(value.type == XR_XIR_I64 && value.payload == 42); xr_xir_value_drop(&value);
                } else {
                    CHECK(result.outcome.status == (observe_cancel ? XR_XIR_CALL_CANCELLED :
                        limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM)); done = false;
                }
            } else { CHECK(status == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM)); done = false; }
        }
        XrXirCallStatus status = xr_xir_instance_free(record->instance);
        CHECK(status == XR_XIR_CALL_READY || status == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM));
        if (status != XR_XIR_CALL_READY) done = false;
        observation_verify(record, done, runs); if (!done) complete = false;
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domains[i]);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
            spent.metadata_allocations == spent.metadata_frees && xr_xir_domain_stats(domains[i]).live_bytes == sizeof(*domains[i]));
        if (cost) { if (!i) *cost = spent; else CHECK(spent.work == cost->work &&
            spent.requested_bytes == cost->requested_bytes && spent.requested_call_bytes == cost->requested_call_bytes); }
        xr_xir_domain_drop(domains[i]);
    }
    CHECK(!runtime_live && !runtime_bytes); return complete;
}
static void observation_resource_axes(CmFixture *fixture) {
    XrXirDomainBudgetStats cost = {0}; CHECK(observation_operation(fixture, false, NULL, &cost, 1));
    const uint64_t exact[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(exact[axis]);
        if (!axis) config.requested_value_limit = exact[axis] - minus;
        else if (axis == 1) config.requested_call_limit = exact[axis] - minus;
        else config.work_limit = exact[axis] - minus;
        CHECK(observation_operation(fixture, false, &config, NULL, 1) == !minus);
    }
    printf("CALL_OBSERVATION_AXES value/call/work=%llu/%llu/%llu exact/minus1 physical=0/0\n",
        (unsigned long long)exact[0], (unsigned long long)exact[1], (unsigned long long)exact[2]);
}
static void observation_binding_work(CmFixture *fixture) {
    CHECK(observation_operation(fixture, false, NULL, NULL, 1));
    const uint64_t prefix = observed[0].bind_before, replay = observed[0].bind_work;
    CHECK(prefix && replay == 6 && observed[1].bind_before == prefix && observed[1].bind_work == replay);
    observe_bind_probe = true;
    for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.work_limit = prefix + replay - minus;
        CHECK(!observation_operation(fixture, false, &config, NULL, 1));
        for (unsigned i = 0; i < CN_INSTANCES; ++i) {
            CHECK(observed[i].bind_before == prefix);
            CHECK(observed[i].bind_status == (minus ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
            CHECK(observed[i].installed == !minus && observed[i].installations == (minus ? 0u : 1u));
            CHECK(observed[i].bind_work == replay - 2 * minus);
        }
    }
    observe_bind_probe = false;
    printf("CALL_OBSERVATION_BIND workPrefix=%llu replay=6 exact/minus1 atomicInstall retryNoRefund physical=0/0\n",
        (unsigned long long)prefix);
}
int main(int argc, char **argv) {
    (void)&cm_resume; (void)&cm_release;
    CHECK(argc == 3); bool native = !strcmp(argv[1], "native-vm"), fi = !strcmp(argv[2], "fi");
    CHECK(native || !strcmp(argv[1], "vm-native"));
    observe_late = !strcmp(argv[2], "late"); observe_cancel = !strcmp(argv[2], "cancel");
    bool binding = !strcmp(argv[2], "binding");
    CHECK(fi || observe_late || observe_cancel || binding || !strcmp(argv[2], "normal"));
    CmFixture fixture = {0}; cm_build(&fixture, native);
    if (binding) observation_binding_work(&fixture);
    else if (!observe_cancel) observation_resource_axes(&fixture);
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    CHECK(observation_operation(&fixture, !fi, NULL, NULL, fi || observe_cancel ? 1 : 2) == !observe_cancel);
    size_t sites = runtime_attempts;
    if (fi) for (size_t site = 0; site < sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site;
        CHECK(!observation_operation(&fixture, false, NULL, NULL, 1) && runtime_attempts > site);
    }
    runtime_fail_at = SIZE_MAX; if (fi) xr_xir_compile_program_drop(fixture.base.program);
    CHECK(fixture.base.code_releases == 1 && !fixture.lowered && !runtime_live && !runtime_bytes);
    cm_observed = NULL; cn_observed = NULL; effects_source_owners_free();
    CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("CALL_OBSERVATION %s %s sites=%zu twoInstances authenticFrames dispatch native-and-VM reentryDenied codeReleases=1 physical=0/0\n",
        argv[1], argv[2], sites); return 0;
}
