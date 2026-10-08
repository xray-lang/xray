/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_slot_state.c - Typed state replacement and detached owned results
 *
 * KEY CONCEPT:
 *   Observe actual entry resumes without changing authenticated call views.
 *   Two instances must retain independent slot contents over repeated entry calls.
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
#include "module_slot_state_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22 && XR_XIR_CALL_ABI_VERSION == 28 &&
    XR_XIR_PROGRAM_ABI_VERSION == 29, "Current execution identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_I64 == 2 && XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33,
    "Independent module execution model");
_Static_assert(XR_XIR_SLOT_LOAD == 4 && XR_XIR_SLOT_INIT == 5 && XR_XIR_SLOT_STORE == 6 &&
    XR_XIR_CONST_STRING == 3 && XR_XIR_PRINT == 24 && XR_XIR_ADD_INT == 25, "Independent slot operations");

#if defined(XR_MODULE_NATIVE)
extern const XrXirCallEntry source_module_slot_state_0_entries[6], source_module_slot_state_1_entries[6];
extern const char source_module_slot_state_0_sha[65], source_module_slot_state_1_sha[65];
static const XrXirCallEntry *native_entries(unsigned graph) {
    const XrXirCallEntry *entries[] = {source_module_slot_state_0_entries, source_module_slot_state_1_entries};
    CHECK(graph < 2); return entries[graph];
}
static const char *native_digest(unsigned graph) {
    const char *digests[] = {source_module_slot_state_0_sha, source_module_slot_state_1_sha};
    CHECK(graph < 2); return digests[graph];
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
static unsigned active_instance, releases;
static unsigned lifecycle_count[2], published[2], released_slots[2];
static uint32_t trace[2][28], trace_count[2];
static size_t resumed[2];

static XrXirAction observed_resume(XrXirCallView *view) {
    uint32_t id = xr_xir_call_current_entry(view->activation);
    CHECK(active_owner && id < 6 && active_instance < 2 && trace_count[active_instance] < 28);
    const XrXirCallEntry *entry = &active_owner->actual[id];
    CHECK(view->environment == entry->environment);
    ++resumed[active_owner->native[id] ? 1 : 0];
    XrXirAction action = entry->resume(view);
    if (action.kind == XR_XIR_ACTION_RETURN) trace[active_instance][trace_count[active_instance]++] = id;
    return action;
}

static void observe_lifecycle(void *context, XrXirLifecycleEvent event, uint32_t index) {
    unsigned instance = *(const unsigned *)context;
    CHECK(instance < 2 && instance == active_instance);
    if (event == XR_XIR_SLOT_PUBLISHED) {
        CHECK(index == 0 && lifecycle_count[instance] == 7 && !published[instance] && !released_slots[instance]);
        ++published[instance]; return;
    }
    if (event == XR_XIR_SLOT_RELEASED) {
        CHECK(index == 0 && lifecycle_count[instance] == 8 && published[instance] == 1 && !released_slots[instance]);
        ++released_slots[instance]; return;
    }
    CHECK(lifecycle_count[instance] < 8);
    unsigned cursor = lifecycle_count[instance]++;
    CHECK(event == (cursor % 2 ? XR_XIR_MODULE_READY : XR_XIR_MODULE_BEGIN));
    CHECK(index == cursor / 2);
    if (cursor == 7) CHECK(published[instance] == 1);
}

static void release_code(void *pointer) {
    ModuleCodeOwner *owner = pointer;
    CHECK(owner == active_owner && !releases);
    xr_xir_compile_artifact_free(owner->lowered);
    active_owner = NULL; xr_compile_resources_free(owner); ++releases;
}

static XrXirArtifact *read_lower(const XrXirCompileContext *context, unsigned graph) {
    const ModuleSlotStateCase *test = &module_slot_state_cases[graph];
    uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_checked_read(context, input, test->length, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "graph=%u admission=%u function=%u block=%u instruction=%u reason=%u\n",
        graph, status, diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.reason);
    CHECK(status == XR_XIR_OK);
    CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    CHECK(module && module->function_count == 6 && module->declarations);
    CHECK(module->declarations->module_count == 4 && module->declarations->entry_function == 5);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(!module->functions[i].parameter_count);
        CHECK(module->functions[i].result == (i == 4 ? test->result : i == 5 ? XR_XIR_I64 : XR_XIR_UNIT));
    }
    CHECK(module->declarations->slot_count == 1 && module->declarations->slots[0].module == 3);
    CHECK(module->declarations->slots[0].type == test->result && module->declarations->slots[0].mutable == 1);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    return lowered;
}

static void emit_source(XrXirArtifact *lowered, unsigned graph, XrXirCSource *source, char digest[65]) {
    char symbol[48]; CHECK(snprintf(symbol, sizeof(symbol), "source_module_slot_state_%u", graph) > 0);
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
    printf("module-slot-state graph=%u generated-bytes=%zu sha256=%s deterministic=1\n", graph, source->length, digest);
}

