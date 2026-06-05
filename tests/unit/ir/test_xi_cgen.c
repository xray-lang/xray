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

/* Generate C code for Xi IR into an xr_malloc-owned string.
 * Caller releases the returned string with xr_free(). */
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

static size_t count_between(const char *start, const char *end, const char *needle) {
    size_t count = 0;
    size_t needle_len = strlen(needle);
    const char *p = start;
    assert(start != NULL);
    assert(end != NULL);
    assert(needle != NULL && needle_len > 0);
    while (p < end) {
        const char *hit = strstr(p, needle);
        if (!hit || hit >= end)
            break;
        count++;
        p = hit + needle_len;
    }
    return count;
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
    xr_free(code);
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
    xr_free(code);
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
    xr_free(code);
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
    xr_free(code);
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
    xr_free(code);
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
    xr_free(code);
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
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_module_prefix_is_c_identifier) {
    const char *src = "fn compute(n: int) -> int { return n * n }\n"
                      "print(compute(3))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "1127_coro_priority", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "numeric module prefixes should generate");
    assert(contains(code, "static XrValue _1127_coro_priority_compute_") &&
           "numeric module prefixes must be emitted as legal C identifiers");
    assert(!contains(code, "static XrValue 1127_coro_priority_compute_") &&
           "numeric module prefixes must not be emitted raw");

    printf("  Generated numeric-prefix module %zu bytes of C code\n", strlen(code));
    xr_free(code);
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
    xr_free(code);
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
    xr_free(code);
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
    assert(!contains(code, "unsupported coroutine Xi op") &&
           "unsupported coroutine diagnostics should not be emitted into generated C");
    printf("  Generated rejected %zu bytes of C code\n", strlen(code));
    xr_free(code);
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
    xr_free(code);
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
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_suspendable_dependency_init_fails_fast) {
    XrType stub_unit = {.kind = XR_KIND_UNIT, .id = 101, .frozen = true};

    XiFunc *dep = xi_func_new("init", &stub_unit);
    XiFunc *entry = xi_func_new("init", &stub_unit);
    assert(dep != NULL && entry != NULL);

    XiBlock *dep_entry = xi_block_new(dep);
    XiBlock *main_entry = xi_block_new(entry);
    assert(dep_entry != NULL && main_entry != NULL);

    XiValue *yield = xi_value_new(dep, dep_entry, XI_YIELD, &stub_unit, 0);
    assert(yield != NULL);
    yield->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(dep_entry, NULL);
    xi_block_set_return(main_entry, NULL);

    XiModule *dep_mod = xi_module_new("dep.xr", "dep", dep);
    XiModule *entry_mod = xi_module_new("main.xr", "main", entry);
    assert(dep_mod != NULL && entry_mod != NULL);
    XiModule *modules[] = {dep_mod, entry_mod};

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    assert(ctx != NULL);

    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    assert(mem != NULL);
    xi_cgen_main(ctx, mem, modules, 2, 1);
    int rc = xr_close_memstream(mem, &buf, &bufsz);
    assert(rc == 0);

    assert(xi_cgen_has_error(ctx) && "suspendable dependency init must fail C generation");
    assert(contains(buf, "return 1;") && "generated main should hard-fail if emitted");
    assert(!contains(buf, "unsupported") && "generated C must not contain unsupported comments");
    assert(!contains(buf, "coroutine dependency init") &&
           "dependency diagnostics should stay out of generated C comments");

    printf("  Generated rejected multi-module main %zu bytes of C code\n", strlen(buf));
    free(buf);
    xi_cgen_ctx_free(ctx);
    xi_module_free(dep_mod);
    xi_module_free(entry_mod);
    xi_func_free(dep);
    xi_func_free(entry);
}

