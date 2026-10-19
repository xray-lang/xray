/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_observation_resources.c - Cumulative observed Source transactions
 */
#define XR_OBSERVATION_RESOURCE_MAIN 1
#include "test_xir_mixed_observation.c"
#include "xir_mixed_observation_faults.h"

int main(int argc, char **argv) {
    (void)&observation_normal_main;
    CHECK(argc == 3); TerminalBackend backend = TERMINAL_MIXED_VM_ROOT;
    while (backend <= TERMINAL_NATIVE && strcmp(argv[1], terminal_backend_names[backend])) ++backend;
    CHECK(backend <= TERMINAL_NATIVE);
    const bool faults = !strcmp(argv[2], "fi");
    CHECK(faults || !strcmp(argv[2], "axes"));
    XrXirProgram *program = observation_program(backend);
    if (faults) observation_faults(program, backend);
    else observation_axes(program, backend);
    xr_xir_compile_program_drop(program);
    CHECK(!observation_code && terminal_code_releases == 1 && !runtime_live && !runtime_bytes);
    effects_source_owners_free();
    printf("Observed Source resources backend=%s mode=%s two Instances two rounds same Domains, cumulative no refund, typed failures and physical0/0\n",
        terminal_backend_names[backend], argv[2]);
    return 0;
}