static void verify_value(const XrXirValue *value, unsigned graph, unsigned repeat) {
    CHECK(!value->reserved);
    if (!graph) { CHECK(value->type == XR_XIR_I64 && value->payload == 41 + (int64_t)repeat); return; }
    const char *bytes = NULL; size_t length = 0; const char *expected = repeat ? "changed" : "module-owned-text";
    CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value, &bytes, &length));
    CHECK(length == strlen(expected) && !memcmp(bytes, expected, length));
}

typedef struct StateOutput { XrXirInstance *instance; unsigned groups; size_t length; char bytes[512]; } StateOutput;
static XrXirOutputStatus state_bytes(void *opaque, XrXirOutputStream stream, const char *bytes, size_t length) {
    StateOutput *capture = opaque; const char *expected = capture->groups == 1 ? "module-owned-text\n" : "changed\n";
    CHECK(stream == XR_XIR_STDOUT && length == strlen(expected) && !memcmp(bytes, expected, length));
    CHECK(capture->length + length <= sizeof(capture->bytes));
    memcpy(capture->bytes + capture->length, bytes, length); capture->length += length; return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus state_group(void *opaque, const XrXirOutputGroup *group) {
    XrXirOutputSink *sink = opaque; StateOutput *capture = sink->context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && group->values);
    CHECK(capture->groups < 24); verify_value(&group->values[0], 1, capture->groups); ++capture->groups;
    CHECK(xr_xir_instance_start(capture->instance, 4, NULL, 0) == XR_XIR_CALL_BUSY);
    return xr_xir_output_render(sink, group);
}

static unsigned original_shapes(const XrXirCompileContext *context) {
    unsigned mismatches = 0;
    for (unsigned i = 2; i < 4; ++i) {
        const ModuleSlotStateCase *test = &module_slot_state_cases[i];
        size_t live = instance_compile_live, bytes = instance_compile_bytes;
        uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
        XrXirArtifact *checked = NULL; XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_checked_read(context, input, test->length, &checked, &diagnostic);
        CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
        if (status == XR_XIR_OK) { CHECK(checked); xr_xir_compile_artifact_free(checked); }
        else CHECK(!checked && diagnostic.status == status);
        CHECK(instance_compile_live == live && instance_compile_bytes == bytes);
        mismatches += status != XR_XIR_OK;
        printf("module-slot-state original=%s expected=0 actual=%u compiler-refund=1 result=%s\n", test->name, status, status == XR_XIR_OK ? "PASS" : "FAIL");
    }
    return mismatches;
}

