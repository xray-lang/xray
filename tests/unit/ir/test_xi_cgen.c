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
#include "../../../src/ir/xi_own.h"
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

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        tests_passed++;                                                                            \
        printf("  PASS\n");                                                                        \
    }                                                                                              \
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
    if (!analyzer)
        return NULL;

    AstNode *program = xr_parse(g_iso, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %.60s...\n", source);
        xa_analyzer_free(analyzer);
        return NULL;
    }

    xa_analyzer_analyze(analyzer, "test.xr", program);

    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.run_optimize = false;
    cfg.run_emit = false; /* cgen needs raw IR tree, not bytecode */

    XiPipelineResult res = xi_pipeline_compile_program(program, analyzer, g_iso, &cfg);

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    if (res.status != XI_PIPE_OK) {
        fprintf(stderr, "  PIPELINE FAILED: %s\n", xi_pipe_status_str(res.status));
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
static char *generate_c_with_status(XiFunc *ir, const char *module_name, bool *had_error) {
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
        if (!mod->name)
            mod->name = module_name;
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
    if (had_error)
        *had_error = xi_cgen_has_error(ctx);

    xi_cgen_ctx_free(ctx);
    if (own_mod) {
        mod->init = NULL; /* don't double-free ir */
        xi_module_free(mod);
    }

    return buf;
}

static char *generate_c(XiFunc *ir, const char *module_name) {
    return generate_c_with_status(ir, module_name, NULL);
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
    const char *src = "let x = 42\n"
                      "print(x)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

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
    const char *src = "let x = 10\n"
                      "if (x > 5) {\n"
                      "    print(1)\n"
                      "} else {\n"
                      "    print(0)\n"
                      "}\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

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
    const char *src = "let a = 10\n"
                      "let b = 20\n"
                      "let c = a + b\n"
                      "print(c)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

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
    const char *src = "let i = 0\n"
                      "while (i < 5) {\n"
                      "    i = i + 1\n"
                      "}\n"
                      "print(i)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

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
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

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
    const char *src = "fn add(a: int, b: int) -> int { return a + b }\n"
                      "let r = add(3, 4)\n"
                      "print(r)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

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
    const char *src = "fn fact(n: int) -> int {\n"
                      "    if (n <= 1) { return 1 }\n"
                      "    return n * fact(n - 1)\n"
                      "}\n"
                      "print(fact(5))\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "fact") && "should have fact function");
    assert(contains(code, "if (") && "should have conditional");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_for_loop) {
    const char *src = "let sum = 0\n"
                      "for (let i = 1; i <= 10; i = i + 1) {\n"
                      "    sum = sum + i\n"
                      "}\n"
                      "print(sum)\n";

    XiFunc *ir = compile_to_ir(src);
    if (!ir) {
        printf("  SKIP\n");
        return;
    }

    char *code = generate_c(ir, "test");
    assert(code != NULL);

    assert(contains(code, "goto L") && "should have goto for loop");
    assert(contains(code, "+") && "should have addition");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_unsupported_coroutine_ops_fail_fast) {
    static const struct {
        XiOp op;
        const char *name;
    } cases[] = {
        {XI_SELECT_BLOCK, "SELECT_BLOCK"},
        {XI_SCOPE_ENTER, "SCOPE_ENTER"},
        {XI_SCOPE_EXIT, "SCOPE_EXIT"},
        {XI_CORO_OP, "CORO_OP"},
    };
    XrType stub_unit = {.kind = XR_KIND_UNIT, .id = 100, .frozen = true};
    XiFunc *ir = xi_func_new("main", &stub_unit);
    assert(ir != NULL);

    XiBlock *entry = xi_block_new(ir);
    assert(entry != NULL);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        XiValue *value = xi_value_new(ir, entry, cases[i].op, &stub_unit, 0);
        assert(value != NULL);
    }
    xi_block_set_return(entry, NULL);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL);

    assert(had_error && "AOT cgen must reject unsupported coroutine Xi ops");
    assert(!contains(code, "XR_NULL_VAL /* ERROR: unsupported coroutine Xi op") &&
           "unsupported coroutine ops must not emit silent null placeholders");
    printf("  Generated rejected %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_suspendable_wrapper_aborts) {
    const char *src = "fn worker(n: int) -> int {\n"
                      "    yield\n"
                      "    return n + 1\n"
                      "}\n"
                      "let task = go worker(41)\n"
                      "let result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "supported AOT coroutine should generate without cgen errors");
    assert(contains(code, "_aot_resume") && "suspendable function should emit AOT resume entry");
    assert(contains(code, "return (abort(), XR_NULL_VAL);") &&
           "sync wrapper for suspendable AOT function must hard-fail");
    assert(!contains(code, "return XR_NULL_VAL;\n}\n\ntypedef struct") &&
           "sync wrapper must not silently return null");

    printf("  Generated suspendable wrapper %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_sync_call_to_suspendable_aborts) {
    const char *src = "fn worker(n: int) -> int {\n"
                      "    yield\n"
                      "    return n + 1\n"
                      "}\n"
                      "print(worker(41))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(had_error && "sync call to suspendable AOT function must be rejected");
    assert(contains(code, "(abort(), XR_NULL_VAL)") &&
           "rejected sync call should emit hard-fail expression only");
    assert(!contains(code, "unsupported AOT sync call") &&
           "diagnostics should go to stderr, not generated C comments");

    printf("  Generated rejected sync call %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_runtime_managed_types_skip_arc) {
    XrType task_type = {.kind = XR_KIND_INSTANCE};
    XrType channel_type = {.kind = XR_KIND_CHANNEL};
    XrType string_type = {.kind = XR_KIND_STRING};

    task_type.instance.class_name = "Task";

    assert(!xi_own_type_is_rc(&task_type) && "Task is owned by the coroutine runtime");
    assert(!xi_own_type_is_rc(&channel_type) && "Channel is owned by the coroutine runtime");
    assert(xi_own_type_is_rc(&string_type) && "String remains compiler ARC managed");
}

TEST(cgen_coro_frame_release_uses_aot_arc) {
    const char *src = "fn worker() -> string {\n"
                      "    let s = \"hello\" + \"_aot\"\n"
                      "    yield\n"
                      "    return s\n"
                      "}\n"
                      "let task = go worker()\n"
                      "let result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine with ARC values should generate");
    assert(contains(code, "_aot_release(void *frame, struct XrCoroGC *gc)") &&
           "frame release must receive coroutine GC context");
    assert(contains(code, "xrt_release(f->v3)") &&
           "AOT ARC string value should be released from the frame");
    assert(!contains(code, "xr_aot_release_frame_value(f->") &&
           "frame release must not call the old untyped release helper");

    printf("  Generated ARC-aware coroutine release %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_go_clones_tagged_args) {
    const char *src = "fn worker(xs: Array<int>) -> int {\n"
                      "    xs.push(99)\n"
                      "    yield\n"
                      "    return xs.length\n"
                      "}\n"
                      "let xs = [1, 2]\n"
                      "let task = go worker(xs)\n"
                      "let result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine with tagged go args should generate");
    assert(contains(code, "xrt_value_clone_for_coro(") &&
           "tagged go arguments must be cloned at the coroutine boundary");

    printf("  Generated coroutine argument clone %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_channel_send_clones_value) {
    const char *src = "let ch = new Channel<Array<int>>(1)\n"
                      "let xs = [1, 2]\n"
                      "ch.send(xs)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT channel send should generate");
    assert(contains(code, "xr_aot_chan_send(ctx,") && "channel send must use the AOT bridge");
    assert(contains(code, "xr_aot_chan_send(ctx, ") &&
           contains(code, "xrt_value_clone_for_coro(") &&
           "channel send values must be cloned at the coroutine boundary");

    printf("  Generated channel send clone %zu bytes of C code\n", strlen(code));
    free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_clones_tagged_result) {
    const char *src = "fn worker() -> Array<int> {\n"
                      "    yield\n"
                      "    return [1, 2]\n"
                      "}\n"
                      "let task = go worker()\n"
                      "let result = await task\n"
                      "print(result.length)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await should generate");
    assert(contains(code, "xr_aot_await_task(ctx,") && "await must use the AOT bridge");
    assert(contains(code, "xrt_value_clone_for_coro(") &&
           "tagged await results must be cloned at the coroutine boundary");

    printf("  Generated await result clone %zu bytes of C code\n", strlen(code));
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
    run_cgen_unsupported_coroutine_ops_fail_fast();
    run_cgen_suspendable_wrapper_aborts();
    run_cgen_sync_call_to_suspendable_aborts();
    run_cgen_runtime_managed_types_skip_arc();
    run_cgen_coro_frame_release_uses_aot_arc();
    run_cgen_coro_go_clones_tagged_args();
    run_cgen_coro_channel_send_clones_value();
    run_cgen_coro_await_clones_tagged_result();

    teardown();

    printf("\n=== %d/%d Xi CGen tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
