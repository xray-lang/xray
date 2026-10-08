/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_cancel_cleanup.c - Cancellation-only initializer cleanup output
 *
 * KEY CONCEPT:
 *   Observe actual entry resumes without changing authenticated call views.
 *   An owned completion Cell suppresses cleanup output only after normal resumption.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_output.h"
#include "base/xsha256.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "module_cancel_cleanup_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22 && XR_XIR_CALL_ABI_VERSION == 28 &&
    XR_XIR_PROGRAM_ABI_VERSION == 29, "Current execution identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_I64 == 2 && XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33 && XR_XIR_PRINT == 24 && XR_XIR_SUSPEND == 29,
    "Independent module execution model");
_Static_assert(XR_XIR_CELL_NEW == 59 && XR_XIR_CELL_READ == 60 && XR_XIR_CELL_WRITE == 61 &&
    XR_XIR_CLEANUP_REGISTER == 108 && XR_XIR_TYPE_CELL == 3, "Independent cleanup model");

#if defined(XR_MODULE_CLEANUP_NATIVE)
extern const XrXirCallEntry source_module_cancel_cleanup_0_entries[6], source_module_cancel_cleanup_1_entries[6];
extern const XrXirCallEntry source_module_cancel_cleanup_2_entries[6], source_module_cancel_cleanup_3_entries[6], source_module_cancel_cleanup_4_entries[6];
extern const char source_module_cancel_cleanup_0_sha[65], source_module_cancel_cleanup_1_sha[65];
extern const char source_module_cancel_cleanup_2_sha[65], source_module_cancel_cleanup_3_sha[65], source_module_cancel_cleanup_4_sha[65];
static const XrXirCallEntry *native_entries(unsigned graph) {
    const XrXirCallEntry *entries[] = {source_module_cancel_cleanup_0_entries, source_module_cancel_cleanup_1_entries,
        source_module_cancel_cleanup_2_entries, source_module_cancel_cleanup_3_entries, source_module_cancel_cleanup_4_entries};
    CHECK(graph < 5); return entries[graph];
}
static const char *native_digest(unsigned graph) {
    const char *digests[] = {source_module_cancel_cleanup_0_sha, source_module_cancel_cleanup_1_sha,
        source_module_cancel_cleanup_2_sha, source_module_cancel_cleanup_3_sha, source_module_cancel_cleanup_4_sha};
    CHECK(graph < 5); return digests[graph];
}
#else
static const XrXirCallEntry *native_entries(unsigned graph) { (void)graph; return NULL; }
#endif

typedef struct ModuleCodeOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry actual[6], observed[6];
    XrXirVmBinding bindings[6];
    bool native[6];
} ModuleCodeOwner;
static ModuleCodeOwner *active_owner;
static unsigned active_instance, releases, failures;
static unsigned lifecycle_count[2];
static uint32_t trace[2][8], trace_count[2];
static size_t resumed[2];
static unsigned cleanup_calls[2], cleanup_returns[2];

static XrXirAction observed_resume(XrXirCallView *view) {
    uint32_t id = xr_xir_call_current_entry(view->activation);
    CHECK(active_owner && id < 6 && active_instance < 2 && trace_count[active_instance] < 8);
    const XrXirCallEntry *entry = &active_owner->actual[id];
    CHECK(view->environment == entry->environment);
    ++resumed[active_owner->native[id] ? 1 : 0];
    XrXirAction action = entry->resume(view);
    if (action.kind == XR_XIR_ACTION_CALL && action.callee == 5) {
        CHECK(id == 1 && (action.flags & XR_XIR_ACTION_CLEANUP)); ++cleanup_calls[active_instance];
    }
    if (action.kind == XR_XIR_ACTION_RETURN) {
        if (id == 5) ++cleanup_returns[active_instance];
        else trace[active_instance][trace_count[active_instance]++] = id;
    }
    return action;
}

