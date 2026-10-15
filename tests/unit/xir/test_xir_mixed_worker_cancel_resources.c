/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_worker_cancel_resources.c - Finite resources at every real worker prefix
 */
#define XR_WORKER_CANCEL_RESOURCE_MAIN
#include "test_xir_mixed_worker_cancel.c"
#include "xir_mixed_worker_cancel_faults.h"
int main(int argc, char **argv) {
    CHECK(argc == 4); (void)worker_cancel_main;
    const WorkerControlCase *oracle = NULL; unsigned backend = 0;
    for (unsigned i = 0; i < 7; ++i) if (!strcmp(argv[1], worker_control_cases[i].name)) oracle = &worker_control_cases[i];
    while (backend < 4 && strcmp(argv[2], worker_backends[backend])) ++backend;
    CHECK(oracle && backend < 4); bool fi = !strcmp(argv[3], "fi"); CHECK(fi || !strcmp(argv[3], "axes"));
    XrXirProgram *program = worker_control_program(oracle, backend);
    unsigned prefixes = worker_cancel_count(program, oracle); CHECK(prefixes);
    worker_cancel_fault_axes(program, oracle, backend, prefixes, fi);
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !runtime_live && !runtime_bytes);
    effects_source_owners_free(); return 0;
}
