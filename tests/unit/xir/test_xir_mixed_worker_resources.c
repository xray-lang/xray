/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_worker_resources.c - Same-Domain worker limits and actual allocation faults
 */
#define XR_WORKER_CONTROL_RESOURCE_MAIN
#include "test_xir_mixed_worker_controls.c"
#include "xir_mixed_worker_faults.h"
int main(int argc, char **argv) {
    CHECK(argc == 4); (void)worker_control_main;
    const WorkerControlCase *oracle = NULL; unsigned backend = 0;
    for (unsigned i = 0; i < 7; ++i) if (!strcmp(argv[1], worker_control_cases[i].name)) oracle = &worker_control_cases[i];
    while (backend < 4 && strcmp(argv[2], worker_backends[backend])) ++backend;
    CHECK(oracle && backend < 4);
    XrXirProgram *program = worker_control_program(oracle, backend);
    if (!strcmp(argv[3], "axes")) worker_axes(program, oracle, backend);
    else { CHECK(!strcmp(argv[3], "fi")); worker_faults(program, oracle, backend); }
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !runtime_live && !runtime_bytes);
    effects_source_owners_free(); return 0;
}