static void observe_lifecycle(void *context, XrXirLifecycleEvent event, uint32_t index) {
    unsigned instance = *(const unsigned *)context;
    CHECK(instance < 2 && instance == active_instance && lifecycle_count[instance] < 8);
    unsigned cursor = lifecycle_count[instance]++;
    CHECK(event == (cursor % 2 ? XR_XIR_MODULE_READY : XR_XIR_MODULE_BEGIN));
    CHECK(index == cursor / 2);
}

static void release_code(void *pointer) {
    ModuleCodeOwner *owner = pointer;
    CHECK(owner == active_owner && !releases);
    xr_xir_compile_artifact_free(owner->lowered);
    active_owner = NULL; xr_compile_resources_free(owner); ++releases;
}

static XrXirArtifact *read_lower(const XrXirCompileContext *context, unsigned graph) {
    const ModuleCancelCleanupCase *test = &module_cancel_cleanup_cases[graph];
    uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(context, input, test->length, &checked, NULL) == XR_XIR_OK);
    CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    CHECK(module && module->function_count == 6 && module->declarations);
    CHECK(module->declarations->module_count == 4 && module->declarations->entry_function == 4);
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(!module->functions[i].parameter_count);
        CHECK(module->functions[i].result == (i == 4 ? XR_XIR_I64 : XR_XIR_UNIT));
    }
    CHECK(module->types && module->types->count == 1 && module->types->nodes[0].kind == XR_XIR_TYPE_CELL);
    CHECK(module->types->nodes[0].element == XR_XIR_BOOL && module->declarations->functions[5].cleanup_owner == 2);
    CHECK(module->functions[5].parameter_count == 1 && module->functions[5].parameters[0] == 256);
    CHECK(module->functions[5].result == XR_XIR_UNIT);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    return lowered;
}

static void emit_source(XrXirArtifact *lowered, unsigned graph, XrXirCSource *source, char digest[65]) {
    char symbol[48]; CHECK(snprintf(symbol, sizeof(symbol), "source_module_cancel_cleanup_%u", graph) > 0);
    CHECK(xr_xir_compile_emit_c(lowered, symbol, 1048576, source) == XR_XIR_OK);
    XrXirCSource repeated = {0};
    CHECK(xr_xir_compile_emit_c(lowered, symbol, 1048576, &repeated) == XR_XIR_OK);
    CHECK(source->text != repeated.text && source->length == repeated.length);
    CHECK(!memcmp(source->text, repeated.text, source->length)); xr_xir_compile_c_source_free(&repeated);
    uint8_t hash[32]; xr_sha256((const uint8_t *)source->text, source->length, hash);
    for (size_t i = 0; i < 32; ++i) {
        digest[2*i] = "0123456789abcdef"[hash[i] >> 4]; digest[2*i+1] = "0123456789abcdef"[hash[i] & 15];
    }
    digest[64] = 0;
    printf("module-cancel-cleanup graph=%u generated-bytes=%zu sha256=%s deterministic=1\n", graph, source->length, digest);
}

typedef struct OutputCapture {
    XrXirInstance *instance;
    unsigned groups, action, reentrant_rejections;
    size_t length;
    char bytes[8];
} OutputCapture;
static const char expected_output[] = "1\n2\n3\n4\n";
static const char cancelled_output[] = "1\n2\n2\n";

static XrXirOutputStatus capture_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    OutputCapture *capture = context;
    CHECK(stream == XR_XIR_STDOUT && length == 2 && capture->length + length <= sizeof(capture->bytes));
    const char *expected = capture->action ? cancelled_output : expected_output;
    CHECK(!memcmp(bytes, expected + capture->length, length));
    if (capture->action >= 3 && capture->groups == 3) return XR_XIR_OUTPUT_ERROR;
    memcpy(capture->bytes + capture->length, bytes, length); capture->length += length;
    return XR_XIR_OUTPUT_OK;
}

