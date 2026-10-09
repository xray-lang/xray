/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cell_owner_source.c - Cell ownership through the real Source VM pipeline
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_source_fixture_owner.h"
#include "xir_cell_owner_source_pipeline.h"

typedef enum CellSourceKind { CELL_SOURCE_VALUE, CELL_SOURCE_FORWARD, CELL_SOURCE_FACTORY,
    CELL_SOURCE_ALIAS, CELL_SOURCE_REJECT, CELL_SOURCE_MODE_REJECT } CellSourceKind;
typedef struct CellSourceCase {
    const char *name;
    CellSourceKind kind;
    int64_t expected;
    XrXirCallStatus failure;
    bool unit;
} CellSourceCase;
static const CellSourceCase cell_source_cases[] = {
    {"forward.xr", CELL_SOURCE_FORWARD, 1, XR_XIR_CALL_READY, false},
    {"local_forward.xr", CELL_SOURCE_VALUE, 42, XR_XIR_CALL_READY, false},
    {"owned_capture.xr", CELL_SOURCE_FACTORY, 12, XR_XIR_CALL_READY, false},
    {"unit_ref.xr", CELL_SOURCE_VALUE, 7, XR_XIR_CALL_READY, true},
    {"actual_alias.xr", CELL_SOURCE_ALIAS, 0, XR_XIR_CALL_BAD_STATE, false},
    {"module_alias_read.xr", CELL_SOURCE_ALIAS, 0, XR_XIR_CALL_BAD_STATE, false},
    {"module_alias_write.xr", CELL_SOURCE_ALIAS, 0, XR_XIR_CALL_BAD_STATE, false},
    {"noescape_capture.xr", CELL_SOURCE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"cleanup_forward.xr", CELL_SOURCE_VALUE, 10, XR_XIR_CALL_READY, false},
    {"recursive_owned_capture.xr", CELL_SOURCE_VALUE, 7, XR_XIR_CALL_READY, false},
    {"scoped_nested_capture.xr", CELL_SOURCE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"scoped_go.xr", CELL_SOURCE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"duplicate_actual.xr", CELL_SOURCE_ALIAS, 0, XR_XIR_CALL_BAD_ARGUMENT, false},
    {"worker_local_ref.xr", CELL_SOURCE_VALUE, 42, XR_XIR_CALL_READY, false},
    {"worker_module_ref.xr", CELL_SOURCE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"duplicate_static.xr", CELL_SOURCE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"worker_local_ref_arg.xr", CELL_SOURCE_VALUE, 42, XR_XIR_CALL_READY, false},
    {"indirect_ref_forward.xr", CELL_SOURCE_FORWARD, 1, XR_XIR_CALL_READY, false},
    {"indirect_ref_local.xr", CELL_SOURCE_VALUE, 42, XR_XIR_CALL_READY, false},
    {"indirect_ref_unit.xr", CELL_SOURCE_VALUE, 7, XR_XIR_CALL_READY, true},
    {"indirect_ref_generic.xr", CELL_SOURCE_VALUE, 41, XR_XIR_CALL_READY, false},
    {"indirect_ref_mode_erasure.xr", CELL_SOURCE_MODE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"indirect_ref_missing_marker.xr", CELL_SOURCE_MODE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"indirect_ref_immutable.xr", CELL_SOURCE_MODE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"indirect_ref_duplicate_static.xr", CELL_SOURCE_MODE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"indirect_ref_wrong_type.xr", CELL_SOURCE_MODE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"indirect_ref_scoped_capture.xr", CELL_SOURCE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"indirect_ref_go.xr", CELL_SOURCE_REJECT, 0, XR_XIR_CALL_READY, false},
    {"indirect_ref_worker_signature.xr", CELL_SOURCE_VALUE, 42, XR_XIR_CALL_READY, false}
};
static void cell_source_physical_zero(const char *name) {
    instance_compile_zero();
    if (runtime_live || runtime_bytes || runtime_owned) runtime_report_residuals(stderr);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
    printf("%s compiler/runtime physical blocks/bytes=0/0\n", name);
}
static XrXirInstance *cell_source_instance(XrXirProgram *program) {
    XrXirInstanceConfig config;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY && instance);
    return instance;
}
static XrXirCallStatus cell_source_execute(XrXirInstance *instance, uint32_t entry,
    XrXirValue *result) {
    CHECK(result && !result->type && !result->reserved && !result->payload);
    XrXirCallStatus status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
    if (status == XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance, result) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_value_valid(result));
    }
    return status;
}
static void cell_source_i64(const XrXirValue *value, int64_t expected) {
    CHECK(xr_xir_value_valid(value) && value->type == XR_XIR_I64 && !value->reserved);
    CHECK(value->payload == expected);
}
static void cell_source_forward(XrXirProgram *program, const CellSourceEntries *entries) {
    XrXirInstance *first = cell_source_instance(program), *fresh = cell_source_instance(program);
    XrXirValue results[3] = {{0}};
    xr_xir_compile_program_drop(program);
    CHECK(cell_source_execute(first, entries->run, &results[0]) == XR_XIR_CALL_RETURNED);
    CHECK(cell_source_execute(first, entries->run, &results[1]) == XR_XIR_CALL_RETURNED);
    CHECK(cell_source_execute(fresh, entries->run, &results[2]) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(fresh) == XR_XIR_CALL_READY);
    cell_source_i64(&results[0], 1);
    cell_source_i64(&results[1], 2);
    cell_source_i64(&results[2], 1);
    for (unsigned i = 0; i < 3; ++i) xr_xir_value_drop(&results[i]);
}
static void cell_source_factory(XrXirProgram *program, const CellSourceEntries *entries) {
    XrXirInstance *instance = cell_source_instance(program);
    XrXirValue results[3] = {{0}}, function = {0};
    CHECK(entries->factory != UINT32_MAX);
    xr_xir_compile_program_drop(program);
    CHECK(cell_source_execute(instance, entries->run, &results[0]) == XR_XIR_CALL_RETURNED);
    CHECK(cell_source_execute(instance, entries->factory, &function) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_function_binding(&function));
    for (unsigned i = 1; i < 3; ++i) {
        CHECK(xr_xir_instance_start_function(instance, &function, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance, &results[i]) == XR_XIR_CALL_RETURNED);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_value_valid(&function) && xr_xir_function_binding(&function));
    cell_source_i64(&results[0], 12);
    cell_source_i64(&results[1], 1);
    cell_source_i64(&results[2], 2);
    for (unsigned i = 0; i < 3; ++i) xr_xir_value_drop(&results[i]);
    xr_xir_value_drop(&function);
}
static void cell_source_alias(XrXirProgram *program, const CellSourceEntries *entries,
    XrXirCallStatus expected) {
    XrXirInstance *instance = cell_source_instance(program);
    XrXirValue rejected = {0}, counter = {0};
    CHECK(entries->counter != UINT32_MAX);
    xr_xir_compile_program_drop(program);
    CHECK(cell_source_execute(instance, entries->run, &rejected) == expected);
    CHECK(!rejected.type && !rejected.reserved && !rejected.payload);
    /* Only a real Source entry observes the counter after the rejected call. */
    CHECK(cell_source_execute(instance, entries->counter, &counter) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    cell_source_i64(&counter, 0);
    xr_xir_value_drop(&counter);
    xr_xir_value_drop(&rejected);
}
static void cell_source_value(XrXirProgram *program, const CellSourceEntries *entries,
    int64_t expected) {
    XrXirInstance *instance = cell_source_instance(program);
    XrXirValue result = {0};
    xr_xir_compile_program_drop(program);
    CHECK(cell_source_execute(instance, entries->run, &result) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    cell_source_i64(&result, expected);
    xr_xir_value_drop(&result);
}
static void cell_source_case(const CellSourceCase *fixture) {
    cell_source_physical_zero("before fresh fixture");
    SourceFixtureOwner owner = {0};
    source_fixture_owner_new(&owner);
    XrXirProgram *program = NULL;
    CellSourceEntries entries = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    XrXirStatus status = cell_source_program(&owner.context, fixture->name, &program, &entries, fixture->unit);
    if (fixture->kind == CELL_SOURCE_MODE_REJECT) {
        CHECK(status == XR_XIR_BAD_TYPE && !program);
    } else if (fixture->kind == CELL_SOURCE_REJECT) {
        CHECK(status != XR_XIR_OK && status != XR_XIR_BUDGET && status != XR_XIR_OUT_OF_MEMORY && !program);
    } else {
        if (status != XR_XIR_OK) fprintf(stderr, "%s whole pipeline status=%u\n", fixture->name, status);
        CHECK(status == XR_XIR_OK && program);
        switch (fixture->kind) {
        case CELL_SOURCE_FORWARD: cell_source_forward(program, &entries); break;
        case CELL_SOURCE_FACTORY: cell_source_factory(program, &entries); break;
        case CELL_SOURCE_ALIAS: cell_source_alias(program, &entries, fixture->failure); break;
        default: cell_source_value(program, &entries, fixture->expected); break;
        }
        program = NULL;
    }
    xr_xir_compile_program_drop(program);
    source_fixture_owner_free(&owner);
    cell_source_physical_zero(fixture->name);
}
int main(void) {
    for (unsigned i = 0; i < sizeof(cell_source_cases) / sizeof(cell_source_cases[0]); ++i) {
        printf("Source Cell case %u: %s\n", i, cell_source_cases[i].name);
        cell_source_case(&cell_source_cases[i]);
    }
    instance_compile_report();
    printf("Source Cell ownership: %zu independent finite owners and real VM executions passed\n",
        sizeof(cell_source_cases) / sizeof(cell_source_cases[0]));
    return 0;
}
