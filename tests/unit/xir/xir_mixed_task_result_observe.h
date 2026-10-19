/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_task_result_observe.h - Observe real stopped activations and metadata
 */
#ifndef XIR_MIXED_TASK_RESULT_OBSERVE_H
#define XIR_MIXED_TASK_RESULT_OBSERVE_H
typedef struct TaskLateWitness {
    unsigned root_providers, worker_providers, yielded_count;
    uint64_t yielded[2];
} TaskLateWitness;

static bool task_late_native(unsigned backend, bool root) {
    return backend == 3 || (backend < 2 && root == (backend == 1));
}
/* Read only stopped, owned activations. Entry tables belong to each Call;
 * no observer, callback wrapper, admission hook or fabricated view is needed. */
static void task_late_frames(const TaskLateFixture *fixture, XrXirInstance *instance,
    XrXirCall *call, TaskLateWitness *witness, bool child) {
    if (!call) return;
    CHECK(!call->driving && call->config.instance == instance && call->config.entries);
    CHECK(call->config.entry_count >= fixture->count && call->config.entry_count <= fixture->count+1);
    CHECK(call->config.admission.domain == instance->domain && call->budget == &instance->budget);
    unsigned frames = 0;
    for (const CallFrame *frame = call->top; frame; frame = frame->parent) {
        CHECK(++frames < 64);
        uintptr_t base = (uintptr_t)call->config.entries, address = (uintptr_t)frame->entry;
        CHECK(address >= base && (address-base) % sizeof(*frame->entry) == 0);
        uint64_t index = (address-base) / sizeof(*frame->entry); CHECK(index < call->config.entry_count);
        if (index == fixture->count || !frame->entered) continue;
        uint32_t f = (uint32_t)index; bool native = task_late_native(fixture->backend, fixture->root[f]);
        const XrXirCallEntry *expected = native ? &p20_task_result_fixture_program.entries[f] : &fixture->vm->entries[f];
        CHECK(frame->entry->resume == expected->resume && frame->entry->environment == expected->environment);
        CHECK(frame->code_kind == (native ? XR_XIR_CALL_CODE_NATIVE : XR_XIR_CALL_CODE_VM));
        unsigned bit = native ? 2u : 1u;
        if (!child && f == p20_task_result_entries[0]) witness->root_providers |= bit;
        if (child && f == p20_task_result_entries[2]) witness->worker_providers |= bit;
    }
}
static void task_late_observe(const TaskLateFixture *fixture, XrXirInstance *instance, TaskLateWitness *witness) {
    CHECK(!instance->driving && instance->executor->config.instance == instance);
    task_late_frames(fixture, instance, instance->call, witness, false);
    unsigned tasks = 0;
    for (const XirTask *task = instance->executor->active; task; task = task->active_next) {
        CHECK(++tasks < 8 && task->executor == instance->executor && task->object.domain == instance->domain);
        CHECK(task->call && task->activation.call == task->call && task->activation.task == task);
        CHECK(task->call->executor_owner == instance->executor && task->call->executor_activation == &task->activation);
        task_late_frames(fixture, instance, task->call, witness, true);
        if (!task->call->next_wake) continue;
        CHECK(task->call->next_wake == 1);
        unsigned i = 0;
        while (i < witness->yielded_count && witness->yielded[i] != task->identity) ++i;
        if (i == witness->yielded_count) {
            CHECK(i < 2); witness->yielded[witness->yielded_count++] = task->identity;
        }
    }
}
static void task_late_metadata_only(const TaskLateFixture *fixture, const XrXirTypeArena *arena) {
    CHECK(arena && arena->context.resources == fixture->resources && effects_compile_live == 2);
    unsigned resources = 0, metadata = 0;
    for (size_t i = 0; i < 32768; ++i) {
        const EffectsCompileAllocation *record = &effects_compile_allocations[i];
        if (!record->pointer) continue;
        uintptr_t base = (uintptr_t)record->pointer, r = (uintptr_t)fixture->resources, a = (uintptr_t)arena;
        bool has_resource = r >= base && r-base < record->bytes;
        bool has_arena = a >= base && a-base < record->bytes;
        CHECK(has_resource != has_arena); resources += has_resource; metadata += has_arena;
        if (has_arena) {
            uintptr_t nodes = (uintptr_t)arena->types.nodes;
            CHECK(nodes >= base && nodes-base <= record->bytes &&
                (uint64_t)arena->types.count*sizeof(*arena->types.nodes) <= record->bytes-(nodes-base));
        }
    }
    CHECK(resources == 1 && metadata == 1);
}
static void task_late_text(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == strlen(expected) && !memcmp(bytes, expected, length));
}
static void task_late_array(const XrXirValue *value, const char *first) {
    XrXirValueAdmission admission = {xr_xir_value_arena(value), NULL, NULL, NULL, 10000, 65536};
    int64_t length = 0; CHECK(xr_xir_array_len(value, &admission, &length) == XR_XIR_VALUE_OK && length == 2);
    const char *expected[] = {first, "birch"};
    for (int64_t at = 0; at < 2; ++at) {
        XrXirValue text = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(value, at, &admission, &text, &fault) == XR_XIR_VALUE_OK);
        task_late_text(&text, expected[at]); xr_xir_value_drop(&text);
    }
}
/* Two owned outcomes outlive the Task as well as execution. A new receiver
 * owns the late COW candidate; its caller reference dies before either read. */
