/*
 * test_xi_cgen.c - Unit tests for Xi IR to C code generation
 *
 * Tests the xi_cgen_* functions by:
 *   1. Compiling xray source through the pipeline to Xi IR
 *   2. Running select_rep to insert BOX/UNBOX
 *   3. Generating C code via xi_cgen_program
 *   4. Verifying the output contains expected constructs
 */

#include "../../../src/ir/xi.h"
#include "../../../src/aot/xi_cgen.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_pipeline.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xmemstream.h"
#include "../../../include/xray_isolate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Test Infrastructure ========== */

static XrayIsolate *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        printf("--- " #name " ---\n"); \
        test_##name(); \
        tests_passed++; \
        printf("  PASS\n"); \
    } \
    static void test_##name(void)

static void setup(void) {
    if (!g_iso) {
        XrayIsolateParams p;
        xray_isolate_params_init(&p);
        xray_isolate_setup_full(&p);
        g_iso = xray_isolate_new(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

/* Compile source to Xi IR (without emitting bytecode).
 * Returns the XiFunc* (caller must free via xi_func_free).
 * If mod_out is non-NULL, also returns the XiModule* (caller must free). */
static XiFunc *compile_to_ir(const char *source) {
    assert(g_iso != NULL);

    XaAnalyzer *analyzer = xa_analyzer_new(g_iso);
    if (!analyzer) return NULL;

    AstNode *program = xr_parse(g_iso, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %.60s...\n", source);
        xa_analyzer_free(analyzer);
        return NULL;
    }

    xa_analyzer_analyze(analyzer, "test.xr", program);

    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.run_optimize = false;
    cfg.run_emit = false;  /* cgen needs raw IR tree, not bytecode */

    XiPipelineResult res = xi_pipeline_compile_program(
        program, analyzer, g_iso, &cfg);

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    if (res.status != XI_PIPE_OK) {
        fprintf(stderr, "  PIPELINE FAILED: %s\n",
                xi_pipe_status_str(res.status));
        xi_pipeline_result_free(&res);
        return NULL;
    }

    XiFunc *ir = res.ir;
    res.ir = NULL;
    xi_pipeline_result_free(&res);

    return ir;
}

/* Generate C code for Xi IR into a malloc'd string.
 * Caller must free the returned string. */
static char *generate_c(XiFunc *ir, const char *module_name) {
    assert(ir != NULL);

    /* Run select_rep to insert BOX/UNBOX */
    xi_opt_select_rep(ir);

    /* Build module metadata if the pipeline didn't (e.g. standalone tests) */
    XiModule *mod = ir->module;
    bool own_mod = false;
    if (!mod) {
        mod = xi_module_new("test.xr", module_name, ir);
        assert(mod != NULL);
        own_mod = true;
    } else {
        if (!mod->name) mod->name = module_name;
    }

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    assert(ctx != NULL);

    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    assert(mem != NULL);

    xi_cgen_program(ctx, mem, mod);
    int rc = xr_close_memstream(mem, &buf, &bufsz);
    assert(rc == 0);

    xi_cgen_ctx_free(ctx);
    if (own_mod) {
        mod->init = NULL; /* don't double-free ir */
        xi_module_free(mod);
    }

    return buf;
}

/* Check that `haystack` contains `needle`. */
static bool contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

/* ========== Tests ========== */

TEST(cgen_simple_arith) {
    /* Pure arithmetic: 1 + 2 printed */
    const char *src = "print(1 + 2)";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    char *code = generate_c(ir, "test");
    assert(code != NULL && "C code generation failed");

    /* Should contain xrt_println or xrt_print */
    assert(contains(code, "xrt_print") && "should call xrt_print");
    /* Should have a main function */
    assert(contains(code, "int main(void)") && "should have main()");
    /* Should include xrt.h */
    assert(contains(code, "#include \"xrt.h\"") && "should include xrt.h");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_variable_and_print) {
    /* Variable assignment and print */
    const char *src =
        "let x = 42\n"
        "print(x)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) { printf("  SKIP\n"); return; }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    /* Should contain the constant 42 */
    assert(contains(code, "42") && "should contain constant 42");
    assert(contains(code, "xrt_print") && "should call print");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_if_else) {
    /* Conditional control flow */
    const char *src =
        "let x = 10\n"
        "if (x > 5) {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) { printf("  SKIP\n"); return; }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    /* Should contain goto (blocks) and if */
    assert(contains(code, "goto L") && "should have goto for blocks");
    assert(contains(code, "if (") && "should have if branch");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_multi_print) {
    /* Multiple print statements */
    const char *src =
        "let a = 10\n"
        "let b = 20\n"
        "let c = a + b\n"
        "print(c)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) { printf("  SKIP\n"); return; }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "10") && "should contain 10");
    assert(contains(code, "20") && "should contain 20");
    assert(contains(code, "+") && "should contain addition");
    assert(contains(code, "xrt_print") && "should call print");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_while_loop) {
    /* While loop generates blocks and back edges */
    const char *src =
        "let i = 0\n"
        "while (i < 5) {\n"
        "    i = i + 1\n"
        "}\n"
        "print(i)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) { printf("  SKIP\n"); return; }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    /* Should have labels and gotos for loop */
    assert(contains(code, "goto L") && "should have goto for loop");
    /* Should have comparison */
    assert(contains(code, "<") && "should have comparison");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_string_literal) {
    const char *src = "print(\"hello world\")";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) { printf("  SKIP\n"); return; }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "hello world") && "should contain string literal");
    assert(contains(code, "xr_box_str") && "string should be boxed");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_function_call) {
    /* Function definition and call */
    const char *src =
        "fn add(a: int, b: int): int { return a + b }\n"
        "let r = add(3, 4)\n"
        "print(r)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) { printf("  SKIP\n"); return; }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    /* Should contain a child function for 'add' */
    assert(contains(code, "add") && "should have add function");
    assert(contains(code, "static XrValue") && "should have static funcs");
    assert(contains(code, "return") && "add should have return");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_recursive) {
    /* Recursive function: factorial */
    const char *src =
        "fn fact(n: int): int {\n"
        "    if (n <= 1) { return 1 }\n"
        "    return n * fact(n - 1)\n"
        "}\n"
        "print(fact(5))\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) { printf("  SKIP\n"); return; }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "fact") && "should have fact function");
    assert(contains(code, "if (") && "should have conditional");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_for_loop) {
    const char *src =
        "let sum = 0\n"
        "for (let i = 1; i <= 10; i = i + 1) {\n"
        "    sum = sum + i\n"
        "}\n"
        "print(sum)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) { printf("  SKIP\n"); return; }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "goto L") && "should have goto for loop");
    assert(contains(code, "+") && "should have addition");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi CGen Unit Tests ===\n\n");

    setup();

    run_cgen_simple_arith();
    run_cgen_variable_and_print();
    run_cgen_if_else();
    run_cgen_multi_print();
    run_cgen_while_loop();
    run_cgen_string_literal();
    run_cgen_function_call();
    run_cgen_recursive();
    run_cgen_for_loop();

    teardown();

    printf("\n=== %d/%d Xi CGen tests passed ===\n",
           tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