static XrXirOutputStatus capture_group(void *context, const XrXirOutputGroup *group) {
    XrXirOutputSink *sink = context; OutputCapture *capture = sink->context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && group->values);
    CHECK(capture->groups < 4 && group->values[0].type == XR_XIR_I64 &&
        !group->values[0].reserved && group->values[0].payload == (capture->action && capture->groups == 2 ? 2 : (int64_t)capture->groups + 1));
    ++capture->groups;
    CHECK(xr_xir_instance_start(capture->instance, 4, NULL, 0) == XR_XIR_CALL_BUSY);
    ++capture->reentrant_rejections;
    return xr_xir_output_render(sink, group);
}

static XrXirInstanceResult poll_terminal(XrXirInstance *instance) {
    XrXirInstanceResult result = {0}; size_t polls = 0;
    do { CHECK(++polls < 32); result = xr_xir_instance_poll_bounded(instance, 10000); }
    while (result.outcome.status == XR_XIR_CALL_READY);
    return result;
}

static void wait_authority(XrXirInstance *instance, XrXirInstanceResult pending) {
    CHECK(pending.outcome.status == XR_XIR_CALL_SUSPENDED && pending.epoch && pending.outcome.wake);
    CHECK(pending.epoch < UINT64_MAX && pending.outcome.wake < UINT64_MAX);
    XrXirWaitRequest request = {0};
    CHECK(xr_xir_instance_wait_request(instance, pending.epoch, pending.outcome.wake, &request) == XR_XIR_CALL_READY);
    CHECK(request.kind == XR_XIR_WAIT_YIELD && !request.reserved && !request.after_ms &&
        !request.subject && !request.generation && !request.ticket);
    XrXirWaitRequest sentinel, before; memset(&sentinel, 0xa5, sizeof(sentinel)); memcpy(&before, &sentinel, sizeof(before));
    CHECK(xr_xir_instance_wait_request(instance, pending.epoch+1, pending.outcome.wake, &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, &before, sizeof(sentinel)));
    CHECK(xr_xir_instance_wait_request(instance, pending.epoch, pending.outcome.wake+1, &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, &before, sizeof(sentinel)));
    CHECK(xr_xir_instance_resume(instance, pending.epoch+1, pending.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, pending.epoch, pending.outcome.wake+1) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_start(instance, 4, NULL, 0) == XR_XIR_CALL_BUSY);
    XrXirInstanceResult repeated = poll_terminal(instance);
    CHECK(repeated.outcome.status == XR_XIR_CALL_SUSPENDED && repeated.epoch == pending.epoch &&
        repeated.outcome.wake == pending.outcome.wake);
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_INITIALIZING);
    CHECK(xr_xir_instance_wait_request(instance, pending.epoch, pending.outcome.wake, &request) == XR_XIR_CALL_READY);
    CHECK(request.kind == XR_XIR_WAIT_YIELD && !request.reserved && !request.after_ms &&
        !request.subject && !request.generation && !request.ticket);
}

static void stop_pending(XrXirInstance *instance, XrXirInstanceResult pending,
    const OutputCapture *capture, unsigned graph, unsigned mode) {
    XrXirCallStatus expected = capture->action == 3 ? XR_XIR_CALL_OUTPUT_ERROR : XR_XIR_CALL_READY;
    XrXirCallStatus actual = xr_xir_instance_stop(instance);
    CHECK(actual == expected && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_DRAINING);
    CHECK(capture->groups == 3 && capture->length == (capture->action == 3 ? 4u : 6u));
    CHECK(!memcmp(capture->bytes, cancelled_output, capture->length));
    size_t attempts = runtime_attempts;
    XrXirWaitRequest sentinel, before; memset(&sentinel, 0xa5, sizeof(sentinel)); memcpy(&before, &sentinel, sizeof(before));
    CHECK(xr_xir_instance_start(instance, 4, NULL, 0) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, pending.epoch, pending.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_wait_request(instance, pending.epoch, pending.outcome.wake, &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, &before, sizeof(sentinel)));
    CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_BAD_STATE);
    CHECK(runtime_attempts == attempts);
    printf("module-stop scenario=%u mode=%u status=%u state=DRAINING groups=3 bytes=%zu admission-revoked=4 sentinel=preserved new-allocations=0 result=PASS\n",
        graph, mode, actual, capture->length);
}

