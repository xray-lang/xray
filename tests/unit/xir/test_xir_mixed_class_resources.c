/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_class_resources.c - Finite Source class transactions and faults
 */
#define XR_CLASS_LIFETIME_RESOURCE_MAIN 1
#include "test_xir_mixed_class_lifetime.c"
#include "xir_mixed_class_faults.h"
int main(int argc, char **argv) {
    (void)&class_lifetime_normal_main;
    CHECK(argc == 3); unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], class_backends[backend])) ++backend;
    CHECK(backend < 4); const bool faults = !strcmp(argv[2], "fi");
    CHECK(faults || !strcmp(argv[2], "axes"));
    XrXirProgram *program = class_lifetime_program(backend);
    if (faults) class_faults(program, backend); else class_axes(program, backend);
    xr_xir_compile_program_drop(program);
    CHECK(class_code_releases == 1 && !runtime_live && !runtime_bytes); effects_source_owners_free();
    printf("Class resources backend=%s mode=%s same Instance/Domain two rounds, typed failures, retained data and physical0/0\n",
        class_backends[backend], argv[2]);
    return 0;
}
