/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_execution.c - Per-instance diamond initialization
 *
 * KEY CONCEPT:
 *   Observe actual entry resumes without changing authenticated call views.
 *   Fixed traces distinguish dependency and lexical order from numeric IDs.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "base/xsha256.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "module_execution_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22 && XR_XIR_CALL_ABI_VERSION == 28 &&
    XR_XIR_PROGRAM_ABI_VERSION == 29, "Current execution identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_I64 == 2 && XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33,
    "Independent module execution model");

#if defined(XR_MODULE_NATIVE)
extern const XrXirCallEntry source_module_execution_0_entries[5], source_module_execution_1_entries[5];
extern const XrXirCallEntry source_module_execution_2_entries[5], source_module_execution_3_entries[5];
extern const char source_module_execution_0_sha[65], source_module_execution_1_sha[65];
extern const char source_module_execution_2_sha[65], source_module_execution_3_sha[65];
static const XrXirCallEntry *native_entries(unsigned graph) {
    const XrXirCallEntry *entries[] = {source_module_execution_0_entries, source_module_execution_1_entries,
        source_module_execution_2_entries, source_module_execution_3_entries};
    CHECK(graph < 4); return entries[graph];
}
static const char *native_digest(unsigned graph) {
    const char *digests[] = {source_module_execution_0_sha, source_module_execution_1_sha,
        source_module_execution_2_sha, source_module_execution_3_sha};
    CHECK(graph < 4); return digests[graph];
}
#else
static const XrXirCallEntry *native_entries(unsigned graph) { (void)graph; return NULL; }
#endif

typedef struct ModuleCodeOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry actual[5], observed[5];
    XrXirVmBinding bindings[5];
    bool native[5];
} ModuleCodeOwner;
static ModuleCodeOwner *active_owner;
static unsigned active_instance, active_graph, releases;
static unsigned lifecycle_count[2];
static uint32_t trace[2][7], trace_count[2];
static size_t resumed[2];

static XrXirAction observed_resume(XrXirCallView *view) {
    uint32_t id = xr_xir_call_current_entry(view->activation);
    CHECK(active_owner && id < 5 && active_instance < 2 && trace_count[active_instance] < 7);
    const XrXirCallEntry *entry = &active_owner->actual[id];
    CHECK(view->environment == entry->environment);
    ++resumed[active_owner->native[id] ? 1 : 0];
    XrXirAction action = entry->resume(view);
    if (action.kind == XR_XIR_ACTION_RETURN) trace[active_instance][trace_count[active_instance]++] = id;
    return action;
}

static void observe_lifecycle(void *context, XrXirLifecycleEvent event, uint32_t index) {
    unsigned instance = *(const unsigned *)context;
    CHECK(instance < 2 && instance == active_instance && lifecycle_count[instance] < 8);
    unsigned cursor = lifecycle_count[instance]++;
    CHECK(event == (cursor % 2 ? XR_XIR_MODULE_READY : XR_XIR_MODULE_BEGIN));
    CHECK(index == module_execution_cases[active_graph].order[cursor / 2]);
}

static void release_code(void *pointer) {
    ModuleCodeOwner *owner = pointer;
    CHECK(owner == active_owner && !releases);
    xr_xir_compile_artifact_free(owner->lowered);
    active_owner = NULL; xr_compile_resources_free(owner); ++releases;
}

static XrXirArtifact *read_lower(const XrXirCompileContext *context, unsigned graph) {
    const ModuleExecutionCase *test = &module_execution_cases[graph];
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
    CHECK(module && module->function_count == 5 && module->declarations);
    CHECK(module->declarations->module_count == 4 && module->declarations->entry_function == 4);
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(!module->functions[i].parameter_count);
        CHECK(module->functions[i].result == (i == 4 ? XR_XIR_I64 : XR_XIR_UNIT));
    }
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    return lowered;
}

static void emit_source(XrXirArtifact *lowered, unsigned graph, XrXirCSource *source, char digest[65]) {
    char symbol[48]; CHECK(snprintf(symbol, sizeof(symbol), "source_module_execution_%u", graph) > 0);
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
    printf("module-execution graph=%u generated-bytes=%zu sha256=%s deterministic=1\n", graph, source->length, digest);
}

