/* Fixed live storage rejection after all module initializers became READY. */
#include "xir/xxir_program.h"
#include "xir/xxir_vm.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "ordinary live %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#ifdef OLL_VM
#define OLL_SOURCE 1
#endif
#include "ordinary_live_limit_compile.h"
#include "ordinary_live_limit_generated.h"

static const uint64_t oll_call_cap = UINT64_C(10111);
static const uint32_t oll_module_mask = (UINT32_C(1) << OLL_MODULES) - 1;
typedef struct OllOwner OllOwner;
typedef struct OllBinding {
    XrXirVmBinding vm;
    XrXirCallEntry actual;
    OllOwner *owner;
    uint32_t function;
    bool native;
} OllBinding;
struct OllOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry *entries;
    OllBinding *bindings;
    OllFixture fixture;
    uintptr_t instance_ids[2];
    uint64_t steps[2][2], releases[2][2], crossings[2], calls[2], deepest[2];
    uint32_t code_releases;
};
typedef struct OllObservation { uint32_t begun, ready, groups; } OllObservation;
typedef struct OllLedger {
    XrXirDomainBudgetStats domain;
    XrXirDomainStats values;
    XrXirCallBudget call;
} OllLedger;

static OllLedger oll_ledger(XrXirInstance *instance) {
    CHECK(instance->budget.work_domain == instance->domain);
    return (OllLedger){xr_xir_domain_budget_stats(instance->domain), xr_xir_domain_stats(instance->domain), instance->budget};
}
static void oll_equal(XrXirInstance *instance, const OllLedger *before) {
    OllLedger after = oll_ledger(instance);
#define OLL_DOMAIN(field) CHECK(after.domain.field == before->domain.field)
    OLL_DOMAIN(requested_bytes); OLL_DOMAIN(requested_limit); OLL_DOMAIN(work); OLL_DOMAIN(work_limit); OLL_DOMAIN(bound);
    OLL_DOMAIN(requested_call_bytes); OLL_DOMAIN(requested_call_limit);
    OLL_DOMAIN(metadata_live); OLL_DOMAIN(metadata_peak); OLL_DOMAIN(metadata_limit);
    OLL_DOMAIN(metadata_allocations); OLL_DOMAIN(metadata_frees);
    OLL_DOMAIN(call_live); OLL_DOMAIN(call_peak); OLL_DOMAIN(call_limit); OLL_DOMAIN(call_allocations); OLL_DOMAIN(call_frees);
#undef OLL_DOMAIN
#define OLL_VALUE(field) CHECK(after.values.field == before->values.field)
    OLL_VALUE(live_bytes); OLL_VALUE(peak_bytes); OLL_VALUE(allocations); OLL_VALUE(frees); OLL_VALUE(reallocations);
#undef OLL_VALUE
#define OLL_CALL(field) CHECK(after.call.field == before->call.field)
    OLL_CALL(byte_limit); OLL_CALL(requested_limit); OLL_CALL(requested_bytes); OLL_CALL(live_bytes); OLL_CALL(peak_bytes);
    OLL_CALL(allocations); OLL_CALL(frees); OLL_CALL(resume_limit); OLL_CALL(resumes); OLL_CALL(transitions);
    OLL_CALL(release_tickets); OLL_CALL(released_frames); OLL_CALL(work_domain); OLL_CALL(exhausted);
#undef OLL_CALL
}
static void oll_no_refund(XrXirInstance *instance, XrXirDomain *domain, XrXirCallBudget *budget, const OllLedger *before) {
    OllLedger after = oll_ledger(instance);
    CHECK(instance->domain == domain && &instance->budget == budget && after.call.work_domain == domain);
    CHECK(after.domain.requested_bytes >= before->domain.requested_bytes && after.domain.work >= before->domain.work &&
        after.domain.requested_call_bytes >= before->domain.requested_call_bytes &&
        after.call.requested_bytes >= before->call.requested_bytes && after.call.resumes >= before->call.resumes);
    CHECK(after.domain.call_limit == oll_call_cap && after.call.byte_limit == oll_call_cap &&
        after.domain.requested_call_limit == before->domain.requested_call_limit &&
        after.domain.work_limit == before->domain.work_limit && after.call.resume_limit == before->call.resume_limit);
}
static void oll_trace(void *opaque, XrXirLifecycleEvent event, uint32_t module) {
    OllObservation *o = opaque; CHECK(module < OLL_MODULES); uint32_t bit = UINT32_C(1) << module;
    if (event == XR_XIR_MODULE_BEGIN) { CHECK(!(o->begun & bit) && !(o->ready & bit)); o->begun |= bit; }
    else { CHECK(event == XR_XIR_MODULE_READY && (o->begun & bit) && !(o->ready & bit)); o->ready |= bit; }
}
static XrXirOutputStatus oll_output(void *opaque, const XrXirOutputGroup *group) {
    OllObservation *o = opaque; CHECK(group); ++o->groups;
    return XR_XIR_OUTPUT_ERROR;
}
static unsigned oll_instance(OllOwner *owner, XrXirInstance *instance) {
    uintptr_t id = (uintptr_t)instance;
    for (unsigned i = 0; i < 2; ++i) {
        if (owner->instance_ids[i] == id) return i;
        if (!owner->instance_ids[i]) { owner->instance_ids[i] = id; return i; }
    }
    CHECK(false); return 0;
}
static XrXirAction oll_resume(XrXirCallView *view) {
    CHECK(view && view->environment && xr_xir_call_admission(view));
    OllBinding *binding = (OllBinding *)view->environment; OllOwner *owner = binding->owner;
    CHECK(owner && binding == &owner->bindings[binding->function] && !owner->code_releases);
    unsigned witness = oll_instance(owner, view->instance);
    ++owner->steps[witness][binding->native];
    if (binding->function == owner->fixture.run || binding->function == owner->fixture.recovery) {
        CHECK(view->instance->state == XR_XIR_INSTANCE_READY && view->instance->cursor == OLL_MODULES &&
            view->instance->current_module == UINT32_MAX);
        for (uint32_t m = 0; m < OLL_MODULES; ++m) CHECK(view->instance->ready[m] == 1);
    }
    if (view->instance->accounting[view->instance->epoch % 2].depth > owner->deepest[witness])
        owner->deepest[witness] = view->instance->accounting[view->instance->epoch % 2].depth;
    const void *environment = view->environment;
    XrXirAction action = binding->actual.resume(view);
    CHECK(view->environment == environment && xr_xir_call_admission(view));
    if (action.kind == XR_XIR_ACTION_CALL) {
        CHECK(action.callee < OLL_FUNCTIONS); ++owner->calls[witness];
        if (binding->native != owner->bindings[action.callee].native) ++owner->crossings[witness];
    }
    return action;
}
static void oll_release(XrXirCallView *view, XrXirCallStatus reason) {
    CHECK(view && view->environment && !xr_xir_call_admission(view));
    OllBinding *binding = (OllBinding *)view->environment;
    unsigned witness = oll_instance(binding->owner, view->instance);
    ++binding->owner->releases[witness][binding->native];
    const void *environment = view->environment;
    binding->actual.release(view, reason);
    CHECK(view->environment == environment);
}
static void oll_code_drop(void *opaque) {
    OllOwner *owner = opaque; CHECK(!owner->code_releases && owner->entries && owner->bindings);
    xr_xir_compile_artifact_free(owner->lowered); owner->lowered = NULL;
    xr_compile_resources_free(owner->entries); owner->entries = NULL;
    xr_compile_resources_free(owner->bindings); owner->bindings = NULL; ++owner->code_releases;
}
static XrXirProgram *oll_build(OllOwner *owner, const char *mode) {
    XrXirCompileContext context; oll_context(&context);
    XrXirProgramSpec spec = ordinary_live_limit_program;
    CHECK(spec.entry_count == OLL_FUNCTIONS && !spec.code.owner && !spec.code.release && spec.declarations->module_count == OLL_MODULES);
    owner->fixture = ordinary_live_limit_fixture;
#ifdef OLL_VM
    XrXirArtifact *checked = NULL;
    if (!strcmp(mode, "source") || !strcmp(mode, "vm")) checked = oll_source(&context);
    else CHECK(xr_xir_compile_checked_read(&context, spec.proof.bytes, spec.proof.length, &checked, NULL) == XR_XIR_OK);
    owner->lowered = oll_lower(&context, checked, &spec.target, &owner->fixture);
    CHECK(owner->fixture.entry == ordinary_live_limit_fixture.entry && owner->fixture.run == ordinary_live_limit_fixture.run &&
        owner->fixture.recovery == ordinary_live_limit_fixture.recovery && owner->fixture.root == ordinary_live_limit_fixture.root);
    XrXirProgramProof proof = xr_xir_compile_program_proof(owner->lowered);
    CHECK(proof.length == spec.proof.length && !memcmp(proof.bytes, spec.proof.bytes, proof.length) &&
        !memcmp(proof.identity, spec.proof.identity, 32));
    spec.proof = proof;
    const XrXirModule *module = xr_xir_compile_artifact_module(owner->lowered);
    spec.declarations = module->declarations; spec.types = module->types;
#else
    CHECK(!strcmp(mode, "native"));
#endif
    void *entries = NULL, *bindings = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, OLL_FUNCTIONS, sizeof(*owner->entries), &entries) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, OLL_FUNCTIONS, sizeof(*owner->bindings), &bindings) == XR_COMPILE_RESOURCE_OK);
    owner->entries = entries; owner->bindings = bindings;
    for (uint32_t f = 0; f < OLL_FUNCTIONS; ++f) {
        OllBinding *binding = &owner->bindings[f]; binding->owner = owner; binding->function = f; binding->native = true;
#ifdef OLL_VM
        if (!strcmp(mode, "source") || !strcmp(mode, "vm")) binding->native = false;
        else if (!strcmp(mode, "native-vm")) binding->native = f == owner->fixture.run;
        else { CHECK(!strcmp(mode, "vm-native")); binding->native = f != owner->fixture.run; }
        if (!binding->native) CHECK(xr_xir_compile_vm_bind(owner->lowered, f, &binding->vm, &binding->actual) == XR_XIR_OK);
        else
#endif
        { CHECK(!spec.entries[f].environment); binding->actual = spec.entries[f]; }
        CHECK(binding->actual.resume && binding->actual.release);
        owner->entries[f] = binding->actual; owner->entries[f].environment = &binding->vm;
        owner->entries[f].resume = oll_resume; owner->entries[f].release = oll_release;
    }
    spec.entries = owner->entries; spec.code = (XrXirCodeLease){owner, oll_code_drop};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    xr_compile_resources_release(context.resources); return program;
}
static void oll_value(const XrXirValue *value) {
    CHECK(value->type == XR_XIR_I64 && !value->reserved && value->payload == INT64_C(41) &&
        xr_xir_value_valid(value) && !xr_xir_value_arena(value));
}
static XrXirInstanceResult oll_drive(XrXirInstance *instance, XrXirInstance *peer, OllObservation *peer_output) {
    XrXirInstanceResult result; unsigned polls = 0;
    do {
        OllLedger before = oll_ledger(peer); OllObservation observed = *peer_output; uint64_t epoch = peer->epoch;
        result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls <= 16000);
        oll_equal(peer, &before); CHECK(peer->epoch == epoch && !memcmp(&observed, peer_output, sizeof(observed)));
    } while (result.outcome.status == XR_XIR_CALL_READY);
    CHECK(xr_xir_call_result_valid(&result.outcome)); return result;
}
static void oll_case(const char *mode, unsigned victim) {
    instance_compile_zero(); CHECK(!runtime_live && !runtime_bytes);
    OllOwner owner = {0}; XrXirProgram *program = oll_build(&owner, mode);
    XrXirInstance *instances[2] = {NULL, NULL}; OllObservation observations[2] = {{0}, {0}};
    XrXirValue peer_value = {0}, recovered_value = {0}; unsigned peer = 1u - victim;
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.metadata_limit = config.value_limit = UINT64_C(65536); config.call_limit = oll_call_cap;
        config.poll_limit = 8000; config.depth_limit = 96;
        config.requested_value_limit = config.requested_call_limit = UINT64_C(67108864); config.work_limit = UINT64_C(128000000);
        config.trace = oll_trace; config.trace_context = &observations[i];
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, oll_output, &observations[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        owner.instance_ids[i] = (uintptr_t)instances[i];
    }
    xr_xir_compile_program_drop(program); CHECK(!owner.code_releases);
    CHECK(xr_xir_instance_start(instances[peer], owner.fixture.recovery, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(oll_drive(instances[peer], instances[victim], &observations[victim]).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instances[peer], &peer_value) == XR_XIR_CALL_RETURNED); oll_value(&peer_value);
    XrXirInstance *identity = instances[victim]; XrXirDomain *domain = identity->domain; XrXirCallBudget *budget = &identity->budget;
    OllLedger peer_before = oll_ledger(instances[peer]); OllObservation peer_observed = observations[peer];
    CHECK(xr_xir_instance_start(identity, owner.fixture.entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult initialized = oll_drive(identity, instances[peer], &observations[peer]);
    CHECK(initialized.outcome.status == XR_XIR_CALL_RETURNED && initialized.epoch == 1 &&
        initialized.outcome.value.type == XR_XIR_I64 && !initialized.outcome.value.reserved && !initialized.outcome.value.payload);
    XrXirValue initialization_value = {0};
    CHECK(xr_xir_instance_take_result(identity, &initialization_value) == XR_XIR_CALL_RETURNED &&
        initialization_value.type == XR_XIR_I64 && !initialization_value.reserved && !initialization_value.payload);
    xr_xir_value_drop(&initialization_value);
    CHECK(identity->state == XR_XIR_INSTANCE_READY && identity->epoch == 1 && !budget->exhausted &&
        observations[victim].begun == oll_module_mask && observations[victim].ready == oll_module_mask && !observations[victim].groups);
    printf("ORDINARY_LIVE_LIMIT_INITIALIZED mode=%s victim=%u cap=10111 modules=%u ready=%u epoch=1 result=0\n",
        mode, victim, OLL_MODULES, observations[victim].ready);
    CHECK(xr_xir_instance_start(identity, owner.fixture.run, NULL, 0) == XR_XIR_CALL_READY);
    uint64_t epoch = identity->epoch; CHECK(epoch == 2);
    OllLedger started = oll_ledger(identity);
    XrXirInstanceResult limited = oll_drive(identity, instances[peer], &observations[peer]);
    CHECK(limited.outcome.status == XR_XIR_CALL_LIMIT && limited.epoch == epoch && identity->epoch == epoch &&
        identity->state == XR_XIR_INSTANCE_READY && !budget->exhausted);
    oll_no_refund(identity, domain, budget, &started);
    CHECK(budget->requested_bytes > started.call.requested_bytes && budget->resumes > started.call.resumes);
    CHECK(owner.calls[victim] >= 3 && owner.deepest[victim] >= 4);
#ifdef OLL_VM
    if (!strcmp(mode, "source") || !strcmp(mode, "vm")) {
        CHECK(owner.steps[victim][0] && owner.releases[victim][0] && !owner.steps[victim][1] && !owner.releases[victim][1]);
    } else CHECK(owner.steps[victim][0] && owner.steps[victim][1] && owner.releases[victim][0] &&
        owner.releases[victim][1] && owner.crossings[victim]);
#else
    CHECK(owner.steps[victim][1] && owner.releases[victim][1] && !owner.steps[victim][0] && !owner.releases[victim][0]);
#endif
    CHECK(observations[victim].begun == oll_module_mask && observations[victim].ready == oll_module_mask && !observations[victim].groups);
    for (uint32_t m = 0; m < OLL_MODULES; ++m) CHECK(identity->ready[m] == 1);
    OllLedger before = oll_ledger(identity); size_t physical = runtime_live, bytes = runtime_bytes, attempts = runtime_attempts;
    XrXirValue occupied = {XR_XIR_I64, 0, INT64_C(54321)}, saved = occupied, empty = {0};
    CHECK(xr_xir_instance_take_result(identity, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &saved, sizeof(saved)));
    CHECK(xr_xir_instance_take_result(identity, &empty) == XR_XIR_CALL_BAD_STATE && !empty.type && !empty.reserved && !empty.payload);
    XrXirCallResult failure = {0}; CHECK(xr_xir_instance_copy_failure(identity, &failure) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&failure));
    XrXirValue invalid_argument = {XR_XIR_I64, 0, INT64_C(123)};
    CHECK(xr_xir_instance_start(identity, owner.fixture.recovery, &invalid_argument, 1) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(identity->epoch == epoch && identity->state == XR_XIR_INSTANCE_READY && !budget->exhausted &&
        invalid_argument.type == XR_XIR_I64 && !invalid_argument.reserved && invalid_argument.payload == INT64_C(123));
    XrXirInstanceResult repeat = xr_xir_instance_poll_bounded(identity, 1);
    CHECK(repeat.outcome.status == XR_XIR_CALL_LIMIT && repeat.epoch == epoch);
    oll_equal(identity, &before); CHECK(runtime_attempts == attempts && runtime_live == physical && runtime_bytes == bytes);
    OllObservation victim_observed = observations[victim];
    XrXirCallStatus start = xr_xir_instance_start(identity, owner.fixture.recovery, NULL, 0);
    bool recovered = start == XR_XIR_CALL_READY;
    if (recovered) {
        CHECK(identity->epoch == epoch + 1 && identity->state == XR_XIR_INSTANCE_READY && !budget->exhausted);
        XrXirInstanceResult result = oll_drive(identity, instances[peer], &observations[peer]);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && result.epoch == epoch + 1); oll_value(&result.outcome.value);
        CHECK(xr_xir_instance_take_result(identity, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &saved, sizeof(saved)));
        CHECK(xr_xir_instance_take_result(identity, &recovered_value) == XR_XIR_CALL_RETURNED); oll_value(&recovered_value);
    } else CHECK(identity->epoch == epoch && identity->state == XR_XIR_INSTANCE_READY);
    oll_no_refund(identity, domain, budget, &before);
    CHECK(instances[victim] == identity && !memcmp(&observations[victim], &victim_observed, sizeof(victim_observed)));
    oll_equal(instances[peer], &peer_before); oll_value(&peer_value);
    CHECK(!memcmp(&observations[peer], &peer_observed, sizeof(peer_observed)) && instances[peer]->epoch == 1);
    printf("ORDINARY_LIVE_LIMIT mode=%s victim=%u cap=10111 modules=%u ready=%u live_limit=%u init_epoch=%llu recovery_start=%u recovery_epoch=%llu exhausted=%u refund=0 expected_recovery=41\n",
        mode, victim, OLL_MODULES, observations[victim].ready, (unsigned)limited.outcome.status, (unsigned long long)epoch,
        (unsigned)start, (unsigned long long)identity->epoch, (unsigned)budget->exhausted);
    for (unsigned w = 0; w < 2; ++w) printf("ORDINARY_LIVE_LIMIT_PROVIDER mode=%s witness=%u native=%llu vm=%llu calls=%llu depth=%llu crossings=%llu native_releases=%llu vm_releases=%llu\n",
        mode, w, (unsigned long long)owner.steps[w][1], (unsigned long long)owner.steps[w][0], (unsigned long long)owner.calls[w],
        (unsigned long long)owner.deepest[w], (unsigned long long)owner.crossings[w],
        (unsigned long long)owner.releases[w][1], (unsigned long long)owner.releases[w][0]);
    XrXirCallStatus stopped = xr_xir_instance_stop(identity), freed = xr_xir_instance_free(identity);
    CHECK(xr_xir_instance_stop(instances[peer]) == XR_XIR_CALL_READY && xr_xir_instance_free(instances[peer]) == XR_XIR_CALL_READY);
    CHECK(owner.code_releases == 1 && !owner.lowered && !owner.entries && !owner.bindings && !runtime_live && !runtime_bytes);
    oll_value(&peer_value); if (recovered) oll_value(&recovered_value);
    xr_xir_value_drop(&peer_value); xr_xir_value_drop(&recovered_value);
    instance_compile_zero();
    printf("ORDINARY_LIVE_LIMIT_FINAL mode=%s victim=%u recovered=%u stop=%u free=%u code_release=1 physical=0/0,0/0\n", mode, victim, (unsigned)recovered, (unsigned)stopped, (unsigned)freed);
    /* The oracle stays fixed even when a replacement cannot fit the original cap. */
    CHECK(recovered && stopped == XR_XIR_CALL_READY && freed == XR_XIR_CALL_READY);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); oll_case(argv[1], 0); oll_case(argv[1], 1); return 0;
}
