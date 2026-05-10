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
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../include/xray_isolate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ========== Test Infrastructure ========== */

static XrayIsolate *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

static void setup(void) {
    if (!g_iso) {
        XrayIsolateParams p;
        xray_isolate_params_init(&p);
        g_iso = xray_isolate_new(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

/* Parse source, run analyzer, attempt to lower.
 * Returns the XiFunc if lowering succeeds, NULL if it fails. */
static XiFunc *try_lower(const char *source) {
    assert(g_iso != NULL);

    AstNode *program = xr_parse(g_iso, source);
    if (!program) return NULL;

    XaAnalyzer *analyzer = xa_analyzer_new(g_iso);
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

    XiFunc *func = xi_lower_program(program, analyzer, g_iso);

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

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        printf("--- %s ---\n", #name); \
        test_##name(); \
        printf("  PASS\n"); \
        tests_passed++; \
    } \
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
    XiFunc *f = try_lower("let x = unknown_y + 1");
    assert(f == NULL && "lowerer must reject unresolved variable in expr");
}

TEST(unresolved_variable_in_function) {
    /* Undeclared variable inside a function body. */
    XiFunc *f = try_lower(
        "fn foo(): int {\n"
        "    return missing_z\n"
        "}\n"
    );
    assert(f == NULL && "lowerer must reject unresolved variable in function");
}

TEST(resolved_variable_accepted) {
    /* Declared variable should lower successfully. */
    XiFunc *f = try_lower("let x = 42\nprint(x)");
    assert(f != NULL && "lowerer must accept resolved variables");
    xi_func_free(f);
}

TEST(declared_and_assigned_accepted) {
    /* Assignment to a declared variable should lower successfully. */
    XiFunc *f = try_lower("let x = 1\nx = x + 2\nprint(x)");
    assert(f != NULL && "lowerer must accept declared+assigned variables");
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    setup();

    run_unresolved_variable_in_print();
    run_unresolved_variable_in_expression();
    run_unresolved_variable_in_function();
    run_resolved_variable_accepted();
    run_declared_and_assigned_accepted();

    teardown();

    printf("\n=== %d/%d Xi Lower Hard-Fail tests passed ===\n",
           tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
