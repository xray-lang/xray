/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_access_native.c - Generated Class permission across native and VM entries
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
#if XR_CLASS_ACCESS_BACKEND!=1 && XR_CLASS_ACCESS_BACKEND!=2
#error "Class access consumer requires an authentic native or mixed route"
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_class_access_compile_faults.h"
#include "xir_cell_lifecycle_runtime_allocations.h"
#include "xir_source_fixture_owner.h"
#include "xir_cell_owner_source_pipeline.h"
#include "xir_class_access_resource_types.h"
#include "xir_class_access_native_backend.h"
#include "xir_class_access_runtime_transactions.h"
#include "xir_class_access_runtime_control.h"
#include "xir_class_access_runtime_retry.h"
#include "xir_class_access_runtime_operation.h"
#include "xir_class_access_runtime_faults.h"
#include "xir_class_access_runtime_axes.h"
#include "xir_class_access_runtime_cases.h"

static void access_native_rejections(void) {
    for (unsigned i=0;i<5;++i) if (access_source_cases[i].rejected) {
        const AccessGeneratedFixture *generated=&access_generated_fixtures[i];
        CHECK(!generated->program && !generated->native && !generated->count);
        CHECK(generated->entries.run==UINT32_MAX && generated->entries.counter==UINT32_MAX &&
            generated->entries.factory==UINT32_MAX);
        XrXirCompileContext context={0};SourceFixtureOwner owner={0};
        const XrCompileResourceLimits limits=access_compile_limits();
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();owner.context=context;
        CHECK(xr_compile_resources_stats(context.resources,&owner.baseline)==XR_COMPILE_RESOURCE_OK);
        XrXirProgram *program=NULL;CellSourceEntries entries={UINT32_MAX,UINT32_MAX,UINT32_MAX};
        CHECK(cell_source_program(&context,access_source_cases[i].name,&program,&entries,false)==XR_XIR_BAD_TYPE);
        CHECK(!program && entries.run==UINT32_MAX && entries.counter==UINT32_MAX && entries.factory==UINT32_MAX);
        source_fixture_owner_free(&owner);access_physical_zero();
        printf("CLASS_ACCESS_BACKEND_REJECT fixture=%s status=%u backend=%u Program=0 generatedC=0\n",
            access_source_cases[i].name,XR_XIR_BAD_TYPE,XR_CLASS_ACCESS_BACKEND);
    }
}
int main(int argc,char **argv) {
    const char *operation=argc==1?"--normal":argc==2?argv[1]:NULL;
    if (!operation) return 2;
    if (!strcmp(operation,"--normal")) {
        access_native_rejections();access_runtime_cases("--runtime-control");
    } else if (!strcmp(operation,"--runtime-fi") || !strcmp(operation,"--runtime-axes") ||
        !strcmp(operation,"--runtime-prepare") || !strcmp(operation,"--runtime-control")) access_runtime_cases(operation);
    else return 2;
    access_physical_zero();CHECK(!cell_runtime_observer_live);
    printf("Class access native consumer: backend=%u operation=%s compiler/runtime physical=0/0\n",
        XR_CLASS_ACCESS_BACKEND,operation);return 0;
}
