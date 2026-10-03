/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_values.c - VM and mixed string calls with results outliving artifacts
 *
 * KEY CONCEPT:
 *   Assert independent expected values and explicit ownership at every exit.
 */


#include "xir/xxir_call.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <pthread.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void bytes_equal(const XrXirValue *value, const char *expected, size_t count) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == count && !memcmp(bytes, expected, count));
}
static void unicode_cases(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats initial = xr_xir_domain_stats(domain);
    static const char valid[] = "A\0\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";
    XrXirValue value = {0}, copy = {0}, empty = {0};
    CHECK(xr_xir_string_new(domain, valid, sizeof(valid) - 1, &value) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&value, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, NULL, 0, &empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_append(&copy, &empty) == XR_XIR_VALUE_OK);
    size_t runes = 0;
    CHECK(xr_xir_string_runes(&copy, &runes) && runes == 5);
    bytes_equal(&copy, valid, sizeof(valid) - 1);
    CHECK(xr_xir_value_copy(&value, &copy) == XR_XIR_VALUE_BAD_ARGUMENT);
    const char *invalid[] = {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\x80", "\xE4\xB8", "\xFF"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        XrXirValue rejected = {0};
        CHECK(xr_xir_string_new(domain, invalid[i], strlen(invalid[i]), &rejected) == XR_XIR_VALUE_BAD_UTF8);
        CHECK(!rejected.type);
    }
    xr_xir_value_drop(&value); xr_xir_value_drop(&copy); xr_xir_value_drop(&empty);
    CHECK(xr_xir_domain_stats(domain).live_bytes == initial.live_bytes);
    xr_xir_domain_drop(domain);
}
static void cow_cases(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValue left = {0}, suffix = {0}, copy = {0};
    CHECK(xr_xir_string_new(domain, "abc", 3, &left) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "!", 1, &suffix) == XR_XIR_VALUE_OK);
    const char *before = NULL, *after = NULL; size_t size = 0;
    CHECK(xr_xir_string_view(&left, &before, &size));
    XrXirDomainStats stats = xr_xir_domain_stats(domain);
    CHECK(xr_xir_string_append(&left, &suffix) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_view(&left, &after, &size) && before == after);
    CHECK(xr_xir_domain_stats(domain).allocations == stats.allocations);
    CHECK(xr_xir_value_copy(&left, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_append(&left, &left) == XR_XIR_VALUE_OK);
    bytes_equal(&left, "abc!abc!", 8); bytes_equal(&copy, "abc!", 4);
    CHECK(xr_xir_string_append(&left, &left) == XR_XIR_VALUE_OK);
    bytes_equal(&left, "abc!abc!abc!abc!", 16);
    CHECK(xr_xir_domain_stats(domain).reallocations == 1);
    xr_xir_value_drop(&copy); xr_xir_value_drop(&suffix);
    xr_xir_domain_drop(domain);
    bytes_equal(&left, "abc!abc!abc!abc!", 16);
    xr_xir_value_drop(&left);
}
static void copy_worker(XrXirValue *source) {
    for (uint32_t i = 0; i < 2000; ++i) {
        XrXirValue copy = {0};
        CHECK(xr_xir_value_copy(source, &copy) == XR_XIR_VALUE_OK);
        if (source->type == XR_XIR_ATOMIC_I64) {
            int64_t previous = 0;
            CHECK(xr_xir_atomic_i64_fetch_add(&copy, 1, &previous));
            CHECK(previous >= 0 && previous < 8000);
        } else if (source->type == XR_XIR_STRING) {
            CHECK(xr_xir_string_append(&copy, source) == XR_XIR_VALUE_OK);
            bytes_equal(&copy, "threadthread", 12);
        }
        xr_xir_value_drop(&copy);
    }
    xr_xir_value_drop(source);
}
#if defined(XR_OS_WINDOWS)
static DWORD WINAPI thread_entry(void *pointer) { copy_worker(pointer); return 0; }
#else
static void *thread_entry(void *pointer) { copy_worker(pointer); return NULL; }
#endif
static XrXirTypeArena *string_cell_arena(XrXirDomain *domain) {
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_STRING};
    XrXirTypes types = {&node, 1, NULL, NULL};
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.metadata_bytes = 65536; budget.work = 100;
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static void concurrent_copies(unsigned kind) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValue value = {0}, copies[4] = {{0}, {0}, {0}, {0}};
    CHECK((kind == 1 ? xr_xir_atomic_i64_new(domain, 0, &value) :
        xr_xir_string_new(domain, "thread", 6, &value)) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = kind == 2 ? string_cell_arena(domain) : NULL;
    if (kind == 2) {
        XrXirValueAdmission admission = {arena, domain, NULL, NULL, 10, 0};
        XrXirValue cell = {0};
        CHECK(xr_xir_cell_new(domain, arena, (XrXirType) 256, &value, &admission, &cell) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&value); value = cell;
    }
#if defined(XR_OS_WINDOWS)
    HANDLE threads[4];
#else
    pthread_t threads[4];
#endif
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(xr_xir_value_copy(&value, &copies[i]) == XR_XIR_VALUE_OK);
#if defined(XR_OS_WINDOWS)
        threads[i] = CreateThread(NULL, 0, thread_entry, &copies[i], 0, NULL);
        CHECK(threads[i] != NULL);
#else
        CHECK(pthread_create(&threads[i], NULL, thread_entry, &copies[i]) == 0);
#endif
    }
    for (uint32_t i = 0; i < 4; ++i) {
#if defined(XR_OS_WINDOWS)
        CHECK(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0);
        CHECK(CloseHandle(threads[i]));
#else
        CHECK(pthread_join(threads[i], NULL) == 0);
#endif
    }
    if (kind == 1) {
        int64_t count = 0;
        CHECK(xr_xir_atomic_i64_load(&value, &count) && count == 8000);
    } else if (kind == 2) {
        XrXirValue content = {0}; CHECK(xr_xir_cell_read(&value, &content) == XR_XIR_VALUE_OK);
        bytes_equal(&content, "thread", 6); xr_xir_value_drop(&content);
    } else bytes_equal(&value, "thread", 6);
    xr_xir_value_drop(&value);
    xr_xir_type_arena_drop(arena);
    XrXirDomainStats stats = xr_xir_domain_stats(domain);
    CHECK(stats.allocations == stats.frees + 1);
    xr_xir_domain_drop(domain);
}
typedef struct TypedOutput { uint32_t seen; bool reject; } TypedOutput;
static XrXirOutputStatus typed_write(void *context, const XrXirOutputGroup *group) {
    CHECK(group && !group->line && group->count == 1);
    XrXirOutputStream stream = group->stream;
    const XrXirValue *value = &group->values[0];
    TypedOutput *output = context;
    CHECK(stream == (output->seen ? XR_XIR_STDERR : XR_XIR_STDOUT));
    CHECK(value->type == (uint32_t) (output->seen ? XR_XIR_I64 : XR_XIR_BOOL));
    CHECK(value->payload == (output->seen ? INT64_MIN : 1));
    ++output->seen;
    return (!output->reject) ? XR_XIR_OUTPUT_OK : XR_XIR_OUTPUT_ERROR;
}
typedef struct TypedFrame { uint32_t pc; XrXirValue value; } TypedFrame;
static XrXirAction typed_resume(XrXirCallView *view) {
    TypedFrame *state = view->state;
    if (state->pc++ == 0) {
        state->value = (XrXirValue) {XR_XIR_BOOL, 0, 1};
        return (XrXirAction) {XR_XIR_ACTION_OUTPUT, XR_XIR_STDOUT, &state->value, 1, {0}, {0}, 0};
    }
    if (state->pc == 2) {
        state->value = (XrXirValue) {XR_XIR_I64, 0, INT64_MIN};
        return (XrXirAction) {XR_XIR_ACTION_OUTPUT, XR_XIR_STDERR, &state->value, 1, {0}, {0}, 0};
    }
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {0, 0, 0}, {0}, 0};
}
static void typed_output(void) {
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT,
        sizeof(TypedFrame), typed_resume, NULL, NULL, 0, 0};
    for (uint32_t mode = 0; mode < 3; ++mode) {
        TypedOutput output = {0, mode == 1};
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = &entry; config.entry_count = 1; config.instance = NULL; config.byte_limit = 65536; config.poll_limit = 10; config.depth_limit = 10; config.accounting = &accounting; config.output = mode == 2 ? (XrXirOutputProvider) {0} : (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, typed_write, &output}; config.admission = (XrXirValueAdmission) {0};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, NULL, 0, &call) == XR_XIR_CALL_READY);
        CHECK(xr_xir_call_poll_bounded(call, UINT64_MAX).status == (mode ? XR_XIR_CALL_OUTPUT_ERROR : XR_XIR_CALL_RETURNED));
        CHECK(output.seen == (mode == 2 ? 0u : mode == 1 ? 1u : 2u));
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
    }
}
static XrXirAction atomic_output_resume(XrXirCallView *view) {
    return (XrXirAction) {XR_XIR_ACTION_OUTPUT, XR_XIR_STDOUT, view->arguments, 1, {0}, {0}, 0};
}
static void atomic_boundaries(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValue cell = {0}, copy = {0};
    CHECK(xr_xir_atomic_i64_new(domain, INT64_MAX, &cell) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&cell, &copy) == XR_XIR_VALUE_OK);
    CHECK(cell.payload == copy.payload);
    int64_t old = 0, value = 0;
    CHECK(xr_xir_atomic_i64_fetch_add(&copy, 1, &old) && old == INT64_MAX);
    CHECK(xr_xir_atomic_i64_load(&cell, &value) && value == INT64_MIN);
    CHECK(!xr_xir_value_argument(&cell, NULL, XR_XIR_STRING));
    int64_t frame = 0;
    CHECK(xr_xir_owned_slot_copy(&frame, 0, NULL, XR_XIR_I64, 42) == XR_XIR_VALUE_BAD_ARGUMENT && !frame);
    const XrXirType type = XR_XIR_ATOMIC_I64;
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, &type, 1, XR_XIR_UNIT, 0, atomic_output_resume, NULL, NULL, 0, 0};
    XrXirCallAccounting accounting = {0};
    TypedOutput output = {0};
    XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = &entry; config.entry_count = 1; config.instance = NULL; config.byte_limit = 65536; config.poll_limit = 10; config.depth_limit = 10; config.accounting = &accounting; config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, typed_write, &output}; config.admission = (XrXirValueAdmission) {0};
    XrXirCall *call = NULL;
    CHECK(xr_xir_call_new(&config, 0, &cell, 1, &call) == XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(call, UINT64_MAX).status == XR_XIR_CALL_BAD_STATE && !output.seen);
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    xr_xir_domain_drop(domain); xr_xir_value_drop(&cell);
    CHECK(xr_xir_atomic_i64_load(&copy, &value) && value == INT64_MIN);
    xr_xir_value_drop(&copy);
}
typedef struct ValueGate { bool active; uint32_t admissions, releases; } ValueGate;
static void value_gate_release(void *owner) { ++((ValueGate *) owner)->releases; }
static XrXirValueStatus value_gate_admit(void *context, const XrXirFunctionBinding *binding,
                                       XrXirType type, uint64_t *work) {
    ValueGate *gate = context;
    if (!*work) return XR_XIR_VALUE_LIMIT;
    --*work; ++gate->admissions;
    return gate->active && binding->owner == gate && binding->release == value_gate_release &&
        binding->entry == 3 && type == (XrXirType) 256 ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static void arena_identity_and_revocation(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    XrXirCallableParameter parameter = {XR_XIR_STRING, 0};
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .parameters = &parameter, .parameter_count = 1, .result = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_CELL, .element = (XrXirType) 256},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) 258},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) 256},
    };
    XrXirTypes types = {nodes, 5, NULL, NULL};
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.parameters = 100; budget.metadata_bytes = 65536; budget.work = 1000;
    XrXirTypeArena *arena = NULL, *foreign = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &foreign) == XR_XIR_VALUE_OK);
    const XrXirTypes *owned = xr_xir_type_arena_types(arena);
    CHECK(owned->nodes != nodes && owned->nodes[0].parameters != &parameter);
    parameter.type = XR_XIR_BOOL; nodes[0].result = XR_XIR_BOOL;
    CHECK(owned->nodes[0].parameters[0].type == XR_XIR_STRING && owned->nodes[0].result == XR_XIR_I64);
    CHECK(xr_xir_type_is_array(owned, (XrXirType) 260) && !xr_xir_type_is_callable(owned, (XrXirType) 260));
    ValueGate gate = {true, 0, 0};
    XrXirFunctionBinding binding = {&gate, value_gate_release, 3, NULL, 0};
    XrXirValueAdmission admission = {arena, domain, value_gate_admit, &gate, 100, 0};
    XrXirValue function = {0}, cell = {0}, copy = {0}, rejected = {0};
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_valid(&function) && xr_xir_value_arena(&function) == arena);
    CHECK(xr_xir_value_argument(&function, arena, (XrXirType) 256));
    CHECK(!xr_xir_value_argument(&function, foreign, (XrXirType) 256));
    CHECK(!xr_xir_value_argument(&function, NULL, (XrXirType) 256));
    int64_t frame_slot = 0;
    CHECK(xr_xir_owned_slot_copy(&frame_slot, 0, arena, (XrXirType) 256, function.payload) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_owned_slot_copy(&frame_slot, 0, foreign, (XrXirType) 256, function.payload) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(frame_slot == function.payload);
    xr_xir_owned_slot_clear(&frame_slot, 0);
    CHECK(!frame_slot && !gate.releases);
    uint32_t admitted = gate.admissions;
    XrXirValueAdmission wrong = {foreign, domain, value_gate_admit, &gate, 100, 0};
    CHECK(xr_xir_value_admit(&function, (XrXirType) 256, &wrong) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(gate.admissions == admitted);
    XrXirValue forged = function; forged.type = 257;
    CHECK(!xr_xir_value_valid(&forged));
    CHECK(xr_xir_value_copy(&forged, &rejected) == XR_XIR_VALUE_BAD_ARGUMENT && !rejected.type);
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 258, &binding, &admission, &rejected) == XR_XIR_VALUE_BAD_ARGUMENT);
    binding.entry = 4;
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &rejected) == XR_XIR_VALUE_BAD_ARGUMENT);
    binding.entry = 3;
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType) 257, &function, &admission, &cell) == XR_XIR_VALUE_OK);
    admission.work = 1;
    CHECK(xr_xir_value_admit(&cell, (XrXirType) 257, &admission) == XR_XIR_VALUE_LIMIT);
    admission.work = 100; gate.active = false;
    CHECK(xr_xir_value_admit(&function, (XrXirType) 256, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_value_admit(&cell, (XrXirType) 257, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_write(&cell, &function, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_value_copy(&function, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_read(&cell, &rejected) == XR_XIR_VALUE_OK);
    xr_xir_type_arena_drop(foreign); xr_xir_type_arena_drop(arena);
    CHECK(xr_xir_value_valid(&function));
    xr_xir_value_drop(&cell); xr_xir_value_drop(&function);
    xr_xir_value_drop(&rejected); CHECK(!gate.releases);
    xr_xir_value_drop(&copy); CHECK(gate.releases == 1);
    CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    xr_xir_domain_drop(domain);
}
#include "xir_array_value_cases.h"
#include "xir_nominal_fixture.h"
static void nominal_arena_admission(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirTypes types = {NULL, 0, &f.table, NULL};
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats initial = xr_xir_domain_stats(domain);
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.parameters = 100; budget.metadata_bytes = 65536; budget.work = 1000;
    uint64_t bytes = budget.metadata_bytes, work = budget.work;
    XrXirTypeArena *arena = (XrXirTypeArena *) (uintptr_t) 1;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!arena && budget.metadata_bytes == bytes && budget.work == work);
    CHECK(xr_xir_domain_stats(domain).live_bytes == initial.live_bytes);
    for (uint32_t i = 0; i < 2; ++i) {
        f.declarations[i].parameter_count = 0; f.declarations[i].constraints = NULL;
    }
    f.fields[0].type = XR_XIR_I64;
    XrXirTypeNode node = {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0}};
    types.nodes = &node; types.count = 1;
    XrXirBudget proof = budget;
    CHECK(xr_xir_types_structure_verify(&types, &proof) == XR_XIR_OK);
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!arena && budget.metadata_bytes == bytes && budget.work == work);
    CHECK(xr_xir_domain_stats(domain).allocations == initial.allocations);
    xr_xir_domain_drop(domain);
}

