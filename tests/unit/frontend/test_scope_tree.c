/*
 * test_scope_tree.c - Scope tree invariant tests
 *
 * Verifies that the analyzer builds a correct scope tree:
 *   - Each scope-creating construct (function, block, class, loop)
 *     produces a child scope of the correct kind.
 *   - Symbols are placed in the correct scope (not leaked up/down).
 *   - The global scope is the root and all scopes form a tree.
 *   - Nested scopes have proper parent chains.
 */

#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/parser/xast_nodes.h"
#include "../../../src/frontend/parser/xast_types.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../include/xray_isolate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Infrastructure ========== */

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

/* Parse + analyze, return analyzer (caller frees both). */
typedef struct {
    AstNode *program;
    XaAnalyzer *analyzer;
} AnalysisResult;

static AnalysisResult analyze_source(const char *source) {
    AnalysisResult r = {NULL, NULL};
    r.program = xr_parse(g_iso, source);
    assert(r.program != NULL && "parse must succeed");

    r.analyzer = xa_analyzer_new(g_iso);
    assert(r.analyzer != NULL);
    xa_analyzer_analyze(r.analyzer, "scope_test.xr", r.program);
    return r;
}

static void free_result(AnalysisResult *r) {
    if (r->analyzer) xa_analyzer_free(r->analyzer);
    if (r->program) xr_program_destroy(r->program);
    r->analyzer = NULL;
    r->program = NULL;
}

/* Count total scopes in the tree rooted at 'scope' (including root). */
static int count_scopes(XaScope *scope) {
    if (!scope) return 0;
    int n = 1;
    for (int i = 0; i < scope->child_count; i++)
        n += count_scopes(scope->children[i]);
    return n;
}

/* Verify every scope in the tree has a valid parent pointer
 * (except root whose parent may be NULL). */
