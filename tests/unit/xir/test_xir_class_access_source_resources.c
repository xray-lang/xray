/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_access_source_resources.c - Full Source and runtime Class access faults
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
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_class_access_compile_faults.h"
#include "xir_cell_lifecycle_runtime_allocations.h"
#include "xir_source_fixture_owner.h"
#include "xir_cell_owner_source_pipeline.h"
#include "xir_class_access_resource_types.h"
#include "xir_class_access_runtime_transactions.h"
#include "xir_class_access_runtime_control.h"
#include "xir_class_access_runtime_retry.h"
#include "xir_class_access_runtime_operation.h"
#include "xir_class_access_runtime_faults.h"
#include "xir_class_access_runtime_axes.h"
#include "xir_class_access_runtime_cases.h"
#include "xir_class_access_compile_operation.h"
#include "xir_class_access_compile_cases.h"
#include "xir_class_access_native_producer.h"
int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"--write-c")) return access_write_c(argc,argv);
    const char *operation=argc==1?"--normal":argc==2?argv[1]:NULL;
    if (!operation) return 2;
    if (!strcmp(operation,"--normal") || !strcmp(operation,"--compiler-fi") || !strcmp(operation,"--compiler-axes"))
        access_compiler_cases(operation);
    else if (!strcmp(operation,"--runtime-fi") || !strcmp(operation,"--runtime-axes") ||
        !strcmp(operation,"--runtime-prepare") || !strcmp(operation,"--runtime-control")) access_runtime_cases(operation);
    else return 2;
    access_physical_zero();CHECK(!cell_runtime_observer_live);
    printf("Class access Source resources: operation=%s compiler/runtime physical=0/0\n",operation);
    return 0;
}
