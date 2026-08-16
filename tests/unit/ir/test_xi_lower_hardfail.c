/*
 * test_xi_lower_hardfail.c - Verify lowerer rejects invalid input
 *
 * Tests the defense-in-depth contract: if the analyzer misses an error
 * (or the pipeline doesn't halt on analyzer diagnostics), the lowerer
 * must hard-fail instead of silently producing LOADNULL or implicit
 * variable creation.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_lower.h"
#include "../../../src/ir/xi_lower_internal.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/frontend/analyzer/xa_typed_program.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../include/xray_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ========== Test Infrastructure ========== */

static XrVMRuntime *g_iso = NULL;
static XrCompilerSession *g_session = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

static void setup(void) {
    if (!g_iso) {
        XrVMConfig p;
        xray_vm_config_init(&p);
        g_iso = xray_vm_new_full(&p);
        g_session = xr_compiler_session_current_for_isolate(g_iso);
        assert(g_session != NULL);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
        g_session = NULL;
    }
}

/* Parse source, run analyzer, attempt to lower.
 * Returns the XiFunc if lowering succeeds, NULL if it fails. */
static XiFunc *try_lower(const char *source) {
    assert(g_iso != NULL);

    AstNode *program = xr_parse(xr_compiler_session_current_for_isolate(g_iso), source);
    if (!program)
        return NULL;

    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    if (!analyzer) {
        xr_program_destroy(program);
        return NULL;
    }
    xa_analyzer_analyze(analyzer, "hardfail_test.xr", program);

    /* Redirect stderr to suppress expected error messages during testing */
#ifdef _WIN32
    freopen("NUL", "w", stderr);
#else
    int saved_fd = dup(STDERR_FILENO);
    freopen("/dev/null", "w", stderr);
#endif

    XaTypedProgramPublishResult typed = xa_typed_program_publish(analyzer, program, NULL, 0);
    XiFunc *func = typed.program ? xi_lower_program(typed.program, g_iso, false) : NULL;
    xa_typed_program_free(typed.program);

#ifdef _WIN32
    freopen("CON", "w", stderr);
#else
    fflush(stderr);
    dup2(saved_fd, STDERR_FILENO);
    close(saved_fd);
#endif

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
    return func;
}

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- %s ---\n", #name);                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* ========== Negative Tests: Lowerer Must Reject ========== */

TEST(unresolved_variable_in_print) {
    /* Variable 'nonexistent' is never declared.  The analyzer flags it,
     * but if lowering proceeds, the lowerer must return NULL. */
    XiFunc *f = try_lower("print(nonexistent_var)");
    assert(f == NULL && "lowerer must reject unresolved variable");
}

TEST(unresolved_variable_in_expression) {
    /* Undeclared variable used in arithmetic. */
    XiFunc *f = try_lower("var x = unknown_y + 1");
    assert(f == NULL && "lowerer must reject unresolved variable in expr");
}

TEST(unresolved_variable_in_function) {
    /* Undeclared variable inside a function body. */
    XiFunc *f = try_lower("fn foo() -> int {\n"
                          "    return missing_z\n"
                          "}\n");
    assert(f == NULL && "lowerer must reject unresolved variable in function");
}

TEST(error_return_type_rejected) {
    XiFunc *f = try_lower("fn bad() -> unknown {\n"
                          "    return 1\n"
                          "}\n");
    assert(f == NULL && "lowerer must reject ErrorType function returns");
}

TEST(error_parameter_type_rejected) {
    XiFunc *f = try_lower("fn bad(x: unknown) -> int {\n"
                          "    return 1\n"
                          "}\n");
    assert(f == NULL && "lowerer must reject ErrorType function parameters");
}

TEST(error_expression_type_rejected) {
    XiFunc *f = try_lower("var x = 1 as unknown\n");
    assert(f == NULL && "lowerer must reject ErrorType expression metadata");
}