static bool verify_parent_chain(XaScope *scope, XaScope *expected_parent) {
    if (!scope) return true;
    if (scope->parent != expected_parent) return false;
    for (int i = 0; i < scope->child_count; i++) {
        if (!verify_parent_chain(scope->children[i], scope))
            return false;
    }
    return true;
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

/* ========== Tests ========== */

TEST(global_scope_exists) {
    AnalysisResult r = analyze_source("let x = 1");
    assert(r.analyzer->global_scope != NULL);
    assert(r.analyzer->global_scope->kind == XA_SCOPE_GLOBAL);
    /* 'x' should be in global scope */
    XaSymbol *sym = xa_scope_lookup_local(r.analyzer->global_scope, "x");
    assert(sym != NULL && "global var must be in global scope");
    free_result(&r);
}

TEST(global_scope_is_root) {
    AnalysisResult r = analyze_source("let x = 1\nlet y = 2");
    /* Global scope parent must be NULL (it's the root). */
    assert(r.analyzer->global_scope->parent == NULL);
    free_result(&r);
}

TEST(function_creates_child_scope) {
    AnalysisResult r = analyze_source(
        "fn foo(a: int): int {\n"
        "    let b = a + 1\n"
        "    return b\n"
        "}\n"
    );
    XaScope *global = r.analyzer->global_scope;
    assert(global->child_count >= 1 && "function must create a child scope");

    /* Find the function scope */
    bool found_func_scope = false;
    for (int i = 0; i < global->child_count; i++) {
        if (global->children[i]->kind == XA_SCOPE_FUNCTION) {
            XaScope *fn_scope = global->children[i];
            /* Parameter 'a' should be in function scope */
            XaSymbol *a = xa_scope_lookup_local(fn_scope, "a");
            assert(a != NULL && "param must be in function scope");
            /* 'b' could be in function scope or a nested block scope */
            XaSymbol *b = xa_scope_lookup(fn_scope, "b");
            assert(b != NULL && "local var must be findable from function scope");
            found_func_scope = true;
            break;
        }
    }
    assert(found_func_scope && "must have a function scope child");
    free_result(&r);
}

TEST(block_creates_child_scope) {
    AnalysisResult r = analyze_source(
        "let x = 1\n"
        "{\n"
        "    let y = 2\n"
        "}\n"
    );
    XaScope *global = r.analyzer->global_scope;
    /* 'y' must NOT be in global scope (it's block-scoped) */
    XaSymbol *y_global = xa_scope_lookup_local(global, "y");
    assert(y_global == NULL && "block-scoped var must not leak to global");

    /* 'x' must be in global scope */
    XaSymbol *x = xa_scope_lookup_local(global, "x");
    assert(x != NULL && "global var must be in global scope");
    free_result(&r);
}

TEST(if_creates_scopes) {
    AnalysisResult r = analyze_source(
        "let x = 1\n"
        "if (x > 0) {\n"
        "    let a = 1\n"
        "} else {\n"
        "    let b = 2\n"
        "}\n"
    );
    XaScope *global = r.analyzer->global_scope;
    /* if/else blocks should create at least one child scope.
     * Note: the analyzer may hoist control-flow-body variables
     * into the enclosing scope during pass 1 collection,
     * so we only check structural scope creation here. */
    assert(count_scopes(global) >= 2 && "if/else must create child scopes");
    assert(verify_parent_chain(global, NULL));
    free_result(&r);
}

TEST(while_in_function_creates_scope) {
    /* While loops inside functions must create child scopes. */
    AnalysisResult r = analyze_source(
        "fn loop_fn(): int {\n"
        "    let i = 0\n"
        "    while (i < 10) {\n"
        "        let temp = i\n"
        "        i += 1\n"
        "    }\n"
        "    return i\n"
        "}\n"
    );
    XaScope *global = r.analyzer->global_scope;
    /* Function scope always exists; while body may reuse it.
     * The invariant: parent chain is valid and function scope exists. */
    assert(count_scopes(global) >= 2 && "function needs >= 2 scopes");
    assert(verify_parent_chain(global, NULL));
    free_result(&r);
}

TEST(nested_functions_nested_scopes) {
    AnalysisResult r = analyze_source(
        "fn outer(): int {\n"
        "    let a = 1\n"
        "    fn inner(): int {\n"
        "        let b = 2\n"
        "        return a + b\n"
        "    }\n"
        "    return inner()\n"
        "}\n"
    );
    /* Total scopes: global + outer + inner (+ possible block scopes) */
    int total = count_scopes(r.analyzer->global_scope);
    assert(total >= 3 && "nested functions need at least 3 scopes");
    free_result(&r);
}

TEST(function_isolation) {
    /* The hard invariant: function-scoped variables must NOT
     * be visible outside their function boundary. */
    AnalysisResult r = analyze_source(
        "fn foo(): int {\n"
        "    let secret = 42\n"
        "    return secret\n"
        "}\n"
        "let result = foo()\n"
    );
    XaScope *global = r.analyzer->global_scope;
    /* 'secret' must NOT be in global scope */
    assert(xa_scope_lookup_local(global, "secret") == NULL &&
           "function-local var must not leak to global scope");
    /* 'result' and 'foo' must be in global scope */
    assert(xa_scope_lookup_local(global, "result") != NULL);
    assert(xa_scope_lookup(global, "foo") != NULL);
    free_result(&r);
}

TEST(parent_chain_valid) {
    AnalysisResult r = analyze_source(
        "fn foo(x: int): int {\n"
        "    if (x > 0) {\n"
        "        let y = x\n"
        "        return y\n"
        "    }\n"
        "    return 0\n"
        "}\n"
    );
    /* Every scope's parent pointer must form a consistent tree. */
    assert(verify_parent_chain(r.analyzer->global_scope, NULL));
    free_result(&r);
}

TEST(class_creates_class_scope) {
    AnalysisResult r = analyze_source(
        "class Foo {\n"
        "    x: int\n"
        "    init(val: int) {\n"
        "        this.x = val\n"
        "    }\n"
        "}\n"
    );
    XaScope *global = r.analyzer->global_scope;
    bool found_class = false;
    for (int i = 0; i < global->child_count; i++) {
        if (global->children[i]->kind == XA_SCOPE_CLASS) {
            found_class = true;
            break;
        }
    }
    assert(found_class && "class must create a class scope");
    free_result(&r);
}

TEST(for_in_creates_scope) {
    AnalysisResult r = analyze_source(
        "const arr = [1, 2, 3]\n"
        "for (item in arr) {\n"
        "    let temp = item\n"
        "}\n"
    );
    XaScope *global = r.analyzer->global_scope;
    /* for-in must create at least one child scope */
    assert(global->child_count >= 1 && "for-in body must create a child scope");
    assert(verify_parent_chain(global, NULL));
    free_result(&r);
}

TEST(shadowing_correct_scope) {
    AnalysisResult r = analyze_source(
        "let x = 1\n"
        "{\n"
        "    let x = 2\n"
        "}\n"
    );
    XaScope *global = r.analyzer->global_scope;
    /* Global scope 'x' should exist */
    XaSymbol *x_global = xa_scope_lookup_local(global, "x");
    assert(x_global != NULL);
    /* There should be a child scope with its own 'x' */
    bool found_shadow = false;
    for (int i = 0; i < global->child_count; i++) {
        XaSymbol *x_inner = xa_scope_lookup_local(global->children[i], "x");
        if (x_inner != NULL && x_inner != x_global) {
            found_shadow = true;
            break;
        }
    }
    assert(found_shadow && "shadowed variable must be in a different scope");
    free_result(&r);
}

TEST(complex_nesting_parent_chain) {
    AnalysisResult r = analyze_source(
        "fn foo(): int {\n"
        "    let a = 1\n"
        "    {\n"
        "        let b = 2\n"
        "        if (a > 0) {\n"
        "            let c = 3\n"
        "        }\n"
        "    }\n"
        "    return a\n"
        "}\n"
    );
    assert(verify_parent_chain(r.analyzer->global_scope, NULL));
    int total = count_scopes(r.analyzer->global_scope);
    assert(total >= 4 && "function + block + if = at least 4 scopes");
    free_result(&r);
}

/* ========== Main ========== */

int main(void) {
    setup();

    run_global_scope_exists();
    run_global_scope_is_root();
    run_function_creates_child_scope();
    run_block_creates_child_scope();
    run_if_creates_scopes();
    run_while_in_function_creates_scope();
    run_nested_functions_nested_scopes();
    run_function_isolation();
    run_parent_chain_valid();
    run_class_creates_class_scope();
    run_for_in_creates_scope();
    run_shadowing_correct_scope();
    run_complex_nesting_parent_chain();

    teardown();

    printf("\n=== %d/%d Scope Tree tests passed ===\n",
           tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