static void execute(const XrXirCompileContext *context, XrXirArtifact *lowered, unsigned graph, unsigned mode) {
    const XrXirCallEntry *native = native_entries(graph);
    if (mode) CHECK(native);
    memset(trace, 0, sizeof(trace)); memset(trace_count, 0, sizeof(trace_count));
    memset(resumed, 0, sizeof(resumed)); memset(lifecycle_count, 0, sizeof(lifecycle_count));
    releases = 0; active_graph = graph;
    ModuleCodeOwner *owner = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    owner->lowered = lowered;
    for (uint32_t f = 0; f < 5; ++f) {
        owner->native[f] = mode == 1 || (mode == 2 && f < 4) || (mode == 3 && f == 4);
        if (owner->native[f]) owner->actual[f] = native[f];
        else CHECK(xr_xir_compile_vm_bind(lowered, f, &owner->bindings[f], &owner->actual[f]) == XR_XIR_OK);
        CHECK(!owner->actual[f].parameter_count && owner->actual[f].result == (f == 4 ? XR_XIR_I64 : XR_XIR_UNIT));
        owner->observed[f] = owner->actual[f]; owner->observed[f].resume = observed_resume;
    }
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        owner->observed, 5, module->declarations, {owner, release_code}, module->types, xr_xir_compile_program_proof(lowered)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK);
    active_owner = owner;
    XrXirInstanceConfig config = {0};
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instances[2] = {NULL, NULL};
    unsigned instance_ids[] = {0, 1}; config.trace = observe_lifecycle;
    for (unsigned i = 0; i < 2; ++i) {
        config.trace_context = &instance_ids[i];
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(instances[0] != instances[1]); xr_xir_compile_program_drop(program);
    XrXirValue escaped[6] = {0}; size_t count = 0;
    for (unsigned repeat = 0; repeat < 3; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        active_instance = i;
        for (uint32_t f = 0; f < 4; ++f)
            CHECK(xr_xir_instance_start(instances[i], f, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start(instances[i], 4, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = {0}; size_t polls = 0;
        do { CHECK(++polls < 32); result = xr_xir_instance_poll_bounded(instances[i], 10000); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && trace_count[i] == 5 + repeat && lifecycle_count[i] == 8);
        for (unsigned j = 0; j < 4; ++j) CHECK(trace[i][j] == module_execution_cases[graph].order[j]);
        for (unsigned j = 4; j < trace_count[i]; ++j) CHECK(trace[i][j] == 4);
        CHECK(xr_xir_instance_take_result(instances[i], &escaped[count]) == XR_XIR_CALL_RETURNED);
        CHECK(escaped[count].type == XR_XIR_I64 && !escaped[count].reserved && escaped[count].payload == 42);
        ++count;
        printf("module-execution graph=%u mode=%u instance=%u repeat=%u initializer-order=%u,%u,%u,%u entry-completions=%u result=42 PASS\n",
            graph, mode, i, repeat, trace[i][0], trace[i][1], trace[i][2], trace[i][3], repeat+1);
    }
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(!releases && active_owner);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY); CHECK(releases == i);
    }
    CHECK(count == 6 && releases == 1 && !active_owner);
    if (mode == 0) CHECK(resumed[0] == 20 && !resumed[1]);
    else if (mode == 1) CHECK(resumed[1] == 20 && !resumed[0]);
    else if (mode == 2) CHECK(resumed[0] == 12 && resumed[1] == 8);
    else CHECK(resumed[0] == 8 && resumed[1] == 12);
    for (size_t i = 0; i < count; ++i) {
        CHECK(escaped[i].type == XR_XIR_I64 && !escaped[i].reserved && escaped[i].payload == 42);
        XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&escaped[i], &copy) == XR_XIR_VALUE_OK);
        CHECK(!memcmp(&escaped[i], &copy, sizeof(copy)));
        xr_xir_value_drop(&copy); xr_xir_value_drop(&escaped[i]);
        CHECK(!copy.type && !copy.payload && !escaped[i].type && !escaped[i].payload);
    }
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    printf("module-execution graph=%u mode=%u calls=6 initializers=8 lifecycle-events=16 private-rejects=24 retained-results=6 owners-dead=1 vm-resumes=%zu native-resumes=%zu code-release=1 result=PASS\n",
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
    for (unsigned graph = 0; graph < 4; ++graph) {
        XrXirArtifact *lowered = read_lower(&context, graph);
        XrXirCSource source = {0}; char digest[65]; emit_source(lowered, graph, &source, digest);
        if (emit) {
            char path[2048]; int size = snprintf(path, sizeof(path), "%s/graph%u.c", argv[2], graph);
            CHECK(size > 0 && (size_t)size < sizeof(path)); FILE *file = fopen(path, "wb"); CHECK(file);
            CHECK(fwrite(source.text, 1, source.length, file) == source.length);
            CHECK(fprintf(file, "\nconst char source_module_execution_%u_sha[65]=\"%s\";\n", graph, digest) > 0);
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
    printf("module-execution allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 "\n",
        stats.allocated_bytes, stats.peak_bytes, stats.work);
    xr_compile_resources_release(context.resources); instance_compile_report();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    puts("module-execution release compiler=0/0 runtime=0/0 table=0 result=PASS");
    return 0;
}