static void execute(const XrXirCompileContext *context, XrXirArtifact *lowered, unsigned graph, unsigned mode, bool stop) {
    unsigned failures_before = failures;
    const XrXirCallEntry *native = native_entries(graph); if (mode) CHECK(native);
    memset(trace, 0, sizeof(trace)); memset(trace_count, 0, sizeof(trace_count));
    memset(resumed, 0, sizeof(resumed)); memset(lifecycle_count, 0, sizeof(lifecycle_count)); releases = 0;
    ModuleCodeOwner *owner = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    owner->lowered = lowered;
    memset(cleanup_calls, 0, sizeof(cleanup_calls)); memset(cleanup_returns, 0, sizeof(cleanup_returns));
    for (uint32_t f = 0; f < 6; ++f) {
        owner->native[f] = mode == 1 || (mode == 2 && (f == 0 || f == 2 || f == 5)) || (mode == 3 && (f == 1 || f == 3 || f == 4));
        if (owner->native[f]) owner->actual[f] = native[f];
        else CHECK(xr_xir_compile_vm_bind(lowered, f, &owner->bindings[f], &owner->actual[f]) == XR_XIR_OK);
        CHECK(owner->actual[f].parameter_count == (f == 5 ? 1u : 0u));
        CHECK(owner->actual[f].result == (f == 4 ? XR_XIR_I64 : XR_XIR_UNIT));
        owner->observed[f] = owner->actual[f]; owner->observed[f].resume = observed_resume;
    }
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        owner->observed, 6, module->declarations, {owner, release_code}, module->types, xr_xir_compile_program_proof(lowered)};
    XrXirProgram *program = NULL; CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK); active_owner = owner;
    XrXirInstanceConfig config = {0}; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instances[2] = {NULL, NULL}; unsigned instance_ids[] = {0, 1};
    OutputCapture captures[2] = {0}; XrXirOutputSink sinks[2] = {0}; XrXirInstanceResult pending[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        captures[i].action = module_cancel_cleanup_cases[graph].action[i];
        sinks[i] = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, capture_bytes, &captures[i], 8};
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, capture_group, &sinks[i]};
        config.trace = observe_lifecycle; config.trace_context = &instance_ids[i];
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY); captures[i].instance = instances[i];
    }
    CHECK(instances[0] != instances[1]); xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        active_instance = i; CHECK(xr_xir_instance_start(instances[i], 4, NULL, 0) == XR_XIR_CALL_READY);
        pending[i] = poll_terminal(instances[i]); wait_authority(instances[i], pending[i]);
        CHECK(captures[i].groups == 2 && captures[i].length == 4 && !memcmp(captures[i].bytes, "1\n2\n", 4));
        CHECK(lifecycle_count[i] == 3 && trace_count[i] == 1 && trace[i][0] == 0);
        printf("module-cancel-cleanup scenario=%u mode=%u instance=%u pending=YIELD all-wait-fields=exact bad-epoch-wake=4 sentinel=preserved start=BUSY prefix=310a320a lifecycle=3 result=PASS\n", graph, mode, i);
    }
    XrXirCallResult retained_failure = {0}; XrXirValue retained_values[8] = {0}; size_t successes = 0;
    for (unsigned repeat = 0; repeat < 4; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        active_instance = i; unsigned action = module_cancel_cleanup_cases[graph].action[i];
        if (action == 2 || action == 4) {
            if (!repeat) {
                XrXirCallStatus expected_free = action == 4 ? XR_XIR_CALL_OUTPUT_ERROR : XR_XIR_CALL_READY;
                XrXirCallStatus actual_free = xr_xir_instance_free(instances[i]);
                CHECK(actual_free != XR_XIR_CALL_BUSY); instances[i] = NULL;
                /* Keep the cleanup error contract and release the surviving peer before failing. */
                if (actual_free != expected_free) ++failures;
                printf("module-cancel-cleanup scenario=%u mode=%u destroy-status=%u expected=%u result=%s\n",
                    graph, mode, actual_free, expected_free, actual_free == expected_free ? "PASS" : "FAIL");
            }
            CHECK(!releases && active_owner && captures[i].groups == 3 && captures[i].length == (action == 4 ? 4u : 6u) && lifecycle_count[i] == 3);
            continue;
        }
        XrXirCallStatus expected = action == 3 ? XR_XIR_CALL_OUTPUT_ERROR : action ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED;
        size_t attempts = runtime_attempts;
        if (!repeat) {
            if (stop && action) stop_pending(instances[i], pending[i], &captures[i], graph, mode);
            else if (action) CHECK(xr_xir_instance_cancel_current(instances[i]) == XR_XIR_CALL_CANCEL_REQUESTED);
            else CHECK(xr_xir_instance_resume(instances[i], pending[i].epoch, pending[i].outcome.wake) == XR_XIR_CALL_READY);
        } else CHECK(xr_xir_instance_start(instances[i], 4, NULL, 0) == (action ? expected : XR_XIR_CALL_READY));
        XrXirInstanceResult result = poll_terminal(instances[i]);
        CHECK(result.outcome.status == expected);
        CHECK(xr_xir_instance_resume(instances[i], pending[i].epoch, pending[i].outcome.wake) == XR_XIR_CALL_BAD_STATE);
        if (action) {
            CHECK(xr_xir_instance_state(instances[i]) == (stop ? XR_XIR_INSTANCE_DRAINING : XR_XIR_INSTANCE_FAILED) && captures[i].groups == 3 && captures[i].length == (action == 3 ? 4u : 6u));
            CHECK(lifecycle_count[i] == 3 && trace_count[i] == 1);
            if (repeat) CHECK(runtime_attempts == attempts);
            else CHECK(xr_xir_instance_copy_failure(instances[i], &retained_failure) == expected);
            XrXirValue sentinel = {XR_XIR_I64, 0, 99};
            CHECK(xr_xir_instance_take_result(instances[i], &sentinel) == XR_XIR_CALL_BAD_STATE);
            CHECK(sentinel.type == XR_XIR_I64 && !sentinel.reserved && sentinel.payload == 99);
        } else {
            CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_READY && captures[i].groups == 4 && captures[i].length == 8);
            CHECK(lifecycle_count[i] == 8 && trace_count[i] == 5+repeat);
            for (unsigned j = 0; j < 4; ++j) CHECK(trace[i][j] == j);
            for (unsigned j = 4; j < trace_count[i]; ++j) CHECK(trace[i][j] == 4);
            CHECK(xr_xir_instance_take_result(instances[i], &retained_values[successes]) == XR_XIR_CALL_RETURNED);
            CHECK(retained_values[successes].type == XR_XIR_I64 && retained_values[successes].payload == 42); ++successes;
        }
        CHECK(!memcmp(captures[i].bytes, action ? cancelled_output : expected_output, captures[i].length));
        printf("module-cancel-cleanup scenario=%u mode=%u instance=%u repeat=%u action=%u status=%u groups=%u bytes=%zu lifecycle=%u result=PASS\n",
            graph, mode, i, repeat, action, result.outcome.status, captures[i].groups, captures[i].length, lifecycle_count[i]);
    }
    if (!mode) CHECK(resumed[0] && !resumed[1]);
    else if (mode == 1) CHECK(resumed[1] && !resumed[0]);
    else CHECK(resumed[0] && resumed[1]);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(cleanup_calls[i] == 1 && cleanup_returns[i] == (captures[i].action >= 3 ? 0u : 1u));
        CHECK(captures[i].groups == (captures[i].action ? 3u : 4u));
        CHECK(!memcmp(captures[i].bytes, captures[i].action ? cancelled_output : expected_output, captures[i].length));
    }
    for (unsigned i = 0; i < 2; ++i) if (instances[i]) {
        active_instance = i; CHECK(!releases && active_owner); CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(releases == 1 && !active_owner);
    if (graph == 1 || graph == 3) {
        XrXirCallStatus expected = graph == 3 ? XR_XIR_CALL_OUTPUT_ERROR : XR_XIR_CALL_CANCELLED;
        CHECK(retained_failure.status == expected && xr_xir_call_result_valid(&retained_failure));
        XrXirCallResult copy = {0}; CHECK(xr_xir_call_result_copy(&retained_failure, &copy) == XR_XIR_VALUE_OK);
        CHECK(copy.status == expected); xr_xir_call_result_drop(&copy); xr_xir_call_result_drop(&retained_failure);
        CHECK(xr_xir_call_result_empty(&copy) && xr_xir_call_result_empty(&retained_failure));
    }
    for (size_t i = 0; i < successes; ++i) {
        CHECK(retained_values[i].type == XR_XIR_I64 && !retained_values[i].reserved && retained_values[i].payload == 42);
        xr_xir_value_drop(&retained_values[i]); CHECK(!retained_values[i].type && !retained_values[i].payload);
    }
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    printf("module-cancel-cleanup scenario=%u mode=%u retained-failure=%u retained-values=%zu destroyed-suspended=%u owners-dead=1 vm-resumes=%zu native-resumes=%zu cleanup-calls=2 cleanup-returns=%u physical=0/0 code-release=1 result=%s\n",
        graph, mode, (graph == 1 || graph == 3) ? 1u : 0u, successes, (graph == 2 || graph == 4) ? 1u : 0u, resumed[0], resumed[1], cleanup_returns[0]+cleanup_returns[1], failures == failures_before ? "PASS" : "FAIL");
}

