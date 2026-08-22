/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_interface_conformance.c - End-to-end analyzer tests covering:
 *
 *   1. Structural method/property conformance for user-defined interfaces
 *      (`class Foo implements MyIface`)
 *
 *   2. Parameterised built-in interface constraints (`<T: Iterable<int>>`)
 *      so that mismatched type arguments produce diagnostics.
 *
 * Each case parses an isolated snippet, runs the analyzer, and inspects
 * the produced diagnostic list directly.
 */
#include "xanalyzer.h"
#include "xanalyzer_symbol.h"
#include "xparse.h"
#include "xtype.h"
#include "xray_vm.h"
#include "xerror.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static XrVMRuntime *g_iso = NULL;
static XrCompilerSession *g_session = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

static void setup(void) {
    if (!g_iso) {
        XrVMConfig p = {0};
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

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  Running %s... ", #name);                                                         \
        test_##name();                                                                             \
        printf("PASSED\n");                                                                        \
        tests_passed++;                                                                            \
    } while (0)

// Parse `source`, analyse it, and count diagnostics with the given code.
// `total_out` (optional) receives the total diagnostic count regardless of code.
static int count_diagnostics(const char *source, int code, int *total_out) {
    AstNode *program = xr_parse(xr_compiler_session_current_for_isolate(g_iso), source);
    if (!program) {
        if (total_out)
            *total_out = -1;
        return -1;
    }

    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    xa_analyzer_analyze(analyzer, "iface_test.xr", program);

    int total = 0;
    int matched = 0;
    XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &total);
    for (XaDiagnostic *d = diag; d; d = d->next) {
        if (d->code == code)
            matched++;
    }

    if (total_out)
        *total_out = total;

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
    return matched;
}

// ============================================================================
// User-defined interface: structural method/property conformance
// ============================================================================

static void test_class_implements_all_methods_no_error(void) {
    const char *src = "interface Stringable2 {\n"
                      "    toLabel() -> string\n"
                      "}\n"
                      "class Item implements Stringable2 {\n"
                      "    name: string\n"
                      "    constructor(n: string) { this.name = n }\n"
                      "    toLabel() -> string { return this.name }\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, &total);
    ASSERT(n == 0);
}

static void test_interface_accepts_pure_implementation(void) {
    const char *src = "interface PureActionOk {\n"
                      "    run(value: i64) -> i64\n"
                      "}\n"
                      "class PureActionImpl implements PureActionOk {\n"
                      "    run(value: i64) -> i64 { return value + 1 }\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, &total);
    ASSERT(n == 0);
    ASSERT(total == 0);
}

static void test_class_missing_method_reports_error(void) {
    const char *src = "interface Greeter {\n"
                      "    greet() -> string\n"
                      "    name() -> string\n"
                      "}\n"
                      "class Bot implements Greeter {\n"
                      "    label: string\n"
                      "    constructor(l: string) { this.label = l }\n"
                      "    greet() -> string { return this.label }\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, &total);
    ASSERT(n == 1);
}

static void test_class_missing_property_reports_error(void) {
    const char *src = "interface Sized {\n"
                      "    size: i64\n"
                      "}\n"
                      "class Buf implements Sized {\n"
                      "    payload: i64\n"
                      "    constructor(p: i64) { this.payload = p }\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, &total);
    ASSERT(n == 1);
}

static void test_class_property_as_getter_accepted(void) {
    const char *src = "interface Sized2 {\n"
                      "    size: i64\n"
                      "}\n"
                      "class Buf2 implements Sized2 {\n"
                      "    internal: i64\n"
                      "    constructor(p: i64) { this.internal = p }\n"
                      "    size: i64 {\n"
                      "        fn() { return this.internal }\n"
                      "    }\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, &total);
    ASSERT(n == 0);
}

static void test_unknown_interface_is_not_audited(void) {
    // Resolution falls back to a class symbol when no interface named
    // 'Phantom' exists; the audit must skip it rather than erroring on
    // every method, otherwise unrelated typos would explode here.
    const char *src = "class Bare implements Phantom {\n"
                      "    name: string\n"
                      "    constructor(n: string) { this.name = n }\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, &total);
    ASSERT(n == 0);
}

static void test_user_interface_constraint_accepts_implementor(void) {
    const char *src = "interface Shape {\n"
                      "    area() -> i64\n"
                      "}\n"
                      "class Circle implements Shape {\n"
                      "    r: i64\n"
                      "    constructor(r: i64) { this.r = r }\n"
                      "    area() -> i64 { return this.r * this.r }\n"
                      "}\n"
                      "fn score<T: Shape>(shape: T) -> i64 {\n"
                      "    return shape.area()\n"
                      "}\n"
                      "fn main() -> i64 {\n"
                      "    return score<Circle>(Circle(7))\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_GENERIC_CONSTRAINT, &total);
    ASSERT(n == 0);
}

static void test_user_interface_constraint_accepts_same_interface_object(void) {
    const char *src = "interface Shape {\n"
                      "    area() -> i64\n"
                      "}\n"
                      "class Circle implements Shape {\n"
                      "    r: i64\n"
                      "    constructor(r: i64) { this.r = r }\n"
                      "    area() -> i64 { return this.r * this.r }\n"
                      "}\n"
                      "fn score<T: Shape>(shape: T) -> i64 {\n"
                      "    return shape.area()\n"
                      "}\n"
                      "fn main() -> i64 {\n"
                      "    var shape: Shape = Circle(7)\n"
                      "    return score<Shape>(shape)\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_GENERIC_CONSTRAINT, &total);
    ASSERT(n == 0);
}

static void test_user_interface_constraint_rejects_non_implementor(void) {
    const char *src = "interface Shape {\n"
                      "    area() -> i64\n"
                      "}\n"
                      "class Circle {\n"
                      "    r: i64\n"
                      "    constructor(r: i64) { this.r = r }\n"
                      "    area() -> i64 { return this.r * this.r }\n"
                      "}\n"
                      "fn score<T: Shape>(shape: T) -> i64 {\n"
                      "    return shape.area()\n"
                      "}\n"
                      "fn main() -> i64 {\n"
                      "    return score<Circle>(Circle(7))\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_GENERIC_CONSTRAINT, &total);
    ASSERT(n == 1);
}

// ============================================================================
// Built-in interfaces with type arguments
// ============================================================================

static void test_iterable_int_satisfied_by_array_int(void) {
    const char *src = "fn consume<T: Iterable<i64>>(xs: T) -> i64 {\n"
                      "    return 0\n"
                      "}\n"
                      "fn main() -> i64 {\n"
                      "    var xs: Array<i64> = [1, 2, 3]\n"
                      "    return consume<Array<i64>>(xs)\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_GENERIC_CONSTRAINT, &total);
    ASSERT(n == 0);
}

static void test_iterable_int_rejects_array_string(void) {
    const char *src = "fn consume<T: Iterable<i64>>(xs: T) -> i64 {\n"
                      "    return 0\n"
                      "}\n"
                      "fn main() -> i64 {\n"
                      "    var xs: Array<string> = [\"a\"]\n"
                      "    return consume<Array<string>>(xs)\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_GENERIC_CONSTRAINT, &total);
    ASSERT(n == 1);
}

static void test_indexable_string_int_rejects_array_int(void) {
    // Indexable<string, int> requires a Map-like keyed container; Array<int>
    // is indexed by int, not string.
    const char *src = "fn consume<T: Indexable<string, i64>>(xs: T) -> i64 {\n"
                      "    return 0\n"
                      "}\n"
                      "fn main() -> i64 {\n"
                      "    var xs: Array<i64> = [1, 2]\n"
                      "    return consume<Array<i64>>(xs)\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_GENERIC_CONSTRAINT, &total);
    ASSERT(n >= 1);
}

static void test_indexable_int_int_accepts_array_int(void) {
    const char *src = "fn consume<T: Indexable<i64, i64>>(xs: T) -> i64 {\n"
                      "    return 0\n"
                      "}\n"
                      "fn main() -> i64 {\n"
                      "    var xs: Array<i64> = [1, 2]\n"
                      "    return consume<Array<i64>>(xs)\n"
                      "}\n";
    int total = 0;
    int n = count_diagnostics(src, XR_ERR_ANALYZE_GENERIC_CONSTRAINT, &total);
    ASSERT(n == 0);
}

int main(void) {
    xr_test_suppress_dialogs();
    printf("Running interface-conformance unit tests...\n\n");

    setup();

    printf("User-defined interface conformance:\n");
    RUN_TEST(class_implements_all_methods_no_error);
    RUN_TEST(interface_accepts_pure_implementation);
    RUN_TEST(class_missing_method_reports_error);
    RUN_TEST(class_missing_property_reports_error);
    RUN_TEST(class_property_as_getter_accepted);
    RUN_TEST(unknown_interface_is_not_audited);
    RUN_TEST(user_interface_constraint_accepts_implementor);
    RUN_TEST(user_interface_constraint_accepts_same_interface_object);
    RUN_TEST(user_interface_constraint_rejects_non_implementor);

    printf("\nParameterised built-in interface constraints:\n");
    RUN_TEST(iterable_int_satisfied_by_array_int);
    RUN_TEST(iterable_int_rejects_array_string);
    RUN_TEST(indexable_string_int_rejects_array_int);
    RUN_TEST(indexable_int_int_accepts_array_int);

    teardown();

    printf("\n----------------------------------------\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