static void task_late_outcomes(const TaskLateFixture *fixture, XrXirValue *value) {
    const XirTask *task = (const XirTask *)(uintptr_t)value->payload;
    CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->call && !task->executor);
    const XrXirTypeArena *arena = xr_xir_value_arena(value); task_late_metadata_only(fixture, arena);
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, (XrXirType)value->type);
    CHECK(node && node->kind == XR_XIR_TYPE_TASK && !node->parameter_span);
    node = xr_xir_type_node(types, node->element);
    CHECK(node && node->kind == XR_XIR_TYPE_ARRAY && node->element == XR_XIR_STRING && !node->parameter_span);
    XrXirCallResult occupied = {.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,917}}, saved;
    memcpy(&saved, &occupied, sizeof(saved));
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(task->object.domain);
    size_t attempts = runtime_attempts, live = runtime_live;
    CHECK(xr_xir_task_copy_outcome(value, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&saved, &occupied, sizeof(saved)));
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(task->object.domain);
    CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
        before.requested_call_bytes == after.requested_call_bytes && attempts == runtime_attempts && live == runtime_live);
    XrXirCallResult first = {0}, second = {0};
    CHECK(xr_xir_task_copy_outcome(value, &first) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_task_copy_outcome(value, &second) == XR_XIR_CALL_RETURNED);
    CHECK(!first.wake && !second.wake && xr_xir_panic_empty(&first.panic) && xr_xir_panic_empty(&second.panic));
    xr_xir_value_drop(value); CHECK(!value->type && !value->reserved && !value->payload);
    task_late_array(&first.value, "cedar"); task_late_array(&second.value, "cedar");
    XrXirDomain *receiver = NULL; CHECK(xr_xir_domain_new(65536, &receiver) == XR_XIR_VALUE_OK);
    XrXirValue replacement = {0}; CHECK(xr_xir_string_new(receiver, "elm", 3, &replacement) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, receiver, NULL, NULL, 10000, 65536};
    XrXirValuePlace place = {(XrXirType)first.value.type, &first.value.payload}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_set(&place, 0, &replacement, &admission, &fault) == XR_XIR_VALUE_OK);
    xr_xir_domain_drop(receiver); xr_xir_value_drop(&replacement);
    task_late_metadata_only(fixture, arena);
    task_late_array(&first.value, "elm"); task_late_array(&second.value, "cedar");
    xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second);
    CHECK(xr_xir_call_result_empty(&first) && xr_xir_call_result_empty(&second));
}
#endif // XIR_MIXED_TASK_RESULT_OBSERVE_H