int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    const char *choice = argv[1]; bool stop = !strncmp(choice, "stop-", 5); if (stop) choice += 5;
    bool emit = !strcmp(choice, "emit") && !stop;
    unsigned mode = !strcmp(choice, "vm") ? 0 : !strcmp(choice, "native") ? 1 :
        !strcmp(choice, "alternating-native") ? 2 : !strcmp(choice, "alternating-vm") ? 3 : 4;
    CHECK((emit && argc == 3) || (!emit && argc == 2 && mode < 4));
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    XrCompileResourceLimits limits = {67108864, 8388608, 128000000};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    for (unsigned graph = 0; graph < 5; ++graph) {
        if (stop && graph != 1 && graph != 3) continue;
        XrXirArtifact *lowered = read_lower(&context, graph);
        XrXirCSource source = {0}; char digest[65]; emit_source(lowered, graph, &source, digest);
        if (emit) {
            char path[2048]; int size = snprintf(path, sizeof(path), "%s/graph%u.c", argv[2], graph);
            CHECK(size > 0 && (size_t)size < sizeof(path)); FILE *file = fopen(path, "wb"); CHECK(file);
            CHECK(fwrite(source.text, 1, source.length, file) == source.length);
            CHECK(fprintf(file, "\nconst char source_module_cancel_cleanup_%u_sha[65]=\"%s\";\n", graph, digest) > 0);
            CHECK(fclose(file) == 0); xr_xir_compile_artifact_free(lowered);
        } else {
#if defined(XR_MODULE_CLEANUP_NATIVE)
            if (mode) CHECK(!strcmp(digest, native_digest(graph)));
#endif
            execute(&context, lowered, graph, mode, stop);
        }
        xr_xir_compile_c_source_free(&source);
    }
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    printf("module-cancel-cleanup allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 "\n",
        stats.allocated_bytes, stats.peak_bytes, stats.work);
    xr_compile_resources_release(context.resources); instance_compile_report();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    puts("module-cancel-cleanup release compiler=0/0 runtime=0/0 table=0 result=PASS");
    printf("module-cancel-cleanup failures=%u result=%s\n", failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
