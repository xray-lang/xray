/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_repl_runtime_authority.c - Original runtime and product-profile obligations
 *
 * KEY CONCEPT:
 *   Covers the REPL symbol table lifecycle, the persistent analyzer
 *   path through xr_repl_eval, the .vars / .type introspection
 *   helpers, the const round-trip, and the XR_ISOLATE_PROFILE_REPL
 *   profile's JIT-off invariant.
 *
 *   Tests link xray_core plus a small slice of CLI (xisolate_profile)
 *   needed to exercise the profile factory.  No interactive readline
 *   path is exercised here — completion is verified by manual REPL
 *   sessions.
 */

#include "../test_framework.h"
#include "xray.h"
#include "xray_vm.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/runtime/xglobal_dict.h"
#include "../../../src/runtime/xexec_state.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../src/plan/target/xr_target_profile.h"
#include "../../../src/runtime/abi/xr_runtime_target_profile.h"
#include "../../../src/module/xmodule_identity.h"
#include "xrepl.h"
#include "xisolate_profile.h"
#include <stdio.h>
#include <string.h>

/* ========== Helpers ========== */

static const XrModuleIdentityAuthority k_repl_memory_authority = {
    .kind = XR_MODULE_IDENTITY_MEMORY,
    .namespace_id = "repl-fixture-v1",
};

static XrVMRuntime *make_repl_iso(void) {
    return xr_isolate_profile_new(XR_ISOLATE_PROFILE_REPL);
}

static XrProto *eval_repl(XrCompilerSession *session, XrVMRuntime *isolate, const char *source) {
    XrReplEvalResult result = xr_repl_eval(session, isolate, source, &k_repl_memory_authority);
    if (result.status == XR_REPL_EVAL_OK)
        return result.proto;
    if (result.proto)
        xr_free_code(isolate, result.proto);
    return NULL;
}

/* Find a symbol by name; returns -1 if not present. */
static int find_symbol(const XrReplSymbolTable *t, const char *name) {
    if (!t)
        return -1;
    for (int i = 0; i < t->count; i++) {
        const char *n = xr_repl_symbol_cname(&t->symbols[i]);
        if (n && strcmp(n, name) == 0)
            return i;
    }
    return -1;
}

TEST(product_profiles_install_exact_authority_and_reject_missing_profile) {
    static const XrIsolateProfile profiles[] = {
        XR_ISOLATE_PROFILE_REPL,
        XR_ISOLATE_PROFILE_RUN,
        XR_ISOLATE_PROFILE_EVAL,
        XR_ISOLATE_PROFILE_TEST,
    };
    XrTargetProfile *expected = NULL;
    char error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&expected, error, sizeof(error)));

    XrVMRuntime *iso = make_repl_iso();
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        if (i != 0)
            iso = xr_isolate_profile_new(profiles[i]);
        ASSERT_NOT_NULL(iso);
        ASSERT_NOT_NULL(iso->vm.globals);
        ASSERT_EQ_INT((int) xr_global_dict_count(iso->vm.globals), 0);

        XrCompilerSession *installed_session = xr_compiler_session_current_for_isolate(iso);
        ASSERT_NOT_NULL(installed_session);
        ASSERT_TRUE(xr_target_profile_require_exact(
            expected, xr_compiler_session_target_profile(installed_session), error, sizeof(error)));

        if (i == 0) {
            XrCompilerSessionConfig config = {
                .vm_host = iso,
                .source_file = "<missing-target-profile>",
                .repl_mode = true,
            };
            XrCompilerSession *missing_session = xr_compiler_session_new(&config);
            ASSERT_NOT_NULL(missing_session);
            ASSERT_NULL(xr_compiler_session_target_profile(missing_session));
            XrReplEvalResult result =
                xr_repl_eval(missing_session, iso, "1 + 2\n", &k_repl_memory_authority);
            ASSERT_EQ(XR_REPL_EVAL_COMPILE_ERROR, result.status);
            ASSERT_NULL(result.proto);
            xr_compiler_session_delete(missing_session);
        }
        xray_vm_delete(iso);
    }
    xr_target_profile_free(expected);
}

TEST(sync_root_elides_coroutine_with_ordinary_allocation) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    ASSERT_NULL(iso->main_coro);

    XrProto *proto = eval_repl(xr_compiler_session_current_for_isolate(iso), iso,
                               "var xs = [1, 2, 3]\nvar n = len(xs)\n");
    ASSERT_NOT_NULL(proto);
    ASSERT_NULL(iso->main_coro);

    XrReplSymbolTable *table = xr_repl_symbols_of(iso);
    int binding = find_symbol(table, "n");
    ASSERT_GE(binding, 0);
    char key[64];
    ASSERT_TRUE(xr_repl_binding_key(table->symbols[binding].symbol_id, key, sizeof(key)));
    XrString *name = xr_string_intern_permanent(iso, key, strlen(key));
    ASSERT_NOT_NULL(name);
    XrValue value = xr_global_dict_get(iso->vm.globals, name);
    ASSERT_TRUE(XR_IS_INT(value));
    ASSERT_EQ_INT((int) XR_TO_INT(value), 3);

    xr_free_code(iso, proto);
    xray_vm_delete(iso);
}

TEST(repl_profile_clears_each_call) {
    /* Each xr_isolate_profile_params call must fully initialize the out
     * struct — leftover bits from a prior call on the same struct
     * must not bleed through.  Set a sentinel before the second call
     * to catch any field that depends on prior content. */
    XrVMConfig p;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_RUN, &p);
    p.trace_execution = true; /* sentinel */
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_REPL, &p);
    ASSERT_FALSE(p.trace_execution);
}

TEST_MAIN_BEGIN()
RUN_TEST(product_profiles_install_exact_authority_and_reject_missing_profile);
RUN_TEST(sync_root_elides_coroutine_with_ordinary_allocation);
RUN_TEST(repl_profile_clears_each_call);
TEST_MAIN_END()
