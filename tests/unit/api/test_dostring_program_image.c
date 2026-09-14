/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dostring_program_image.c - An eval'd script runs the modules of its
 *                                 graph as compiled for it.
 *
 * KEY CONCEPT:
 *   Generics are specialized graph-wide: a script that instantiates a generic
 *   declared by a standard library module materializes the clone in that
 *   module. The runtime's embedded copy of the module was compiled once for
 *   no program in particular and does not contain the clone, so the script's
 *   own compilation of the module must be the one that loads. The program
 *   image built from the compiled graph supplies exactly that, for standard
 *   library layers and file modules alike.
 *
 *   Each script states its expectation with the builtin `assert`, so a
 *   failure names the condition that broke rather than a bare exit code.
 */

#include "../test_framework.h"
#include "xray_vm.h"
#include "runtime/xisolate_api.h"
#include "module/xmodule_identity.h"
#include <stddef.h>

static const XrModuleIdentityAuthority k_program_image_memory_authority = {
    .kind = XR_MODULE_IDENTITY_MEMORY,
    .namespace_id = "dostring-program-image-fixture-v1",
};

static int run_eval_script(const char *source) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    if (!iso)
        return -1;
    int rc = xr_isolate_dostring(iso, source, &k_program_image_memory_authority);
    xray_vm_delete(iso);
    return rc;
}

/* A generic class declared by an embedded standard library module,
 * instantiated at a type the runtime's own copy of the module never saw. */
TEST(dostring_runs_specialized_stdlib_generic_class) {
    ASSERT_EQ_INT(run_eval_script("import http\n"
                                  "var r = http.Router<i64>()\n"
                                  "assert(r.add(\"GET\", \"/x\", 3))\n"
                                  "var found = r.find(\"GET\", \"/x\")\n"
                                  "assert(found != null)\n"
                                  "assert(found!.value == 3)\n"),
                  0);
}

/* The same through a selective import, whose private specialization import
 * is bound before the first statement that reads it. */
TEST(dostring_runs_specialized_generic_through_selective_import) {
    ASSERT_EQ_INT(run_eval_script("import { WorkQueue } from sync\n"
                                  "var q = WorkQueue<i64>()\n"
                                  "assert(q.push(7))\n"
                                  "assert(q.length == 1)\n"
                                  "assert(q.pop() == 7)\n"),
                  0);
}

/* A hybrid module keeps its native exports under the program's script layer. */
TEST(dostring_keeps_native_exports_under_program_script_layer) {
    ASSERT_EQ_INT(run_eval_script("import sync\n"
                                  "var m = sync.Mutex<i64>(5)\n"
                                  "var latch = sync.CountdownLatch(1)\n"
                                  "latch.done()\n"
                                  "assert(latch.remaining == 0)\n"),
                  0);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("api/dostring-program-image");
RUN_TEST(dostring_runs_specialized_stdlib_generic_class);
RUN_TEST(dostring_runs_specialized_generic_through_selective_import);
RUN_TEST(dostring_keeps_native_exports_under_program_script_layer);
TEST_MAIN_END()
