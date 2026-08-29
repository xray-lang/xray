/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dostring_module_defaults.c - An eval'd script resolves the exports of
 *                                   the modules it imports.
 *
 * KEY CONCEPT:
 *   An in-memory entry module (xr_isolate_dostring) has no source_path -- that
 *   is its ordinary shape, not a defect. Export lookup used to require one,
 *   so every `mod.fn(...)` call in an eval'd script resolved to no callee
 *   links. Losing the links silently disabled caller-side default-argument
 *   filling: the call was emitted with only its supplied arguments, and the
 *   omitted parameter reached the callee as an uninitialized register.
 *
 *   For a scalar default that surfaces as a wrong value or a typed panic; for
 *   a structural-object default it was a wild pointer read inside
 *   OP_OBJECT_GET, since the callee dereferenced whatever the caller's
 *   register happened to hold. Both shapes are covered here.
 *
 *   Each script states its expectation with the builtin `assert`, which is
 *   available to an eval'd script and reports a violated one as
 *   AssertionFailure at its own line -- so a failure here names the condition
 *   that broke, not just a non-zero result.
 */

#include "../test_framework.h"
#include "xray_vm.h"
#include "runtime/xisolate_api.h"
#include "module/xmodule_identity.h"
#include <stddef.h>

static const XrModuleIdentityAuthority k_dostring_defaults_memory_authority = {
    .kind = XR_MODULE_IDENTITY_MEMORY,
    .namespace_id = "dostring-module-defaults-fixture-v1",
};

static int run_eval_script(const char *source) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    if (!iso)
        return -1;
    int rc = xr_isolate_dostring(iso, source, &k_dostring_defaults_memory_authority);
    xray_vm_delete(iso);
    return rc;
}

/* A scalar default (`padding: string = " "`) omitted at the call site. Without
 * the callee links the parameter arrives as an unrelated live register, which
 * `padStart` then measures -- historically an E0404 "does not implement
 * Lengthable" panic rather than a padded string. */
TEST(dostring_fills_scalar_default_argument_across_modules) {
    ASSERT_EQ_INT(run_eval_script("import text\n"
                                  "assert(text.padStart(\"7\", 3) == \"  7\")\n"),
                  0);
}

/* The same omission with a structural-object default
 * (`options: XmlOptions = XML_DEFAULT_OPTIONS`). The callee reads the
 * parameter's fields by ordinal, so a non-object in that register is
 * dereferenced as an instance: this crashed in xr_instance_get_dynamic_field
 * on a string payload rather than reporting anything. */
TEST(dostring_fills_structural_default_argument_across_modules) {
    ASSERT_EQ_INT(run_eval_script("import xml\n"
                                  "var report = xml.parseReport(\"<root><item a=\\\"1\\\">x</item>"
                                  "<!--c--></root>\")\n"
                                  "assert(len(report.diagnostics) == 0)\n"
                                  "assert(report.doc != null)\n"),
                  0);
}

/* The default is the declaration's, not the caller's: an explicitly supplied
 * argument must still win. This pins the filling to omitted parameters only,
 * so a future fix for the above cannot start overwriting real arguments. */
TEST(dostring_keeps_explicit_argument_over_module_default) {
    ASSERT_EQ_INT(run_eval_script("import text\n"
                                  "assert(text.padStart(\"7\", 3, \"0\") == \"007\")\n"),
                  0);
}

/* Export lookup is what regressed, so keep a direct read of a module export
 * that takes no defaulted call at all: a broken lookup must not be able to
 * pass this suite by only repairing the argument-filling path. */
TEST(dostring_resolves_plain_module_export) {
    ASSERT_EQ_INT(run_eval_script("import text\n"
                                  "assert(text.padEnd(\"7\", 3, \"0\") == \"700\")\n"),
                  0);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("api/dostring-module-defaults");
RUN_TEST(dostring_fills_scalar_default_argument_across_modules);
RUN_TEST(dostring_fills_structural_default_argument_across_modules);
RUN_TEST(dostring_keeps_explicit_argument_over_module_default);
RUN_TEST(dostring_resolves_plain_module_export);
TEST_MAIN_END()