TEST(error_type_or_any_does_not_fallback_to_unknown) {
    xr_compiler_session_install_analyzer_pool(g_session);

    XiLower l;
    xi_lower_init(&l, NULL, g_iso);

    XrType *error_type = xr_type_new_error(NULL);
    assert(error_type != NULL);
    XrType *result = xi_lower_type_or_any(&l, error_type, "test recovery", 0);

    if (result != error_type || result->kind != XR_KIND_ERROR || !xr_type_contains_error(result) ||
        !l.had_error) {
        fprintf(stderr, "ErrorType recovery fell back to a runtime/ABI fallback type\n");
        abort();
    }

    xi_lower_cleanup(&l);
}

TEST(error_source_var_metadata_rejected) {
    xr_compiler_session_install_analyzer_pool(g_session);

    XiLower l;
    xi_lower_init(&l, NULL, g_iso);
    l.func = xi_func_new("error_source_var_metadata", l.type_unit);
    assert(l.func != NULL);

    XrType *error_array = xr_type_new_array(NULL, xr_type_new_error(NULL));
    assert(error_array != NULL);
    int var_id = xi_lower_var_create(&l, 77, "bad", error_array);
    assert(var_id >= 0);

    bool ok = xi_lower_capture_source_vars(&l);
    assert(!ok && "source variable metadata must reject compiler-only ErrorType");
    assert(l.had_error && "source variable metadata rejection must hard-fail lowering");
    assert(l.func->source_var_count == 0);

    xi_func_free(l.func);
    xi_lower_cleanup(&l);
}

TEST(error_capture_metadata_rejected) {
    xr_compiler_session_install_analyzer_pool(g_session);

    XiLower parent;
    XiLower child;
    xi_lower_init(&parent, NULL, g_iso);
    xi_lower_init(&child, NULL, g_iso);
    parent.func = xi_func_new("error_capture_parent", parent.type_unit);
    child.func = xi_func_new("error_capture_child", child.type_unit);
    assert(parent.func != NULL);
    assert(child.func != NULL);
    child.parent = &parent;

    XrType *error_array = xr_type_new_array(NULL, xr_type_new_error(NULL));
    assert(error_array != NULL);
    int var_id = xi_lower_var_create(&parent, 88, "bad", error_array);
    assert(var_id >= 0);

    XrType *out_type = NULL;
    int capture_id = xi_lower_resolve_upvalue(&child, 88, "bad", &out_type);
    assert(capture_id < 0 && "capture metadata must reject compiler-only ErrorType");
    assert(child.had_error && "capture metadata rejection must hard-fail child lowering");
    assert(child.func->ncaptures == 0);

    xi_func_free(child.func);
    xi_func_free(parent.func);
    xi_lower_cleanup(&child);
    xi_lower_cleanup(&parent);
}

TEST(resolved_variable_accepted) {
    /* Declared variable should lower successfully. */
    XiFunc *f = try_lower("var x = 42\nprint(x)");
    assert(f != NULL && "lowerer must accept resolved variables");
    xi_func_free(f);
}

TEST(declared_and_assigned_accepted) {
    /* Assignment to a declared variable should lower successfully. */
    XiFunc *f = try_lower("var x = 1\nx = x + 2\nprint(x)");
    assert(f != NULL && "lowerer must accept declared+assigned variables");
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    setup();

    run_unresolved_variable_in_print();
    run_unresolved_variable_in_expression();
    run_unresolved_variable_in_function();
    run_error_return_type_rejected();
    run_error_parameter_type_rejected();
    run_error_expression_type_rejected();
    run_error_type_or_any_does_not_fallback_to_unknown();
    run_error_source_var_metadata_rejected();
    run_error_capture_metadata_rejected();
    run_resolved_variable_accepted();
    run_declared_and_assigned_accepted();

    teardown();

    printf("\n=== %d/%d Xi Lower Hard-Fail tests passed ===\n", tests_passed,
           tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
