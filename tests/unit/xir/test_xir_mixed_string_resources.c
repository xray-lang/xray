/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_string_resources.c - Owned child strings preserve finite execution ledgers
 */
#define XR_STRING_WORKER_RESOURCE_MAIN
#include "test_xir_mixed_string_worker.c"
#include "xir_mixed_retained_faults.h"
static int string_resources_main(int argc, char **argv) {
    CHECK(argc == 3); (void)string_worker_main;
    unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], worker_backends[backend])) ++backend;
    CHECK(backend < 4); bool fi = !strcmp(argv[2], "fi"); CHECK(fi || !strcmp(argv[2], "axes"));
    const WorkerControlCase *control = &worker_control_cases[0];
    RetainedFaultOracle oracle = {.control=control, .observed_instance=&string_worker_instance,
        .observed_task=&string_worker_pending, .expect_pending=string_worker_expect_pending, .check_value=string_worker_bytes};
    XrXirProgram *program = worker_control_program(control, backend); retained_fault_axes(program, &oracle, backend, fi);
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !runtime_live && !runtime_bytes);
    effects_source_owners_free(); return 0;
}
int main(int argc, char **argv) { return string_resources_main(argc, argv); }