static void nominal_arena_ownership(void) {
    NominalIdentityFixture f; nominal_identity_fixture(&f);
    XrXirType field_types[] = {XR_XIR_I64, XR_XIR_STRING};
    XrXirTypeNode node = {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, NULL, 0, field_types, 2}};
    XrXirTypes types = {&node, 1, &f.table, NULL};
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.parameters = 100; budget.metadata_bytes = 65536; budget.work = 10000;
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    uint64_t bytes = 65536 - budget.metadata_bytes, work = 10000 - budget.work;
    CHECK(xr_xir_domain_stats(domain).live_bytes == baseline + bytes);
    const XrXirNominalTable *table = xr_xir_type_arena_types(arena)->nominals;
    CHECK(table && table != &f.table && table->identities != f.identities && table->count == 2);
    CHECK((uintptr_t) table->identities % _Alignof(XrXirNominalIdentity) == 0);
    CHECK((uintptr_t) table->identities[1].fields % _Alignof(XrXirNominalFieldIdentity) == 0);
    CHECK(table->identities[0].fields != table->identities[1].fields);
    CHECK(xr_xir_type_arena_retain(arena));
    xr_xir_type_arena_drop(arena);
    CHECK(!memcmp(table->identities[0].module.bytes, "alpha", 5));
    xr_xir_type_arena_drop(arena);
    CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    for (unsigned mode = 0; mode < 2; ++mode) {
        budget.metadata_bytes = mode ? 65536 : bytes - 1;
        budget.work = mode ? work - 1 : 10000;
        uint64_t before_bytes = budget.metadata_bytes, before_work = budget.work;
        CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_LIMIT);
        CHECK(!arena && budget.metadata_bytes == before_bytes && budget.work == before_work);
        CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    }
    budget.metadata_bytes = bytes; budget.work = work;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    CHECK(!budget.metadata_bytes && !budget.work);
    memset(&f, 0xCC, sizeof(f)); memset(field_types, 0xCC, sizeof(field_types)); memset(&node, 0xCC, sizeof(node));
    CHECK(xr_xir_type_arena_types(arena)->nodes[0].nominal.fields[0] == XR_XIR_I64);
    CHECK(xr_xir_type_arena_types(arena)->nodes[0].nominal.fields[1] == XR_XIR_STRING);
    table = xr_xir_type_arena_types(arena)->nominals;
    CHECK(!memcmp(table->identities[0].fields[0].name.bytes, "value", 5));
    CHECK(table->identities[0].fields[0].name.bytes[5] == 0);
    CHECK(table->identities[1].fields[0].flags == XR_XIR_FIELD_MUTABLE);
    xr_xir_type_arena_drop(arena);
    CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    xr_xir_domain_drop(domain);
}