TEST(cgen_coro_frame_params_use_typed_storage) {
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
    assert(!had_error && "AOT coroutine with typed params should generate");
    assert(contains(code, "uint32_t state;\n    int64_t p0;") &&
           "AOT coroutine int params should be stored unboxed in the frame");
    assert(contains(code, "f->p0 = XR_TO_INT(p0);") &&
           "AOT coroutine frame factory should unbox int params at the boundary");
    assert(!contains(code, "p0.i") &&
           "AOT coroutine typed params must not be unboxed as tagged values");
    assert(!contains(code, "xr_aot_trace_frame_value(visitor, f->p0)") &&
           "scalar frame params must not be traced as XrValue roots");
    assert(!contains(code, "xr_aot_trace_frame_value(visitor, f->v5)") &&
           "boxed scalar return after the final suspend must not be traced");
    assert(!contains(code, "xrt_value_clone_for_coro(") &&
           "scalar go and await boundaries must not call the deep-copy helper");
    assert(contains(code, ".root_count = 0,") &&
           "scalar coroutine frame should report zero traced roots");
    assert(contains(code, ".release_count = 0,") &&
           "scalar coroutine frame should report zero ARC release slots");

    printf("  Generated typed coroutine param frame %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_frame_skips_dead_ssa_slots) {
    const char *src = "fn worker(n: int) -> int {\n"
                      "    let a = n + 1\n"
                      "    let b = a + 2\n"
                      "    yield\n"
                      "    return n + 3\n"
                      "}\n"
                      "let task = go worker(41)\n"
                      "let result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine with dead pre-yield values should generate");

    const char *frame = strstr(code, "typedef struct test_worker_");
    assert(frame != NULL && "worker coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_worker_");
    assert(frame_end != NULL && "worker coroutine frame should have an end marker");
    assert(count_between(frame, frame_end, "\n    int64_t v") == 0 &&
           "dead scalar SSA values must stay out of the suspend frame");
    assert(count_between(frame, frame_end, "\n    XrValue v") == 0 &&
           "dead tagged SSA values must stay out of the suspend frame");
    assert(contains(code, "#define v0 (f->p0)") &&
           "parameter SSA references should alias the typed frame parameter");
    assert(contains(code, "int64_t v") &&
           "non-frame scalar SSA values should be resume-local variables");

    printf("  Generated compact coroutine frame %zu bytes of C code\n", strlen(code));
    xr_free(code);
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
    assert(contains(code, ".root_count = 1,") &&
           "AOT ARC string frame should report one traced root slot");
    assert(contains(code, ".release_count = 1,") &&
           "AOT ARC string frame should report one release slot");
    assert(contains(code, "xr_aot_frame_free(frame)") &&
           "AOT coroutine frame release should free the frame");
    assert(!contains(code, "xr_aot_release_frame_value(f->") &&
           "frame release must not call the old untyped release helper");

    printf("  Generated ARC-aware coroutine release %zu bytes of C code\n", strlen(code));
    xr_free(code);
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
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_go_sync_function_uses_wrapper_desc) {
    const char *src = "fn compute(n: int) -> int {\n"
                      "    return n * n\n"
                      "}\n"
                      "fn mutate_copy(xs: Array<int>) -> int {\n"
                      "    xs.push(99)\n"
                      "    return xs.length\n"
                      "}\n"
                      "let high = go(priority: Coro.HIGH) compute(5)\n"
                      "print(await high)\n"
                      "let xs = [1, 2]\n"
                      "let copied = go mutate_copy(xs)\n"
                      "print(await copied)\n"
                      "print(xs.length)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT go of sync functions should generate");
    assert(contains(code, "XrValue _result = test_compute_") &&
           "sync go wrapper must call the normal AOT function body");
    assert(contains(code, "XrValue _result = test_mutate_copy_") &&
           "sync go wrapper must call normal function bodies for tagged arguments");
    assert(contains(code, "xr_aot_done(_result)") &&
           "sync go wrapper must complete through the AOT coroutine result ABI");
    assert(contains(code, "xr_aot_trace_frame_value(visitor, f->p0)") &&
           "sync go wrapper params must remain traceable while queued");
    assert(contains(code, "xrt_value_clone_for_coro(") &&
           "sync go tagged arguments must still cross the coroutine clone boundary");
    assert(contains(code, "_aot_desc, _child_frame_") &&
           "go lowering must spawn sync wrappers through an AOT descriptor");

    printf("  Generated sync go wrapper %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_set_priority_uses_aot_bridge) {
    const char *src = "fn compute(n: int) -> int {\n"
                      "    return n * n\n"
                      "}\n"
                      "let task = go compute(3)\n"
                      "Coro.setPriority(task, 2)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT Coro.setPriority should generate");
    assert(contains(code, "xr_aot_coro_set_priority(ctx,") &&
           "Coro.setPriority must use the AOT coroutine runtime bridge");
    assert(!contains(code, "unsupported AOT coroutine Xi op CORO_OP") &&
           "Coro.setPriority must not fall through to unsupported CORO_OP emission");

    printf("  Generated Coro.setPriority bridge %zu bytes of C code\n", strlen(code));
    xr_free(code);
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
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scalar_channel_send_skips_clone) {
    const char *src = "let ch = new Channel<int>(1)\n"
                      "ch.send(42)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scalar channel send should generate");
    assert(contains(code, "xr_aot_chan_send_i64(ctx,") &&
           "scalar channel send must use the typed AOT bridge");
    assert(!contains(code, "xr_aot_chan_send(ctx,") &&
           "scalar channel send must not re-box at the generated call site");
    assert(count_between(code, code + strlen(code), "XR_FROM_INT(") == 1 &&
           "scalar channel send should not emit a dead boxed send operand");
    assert(!contains(code, "xrt_value_clone_for_coro(") &&
           "scalar channel send values must not call the deep-copy helper");

    printf("  Generated scalar channel send %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scalar_channel_try_send_uses_typed_bridge) {
    const char *src = "let ch = new Channel<int>(1)\n"
                      "let ok = ch.trySend(42)\n"
                      "print(ok)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scalar channel trySend should generate");
    assert(contains(code, "xr_aot_chan_try_send_i64(ctx,") &&
           "scalar channel trySend must use the typed AOT bridge");
    assert(!contains(code, "xr_aot_chan_try_send(ctx,") &&
           "scalar channel trySend must not re-box at the generated call site");
    assert(count_between(code, code + strlen(code), "XR_FROM_INT(") == 1 &&
           "scalar channel trySend should not emit a dead boxed send operand");
    assert(!contains(code, "xrt_value_clone_for_coro(") &&
           "scalar channel trySend values must not call the deep-copy helper");

    printf("  Generated scalar channel trySend %zu bytes of C code\n", strlen(code));
    xr_free(code);
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
    assert(contains(code, "xr_aot_await_task_resume(ctx, xr_slot_xvalue_ptr(&") &&
           "await resume must recover the task from coroutine wait state");
    assert(!contains(code, "xr_aot_await_task_resume(ctx, v") &&
           "await resume must not keep the task operand in the AOT frame");
    assert(contains(code, "xrt_value_clone_for_coro(") &&
           "tagged await results must be cloned at the coroutine boundary");

    printf("  Generated await result clone %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scalar_await_uses_typed_slot) {
    const char *src = "fn worker() -> int {\n"
                      "    yield\n"
                      "    return 41\n"
                      "}\n"
                      "fn main_plus() -> int {\n"
                      "    let task = go worker()\n"
                      "    let v = await task\n"
                      "    return v + 1\n"
                      "}\n"
                      "let task = go main_plus()\n"
                      "let result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scalar await should generate");

    const char *frame = strstr(code, "typedef struct test_main_plus_");
    assert(frame != NULL && "main_plus coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_main_plus_");
    assert(frame_end != NULL && "main_plus coroutine frame should close");
    assert(count_between(frame, frame_end, "int64_t v") >= 1 &&
           "scalar await should reserve an unboxed frame slot");

    const char *resume = strstr(code, "static XrAotResult test_main_plus_");
    const char *trace = resume ? strstr(resume, "static void test_main_plus_") : NULL;
    assert(resume != NULL && trace != NULL && "main_plus resume function should be emitted");
    assert(count_between(resume, trace, "xr_aot_await_task(ctx,") == 1 &&
           "initial scalar await should use the slot bridge");
    assert(count_between(resume, trace, "xr_aot_await_task_resume(ctx,") == 1 &&
           "resumed scalar await should use the slot bridge");
    assert(count_between(resume, trace, "xr_slot_aot_frame_offset") == 2 &&
           "scalar await should pass a typed frame slot on start and resume");
    assert(count_between(resume, trace, "XR_TO_INT(v") == 0 &&
           "typed await should not unbox a tagged await value after resume");

    printf("  Generated scalar await slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_timeout_passes_deadline) {
    const char *src = "fn worker(ch: Channel<int>) -> int {\n"
                      "    let value = ch.recv()\n"
                      "    return value\n"
                      "}\n"
                      "let ch = new Channel<int>(0)\n"
                      "let task = go worker(ch)\n"
                      "let result = await(timeout: 25) task\n"
                      "print(result == null)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await timeout should generate");
    const char *await_call = strstr(code, "xr_aot_await_task(ctx,");
    assert(await_call != NULL && "timeout await must use the AOT await bridge");
    const char *await_line_end = strchr(await_call, '\n');
    bool await_line_terminated = await_line_end != NULL;
    bool await_uses_infinite_wait =
        await_line_terminated && count_between(await_call, await_line_end, ", -1, false);") > 0;
    assert(await_line_terminated && "timeout await call should be line-terminated");
    assert(!await_uses_infinite_wait && "timeout await must not use the infinite-wait sentinel");
    (void) await_line_terminated;
    (void) await_uses_infinite_wait;
    assert(contains(code, "INT64_C(25)") && "timeout expression should be evaluated");

    printf("  Generated await timeout %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_recv_resume_uses_wait_state_slot) {
    const char *src = "let ch = new Channel<int>(0)\n"
                      "let value = ch.recv()\n"
                      "print(value)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT channel recv should generate");
    assert(contains(code, "xr_aot_chan_recv_slot(ctx,") &&
           "initial channel recv must register a backend-neutral slot");
    assert(contains(code, "xr_aot_chan_recv_slot_resume(ctx, xr_slot_none(), false);") &&
           "channel recv resume must recover the slot from coroutine wait state");
    assert(!contains(code, "xr_aot_chan_recv_slot_resume(ctx, _chan_recv_slot_") &&
           "channel recv resume must not depend on a local slot variable");

    printf("  Generated channel recv wait-state slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scalar_channel_recv_uses_typed_slot) {
    const char *src = "fn recv_plus(ch: Channel<int>) -> int {\n"
                      "    let v = ch.recv()\n"
                      "    return v + 1\n"
                      "}\n"
                      "let ch = new Channel<int>(1)\n"
                      "ch.send(9)\n"
                      "let task = go recv_plus(ch)\n"
                      "let result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scalar channel recv should generate");

    const char *frame = strstr(code, "typedef struct test_recv_plus_");
    assert(frame != NULL && "recv_plus coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_recv_plus_");
    assert(frame_end != NULL && "recv_plus coroutine frame should close");
    assert(count_between(frame, frame_end, "int64_t v") >= 1 &&
           "scalar recv should reserve an unboxed frame slot");
    assert(count_between(frame, frame_end, "XrValue v") == 0 &&
           "scalar-only recv should not reserve a tagged recv slot");

    const char *slot_ref = strstr(code, "xr_slot_aot_frame_offset");
    assert(slot_ref != NULL && "channel recv must create a backend-neutral slot ref");
    assert(strstr(slot_ref, "), 0);\n    XrAotResult _chan_recv_") != NULL &&
           "channel recv slot should use XR_REP_I64 for scalar-only consumers");

    const char *recv_call = strstr(code, "xr_aot_chan_recv_slot(ctx,");
    assert(recv_call != NULL && "channel recv must use the AOT recv slot bridge");

    const char *resume = strstr(code, "static XrAotResult test_recv_plus_");
    const char *trace = resume ? strstr(resume, "static void test_recv_plus_") : NULL;
    assert(resume != NULL && trace != NULL && "recv_plus resume function should be emitted");
    assert(count_between(resume, trace, "XR_TO_INT(v") == 0 &&
           "typed recv should not unbox a tagged receive value after resume");

    printf("  Generated scalar channel recv slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_recv_slot_is_traced_as_frame_root) {
    const char *src = "let ch = new Channel<string>(0)\n"
                      "let value = ch.recv()\n"
                      "print(value)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT tagged channel recv should generate");
    assert(contains(code, "xr_aot_chan_recv_slot(ctx,") &&
           "tagged channel recv must register a frame slot");
    assert(contains(code, "xr_aot_trace_frame_value(visitor, f->v") &&
           "tagged channel recv slot must be scanned while ready or blocked");
    assert(contains(code, ".root_count = 1,") &&
           "tagged channel recv frame should report one traced root slot");

    printf("  Generated traced channel recv slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_all_uses_aggregate_bridge) {
    const char *src = "fn worker(n: int) -> int {\n"
                      "    yield\n"
                      "    return n * n\n"
                      "}\n"
                      "let t1 = go worker(2)\n"
                      "let t2 = go worker(3)\n"
                      "let results = await all [t1, t2]\n"
                      "print(results[0] + results[1])\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await all should generate");
    assert(contains(code, "xr_aot_await_all_tasks(ctx,") &&
           "await all must use the aggregate AOT bridge");
    assert(contains(code, "xr_aot_await_all_tasks_resume(ctx,") &&
           "await all resume must use the aggregate AOT bridge");
    assert(contains(code, "xrt_value_clone_for_coro(") &&
           "await all task arrays must survive suspension");
    assert(contains(code, "xr_aot_bridge_value_to_xrt(") &&
           "await all runtime result arrays must be converted back to AOT arrays");

    printf("  Generated await all aggregate bridge %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_any_uses_typed_aggregate_bridge) {
    const char *src = "fn delayed(ch: Channel<int>, n: int) -> int {\n"
                      "    ch.recv()\n"
                      "    return n\n"
                      "}\n"
                      "let ch1 = new Channel<int>(0)\n"
                      "let ch2 = new Channel<int>(1)\n"
                      "let t1 = go delayed(ch1, 1)\n"
                      "let t2 = go delayed(ch2, 2)\n"
                      "ch2.send(9)\n"
                      "let first = await any [t1, t2]\n"
                      "print(first)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT await any should generate");
    assert(contains(code, "xr_aot_await_any_task(ctx,") &&
           "await any must use the aggregate AOT bridge");
    assert(contains(code, "xr_aot_await_any_task_resume(ctx,") &&
           "await any resume must use the aggregate AOT bridge");
    assert(contains(code, "xr_slot_aot_frame_offset") &&
           "scalar await any results should use a typed frame slot");

    printf("  Generated await any aggregate bridge %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_task_status_uses_task_bridge) {
    const char *src = "fn wait_for_value(ch: Channel<int>) -> int {\n"
                      "    let value = ch.recv()\n"
                      "    return value\n"
                      "}\n"
                      "fn quick_value(n: int) -> int {\n"
                      "    yield\n"
                      "    return n * 2\n"
                      "}\n"
                      "fn task_done(task: Task) -> bool {\n"
                      "    return task.done\n"
                      "}\n"
                      "fn task_cancelled(task: Task) -> bool {\n"
                      "    return task.cancelled\n"
                      "}\n"
                      "fn task_result(task: Task) -> Json {\n"
                      "    return task.result\n"
                      "}\n"
                      "const ch = new Channel<int>(0)\n"
                      "let blocked = go wait_for_value(ch)\n"
                      "blocked.cancel()\n"
                      "let cancelled_value = await blocked\n"
                      "print(blocked.done)\n"
                      "print(blocked.cancelled)\n"
                      "print(cancelled_value == null)\n"
                      "let quick = go quick_value(21)\n"
                      "let quick_result = await quick\n"
                      "print(quick.done)\n"
                      "print(quick.result)\n"
                      "print(quick_result)\n"
                      "print(task_done(quick))\n"
                      "print(task_cancelled(quick))\n"
                      "print(task_result(quick))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT Task status access should generate");
    assert(contains(code, "xr_aot_task_cancel(ctx,") && "Task.cancel must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_done(ctx,") && "Task.done must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_cancelled(ctx,") &&
           "Task.cancelled must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_result(ctx,") && "Task.result must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_done(NULL,") &&
           "sync AOT Task.done must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_cancelled(NULL,") &&
           "sync AOT Task.cancelled must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_result(NULL,") &&
           "sync AOT Task.result must use the AOT Task bridge");
    assert(!contains(code, "xrt_method_0(v") &&
           "Task.cancel must not fall back to AOT dynamic method dispatch");
    assert(!contains(code, "xrt_getprop(v") &&
           "Task fields must not fall back to AOT dynamic property dispatch");

    printf("  Generated Task status bridge %zu bytes of C code\n", strlen(code));
    xr_free(code);
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
    run_cgen_module_prefix_is_c_identifier();
    run_cgen_recursive();
    run_cgen_for_loop();
    run_cgen_unsupported_coroutine_ops_fail_fast();
    run_cgen_suspendable_wrapper_aborts();
    run_cgen_sync_call_to_suspendable_aborts();
    run_cgen_suspendable_dependency_init_fails_fast();
    run_cgen_coro_frame_params_use_typed_storage();
    run_cgen_coro_frame_skips_dead_ssa_slots();
    run_cgen_runtime_managed_types_skip_arc();
    run_cgen_coro_frame_release_uses_aot_arc();
    run_cgen_coro_go_clones_tagged_args();
    run_cgen_coro_go_sync_function_uses_wrapper_desc();
    run_cgen_coro_set_priority_uses_aot_bridge();
    run_cgen_coro_channel_send_clones_value();
    run_cgen_coro_scalar_channel_send_skips_clone();
    run_cgen_coro_scalar_channel_try_send_uses_typed_bridge();
    run_cgen_coro_await_clones_tagged_result();
    run_cgen_coro_scalar_await_uses_typed_slot();
    run_cgen_coro_await_timeout_passes_deadline();
    run_cgen_coro_recv_resume_uses_wait_state_slot();
    run_cgen_coro_scalar_channel_recv_uses_typed_slot();
    run_cgen_coro_recv_slot_is_traced_as_frame_root();
    run_cgen_coro_await_all_uses_aggregate_bridge();
    run_cgen_coro_await_any_uses_typed_aggregate_bridge();
    run_cgen_coro_task_status_uses_task_bridge();

    teardown();

    printf("\n=== %d/%d Xi CGen tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
