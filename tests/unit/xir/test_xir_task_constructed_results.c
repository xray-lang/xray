/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_constructed_results.c - Typed sticky Task results across providers
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_packet_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Task constructed FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_task_constructed_pipeline.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_atomic.h"
#include "task_constructed_generated.h"

enum { CONSTRUCTED_FUNCTIONS = 128 };
typedef struct ConstructedFixture {
    XrXirProgram *program;
    XrXirArtifact *lowered;
    XrXirCallEntry entries[CONSTRUCTED_FUNCTIONS];
    XrXirVmBinding bindings[CONSTRUCTED_FUNCTIONS];
    uint32_t entry[CONSTRUCTED_CASES][2];
    unsigned code_releases;
} ConstructedFixture;
static void constructed_code_drop(void *owner) {
    ConstructedFixture *fixture = owner;
    CHECK(fixture->lowered && !fixture->code_releases++);
    xr_xir_compile_artifact_free(fixture->lowered); fixture->lowered = NULL;
}
static void constructed_build(ConstructedFixture *fixture, const char *backend) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    fixture->lowered = constructed_lower(context, fixture->entry, false);
    const XrXirModule *module = xr_xir_compile_artifact_module(fixture->lowered);
    const XrXirProgramSpec *native = &task_constructed_native_program;
    CHECK(module->function_count <= CONSTRUCTED_FUNCTIONS && module->function_count == native->entry_count);
    XrXirProgramPacket packet = xir_program_packet_borrow(fixture->lowered);
    CHECK(packet.length == native->packet.length && !memcmp(packet.bytes, native->packet.bytes, packet.length));
    bool vm = !strcmp(backend, "vm"), all_native = !strcmp(backend, "native");
    bool root_native = !strcmp(backend, "native-vm");
    CHECK(vm || all_native || root_native || !strcmp(backend, "vm-native"));
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool worker = module->functions[f].name_length >= 7 && !memcmp(module->functions[f].name, "worker_", 7);
        bool use_native = all_native || (!vm && (worker != root_native));
        XrXirCallEntry entry = {0};
        CHECK(xr_xir_compile_vm_bind(fixture->lowered, f, &fixture->bindings[f], &entry) == XR_XIR_OK);
        fixture->entries[f] = use_native ? native->entries[f] : entry;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, native->target, fixture->entries, module->function_count,
        module->declarations, {fixture, constructed_code_drop}, module->types, packet};
    CHECK(xr_xir_compile_program_seal(context, &spec, &fixture->program) == XR_XIR_OK);
    for (unsigned n = 0; n < CONSTRUCTED_CASES; ++n) for (unsigned mode = 0; mode < 2; ++mode)
        CHECK(fixture->entry[n][mode] == constructed_entries[n][mode]);
}
static bool constructed_i64_array(const XrXirValue *value, XrXirValueAdmission *admission) {
    int64_t length = 0; XrXirValueStatus status = xr_xir_array_len(value, admission, &length);
    if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
    CHECK(length == 2);
    for (int64_t i = 0; i < 2; ++i) {
        XrXirValue field = {0}; XrXirFaultDetail fault = {0};
        status = xr_xir_array_get(value, i, admission, &field, &fault);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        CHECK(field.type == XR_XIR_I64 && field.payload == 10 * (i + 1)); xr_xir_value_drop(&field);
    }
    return true;
}
static bool constructed_value(const XrXirValue *value, unsigned family) {
    CHECK(xr_xir_value_valid(value));
    const XirObject *object = (const XirObject *)(uintptr_t)value->payload;
    XrXirValueAdmission admission = {object->arena, object->domain, NULL, NULL, 1000000, 16777216};
    if (!family) return constructed_i64_array(value, &admission);
    if (family == 1) {
        int64_t length = 0; XrXirValueStatus status = xr_xir_array_len(value, &admission, &length);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        CHECK(length == 2); const char *words[] = {"owned", "array", "typed"}; unsigned word = 0;
        for (unsigned i = 0; i < 2; ++i) {
            XrXirValue inner = {0}; XrXirFaultDetail fault = {0};
            status = xr_xir_array_get(value, i, &admission, &inner, &fault);
            if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
            status = xr_xir_array_len(&inner, &admission, &length);
            if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&inner); CHECK(status == XR_XIR_VALUE_LIMIT); return false; }
            CHECK(length == (i ? 1 : 2));
            for (int64_t j = 0; j < length; ++j) {
                XrXirValue text = {0}; status = xr_xir_array_get(&inner, j, &admission, &text, &fault);
                if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&inner); CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
                const char *bytes = NULL; size_t size = 0;
                CHECK(xr_xir_string_view(&text, &bytes, &size) && size == 5 && !memcmp(bytes, words[word++], size));
                xr_xir_value_drop(&text);
            }
            xr_xir_value_drop(&inner);
        }
        CHECK(word == 3); return true;
    }
    if (family == 2 || family == 3) {
        bool some = false; const XrXirValue *payload = NULL;
        CHECK(xr_xir_nullable_view(value, &some, &payload) && some && payload);
        if (family == 3) return constructed_i64_array(payload, &admission);
        CHECK(payload->type == XR_XIR_I64 && payload->payload == 42); return true;
    }
    if (family == 4) {
        XrXirValue number = {0}, text = {0};
        XrXirValueStatus status = xr_xir_tuple_get(value, 0, &number);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        status = xr_xir_tuple_get(value, 1, &text);
        if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&number); CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        const char *bytes = NULL; size_t length = 0;
        CHECK(number.type == XR_XIR_I64 && number.payload == 42 && xr_xir_string_view(&text, &bytes, &length) && length == 5 && !memcmp(bytes, "owned", length));
        xr_xir_value_drop(&number); xr_xir_value_drop(&text); return true;
    }
    if (family == 5) {
        XrXirEnumBorrow record = {0}; CHECK(xr_xir_enum_borrow(value, &record) == XR_XIR_VALUE_OK);
        CHECK(record.name.length == 6 && !memcmp(record.name.bytes, "Choice", 6) && record.member.length == 8 && !memcmp(record.member.bytes, "Selected", 8) && record.field_count == 1);
        return constructed_i64_array(&record.fields[0], &admission);
    }
    if (family == 6) {
        XrXirAtomicRequest request = {value, NULL, NULL, 0, XR_XIR_ATOMIC_OPERATION_LOAD, XR_XIR_I64};
        XrXirAtomicProgress progress = {0}; XrXirValue number = {0};
        XrXirAtomicOutcome outcome = xr_xir_atomic_start(&request, &admission, NULL, &progress, &number);
        xr_xir_atomic_progress_clear(&progress);
        if (outcome.status != XR_XIR_RUN_OK || outcome.permission != XR_XIR_CALL_READY) {
            CHECK(outcome.status == XR_XIR_RUN_STEP_LIMIT || outcome.status == XR_XIR_RUN_FRAME_LIMIT || outcome.permission == XR_XIR_CALL_LIMIT || outcome.status == XR_XIR_RUN_OUT_OF_MEMORY); return false;
        }
        CHECK(!outcome.continuing && number.type == XR_XIR_I64 && number.payload == 42); xr_xir_value_drop(&number); return true;
    }
    CHECK(family == 7); XrXirCallResult inner = {0}; XrXirCallStatus status = xr_xir_task_copy_outcome(value, &inner);
    if (status != XR_XIR_CALL_RETURNED) { CHECK(status == XR_XIR_CALL_LIMIT); return false; }
    CHECK(inner.value.type == XR_XIR_I64 && inner.value.payload == 42); xr_xir_call_result_drop(&inner); return true;
}
static bool constructed_operation(ConstructedFixture *fixture, unsigned family, bool consume_program,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    XrXirInstance *instances[2] = {0}; XrXirDomain *domains[2] = {0}; XrXirValue tasks[2] = {{0}};
    bool completed = true;
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        if (limits) config = *limits;
        XrXirCallStatus status = xr_xir_instance_new(fixture->program, &config, &instances[i]);
        if (status != XR_XIR_CALL_READY) { CHECK(status == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM)); completed = false; }
        else { domains[i] = instances[i]->domain; CHECK(xr_xir_domain_retain(domains[i])); }
    }
    if (consume_program) { xr_xir_compile_program_drop(fixture->program); fixture->program = NULL; }
    for (unsigned i = 0; i < 2; ++i) if (instances[i]) {
        for (unsigned mode = 0; mode < 2; ++mode) {
            XrXirCallStatus status = xr_xir_instance_start(instances[i], fixture->entry[family][mode], NULL, 0);
            if (status == XR_XIR_CALL_READY) {
                XrXirInstanceResult result = {0}; unsigned polls = 0;
                do { result = xr_xir_instance_poll_bounded(instances[i], 1); CHECK(++polls < 4096); }
                while (result.outcome.status == XR_XIR_CALL_READY);
                if (result.outcome.status == XR_XIR_CALL_RETURNED) {
                    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instances[i], &value) == XR_XIR_CALL_RETURNED);
                    if (mode) tasks[i] = value;
                    else { if (!constructed_value(&value, family)) completed = false; xr_xir_value_drop(&value); }
                } else { CHECK(result.outcome.status == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM)); completed = false; break; }
            } else { CHECK(status == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM)); completed = false; break; }
        }
        XrXirCallStatus freed = xr_xir_instance_free(instances[i]);
        CHECK(freed == XR_XIR_CALL_READY || freed == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM));
        if (freed != XR_XIR_CALL_READY) completed = false;
    }
    if (consume_program) CHECK(fixture->code_releases == 1 && !fixture->lowered);
    for (unsigned i = 0; i < 2; ++i) if (tasks[i].type) {
        const XirTask *task = (const XirTask *)(uintptr_t)tasks[i].payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->executor && !task->call);
        XrXirCallResult first = {0}, second = {0};
        XrXirCallStatus status = xr_xir_task_copy_outcome(&tasks[i], &first);
        if (status == XR_XIR_CALL_RETURNED) {
            XrXirCallStatus copied = xr_xir_task_copy_outcome(&tasks[i], &second);
            if (!constructed_value(&first.value, family)) completed = false;
            if (copied == XR_XIR_CALL_RETURNED) { if (!constructed_value(&second.value, family)) completed = false; }
            else { CHECK(limits && copied == XR_XIR_CALL_LIMIT && xr_xir_call_result_empty(&second)); completed = false; }
            CHECK(!first.wake && !second.wake && xr_xir_panic_empty(&first.panic) && xr_xir_panic_empty(&second.panic));
            xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second);
        } else { CHECK(limits && status == XR_XIR_CALL_LIMIT && xr_xir_call_result_empty(&first)); completed = false; }
        CHECK(task->outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_value_valid(&task->outcome.value));
        xr_xir_value_drop(&tasks[i]);
    }
    for (unsigned i = 0; i < 2; ++i) if (domains[i]) {
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domains[i]);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
            spent.metadata_allocations == spent.metadata_frees && xr_xir_domain_stats(domains[i]).live_bytes == sizeof(*domains[i]));
        if (cost) { if (!i) *cost = spent; else CHECK(spent.work == cost->work &&
            spent.requested_bytes == cost->requested_bytes && spent.requested_call_bytes == cost->requested_call_bytes); }
        xr_xir_domain_drop(domains[i]);
    }
    CHECK(!runtime_live && !runtime_bytes); return completed;
}
static void constructed_resource_axes(ConstructedFixture *fixture, unsigned family) {
    XrXirDomainBudgetStats cost = {0}; CHECK(constructed_operation(fixture, family, false, NULL, &cost));
    const uint64_t exact[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(exact[axis]);
        if (!axis) config.requested_value_limit = exact[axis] - minus;
        else if (axis == 1) config.requested_call_limit = exact[axis] - minus;
        else config.work_limit = exact[axis] - minus;
        CHECK(constructed_operation(fixture, family, false, &config, NULL) == !minus);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 4); unsigned family = CONSTRUCTED_CASES;
    for (unsigned i = 0; i < CONSTRUCTED_CASES; ++i) if (!strcmp(argv[1], constructed_names[i])) family = i;
    CHECK(family < CONSTRUCTED_CASES); bool fi = !strcmp(argv[3], "fi"); CHECK(fi || !strcmp(argv[3], "normal"));
    ConstructedFixture fixture = {0}; constructed_build(&fixture, argv[2]); constructed_resource_axes(&fixture, family);
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    CHECK(constructed_operation(&fixture, family, !fi, NULL, NULL)); size_t sites = runtime_attempts;
    if (fi) for (size_t site = 0; site < sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site;
        CHECK(!constructed_operation(&fixture, family, false, NULL, NULL) && runtime_attempts > site);
    }
    runtime_fail_at = SIZE_MAX; if (fi) xr_xir_compile_program_drop(fixture.program);
    CHECK(fixture.code_releases == 1 && !fixture.lowered && !runtime_live && !runtime_bytes);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("TASK_CONSTRUCTED %s %s %s sites=%zu typedGraph stickyCopies2 twoInstances postInstance program=%s codeReleases=1 physical=0/0\n",
        argv[1], argv[2], argv[3], sites, fi ? "guard" : "lastLease"); return 0;
}