#include "xir_struct_value_cases.h"
static void string_queries(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    const char text[] = "a\0b\xF0\x9F\x98\x80";
    XrXirValue a = {0}, b = {0}, prefix = {0}, empty = {0}, different = {0};
    CHECK(xr_xir_string_new(domain, text, sizeof(text) - 1, &a) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, text, sizeof(text) - 1, &b) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "a", 1, &prefix) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, NULL, 0, &empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "a\0c\xF0\x9F\x98\x80", 7, &different) == XR_XIR_VALUE_OK);
    uint64_t before = xr_xir_domain_stats(domain).live_bytes;
    bool equal = false; int64_t length = -1;
    CHECK(xr_xir_string_equal(&a, &b, &equal) && equal);
    CHECK(xr_xir_string_equal(&a, &prefix, &equal) && !equal);
    CHECK(xr_xir_string_equal(&a, &different, &equal) && !equal);
    CHECK(xr_xir_string_equal(&empty, &empty, &equal) && equal);
    CHECK(xr_xir_string_length(&a, &length) == XR_XIR_VALUE_OK && length == 4);
    CHECK(xr_xir_string_length(&empty, &length) == XR_XIR_VALUE_OK && !length);
    XrXirValue invalid = {0}; length = 19; equal = true;
    CHECK(xr_xir_string_length(&invalid, &length) == XR_XIR_VALUE_BAD_ARGUMENT && length == 19);
    CHECK(!xr_xir_string_equal(&a, &invalid, &equal) && equal);
    CHECK(!xr_xir_string_equal(&a, &b, NULL));
    CHECK(xr_xir_domain_stats(domain).live_bytes == before);
    xr_xir_domain_drop(domain);
    xr_xir_value_drop(&a);
    CHECK(xr_xir_string_length(&b, &length) == XR_XIR_VALUE_OK && length == 4);
    xr_xir_value_drop(&b); xr_xir_value_drop(&prefix); xr_xir_value_drop(&empty); xr_xir_value_drop(&different);
}
static void string_predicates(void) {
    static const struct { const char *text, *pattern; size_t text_length, pattern_length;
        bool contains, starts, ends; } cases[] = {
        {"", "", 0, 0, true, true, true},
        {"", "a", 0, 1, false, false, false},
        {"abc", "", 3, 0, true, true, true},
        {"abc", "abc", 3, 3, true, true, true},
        {"abc", "abcd", 3, 4, false, false, false},
        {"abc", "b", 3, 1, true, false, false},
        {"a\0b", "a\0", 3, 2, true, true, false},
        {"a\0b", "\0b", 3, 2, true, false, true},
        {"a\0b", "a\0c", 3, 3, false, false, false},
        {"\xE4\xB8\xAD\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80", 7, 4, true, false, true},
        {"e\xCC\x81", "\xC3\xA9", 3, 2, false, false, false},
        {"xxabcdefghijklmnop", "abcdefghijklmnop", 18, 16, true, false, true},
        {"xxabcdefghijklmnoq", "abcdefghijklmnop", 18, 16, false, false, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrXirDomain *domain = NULL;
        CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
        XrXirValue text = {0}, pattern = {0}, copy = {0};
        CHECK(xr_xir_string_new(domain, cases[i].text, cases[i].text_length, &text) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domain, cases[i].pattern, cases[i].pattern_length, &pattern) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_value_copy(&text, &copy) == XR_XIR_VALUE_OK);
        XrXirDomainStats before = xr_xir_domain_stats(domain); bool result;
        CHECK(xr_xir_string_contains(&text, &pattern, &result) && result == cases[i].contains);
        CHECK(xr_xir_string_starts_with(&text, &pattern, &result) && result == cases[i].starts);
        CHECK(xr_xir_string_ends_with(&text, &pattern, &result) && result == cases[i].ends);
        CHECK(xr_xir_string_contains(&text, &copy, &result) && result);
        XrXirValue invalid = {0}; result = true;
        CHECK(!xr_xir_string_contains(&invalid, &pattern, &result) && result);
        CHECK(!xr_xir_string_starts_with(&text, &invalid, &result) && result);
        CHECK(!xr_xir_string_ends_with(&invalid, &pattern, &result) && result);
        CHECK(!xr_xir_string_contains(&text, &pattern, NULL));
        CHECK(!xr_xir_string_starts_with(&text, &pattern, NULL));
        CHECK(!xr_xir_string_ends_with(&text, &pattern, NULL));
        XrXirDomainStats after = xr_xir_domain_stats(domain);
        CHECK(before.live_bytes == after.live_bytes && before.allocations == after.allocations &&
            before.frees == after.frees && before.reallocations == after.reallocations);
        xr_xir_domain_drop(domain); xr_xir_value_drop(&text);
        CHECK(xr_xir_string_contains(&copy, &pattern, &result) && result == cases[i].contains);
        CHECK(xr_xir_string_starts_with(&copy, &pattern, &result) && result == cases[i].starts);
        CHECK(xr_xir_string_ends_with(&copy, &pattern, &result) && result == cases[i].ends);
        xr_xir_value_drop(&copy); xr_xir_value_drop(&pattern);
    }
}
static void string_search_coordinates(void) {
    static const struct { const char *text, *pattern; size_t bytes, pattern_bytes;
        int64_t start, first, last; } cases[] = {
        {"", "", 0, 0, 0, 0, 0}, {"", "a", 0, 1, 0, -1, -1},
        {"abcabc", "bc", 6, 2, 2, 4, 4}, {"abc", "", 3, 0, 3, 3, 3},
        {"abc", "c", 3, 1, 3, -1, 2}, {"abc", "abcd", 3, 4, 0, -1, -1},
        {"A\xE4\xB8\xAD\xF0\x9F\x98\x80" "B", "B", 9, 1, 0, 3, 3},
        {"A\xE4\xB8\xAD\xF0\x9F\x98\x80" "B", "\xF0\x9F\x98\x80", 9, 4, 3, -1, 2},
        {"\xE4\xB8\xAD" "a\xE4\xB8\xAD" "a", "a", 8, 1, 2, 3, 3},
        {"e\xCC\x81", "\xC3\xA9", 3, 2, 0, -1, -1},
        {"e\xCC\x81", "\xCC\x81", 3, 2, 0, 1, 1},
        {"a\0b\0", "\0", 4, 1, 2, 3, 3},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrXirDomain *domain = NULL; XrXirValue text = {0}, pattern = {0};
        CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domain, cases[i].text, cases[i].bytes, &text) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domain, cases[i].pattern, cases[i].pattern_bytes, &pattern) == XR_XIR_VALUE_OK);
        XrXirDomainStats before = xr_xir_domain_stats(domain); int64_t result = 99, count = 0;
        CHECK(xr_xir_string_index_of(&text, &pattern, cases[i].start, &result) == XR_XIR_VALUE_OK && result == cases[i].first);
        CHECK(xr_xir_string_last_index_of(&text, &pattern, &result) == XR_XIR_VALUE_OK && result == cases[i].last);
        CHECK(xr_xir_string_length(&text, &count) == XR_XIR_VALUE_OK);
        result = 99;
        CHECK(xr_xir_string_index_of(&text, &pattern, -1, &result) == XR_XIR_VALUE_BOUNDS && result == 99);
        CHECK(xr_xir_string_index_of(&text, &pattern, count + 1, &result) == XR_XIR_VALUE_BOUNDS && result == 99);
        XrXirValue invalid = {0};
        CHECK(xr_xir_string_index_of(&invalid, &pattern, 0, &result) == XR_XIR_VALUE_BAD_ARGUMENT && result == 99);
        CHECK(xr_xir_string_last_index_of(&text, &invalid, &result) == XR_XIR_VALUE_BAD_ARGUMENT && result == 99);
        CHECK(xr_xir_string_index_of(&text, &pattern, 0, NULL) == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_string_last_index_of(&text, &pattern, NULL) == XR_XIR_VALUE_BAD_ARGUMENT);
        XrXirDomainStats after = xr_xir_domain_stats(domain);
        CHECK(before.allocations == after.allocations && before.frees == after.frees &&
            before.live_bytes == after.live_bytes && before.reallocations == after.reallocations);
        xr_xir_domain_drop(domain);
        CHECK(xr_xir_string_index_of(&text, &pattern, cases[i].start, &result) == XR_XIR_VALUE_OK && result == cases[i].first);
        xr_xir_value_drop(&text); xr_xir_value_drop(&pattern);
    }
}
int main(void) {
    string_search_coordinates();
    string_predicates();
    string_queries();
    struct_value_cases();
    nominal_arena_ownership();
    nominal_arena_admission();
    array_value_cases();
    arena_identity_and_revocation(); unicode_cases(); cow_cases(); concurrent_copies(0); concurrent_copies(1); concurrent_copies(2); typed_output(); atomic_boundaries();
    puts("Strict Unicode, CoW growth, independent lifetime and concurrent owned copies passed");
    return 0;
}
