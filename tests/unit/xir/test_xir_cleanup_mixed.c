/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cleanup_mixed.c - Source cleanup across both VM/native body boundaries
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_cleanup_source_cases.h"
XR_DATA const XrXirProgramSpec cleanup_source_program;
XR_DATA const uint32_t cleanup_source_functions[CLEANUP_SOURCE_FUNCTIONS];
XR_DATA const uint32_t cleanup_source_fatal_functions[4];
static void cleanup_source_mixed(bool native_cleanup, unsigned fatal_mode) {
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_checked_read(cleanup_source_program.proof.bytes, cleanup_source_program.proof.length,
        NULL, &checked, NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    XrXirCallEntry *entries = xr_calloc(module->function_count, sizeof(*entries));
    XrXirVmBinding *bindings = xr_calloc(module->function_count, sizeof(*bindings));
    CHECK(entries && bindings);
    unsigned native = 0, vm = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool cleanup = module->declarations->functions[f].cleanup_owner != 0;
        if (cleanup == native_cleanup) { entries[f] = cleanup_source_program.entries[f]; ++native; }
        else { CHECK(xr_xir_vm_bind(lowered, f, &bindings[f], &entries[f]) == XR_XIR_OK); ++vm; }
    }
    CHECK(native && vm);
    XrXirProgramSpec spec = cleanup_source_program; spec.entries = entries;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&spec, (XrXirProgramBudget){16777216, 64000000}, &program) == XR_XIR_OK);
    if (fatal_mode < 4) cleanup_source_fatal(program, cleanup_source_fatal_functions[fatal_mode]);
    cleanup_source_allocations(program, cleanup_source_functions);
    cleanup_source_cases(program, cleanup_source_functions);
    xr_xir_artifact_free(lowered); xr_free(entries); xr_free(bindings);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 4 && !strcmp(argv[1], "--fatal")));
    if (argc == 4) {
        unsigned mode = (unsigned)atoi(argv[2]); CHECK(mode < 4);
        cleanup_source_mixed(atoi(argv[3]) != 0, mode);
    }
    cleanup_source_mixed(false, 4); cleanup_source_mixed(true, 4);
    puts("Source cleanup in both VM/native directions matched independent expectations");
    return 0;
}