static void execute(const XrXirCompileContext *context, XrXirArtifact *lowered, unsigned graph, unsigned mode) {
    const XrXirCallEntry *native = native_entries(graph);
    if (mode) CHECK(native);
    memset(trace, 0, sizeof(trace)); memset(trace_count, 0, sizeof(trace_count));
    memset(resumed, 0, sizeof(resumed)); memset(lifecycle_count, 0, sizeof(lifecycle_count));
    releases = 0; memset(published, 0, sizeof(published)); memset(released_slots, 0, sizeof(released_slots));
    ModuleCodeOwner *owner = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    owner->lowered = lowered;
    for (uint32_t f = 0; f < 6; ++f) {
        owner->native[f] = mode == 1 || (mode == 2 && f < 4) || (mode == 3 && f == 4);
        if (owner->native[f]) owner->actual[f] = native[f];
        else CHECK(xr_xir_compile_vm_bind(lowered, f, &owner->bindings[f], &owner->actual[f]) == XR_XIR_OK);
        CHECK(!owner->actual[f].parameter_count && owner->actual[f].result == (f == 4 ? module_slot_state_cases[graph].result : f == 5 ? XR_XIR_I64 : XR_XIR_UNIT));
        owner->observed[f] = owner->actual[f]; owner->observed[f].resume = observed_resume;
    }
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        owner->observed, 6, module->declarations, {owner, release_code}, module->types, xr_xir_compile_program_proof(lowered)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK);
    active_owner = owner;
    XrXirInstanceConfig config = {0};
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instances[2] = {NULL, NULL};
    unsigned instance_ids[] = {0, 1}; config.trace = observe_lifecycle;
    StateOutput captures[2] = {0}; XrXirOutputSink sinks[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        config.trace_context = &instance_ids[i];
        sinks[i] = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, state_bytes, &captures[i], 32};
        if (graph) config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, state_group, &sinks[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY); captures[i].instance = instances[i];
    }
    CHECK(instances[0] != instances[1]); xr_xir_compile_program_drop(program);
    XrXirValue escaped[48] = {0}; size_t count = 0;
    for (unsigned repeat = 0; repeat < 24; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        active_instance = i;
        for (uint32_t f = 0; f < 4; ++f)
            CHECK(xr_xir_instance_start(instances[i], f, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start(instances[i], 4, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = {0}; size_t polls = 0;
        do { CHECK(++polls < 32); result = xr_xir_instance_poll_bounded(instances[i], 10000); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && trace_count[i] == 5 + repeat && lifecycle_count[i] == 8);
        for (unsigned j = 0; j < 4; ++j) CHECK(trace[i][j] == j);
        for (unsigned j = 4; j < trace_count[i]; ++j) CHECK(trace[i][j] == 4);
        CHECK(xr_xir_instance_take_result(instances[i], &escaped[count]) == XR_XIR_CALL_RETURNED);
        verify_value(&escaped[count], graph, repeat);
        ++count;
        CHECK(published[i] == 1 && !released_slots[i]);
        CHECK(captures[i].groups == (graph ? repeat+1 : 0));
        CHECK(captures[i].length == (graph ? 18u + 8u*repeat : 0u));
        printf("module-slot-state graph=%u mode=%u instance=%u repeat=%u initializer-order=%u,%u,%u,%u entry-completions=%u value-verified=1 result=PASS\n",
            graph, mode, i, repeat, trace[i][0], trace[i][1], trace[i][2], trace[i][3], repeat+1);
    }
    for (unsigned i = 0; i < 2; ++i) {
        active_instance = i; CHECK(!releases && active_owner);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY); CHECK(releases == i && released_slots[i] == 1);
    }
    CHECK(count == 48 && releases == 1 && !active_owner);
    if (mode == 0) CHECK(resumed[0] && !resumed[1]);
    else if (mode == 1) CHECK(resumed[1] && !resumed[0]);
    else CHECK(resumed[0] && resumed[1]);
    if (graph) CHECK(escaped[0].payload != escaped[1].payload && runtime_live && runtime_bytes);
    for (size_t i = 0; i < count; ++i) {
        verify_value(&escaped[i], graph, (unsigned)(i / 2));
        XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&escaped[i], &copy) == XR_XIR_VALUE_OK);
        verify_value(&copy, graph, (unsigned)(i / 2));
        xr_xir_value_drop(&copy); xr_xir_value_drop(&escaped[i]);
        CHECK(!copy.type && !copy.payload && !escaped[i].type && !escaped[i].payload);
    }
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    printf("module-slot-state graph=%u mode=%u calls=48 initializers=8 lifecycle-events=20 slot-published=2 slot-released=2 private-rejects=192 retained-results=48 owners-dead=1 vm-resumes=%zu native-resumes=%zu code-release=1 result=PASS\n",
        graph, mode, resumed[0], resumed[1]);
}

int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    bool emit = !strcmp(argv[1], "emit");
    unsigned mode = !strcmp(argv[1], "vm") ? 0 : !strcmp(argv[1], "native") ? 1 :
        !strcmp(argv[1], "native-inits") ? 2 : !strcmp(argv[1], "vm-inits") ? 3 : 4;
    CHECK((emit && argc == 3) || (!emit && argc == 2 && mode < 4));
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    XrCompileResourceLimits limits = {67108864, 8388608, 128000000};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    unsigned mismatches = emit ? 0 : original_shapes(&context);
    for (unsigned graph = 0; graph < 2; ++graph) {
        XrXirArtifact *lowered = read_lower(&context, graph);
        XrXirCSource source = {0}; char digest[65]; emit_source(lowered, graph, &source, digest);
        if (emit) {
            char path[2048]; int size = snprintf(path, sizeof(path), "%s/graph%u.c", argv[2], graph);
            CHECK(size > 0 && (size_t)size < sizeof(path)); FILE *file = fopen(path, "wb"); CHECK(file);
            CHECK(fwrite(source.text, 1, source.length, file) == source.length);
            CHECK(fprintf(file, "\nconst char source_module_slot_state_%u_sha[65]=\"%s\";\n", graph, digest) > 0);
            CHECK(fclose(file) == 0); xr_xir_compile_artifact_free(lowered);
        } else {
#if defined(XR_MODULE_NATIVE)
            if (mode) CHECK(!strcmp(digest, native_digest(graph)));
#endif
            execute(&context, lowered, graph, mode);
        }
        xr_xir_compile_c_source_free(&source);
    }
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    printf("module-slot-state allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 "\n",
        stats.allocated_bytes, stats.peak_bytes, stats.work);
    xr_compile_resources_release(context.resources); instance_compile_report();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    puts("module-slot-state release compiler=0/0 runtime=0/0 table=0 result=PASS");
    printf("module-slot-state original-shape-mismatches=%u result=%s\n", mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
