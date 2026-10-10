/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cell_loan_lifecycle.c - Actual Source frames restore Cell loans on every exit
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_enum.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_cell_lifecycle_runtime_allocations.h"
#include "xir_source_fixture_owner.h"
#include "xir_cell_owner_source_pipeline.h"
#include "xir_cell_loan_lifecycle_cases.h"

static void cell_life_zero(void) {
    instance_compile_zero();
    if (runtime_live || runtime_bytes || runtime_owned) runtime_report_residuals(stderr);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
}
static XrXirProgram *cell_life_program(const XrXirCompileContext *context, const char *fixture, uint32_t *entries) {
    XrXirArtifact *lowered = NULL;
    XrXirProgram *program = NULL;
    CHECK(cell_source_lower(context, fixture, &lowered, false) == XR_XIR_OK && lowered);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    const char *names[] = {"run", "cellCounter", "caughtThrow", "caughtPanic", "uncaughtThrow", "suspended"};
    CHECK(module && module->declarations && module->declarations->functions);
    for (unsigned i = 0; i < CELL_LIFE_ENTRIES; ++i) entries[i] = cell_source_entry(module, names[i], true);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered && program);
    return program;
}
static void cell_life_mode(const char *fixture, unsigned mode) {
    CHECK(mode < CELL_LIFE_MODES);
    cell_life_zero();
    SourceFixtureOwner owner = {0};
    source_fixture_owner_new(&owner);
    uint32_t entries[CELL_LIFE_ENTRIES];
    XrXirProgram *program = cell_life_program(&owner.context, fixture, entries);
    const int64_t thrown[] = {11}, resumed[] = {1, 13, 117}, cancelled[] = {1, 11, 111};
    CellLifecycleLog log = {0};
    if (mode == CELL_LIFE_THROW || mode == CELL_LIFE_PANIC || mode == CELL_LIFE_UNCAUGHT) {
        log.expected = thrown;
        log.count = 1;
    } else if (mode >= CELL_LIFE_RESUME) {
        log.expected = mode == CELL_LIFE_CANCEL || mode == CELL_LIFE_STOP || mode == CELL_LIFE_CLOSE ? cancelled : resumed;
        log.count = 3;
    }
    log.entries = entries;
    log.reentrant = mode == CELL_LIFE_REENTRANT;
    if (log.reentrant) log.other = cell_life_instance(program, NULL);
    XrXirInstance *instance = cell_life_instance(program, &log);
    log.active = instance;
    xr_xir_compile_program_drop(program);
    XrXirValue result = {0};
    if (mode < CELL_LIFE_RESUME) cell_life_terminal(instance, entries, mode, &result);
    else cell_life_suspended(&instance, entries, mode, &result);
    CHECK(log.at == log.count && log.reentered == (unsigned)log.reentrant);
    if (instance) {
        cell_life_cost(instance, mode);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    if (log.other) {
        CHECK(xr_xir_instance_free(log.other) == XR_XIR_CALL_READY);
        cell_life_number(&log.other_result, 2);
        cell_life_number(&log.other_counter, 2);
    }
    if (mode == CELL_LIFE_UNCAUGHT) cell_life_problem(&result);
    else if (mode == CELL_LIFE_RETURN) cell_life_number(&result, 2);
    else if (mode == CELL_LIFE_THROW || mode == CELL_LIFE_PANIC) cell_life_number(&result, 12);
    else if (mode == CELL_LIFE_RESUME || mode == CELL_LIFE_REENTRANT) cell_life_number(&result, 3);
    else CHECK(!result.type && !result.reserved && !result.payload);
    xr_xir_value_drop(&result);
    xr_xir_value_drop(&log.other_result);
    xr_xir_value_drop(&log.other_counter);
    source_fixture_owner_free(&owner);
    cell_life_zero();
    printf("Cell lifecycle mode%u compiler/runtime physical blocks/bytes=0/0\n", mode);
}
static void cell_life_existing(const char *name, bool unit) {
    cell_life_zero();
    SourceFixtureOwner owner = {0};
    source_fixture_owner_new(&owner);
    XrXirProgram *program = NULL;
    CellSourceEntries entries = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    CHECK(cell_source_program(&owner.context, name, &program, &entries, unit) == XR_XIR_OK && program);
    XrXirInstance *instance = cell_life_instance(program, NULL);
    xr_xir_compile_program_drop(program);
    XrXirValue results[2] = {{0}};
    for (unsigned pass = 0; pass < 2; ++pass) {
        XrXirCallStatus status = cell_life_run(instance, entries.run, &results[pass]);
        if (unit) CHECK(status == XR_XIR_CALL_RETURNED);
        else {
            CHECK(status == XR_XIR_CALL_BAD_ARGUMENT && !results[pass].type && !results[pass].payload);
            CHECK(entries.counter != UINT32_MAX);
            XrXirValue counter = {0};
            CHECK(cell_life_run(instance, entries.counter, &counter) == XR_XIR_CALL_RETURNED);
            cell_life_number(&counter, 0);
            xr_xir_value_drop(&counter);
        }
    }
    cell_life_cost(instance, unit ? 10 : 9);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    for (unsigned pass = 0; pass < 2; ++pass) {
        if (unit) cell_life_number(&results[pass], 7);
        xr_xir_value_drop(&results[pass]);
    }
    source_fixture_owner_free(&owner);
    cell_life_zero();
    printf("Cell existing %s unchanged expectations and compiler/runtime physical0\n", name);
}
#include "xir_cell_lifecycle_resources.h"
int main(int argc, char **argv) {
    const char *fixtures[] = {"lifecycle.xr", "indirect_ref_lifecycle.xr", "project_lifecycle.xr", "class_project_lifecycle.xr"};
    if (argc == 2) {
        CHECK(!strcmp(argv[1],"--runtime-normal") || !strcmp(argv[1],"--runtime-fi") ||
            !strcmp(argv[1],"--runtime-axes") || !strcmp(argv[1],"--runtime-prepare"));
        cell_life_runtime_resources(fixtures, sizeof(fixtures)/sizeof(fixtures[0]), argv[1]);
        instance_compile_report();
        puts("Cell lifecycle real runtime resource scenarios complete; exact results and physical0 preserved");
        return 0;
    }
    CHECK(argc == 1);
    for (unsigned f = 0; f < sizeof(fixtures)/sizeof(fixtures[0]); ++f)
        for (unsigned mode = 0; mode < CELL_LIFE_MODES; ++mode) cell_life_mode(fixtures[f],mode);
    cell_life_existing("duplicate_actual.xr", false);
    cell_life_existing("unit_ref.xr", true);
    instance_compile_report();
    puts("Source Cell loan exits, suspended cleanup, repeated preparation rejection and reentrant isolation passed");
    return 0;
}
