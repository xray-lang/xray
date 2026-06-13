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
#include "../../../src/aot/xaot_bundle.h"
#include "../../../src/aot/xaot_prepare.h"
#include "../../../src/aot/xaot_verify.h"
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

typedef struct TestAotPlan {
    XaotBundle bundle;
    bool initialized;
} TestAotPlan;

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

static void test_aot_plan_prepare(TestAotPlan *plan, XiModule **modules, uint32_t nmodules,
                                  uint32_t entry_module) {
    char verify_err[256];

    assert(plan != NULL);
    memset(plan, 0, sizeof(*plan));
    assert(xaot_bundle_init(&plan->bundle, modules, nmodules, entry_module) &&
           "AOT bundle init failed");
    plan->initialized = true;
    assert(xaot_prepare_bundle(&plan->bundle, NULL) && "AOT prepare failed");
    assert(
        xaot_verify_bundle(&plan->bundle, XAOT_VERIFY_AOT_READY, verify_err, sizeof(verify_err)) &&
        "AOT verify failed");
}

static void test_aot_plan_free(TestAotPlan *plan) {
    if (plan && plan->initialized)
        xaot_bundle_free(&plan->bundle);
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
static char *generate_c_with_status_and_stats(XiFunc *ir, const char *module_name, bool *had_error,
                                              XiCgenCoroFrameStats *coro_stats) {
    assert(ir != NULL);

    /* AOT codegen owns a native scalar boundary; tagged adapters are emitted
     * only where dynamic closure calls require them. */
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    xi_opt_select_rep_with_policy(ir, &policy);

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
    XiModule *modules[] = {mod};
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 1, 0);

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    assert(ctx != NULL);
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);

    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    assert(mem != NULL);

    xi_cgen_program(ctx, mem, mod);
    int rc = xr_close_memstream(mem, &buf, &bufsz);
    assert(rc == 0);
    if (had_error)
        *had_error = xi_cgen_has_error(ctx);
    if (coro_stats)
        *coro_stats = xi_cgen_coro_frame_stats(ctx);

    xi_cgen_ctx_free(ctx);
    test_aot_plan_free(&plan);
    if (own_mod) {
        mod->init = NULL; /* don't double-free ir */
        xi_module_free(mod);
    }

    return buf;
}

static char *generate_c_with_status_and_cgen_stats(XiFunc *ir, const char *module_name,
                                                   bool *had_error, XiCgenStats *cgen_stats) {
    assert(ir != NULL);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    xi_opt_select_rep_with_policy(ir, &policy);

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
    XiModule *modules[] = {mod};
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 1, 0);

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    assert(ctx != NULL);
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);

    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    assert(mem != NULL);

    xi_cgen_program(ctx, mem, mod);
    int rc = xr_close_memstream(mem, &buf, &bufsz);
    assert(rc == 0);
    if (had_error)
        *had_error = xi_cgen_has_error(ctx);
    if (cgen_stats)
        *cgen_stats = xi_cgen_stats(ctx);

    xi_cgen_ctx_free(ctx);
    test_aot_plan_free(&plan);
    if (own_mod) {
        mod->init = NULL; /* don't double-free ir */
        xi_module_free(mod);
    }

    return buf;
}

static char *generate_c_with_status(XiFunc *ir, const char *module_name, bool *had_error) {
    return generate_c_with_status_and_stats(ir, module_name, had_error, NULL);
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

static bool nonzero_state_precedes_call(const char *code, const char *call) {
    const char *call_pos = strstr(code, call);
    const char *last_state = NULL;
    const char *p = code;
    assert(code != NULL);
    assert(call != NULL);
    if (!call_pos)
        return false;

    while ((p = strstr(p, "f->state = ")) != NULL && p < call_pos) {
        last_state = p;
        p++;
    }
    if (!last_state)
        return false;

    const char *value = strstr(last_state, "= ");
    return value && value[2] != '0';
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
    /* Should have a main function that accepts script arguments. */
    assert(contains(code, "int main(int argc, char **argv)") && "should have main()");
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
    assert(contains(code, "static const xrt_str_t _xstr_") &&
           "literal should have a static header with precomputed hash");
    assert(contains(code, "xr_str_lit(&_xstr_") && "use site should reference the static header");

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

TEST(cgen_stats_tracks_native_abi) {
    const char *src = "fn inc(x: int) -> int { return x + 1 }\n"
                      "print(inc(41))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    XiCgenStats stats = {0};
    char *code = generate_c_with_status_and_cgen_stats(ir, "test", &had_error, &stats);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "stats smoke program should generate");
    assert(stats.functions_total >= 2 && "module init plus static function should be counted");
    assert(stats.functions_native_abi >= 1 && "inc should use native scalar ABI");
    assert(stats.functions_tagged_abi >= 1 && "module init remains a tagged boundary");
    assert(stats.boxed_adapters >= 1 && "native function should expose a boxed adapter");

    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_module_prefix_is_c_identifier) {
    const char *src = "fn compute(n: int) -> int { return n * n }\n"
                      "print(compute(3))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "1127_coro_numeric_prefix", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "numeric module prefixes should generate");
    assert(contains(code, "static XrValue _1127_coro_numeric_prefix_compute_") &&
           "numeric module prefixes must be emitted as legal C identifiers");
    assert(!contains(code, "static XrValue 1127_coro_numeric_prefix_compute_") &&
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

TEST(cgen_typed_array_uses_raw_storage_fast_path) {
    const char *src = "fn sum() -> int {\n"
                      "    let values: Array<int> = []\n"
                      "    values.push(41)\n"
                      "    return values[0] + values.length\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array fast path should generate");
    assert((contains(code, "xrt_array_new_typed(") || contains(code, "xrt_array_new_typed_ptr(")) &&
           "Array<int> creation must preserve typed storage");
    assert(contains(code, "XR_ELEM_I64") && "Array<int> must use the I64 typed element layout");
    assert(contains(code, "((int64_t*)_a->data)") &&
           "Array<int> index reads and writes must access raw typed storage");
    assert((contains(code, "((xrt_array_t*)") || contains(code, "->len")) &&
           "Array<int>.length must read the runtime array length directly");
    assert(!contains(code, "xrt_method_1(") &&
           "Array<int>.push must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "Array<int>.length must not fall back to dynamic property dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<int> index read must not fall back to runtime index dispatch");

    printf("  Generated typed array fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_u8_uses_byte_storage_fast_path) {
    const char *src = "fn sum() -> int {\n"
                      "    let bytes: Array<uint8> = []\n"
                      "    bytes.push(300)\n"
                      "    return bytes[0] + bytes.length\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed byte array fast path should generate");
    assert((contains(code, "xrt_array_new_typed(") || contains(code, "xrt_array_new_typed_ptr(")) &&
           "Array<uint8> creation must preserve typed storage");
    assert(contains(code, "XR_ELEM_U8") && "Array<uint8> must use the U8 typed element layout");
    assert(contains(code, "((uint8_t*)_a->data)") &&
           "Array<uint8> index reads and writes must access raw byte storage");
    assert(contains(code, "(uint8_t)") &&
           "Array<uint8> writes must narrow to the byte storage width");
    assert(!contains(code, "xrt_method_1(") &&
           "Array<uint8>.push must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "Array<uint8>.length must not fall back to dynamic property dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<uint8> index read must not fall back to runtime index dispatch");

    printf("  Generated typed byte array fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_bytes_methods_use_raw_memory_helpers) {
    const char *src = "fn run() -> int {\n"
                      "    let src = new Bytes(8)\n"
                      "    src[0] = 1\n"
                      "    src[1] = 2\n"
                      "    src[2] = 3\n"
                      "    src[3] = 4\n"
                      "    let dst = new Bytes(8)\n"
                      "    dst.copyFrom(src, 0, 0, 4)\n"
                      "    dst.copyWithin(2, 0, 4)\n"
                      "    dst.repeatFrom(6, 2, 2)\n"
                      "    return dst.loadU32LE(0) + dst.loadU64LE(0) + dst[6]\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "Bytes raw memory helpers should generate");

    const char *fn = strstr(code, "static int64_t test_run_");
    assert(fn != NULL && "run declaration should exist");
    fn = strstr(fn + 1, "static int64_t test_run_");
    assert(fn != NULL && "run definition should exist");
    const char *fn_end = strstr(fn, "static XrValue test_run_");
    assert(fn_end != NULL && "boxed run adapter should follow typed function");
    const char *fn_body = strchr(fn, '{');
    assert(fn_body != NULL && fn_body < fn_end && "run function body should be bounded");

    assert(count_between(fn_body, fn_end, "xrt_bytes_load_u32_le_raw(") > 0 &&
           "Bytes.loadU32LE must lower to the raw AOT helper");
    assert(count_between(fn_body, fn_end, "xrt_bytes_load_u64_le_raw(") > 0 &&
           "Bytes.loadU64LE must lower to the raw AOT helper");
    assert(count_between(fn_body, fn_end, "xrt_bytes_copy_from_raw(") > 0 &&
           "Bytes.copyFrom must lower to the raw AOT helper");
    assert(count_between(fn_body, fn_end, "xrt_bytes_copy_within_raw(") > 0 &&
           "Bytes.copyWithin must lower to the raw AOT helper");
    assert(count_between(fn_body, fn_end, "xrt_bytes_repeat_from_raw(") > 0 &&
           "Bytes.repeatFrom must lower to the raw AOT helper");
    assert(count_between(fn_body, fn_end, "xrt_bytes_load_u32_le_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_bytes_load_u64_le_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_bytes_copy_from_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_bytes_copy_within_value(") == 0 &&
           count_between(fn_body, fn_end, "xrt_bytes_repeat_from_value(") == 0 &&
           "Bytes hot path must not call boxed value helpers");
    assert(count_between(fn_body, fn_end, "xrt_method_") == 0 &&
           count_between(fn_body, fn_end, "xrt_index_get(") == 0 &&
           "Bytes hot path must not fall back to dynamic dispatch");

    printf("  Generated Bytes raw helper fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_i16_and_u32_use_raw_storage_fast_path) {
    const char *src = "fn mix() -> int {\n"
                      "    let i16s: Array<int16> = []\n"
                      "    let u32s: Array<uint32> = []\n"
                      "    i16s.push(32768)\n"
                      "    u32s.push(4294967295)\n"
                      "    return i16s[0] + u32s[0] + i16s.length + u32s.length\n"
                      "}\n"
                      "print(mix())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed sub-width array fast path should generate");
    assert(contains(code, "XR_ELEM_I16") && "Array<int16> must use the I16 typed element layout");
    assert(contains(code, "((int16_t*)_a->data)") &&
           "Array<int16> index reads and writes must access raw int16 storage");
    assert(contains(code, "(int16_t)") &&
           "Array<int16> writes must narrow to the signed 16-bit storage width");
    assert(contains(code, "XR_ELEM_U32") && "Array<uint32> must use the U32 typed element layout");
    assert(contains(code, "((uint32_t*)_a->data)") &&
           "Array<uint32> index reads and writes must access raw uint32 storage");
    assert(contains(code, "(uint32_t)") &&
           "Array<uint32> writes must narrow to the unsigned 32-bit storage width");
    assert(!contains(code, "xrt_method_1(") &&
           "sub-width typed array push must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "sub-width typed array index read must not fall back to runtime index dispatch");

    printf("  Generated typed sub-width array fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_float_and_bool_use_raw_storage_fast_path) {
    const char *src = "fn mix() -> float {\n"
                      "    let values: Array<float> = []\n"
                      "    let samples: Array<float32> = []\n"
                      "    let flags: Array<bool> = []\n"
                      "    values.push(3.5)\n"
                      "    samples.push(1.25)\n"
                      "    flags.push(true)\n"
                      "    if (flags[0]) {\n"
                      "        return values[0] + samples[0] + values.length\n"
                      "    }\n"
                      "    return 0.0\n"
                      "}\n"
                      "print(mix())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed float/bool array fast path should generate");
    assert(contains(code, "XR_ELEM_F64") && "Array<float> must use the F64 typed element layout");
    assert(contains(code, "((double*)_a->data)") &&
           "Array<float> index reads and writes must access raw double storage");
    assert(contains(code, "XR_ELEM_F32") && "Array<float32> must use the F32 typed element layout");
    assert(contains(code, "((float*)_a->data)") &&
           "Array<float32> index reads and writes must access raw float storage");
    assert(!contains(code, "(double)(float)((float*)") &&
           "Array<float32> raw loads are already float-rounded");
    assert(contains(code, "XR_ELEM_BOOL") && "Array<bool> must use the BOOL typed element layout");
    assert(contains(code, "((uint8_t*)_a->data)") &&
           "Array<bool> index reads and writes must access raw byte storage");
    assert(!contains(code, "xrt_method_1(") &&
           "typed array push must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "typed array length must not fall back to dynamic property dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "typed array index read must not fall back to runtime index dispatch");

    printf("  Generated typed float/bool array fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_inlined_struct_uses_native_field_storage) {
    const char *src = "struct Sample {\n"
                      "    x: int\n"
                      "    y: float\n"
                      "    ok: bool\n"
                      "    byte: uint8\n"
                      "}\n"
                      "fn run() -> int {\n"
                      "    let p = Sample{x: 41, y: 2.5, ok: true, byte: 300}\n"
                      "    p.x = p.x + 1\n"
                      "    p.byte = p.byte + 1\n"
                      "    if (p.ok) {\n"
                      "        return p.x + p.byte\n"
                      "    }\n"
                      "    return 0\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "inlined struct native fields should generate");
    assert(contains(code, "struct { int64_t f0; double f1; uint8_t f2; uint8_t f3; }") &&
           "inlined struct must use native C field storage");
    assert(contains(code, "(uint8_t)") && "sub-width struct stores must narrow to storage width");
    assert(!contains(code, "XrValue f0") && "inlined scalar struct fields must not be boxed");
    assert(!contains(code, "xrt_getprop(") &&
           "inlined scalar struct field reads must not use dynamic property dispatch");
    assert(!contains(code, "xrt_map_set(") &&
           "inlined scalar struct field writes must not use map-style dynamic storage");

    printf("  Generated native struct field fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_escaping_struct_uses_heap_native_storage) {
    const char *src = "struct Sample {\n"
                      "    x: int\n"
                      "    y: float\n"
                      "    ok: bool\n"
                      "    byte: uint8\n"
                      "}\n"
                      "let p = Sample{x: 41, y: 2.5, ok: true, byte: 300}\n"
                      "p.x = p.x + 1\n"
                      "p.byte = p.byte + 1\n"
                      "if (p.ok) {\n"
                      "    print(p.x + p.byte)\n"
                      "} else {\n"
                      "    print(0)\n"
                      "}\n"
                      "print(p.y)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "escaping struct heap-native path should generate");
    assert(contains(code, "typedef struct xrt_struct_test_") &&
           "escaping primitive struct must emit a native heap layout");
    assert(contains(code, "XR_TAG_STRUCT_REF") &&
           "escaping primitive struct must allocate as an AOT struct reference");
    assert(contains(code, "->f0") && contains(code, "->f1") && contains(code, "->f2") &&
           contains(code, "->f3") && "escaping primitive struct fields must use direct access");
    assert(!contains(code, "xrt_map_new(") && "primitive struct must not allocate runtime map");
    assert(!contains(code, "xrt_map_get(") && "primitive struct reads must not use runtime map");
    assert(!contains(code, "xrt_map_set(") && "primitive struct writes must not use runtime map");
    assert(!contains(code, "xrt_call_method(") &&
           "escaping struct path must not call an undeclared constructor helper");

    printf("  Generated escaping struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_escaping_struct_string_field_uses_heap_native_storage) {
    const char *src = "struct Item {\n"
                      "    count: int\n"
                      "    name: string\n"
                      "}\n"
                      "let item = Item{count: 2, name: \"hi\"}\n"
                      "item.count = item.count + 3\n"
                      "print(item.count)\n"
                      "print(item.name)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "string-field heap-native struct path should generate");
    assert(contains(code, "typedef struct xrt_struct_test_") &&
           "mixed scalar/string struct must emit a native heap layout");
    assert(contains(code, "XrValue f1") &&
           "string struct field must be stored as a tagged immutable reference field");
    assert(contains(code, "XR_TAG_STRUCT_REF") &&
           "mixed scalar/string struct must allocate as an AOT struct reference");
    assert(contains(code, "->f0") && contains(code, "->f1") &&
           "mixed scalar/string struct fields must use direct access");
    assert(!contains(code, "xrt_map_new(") && "mixed scalar/string struct must not allocate map");
    assert(!contains(code, "xrt_map_get(") && "mixed scalar/string struct reads must not use map");
    assert(!contains(code, "xrt_map_set(") && "mixed scalar/string struct writes must not use map");

    printf("  Generated string-field struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_nested_struct_field_uses_embedded_heap_native_storage) {
    const char *src = "struct Point {\n"
                      "    x: int\n"
                      "    y: int\n"
                      "}\n"
                      "struct Box {\n"
                      "    p: Point\n"
                      "    z: int\n"
                      "}\n"
                      "let b = Box{p: Point{x: 1, y: 2}, z: 3}\n"
                      "b.p.x = b.p.x + 4\n"
                      "print(b.p.x + b.p.y + b.z)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "nested struct heap-native path should generate");
    const char *code_end = code + strlen(code);
    assert(count_between(code, code_end, "typedef struct xrt_struct_test_") >= 2 &&
           "nested struct path must emit parent and child native heap layouts");
    assert(contains(code, "xrt_struct_test_") && contains(code, " f0;") &&
           "parent heap layout must embed the child native struct field");
    assert(contains(code, "memcpy(&") &&
           "nested struct field assignment must copy embedded native bytes");
    assert(contains(code, "*)&") &&
           "nested struct field reads must use direct access through the embedded field address");
    assert(!contains(code, "xrt_map_new(") && "nested struct must not allocate map storage");
    assert(!contains(code, "xrt_map_get(") && "nested struct reads must not use map storage");
    assert(!contains(code, "xrt_map_set(") && "nested struct writes must not use map storage");

    printf("  Generated nested struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_fixed_array_struct_field_uses_embedded_heap_native_storage) {
    const char *src = "struct Buf {\n"
                      "    data: [4]uint8\n"
                      "    bias: int\n"
                      "}\n"
                      "let buf = Buf{data: [1, 2, 3, 4], bias: 5}\n"
                      "fn run(n: int) -> int {\n"
                      "    let i = 0\n"
                      "    while (i < n) {\n"
                      "        buf.data[0] = i\n"
                      "        buf.data[1] = buf.data[0]\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return buf.data[1]\n"
                      "}\n"
                      "print(run(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "fixed-array struct field heap-native path should generate");

    const char *fn = strstr(code, "static int64_t test_run_");
    assert(fn != NULL && "run declaration should exist");
    fn = strstr(fn + 1, "static int64_t test_run_");
    assert(fn != NULL && "run definition should exist");
    const char *fn_end = strstr(fn, "static XrValue test_run_");
    assert(fn_end != NULL && "boxed run adapter should follow typed function");
    const char *fn_body = strchr(fn, '{');
    assert(fn_body != NULL && fn_body < fn_end && "run function body should be bounded");

    assert(contains(code, "uint8_t f0[4]") &&
           "fixed array field must be embedded in the native heap layout");
    assert(contains(code, "xrt_fixed_array_copy") &&
           "fixed array field initialization must copy into embedded storage");
    assert(count_between(fn_body, fn_end, "f0[") > 0 &&
           "fixed array hot path must use direct C array indexing");
    assert(count_between(fn_body, fn_end, "\n    XrValue v") == 0 &&
           "fixed array hot function must not materialize tagged array refs");
    assert(count_between(fn_body, fn_end, "xrt_index_get(") == 0 &&
           count_between(fn_body, fn_end, "xrt_index_set(") == 0 &&
           "fixed array hot path must not call generic index helpers");
    assert(!contains(code, "xrt_map_new(") && !contains(code, "xrt_map_get(") &&
           !contains(code, "xrt_map_set(") && "fixed array struct must not use map storage");

    printf("  Generated fixed-array struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_shared_struct_alias_elides_tagged_hot_locals) {
    const char *src = "struct Cell {\n"
                      "    a: int\n"
                      "    b: int\n"
                      "    step: int\n"
                      "}\n"
                      "let cell = Cell{a: 1, b: 2, step: 3}\n"
                      "fn run(n: int) -> int {\n"
                      "    let p = cell\n"
                      "    let i = 0\n"
                      "    let sum = 0\n"
                      "    while (i < n) {\n"
                      "        p.a = p.a + i\n"
                      "        p.b = p.b + p.step\n"
                      "        sum = sum + p.a - p.b\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return sum + p.a + p.b\n"
                      "}\n"
                      "print(run(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "shared struct alias fast path should generate");

    const char *fn = strstr(code, "static int64_t test_run_");
    assert(fn != NULL && "run declaration should exist");
    fn = strstr(fn + 1, "static int64_t test_run_");
    assert(fn != NULL && "run definition should exist");
    const char *fn_end = strstr(fn, "static XrValue test_run_");
    assert(fn_end != NULL && "boxed run adapter should follow typed function");
    const char *fn_body = strchr(fn, '{');
    assert(fn_body != NULL && fn_body < fn_end && "run function body should be bounded");

    assert(contains(code, "XR_TAG_STRUCT_REF") &&
           "shared primitive struct must use native heap storage");
    assert(count_between(fn_body, fn_end, "\n    XrValue v") == 0 &&
           "shared struct hot function must not materialize tagged local aliases");
    assert(count_between(fn_body, fn_end, "xrt_shared[") > 0 &&
           count_between(fn_body, fn_end, "].ptr") > 0 &&
           "shared struct fields should read the shared slot pointer directly");
    assert(count_between(fn_body, fn_end, "xrt_release(") == 0 &&
           "elided shared struct alias should not emit a dead release");
    assert(count_between(fn_body, fn_end, "xrt_map_get") == 0 &&
           count_between(fn_body, fn_end, "xrt_map_set") == 0 &&
           "shared struct hot path must not cross the map boundary");

    printf("  Generated shared struct alias fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_method_caches_receiver_scalar_fields) {
    const char *src = "class Counter {\n"
                      "    value: int\n"
                      "    step: int\n"
                      "    constructor(init: int, step: int) {\n"
                      "        this.value = init\n"
                      "        this.step = step\n"
                      "    }\n"
                      "    bump(n: int) -> int {\n"
                      "        let i = 0\n"
                      "        let sum = 0\n"
                      "        while (i < n) {\n"
                      "            this.value = this.value + this.step\n"
                      "            sum = sum + this.value\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return sum + this.value\n"
                      "    }\n"
                      "}\n"
                      "let c = Counter(1, 3)\n"
                      "print(c.bump(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class receiver field cache should generate");

    const char *fn = strstr(code, "static int64_t test_bump_");
    assert(fn != NULL && "bump method should use typed ABI");
    fn = strstr(fn + 1, "static int64_t test_bump_");
    assert(fn != NULL && "bump method definition should follow its declaration");
    const char *fn_end = strstr(fn, "static XrValue test_bump_");
    assert(fn_end != NULL && "boxed bump adapter should follow typed method");

    assert(contains(code, "typedef struct xrt_native_test_Counter") &&
           "primitive-layout classes should emit an AOT native receiver type");
    assert(contains(fn, "xrt_native_test_Counter *p0") &&
           "bump method receiver should use native pointer ABI");
    assert(count_between(fn, fn_end, "_cf") > 0 &&
           "receiver scalar fields should be cached in native locals");
    assert(count_between(fn, fn_end, "xrt_map_get") == 0 &&
           "native receiver method body should not read fields through map boundary");
    assert(count_between(fn, fn_end, "xrt_map_set") == 0 &&
           "native receiver method body should not flush fields through map boundary");
    const char *value_load = strstr(fn, "p0->f0");
    const char *step_load = strstr(fn, "p0->f1");
    assert(value_load != NULL && value_load < fn_end && step_load != NULL && step_load < fn_end &&
           value_load < step_load && "receiver scalar field cache should follow class layout");
    assert(count_between(fn, fn_end, "XR_FROM_INT(v") == 0 &&
           "method body should not box scalar field store temporaries");

    printf("  Generated class receiver field cache %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_set_length_size_sum_uses_native_arithmetic) {
    const char *src = "class Bag {\n"
                      "    values: Set<int>\n"
                      "    constructor() {\n"
                      "        this.values = #[]\n"
                      "    }\n"
                      "    fill(n: int) -> int {\n"
                      "        let i = 0\n"
                      "        while (i < n) {\n"
                      "            this.values.add(i)\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return this.values.length + this.values.size\n"
                      "    }\n"
                      "}\n"
                      "let bag = Bag()\n"
                      "print(bag.fill(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class Set<int> length/size sum should generate");

    const char *fn = strstr(code, "static int64_t test_fill_");
    assert(fn != NULL && "fill method should use typed ABI");
    fn = strstr(fn + 1, "static int64_t test_fill_");
    assert(fn != NULL && "fill method definition should follow its declaration");
    const char *fn_end = strstr(fn, "static XrValue test_fill_");
    assert(fn_end != NULL && "boxed fill adapter should follow typed method");

    assert(contains(code, "xrt_set_new_typed(0, XR_ELEM_I64)") &&
           "Set<int> class field constructor should use typed int64 set storage");
    assert(count_between(fn, fn_end, "xrt_set_add_i64(") == 1 &&
           "Set<int>.add should use the int64 direct helper");
    assert(count_between(fn, fn_end, "int64_t v") > 0 && count_between(fn, fn_end, "->len") > 0 &&
           "Set<int>.length/size should be materialized as scalar field loads");
    assert(count_between(fn, fn_end, "XR_FROM_INT((p0)->") == 0 &&
           "Set<int>.length/size should not box the native length field");
    assert(count_between(fn, fn_end, "xrt_add(") == 0 &&
           "Set<int>.length + size should use native integer arithmetic");
    assert(count_between(fn, fn_end, "xrt_i64_add(") > 0 &&
           "Set<int>.length + size should emit a native int64 add");

    printf("  Generated class Set<int> scalar length/size fast path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_set_u8_uses_typed_direct_helpers) {
    const char *src = "class Bag {\n"
                      "    values: Set<uint8>\n"
                      "    constructor() {\n"
                      "        this.values = #[]\n"
                      "    }\n"
                      "    fill(n: int) -> int {\n"
                      "        let i = 0\n"
                      "        while (i < n) {\n"
                      "            this.values.add(i)\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return this.values.length\n"
                      "    }\n"
                      "    scan(n: int) -> int {\n"
                      "        let i = 0\n"
                      "        let hits = 0\n"
                      "        while (i < n) {\n"
                      "            if (this.values.has(i)) {\n"
                      "                hits = hits + i\n"
                      "            }\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return hits\n"
                      "    }\n"
                      "    prune(n: int) -> int {\n"
                      "        let i = 0\n"
                      "        let removed = 0\n"
                      "        while (i < n) {\n"
                      "            if (this.values.delete(i)) {\n"
                      "                removed = removed + 1\n"
                      "            }\n"
                      "            i = i + 2\n"
                      "        }\n"
                      "        return removed\n"
                      "    }\n"
                      "}\n"
                      "let bag = Bag()\n"
                      "print(bag.fill(10) + bag.scan(10) + bag.prune(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class Set<uint8> direct helpers should generate");

    const char *fill = strstr(code, "static int64_t test_fill_");
    assert(fill != NULL && "fill method should use typed ABI");
    fill = strstr(fill + 1, "static int64_t test_fill_");
    assert(fill != NULL && "fill method definition should follow its declaration");
    const char *fill_end = strstr(fill, "static XrValue test_fill_");
    assert(fill_end != NULL && "boxed fill adapter should follow typed method");
    const char *scan = strstr(code, "static int64_t test_scan_");
    assert(scan != NULL && "scan method should use typed ABI");
    scan = strstr(scan + 1, "static int64_t test_scan_");
    assert(scan != NULL && "scan method definition should follow its declaration");
    const char *scan_end = strstr(scan, "static XrValue test_scan_");
    assert(scan_end != NULL && "boxed scan adapter should follow typed method");
    const char *prune = strstr(code, "static int64_t test_prune_");
    assert(prune != NULL && "prune method should use typed ABI");
    prune = strstr(prune + 1, "static int64_t test_prune_");
    assert(prune != NULL && "prune method definition should follow its declaration");
    const char *prune_end = strstr(prune, "static XrValue test_prune_");
    assert(prune_end != NULL && "boxed prune adapter should follow typed method");

    assert(contains(code, "xrt_set_new_typed(0, XR_ELEM_U8)") &&
           "Set<uint8> class field constructor should use byte set storage");
    assert(count_between(fill, fill_end, "xrt_set_add_i64_typed(") == 1 &&
           "Set<uint8>.add should use the typed integer direct helper");
    assert(count_between(scan, scan_end, "xrt_set_has_i64_typed(") == 1 &&
           "Set<uint8>.has should use the typed integer direct helper");
    assert(count_between(prune, prune_end, "xrt_set_delete_i64_typed(") == 1 &&
           "Set<uint8>.delete should use the typed integer direct helper");
    assert(contains(code, "XR_ELEM_U8") && "Set<uint8> helper calls should pass XR_ELEM_U8");
    assert(!contains(code, "xrt_set_add(") && !contains(code, "xrt_set_has(") &&
           !contains(code, "xrt_set_delete(") &&
           "Set<uint8> class hot methods should not fall back to tagged set helpers");

    xr_free(code);
    xi_func_free(ir);
}
TEST(cgen_class_map_i64_i64_uses_typed_direct_helpers) {
    const char *src = "class Bag {\n"
                      "    values: Map<int, int>\n"
                      "    constructor() {\n"
                      "        this.values = #{}\n"
                      "    }\n"
                      "    fill(n: int) -> int {\n"
                      "        let i = 0\n"
                      "        while (i < n) {\n"
                      "            this.values.set(i, i * 3 + 1)\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return this.values.size\n"
                      "    }\n"
                      "    scan(n: int) -> int {\n"
                      "        let i = 0\n"
                      "        let hits = 0\n"
                      "        while (i < n) {\n"
                      "            if (this.values.has(i)) {\n"
                      "                hits = hits + this.values.get(i)\n"
                      "            }\n"
                      "            i = i + 1\n"
                      "        }\n"
                      "        return hits\n"
                      "    }\n"
                      "    prune(n: int) -> int {\n"
                      "        let i = 0\n"
                      "        let removed = 0\n"
                      "        while (i < n) {\n"
                      "            if (this.values.delete(i)) {\n"
                      "                removed = removed + 1\n"
                      "            }\n"
                      "            i = i + 2\n"
                      "        }\n"
                      "        return removed\n"
                      "    }\n"
                      "}\n"
                      "let bag = Bag()\n"
                      "print(bag.fill(10) + bag.scan(10) + bag.prune(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class Map<int,int> direct helpers should generate");

    const char *fill = strstr(code, "static int64_t test_fill_");
    assert(fill != NULL && "fill method should use typed ABI");
    fill = strstr(fill + 1, "static int64_t test_fill_");
    assert(fill != NULL && "fill method definition should follow its declaration");
    const char *fill_end = strstr(fill, "static XrValue test_fill_");
    assert(fill_end != NULL && "boxed fill adapter should follow typed method");
    const char *scan = strstr(code, "static int64_t test_scan_");
    assert(scan != NULL && "scan method should use typed ABI");
    scan = strstr(scan + 1, "static int64_t test_scan_");
    assert(scan != NULL && "scan method definition should follow its declaration");
    const char *scan_end = strstr(scan, "static XrValue test_scan_");
    assert(scan_end != NULL && "boxed scan adapter should follow typed method");
    const char *prune = strstr(code, "static int64_t test_prune_");
    assert(prune != NULL && "prune method should use typed ABI");
    prune = strstr(prune + 1, "static int64_t test_prune_");
    assert(prune != NULL && "prune method definition should follow its declaration");
    const char *prune_end = strstr(prune, "static XrValue test_prune_");
    assert(prune_end != NULL && "boxed prune adapter should follow typed method");

    assert(contains(code, "xrt_map_new_typed(0, XR_ELEM_I64, XR_ELEM_I64)") &&
           "Map<int,int> class field constructor should use typed map storage");
    assert(count_between(fill, fill_end, "xrt_map_set_i64_i64_typed(") == 1 &&
           "Map<int,int>.set should use the typed integer direct helper");
    assert(count_between(scan, scan_end, "xrt_map_has_i64_typed(") == 1 &&
           "Map<int,int>.has should use the typed integer direct helper");
    assert(count_between(scan, scan_end, "xrt_map_find_i64_typed(") == 1 &&
           count_between(scan, scan_end, "xrt_map_get_i64_value_typed(") == 1 &&
           "Map<int,int>.get guarded by has should use typed find plus typed value load");
    assert(count_between(scan, scan_end, "\n    XrValue v") == 0 &&
           count_between(scan, scan_end, "XR_FROM_INT(") == 0 &&
           count_between(scan, scan_end, "xrt_add(") == 0 &&
           "Map<int,int>.get guarded by has should avoid tagged result and arithmetic");
    assert(count_between(prune, prune_end, "xrt_map_delete_i64_typed(") == 1 &&
           "Map<int,int>.delete should use the typed integer direct helper");
    assert(count_between(fill, fill_end, "xrt_map_set(") == 0 &&
           count_between(scan, scan_end, "xrt_map_has(") == 0 &&
           count_between(scan, scan_end, "xrt_map_get(") == 0 &&
           count_between(prune, prune_end, "xrt_map_delete(") == 0 &&
           "Map<int,int> class hot methods should not fall back to boxed map helpers");

    xr_free(code);
    xi_func_free(ir);
}
TEST(cgen_class_bool_key_map_uses_specialized_direct_helpers) {
    const char *src =
        "class Bag { values: Map<bool, float32>\n"
        "constructor() { this.values = #{} } fill() -> int { this.values.set(true, 1.5); "
        "this.values.set(false, 2.25); return this.values.size } "
        "scan() -> float { let sum = 0.0; if (this.values.has(true)) { sum = sum + "
        "this.values.get(true) }; if (this.values.has(false)) { sum = sum + this.values.get(false) "
        "}; return sum } prune() -> int { if (this.values.delete(false)) { return "
        "this.values.length }; return 0 }\n"
        "} class IntBag { values: Map<bool, int>\n"
        "constructor() { this.values = #{} } fill() -> int { this.values.set(true, 11); "
        "this.values.set(false, 23); return this.values.size } "
        "scan() -> int { let sum = 0; if (this.values.has(true)) { sum = sum + "
        "this.values.get(true) }; if (this.values.has(false)) { sum = sum + this.values.get(false) "
        "}; return sum } prune() -> int { if (this.values.delete(false)) { return "
        "this.values.length }; return 0 }\n"
        "} let bag = Bag(); print(bag.fill() + bag.prune()); print(bag.scan())\n"
        "let int_bag = IntBag(); print(int_bag.fill() + int_bag.prune()); print(int_bag.scan())\n";
    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "class bool-key Map direct helpers should generate");
    const char *code_end = code + strlen(code);
    assert(contains(code, "xrt_map_new_typed(0, XR_ELEM_BOOL, XR_ELEM_F32)"));
    assert(count_between(code, code_end, "xrt_map_set_bool_f32_typed(") == 2 &&
           count_between(code, code_end, "xrt_map_has_bool_f32_typed(") == 2 &&
           count_between(code, code_end, "xrt_map_delete_bool_f32_typed(") == 1);
    assert(contains(code, "xrt_map_new_typed(0, XR_ELEM_BOOL, XR_ELEM_I64)"));
    assert(count_between(code, code_end, "xrt_map_set_bool_i64_typed(") == 2 &&
           count_between(code, code_end, "xrt_map_has_bool_i64_typed(") == 2 &&
           count_between(code, code_end, "xrt_map_delete_bool_i64_typed(") == 1);
    assert(count_between(code, code_end, "xrt_map_find_i64_typed(") == 4 &&
           count_between(code, code_end, "xrt_map_get_f64_value_typed(") == 2 &&
           count_between(code, code_end, "xrt_map_get_i64_value_typed(") == 2);
    assert(!contains(code, "xrt_map_set_i64_i64_typed(") &&
           !contains(code, "xrt_map_get_i64_i64_typed(") &&
           !contains(code, "xrt_map_set_i64_f64_typed(") &&
           !contains(code, "xrt_map_has_i64_typed(") &&
           !contains(code, "xrt_map_get_i64_f64_typed(") &&
           !contains(code, "xrt_map_delete_i64_typed(") &&
           "bool-key Map class hot methods should not use generic i64 helpers");
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_map_bool_value_guarded_condition_uses_native) {
    const char *src =
        "class Bag { values: Map<int, bool>\n"
        "constructor() { this.values = #{} } "
        "fill(n: int) -> int { let i = 0; while (i < n) { this.values.set(i, i % 3 == 0); "
        "i = i + 1 }; return this.values.size } "
        "count(n: int) -> int { let i = 0; let total = 0; while (i < n) { "
        "if (this.values.has(i)) { if (this.values.get(i)) { total = total + 1 } }; "
        "i = i + 1 }; return total }\n"
        "} let bag = Bag(); print(bag.fill(8)); print(bag.count(8))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    char *code = generate_c(ir, "test");
    assert(code != NULL && "C code generation failed");
    const char *code_end = code + strlen(code);
    const char *count = strstr(code, "static int64_t test_count_");
    assert(count != NULL && "count method should use typed ABI");
    count = strstr(count + 1, "static int64_t test_count_");
    assert(count != NULL && "count method definition should follow its declaration");
    const char *count_end = strstr(count, "static XrValue test_count_");
    assert(count_end != NULL && "boxed count adapter should follow typed method");
    assert(count_between(code, code_end, "xrt_map_find_i64_typed(") >= 1 &&
           count_between(code, code_end, "xrt_map_get_i64_value_typed(") >= 1 &&
           "Map<int,bool>.get guarded by has should keep typed storage");
    assert(count_between(count, count_end, "\n    XrValue v") == 0 &&
           count_between(count, count_end, "XR_FROM_BOOL(") == 0 &&
           count_between(count, count_end, "xr_truthy(") == 0 &&
           "Map<int,bool>.get guarded by has should use a native bool condition");

    printf("  Generated guarded bool map condition %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_map_bool_value_unguarded_condition_uses_truthy) {
    const char *src = "class Bag { values: Map<int, bool>\n"
                      "constructor() { this.values = #{}; this.values.set(1, true) } "
                      "count() -> int { if (this.values.get(1)) { return 1 }; return 0 }\n"
                      "} let bag = Bag(); print(bag.count())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    char *code = generate_c(ir, "test");
    assert(code != NULL && "C code generation failed");
    const char *count = strstr(code, "static int64_t test_count_");
    assert(count != NULL && "count method should use typed ABI");
    count = strstr(count + 1, "static int64_t test_count_");
    assert(count != NULL && "count method definition should follow its declaration");
    const char *count_end = strstr(count, "static XrValue test_count_");
    assert(count_end != NULL && "boxed count adapter should follow typed method");
    assert(count_between(count, count_end, "XrValue ") >= 1 &&
           count_between(count, count_end, "XR_FROM_BOOL(") >= 1 &&
           count_between(count, count_end, "xr_truthy(") >= 1 &&
           "unguarded Map<int,bool>.get must keep nullable tagged truthiness");

    printf("  Generated unguarded bool map condition %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_inherited_class_uses_native_base_layout) {
    const char *src = "class Shape {\n"
                      "    kind: int\n"
                      "    constructor(kind: int) {\n"
                      "        this.kind = kind\n"
                      "    }\n"
                      "    kind_plus() -> int {\n"
                      "        return this.kind + 7\n"
                      "    }\n"
                      "}\n"
                      "class Rect extends Shape {\n"
                      "    w: int\n"
                      "    h: int\n"
                      "    constructor(w: int, h: int) {\n"
                      "        super(1)\n"
                      "        this.w = w\n"
                      "        this.h = h\n"
                      "    }\n"
                      "    area() -> int {\n"
                      "        return this.w * this.h\n"
                      "    }\n"
                      "    kind_plus() -> int {\n"
                      "        return this.kind + this.w\n"
                      "    }\n"
                      "    score_with_area() -> int {\n"
                      "        return this.area() + this.kind\n"
                      "    }\n"
                      "}\n"
                      "let r = Rect(2, 3)\n"
                      "print(r.area())\n"
                      "print(r.kind_plus())\n"
                      "print(r.score_with_area())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "inherited class native layout should generate");

    assert(contains(code, "typedef struct xrt_native_test_Shape { int64_t f0; }") &&
           "base class should emit native scalar layout");
    assert(contains(code, "typedef struct xrt_native_test_Rect { xrt_native_test_Shape base; "
                          "int64_t f1; int64_t f2; }") &&
           "derived class should embed the base layout as a prefix");
    assert(contains(code, "&((p0)->base)") &&
           "super constructor should receive a pointer to the embedded base layout");

    const char *area = strstr(code, "int64_t test_area_");
    assert(area != NULL && "area method should use typed ABI");
    area = strstr(area + 1, "int64_t test_area_");
    assert(area != NULL && "area method definition should follow its declaration");
    const char *area_end = strstr(area, "static XrValue test_area_");
    assert(area_end != NULL && "boxed area adapter should follow typed method");
    assert(contains(area, "xrt_native_test_Rect *p0") &&
           "derived method receiver should use native pointer ABI");
    assert(count_between(area, area_end, "p0->f1") > 0 &&
           count_between(area, area_end, "p0->f2") > 0 &&
           "derived method should read own fields from native layout");
    assert(count_between(area, area_end, "xrt_map_get") == 0 &&
           count_between(area, area_end, "xrt_map_set") == 0 &&
           "derived method body should not cross the map boundary");

    const char *kind = strstr(area_end, "int64_t test_kind_plus_");
    assert(kind != NULL && "derived kind_plus method should follow area adapter");
    const char *kind_end = strstr(kind, "static XrValue test_kind_plus_");
    assert(kind_end != NULL && "boxed kind_plus adapter should follow typed method");
    assert(contains(kind, "xrt_native_test_Rect *p0") &&
           "derived override receiver should use native pointer ABI");
    assert(count_between(kind, kind_end, "p0->base.f0") > 0 &&
           "derived method should read inherited fields through the native base prefix");
    assert(count_between(kind, kind_end, "p0->f1") > 0 &&
           "derived method should still read own fields directly");
    assert(count_between(kind, kind_end, "xrt_map_get") == 0 &&
           count_between(kind, kind_end, "xrt_map_set") == 0 &&
           "inherited field hot path should not cross the map boundary");

    const char *score = strstr(kind_end, "int64_t test_score_with_area_");
    assert(score != NULL && "score_with_area method should follow kind_plus adapter");
    const char *score_end = strstr(score, "static XrValue test_score_with_area_");
    assert(score_end != NULL && "boxed score_with_area adapter should follow typed method");
    assert(count_between(score, score_end, "test_area_") > 0 &&
           "nested native method call should remain a direct C call");
    assert(count_between(score, score_end, "base.f0") > 0 &&
           "nested native method call body should still read inherited fields directly");
    assert(count_between(score, score_end, "xrt_has_pending_error") == 0 &&
           "no-throw nested native method call must not keep a dead error check");

    printf("  Generated inherited class native layout %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_slice_preserves_raw_storage_fast_path) {
    const char *src = "fn sum() -> int {\n"
                      "    let bytes: Array<uint8> = []\n"
                      "    bytes.push(1)\n"
                      "    bytes.push(2)\n"
                      "    bytes.push(3)\n"
                      "    let mid = bytes[1:3]\n"
                      "    return mid[0] + mid.length\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array slice fast path should generate");
    assert(contains(code, "xrt_slice(") &&
           "typed array slice must call the typed-preserving AOT slice helper");
    assert(contains(code, "XR_ELEM_U8") && "Array<uint8> source must keep byte storage");
    assert((contains(code, "((uint8_t*)_a->data)") || contains(code, "uint8_t *_ad")) &&
           "Array<uint8> slice reads must access raw byte storage");
    assert(contains(code, "((xrt_array_t*)") &&
           "Array<uint8> slice length must read the runtime array length directly");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<uint8> slice index read must not fall back to runtime index dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "Array<uint8> slice length must not fall back to dynamic property dispatch");
    assert(!contains(code, "xrt_method_") &&
           "Array<uint8> slice expression must not use dynamic method dispatch");

    printf("  Generated typed array slice fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typeof_as_and_slice_use_direct_drivers) {
    const char *src = "fn run() -> int {\n"
                      "    let arr: Array<int> = [1, 2, 3]\n"
                      "    let s = arr[0:2]\n"
                      "    let label = 42 as string\n"
                      "    if (typeof(s) == \"Array\" && label == \"42\") {\n"
                      "        return s.length\n"
                      "    }\n"
                      "    return 0\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typeof/as/slice direct drivers should generate");

    assert(contains(code, "xrt_typeof_str(") && "typeof() must use the direct AOT typename helper");
    assert(contains(code, "xrt_to_string(") &&
           "unsafe as string must use the direct AOT conversion helper");
    assert(contains(code, "xrt_slice(") && "slice expression must use the direct AOT slice helper");
    assert(!contains(code, "xr_typename(") && !contains(code, "xr_typeof_id(") &&
           "AOT code must not reference stale typeof helper names");

    printf("  Generated typeof/as/slice direct drivers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_range_uses_direct_aot_driver) {
    const char *src = "let r = 2..6\n"
                      "print(r)\n"
                      "print(typeof(r))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "range direct driver should generate");

    assert(contains(code, "xrt_range_from_i64(") &&
           "range expression must use the direct AOT range helper");
    assert(contains(code, "xrt_typeof_str(") && "typeof(range) must use direct typeof helper");
    assert(!contains(code, "xrt_range(XR_FROM_INT") &&
           "range creation must not box start/end before the AOT helper");

    printf("  Generated range direct driver %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_slice_loop_uses_guarded_unchecked_raw_load) {
    const char *src = "fn sum(n: int) -> int {\n"
                      "    let bytes: Array<uint8> = []\n"
                      "    let i = 0\n"
                      "    while (i < n) {\n"
                      "        bytes.push(i)\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    let mid = bytes[1:n - 1]\n"
                      "    let total = 0\n"
                      "    i = 0\n"
                      "    while (i < mid.length) {\n"
                      "        total = total + mid[i]\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return total\n"
                      "}\n"
                      "print(sum(200))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array guarded slice loop should generate");
    const char *code_end = code + strlen(code);
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "guarded Array<uint8> fill loop must preallocate uninitialized typed storage");
    assert(contains(code, "uint8_t *_ad") &&
           "guarded Array<uint8> fill loop must cache raw byte storage");
    assert(count_between(code, code_end, "uint8_t *_ad") >= 2 &&
           "guarded Array<uint8> slice loop must cache source and slice data pointers");
    assert(!contains(code, "_a->len =") &&
           "guarded typed array fill loop must use final len store outside the push body");
    assert(!contains(code, "_a->len >= _a->cap") &&
           "guarded typed array fill loop must not keep per-push capacity checks");
    assert(!contains(code, "XRT_REALLOC(_a->data") &&
           "guarded typed array fill loop must not keep per-push realloc paths");
    assert(!contains(code, "xrt_has_pending_error()) {\n        return 0;") &&
           "guarded typed array fill loop must not keep dead error propagation checks");
    assert(!contains(code, "xrt_index_get(") &&
           "guarded typed array loop must not fall back to runtime index dispatch");
    assert(!contains(code, "if (_idx < 0)") &&
           "guarded typed array loop must not keep negative-index adjustment");
    assert(!contains(code, "_idx >= 0 && _idx < _a->len") &&
           "guarded typed array loop must not keep the bounds ternary");
    assert(!contains(code, "XR_TO_INT(v") && "guarded typed array loop index must stay native");

    printf("  Generated guarded typed array slice loop %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_branchy_fill_loop_uses_preallocated_raw_store) {
    const char *src = "fn sum(n: int) -> float {\n"
                      "    let values: Array<float> = []\n"
                      "    let i = 0\n"
                      "    let x = 1.0\n"
                      "    while (i < n) {\n"
                      "        values.push(x)\n"
                      "        x = x + 0.25\n"
                      "        if (x > 17.0) {\n"
                      "            x = 1.0\n"
                      "        }\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    let total = 0.0\n"
                      "    i = 0\n"
                      "    while (i < values.length) {\n"
                      "        total = total + values[i]\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return total\n"
                      "}\n"
                      "print(sum(200))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array branchy fill loop should generate");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "branchy Array<float> fill loop must preallocate uninitialized typed storage");
    assert(contains(code, "double *_ad") &&
           "branchy Array<float> fill loop must cache raw double storage");
    assert(!contains(code, "_a->len =") &&
           "branchy typed array fill loop must use final len store outside the push body");
    assert(!contains(code, "_a->len >= _a->cap") &&
           "branchy typed array fill loop must not keep per-push capacity checks");
    assert(!contains(code, "XRT_REALLOC(_a->data") &&
           "branchy typed array fill loop must not keep per-push realloc paths");
    assert(!contains(code, "xrt_has_pending_error()) {\n        return 0;") &&
           "branchy typed array fill loop must not keep dead error propagation checks");
    assert(!contains(code, "xr_typed_get(") && !contains(code, "xr_typed_set(") &&
           !contains(code, "xrt_release(") &&
           "branchy typed array fill loop must not use typed runtime switches or no-op release");

    printf("  Generated branchy typed array fill loop %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_filter_preserves_raw_storage_fast_path) {
    const char *src = "fn sum() -> int {\n"
                      "    let bytes: Array<uint8> = []\n"
                      "    bytes.push(1)\n"
                      "    bytes.push(2)\n"
                      "    bytes.push(3)\n"
                      "    let kept = bytes.filter(fn(x: uint8) -> bool { return x > 1 })\n"
                      "    kept.push(9)\n"
                      "    return kept[0] + kept[2] + kept.length\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array filter fast path should generate");
    assert(!contains(code, "xrt_array_filter_typed(") &&
           "pure Array<uint8>.filter callback must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new((void*)test___anonymous__") &&
           "pure inlined Array<uint8>.filter must not allocate a callback closure");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "inlined Array<uint8>.filter must preallocate typed result storage");
    assert(contains(code, "XR_ELEM_U8") &&
           "Array<uint8>.filter result must use the U8 typed element layout");
    assert(contains(code, "uint8_t *_dstd") &&
           "inlined Array<uint8>.filter must write through a typed result pointer");
    assert(contains(code, "uint8_t *_srcd") &&
           "inlined Array<uint8>.filter must read through a typed source pointer");
    assert(contains(code, "test___anonymous__") &&
           "inlined Array<uint8>.filter must call the callback's native function");
    assert((contains(code, "((uint8_t*)_a->data)") || contains(code, "uint8_t *_dstd")) &&
           "Array<uint8> filter result reads and writes must access raw byte storage");
    assert(!contains(code, "xrt_method_1(") &&
           "Array<uint8>.filter must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<uint8> filter result index read must not fall back to runtime index dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "Array<uint8> filter result length must not fall back to dynamic property dispatch");

    printf("  Generated typed array filter fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_map_uses_typed_result_storage_fast_path) {
    const char *src = "fn sum() -> int {\n"
                      "    let values: Array<int> = []\n"
                      "    values.push(1)\n"
                      "    values.push(2)\n"
                      "    values.push(3)\n"
                      "    let mapped = values.map(fn(x: int) -> int { return x + 2 })\n"
                      "    mapped.push(9)\n"
                      "    return mapped[0] + mapped[3] + mapped.length\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array map fast path should generate");
    assert(!contains(code, "xrt_array_map_typed(") &&
           "pure Array<int>.map callback must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new((void*)test___anonymous__") &&
           "pure inlined Array<int>.map must not allocate a callback closure");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "inlined Array<int>.map must preallocate typed result storage");
    assert(contains(code, "XR_ELEM_I64") &&
           "Array<int>.map result must use the I64 typed element layout");
    assert(contains(code, "int64_t *_dstd") &&
           "inlined Array<int>.map must write through a typed result pointer");
    assert(contains(code, "int64_t *_srcd") &&
           "inlined Array<int>.map must read through a typed source pointer");
    assert(contains(code, "test___anonymous__") &&
           "inlined Array<int>.map must call the callback's native function");
    assert((contains(code, "((int64_t*)_a->data)") || contains(code, "int64_t *_dstd")) &&
           "Array<int>.map result reads and writes must access raw int64 storage");
    assert(!contains(code, "xrt_method_1(") &&
           "Array<int>.map must not fall back to dynamic method dispatch");
    assert(!contains(code, "xrt_index_get(") &&
           "Array<int>.map result index read must not fall back to runtime index dispatch");
    assert(!contains(code, "xrt_getprop(") &&
           "Array<int>.map result length must not fall back to dynamic property dispatch");

    printf("  Generated typed array map fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_map_readonly_result_caches_data_pointer) {
    const char *src = "fn sum() -> int {\n"
                      "    let values: Array<int> = []\n"
                      "    values.push(1)\n"
                      "    values.push(2)\n"
                      "    values.push(3)\n"
                      "    let mapped = values.map(fn(x: int) -> int { return x + 2 })\n"
                      "    let i = 0\n"
                      "    let total = 0\n"
                      "    while (i < mapped.length) {\n"
                      "        total += mapped[i]\n"
                      "        i += 1\n"
                      "    }\n"
                      "    return total\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    const char *code_end = code + strlen(code);
    assert(!had_error && "read-only typed array map scan should generate");
    assert(!contains(code, "xrt_array_map_typed(") &&
           "read-only pure Array<int>.map must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new((void*)test___anonymous__") &&
           "read-only pure Array<int>.map must not allocate a callback closure");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "read-only pure Array<int>.map must preallocate typed result storage");
    assert(contains(code, "int64_t *_ad") &&
           "read-only map result must cache the typed data pointer");
    assert(count_between(code, code_end, "int64_t *_ad") >= 1 &&
           "read-only map result should have at least one cached data pointer");
    assert(count_between(code, code_end, "_ad") >= 2 &&
           "read-only map result scan must use the cached data pointer");
    assert(!contains(code, "xrt_index_get(") &&
           "read-only map result scan must not fall back to runtime index dispatch");

    printf("  Generated read-only typed array map scan %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_map_captured_callback_uses_runtime_helper) {
    const char *src = "fn sum() -> int {\n"
                      "    let values: Array<int> = []\n"
                      "    values.push(1)\n"
                      "    values.push(2)\n"
                      "    let offset = 3\n"
                      "    let mapped = values.map(fn(x: int) -> int { return x + offset })\n"
                      "    return mapped[0] + mapped[1]\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "captured typed array map should generate");
    assert(contains(code, "xrt_array_map_typed(") &&
           "captured Array<int>.map callback must keep the closure helper path");
    assert(contains(code, "xrt_closure_new((void*)test___anonymous__") &&
           "captured Array<int>.map callback must still allocate a closure env");
    assert(contains(code, "XR_ELEM_I64") &&
           "captured Array<int>.map result must preserve typed storage");
    assert(!contains(code, "xrt_method_1(") &&
           "captured Array<int>.map must not fall back to dynamic method dispatch");

    printf("  Generated captured typed array map helper path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_filter_readonly_result_caches_data_pointer) {
    const char *src = "fn sum() -> int {\n"
                      "    let bytes: Array<uint8> = []\n"
                      "    bytes.push(1)\n"
                      "    bytes.push(2)\n"
                      "    bytes.push(3)\n"
                      "    let kept = bytes.filter(fn(x: uint8) -> bool { return x > 1 })\n"
                      "    let i = 0\n"
                      "    let total = 0\n"
                      "    while (i < kept.length) {\n"
                      "        total += kept[i]\n"
                      "        i += 1\n"
                      "    }\n"
                      "    return total\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    const char *code_end = code + strlen(code);
    assert(!had_error && "read-only typed array filter scan should generate");
    assert(!contains(code, "xrt_array_filter_typed(") &&
           "read-only pure Array<uint8>.filter must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new((void*)test___anonymous__") &&
           "read-only pure Array<uint8>.filter must not allocate a callback closure");
    assert((contains(code, "xrt_array_new_typed_uninit(") ||
            contains(code, "xrt_array_new_typed_uninit_ptr(")) &&
           "read-only pure Array<uint8>.filter must preallocate typed result storage");
    assert(contains(code, "uint8_t *_ad") &&
           "read-only filter result must cache the typed data pointer");
    assert(count_between(code, code_end, "uint8_t *_ad") >= 1 &&
           "read-only filter result should have at least one cached data pointer");
    assert(count_between(code, code_end, "_ad") >= 2 &&
           "read-only filter result scan must use the cached data pointer");
    assert(!contains(code, "xrt_index_get(") &&
           "read-only filter result scan must not fall back to runtime index dispatch");

    printf("  Generated read-only typed array filter scan %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_filter_captured_callback_uses_runtime_helper) {
    const char *src = "fn sum() -> int {\n"
                      "    let values: Array<int> = []\n"
                      "    values.push(1)\n"
                      "    values.push(4)\n"
                      "    let limit = 2\n"
                      "    let kept = values.filter(fn(x: int) -> bool { return x > limit })\n"
                      "    return kept[0]\n"
                      "}\n"
                      "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "captured typed array filter should generate");
    assert(contains(code, "xrt_array_filter_typed(") &&
           "captured Array<int>.filter callback must keep the closure helper path");
    assert(contains(code, "xrt_closure_new((void*)test___anonymous__") &&
           "captured Array<int>.filter callback must still allocate a closure env");
    assert(contains(code, "XR_ELEM_I64") &&
           "captured Array<int>.filter result must preserve typed storage");
    assert(!contains(code, "xrt_method_1(") &&
           "captured Array<int>.filter must not fall back to dynamic method dispatch");

    printf("  Generated captured typed array filter helper path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_reduce_uses_native_accumulator_fast_path) {
    const char *src =
        "fn sum() -> int {\n"
        "    let values: Array<int> = []\n"
        "    values.push(1)\n"
        "    values.push(2)\n"
        "    values.push(3)\n"
        "    return values.reduce(fn(acc: int, x: int) -> int { return acc + x }, 0)\n"
        "}\n"
        "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "typed array reduce fast path should generate");
    assert(!contains(code, "xrt_array_reduce_typed(") &&
           "pure Array<int>.reduce callback must inline instead of using boxed runtime helper");
    assert(!contains(code, "xrt_closure_new((void*)test___anonymous__") &&
           "pure inlined Array<int>.reduce must not allocate a callback closure");
    assert(contains(code, "int64_t _acc") &&
           "inlined Array<int>.reduce must use a native accumulator");
    assert(contains(code, "int64_t *_srcd") &&
           "inlined Array<int>.reduce must read through a typed source pointer");
    assert(contains(code, "test___anonymous__") &&
           "inlined Array<int>.reduce must call the callback's native function");
    assert(!contains(code, "xrt_method_2(") &&
           "Array<int>.reduce must not fall back to dynamic method dispatch");

    printf("  Generated typed array reduce fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_typed_array_reduce_captured_callback_uses_runtime_helper) {
    const char *src =
        "fn sum() -> int {\n"
        "    let values: Array<int> = []\n"
        "    values.push(1)\n"
        "    values.push(2)\n"
        "    let offset = 3\n"
        "    return values.reduce(fn(acc: int, x: int) -> int { return acc + x + offset }, 0)\n"
        "}\n"
        "print(sum())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "captured typed array reduce should generate");
    assert(contains(code, "xrt_array_reduce_typed(") &&
           "captured Array<int>.reduce callback must keep the closure helper path");
    assert(contains(code, "xrt_closure_new((void*)test___anonymous__") &&
           "captured Array<int>.reduce callback must still allocate a closure env");
    assert(!contains(code, "xrt_method_2(") &&
           "captured Array<int>.reduce must not fall back to dynamic method dispatch");

    printf("  Generated captured typed array reduce helper path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_int_const_div_mod_uses_native_ops) {
    const char *src = "fn fast(n: int) -> int {\n"
                      "    return (n / 5) + (n % 7)\n"
                      "}\n"
                      "fn checked(n: int, d: int) -> int {\n"
                      "    return n % d\n"
                      "}\n"
                      "print(fast(42))\n"
                      "print(checked(42, 6))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "integer div/mod fast path should generate");

    const char *code_end = code + strlen(code);
    assert(contains(code, "static int64_t test_fast_") &&
           contains(code, "static int64_t test_checked_") &&
           "both test functions should be generated");
    assert(count_between(code, code_end, "xrt_int_div(") == 0 &&
           "constant non-zero integer division must use native /");
    assert(count_between(code, code_end, "xrt_int_mod(") == 1 &&
           "constant non-zero integer modulo must use native %");
    assert(count_between(code, code_end, " / ") >= 1 &&
           "constant non-zero integer division should emit C /");
    assert(count_between(code, code_end, " % ") >= 1 &&
           "constant non-zero integer modulo should emit C %");

    printf("  Generated integer div/mod fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unsupported_coroutine_ops_fail_fast) {
    static const struct {
        XiOp op;
        const char *name;
    } cases[] = {
        {XI_GO, "GO"},
        {XI_AWAIT, "AWAIT"},
        {XI_CHAN_SEND, "CHAN_SEND"},
        {XI_CHAN_RECV, "CHAN_RECV"},
        {XI_CHAN_TRY_SEND, "CHAN_TRY_SEND"},
        {XI_CHAN_TRY_RECV, "CHAN_TRY_RECV"},
        {XI_CHAN_IS_CLOSED, "CHAN_IS_CLOSED"},
        {XI_SELECT_BLOCK, "SELECT_BLOCK"},
        {XI_TIME_AFTER, "TIME_AFTER"},
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
    size_t abort_count = count_between(code, code + strlen(code),
                                       "return (abort(), xr_aot_error(XR_NULL_VAL, false));");
    assert(abort_count == sizeof(cases) / sizeof(cases[0]) &&
           "each unsupported coroutine op must emit an abort expression");
    assert(!contains(code, "XR_NULL_VAL /* ERROR: unsupported coroutine Xi op") &&
           "unsupported coroutine ops must not emit silent null placeholders");
    assert(!contains(code, "XR_NULL_VAL /* ERROR: unsupported AOT coroutine Xi op") &&
           "unsupported AOT coroutine ops must not emit silent null placeholders");
    assert(!contains(code, "unsupported coroutine Xi op") &&
           "unsupported coroutine diagnostics should not be emitted into generated C");
    printf("  Generated rejected %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unresolved_import_fails_fast) {
    XrType stub_unit = {.kind = XR_KIND_UNIT, .id = 102, .frozen = true};
    XrType stub_string = {.kind = XR_KIND_STRING, .id = 103, .frozen = true};
    XiFunc *ir = xi_func_new("main", &stub_unit);
    assert(ir != NULL);

    XiBlock *entry = xi_block_new(ir);
    assert(entry != NULL);

    XiImportRef *ref = (XiImportRef *) xi_func_arena_alloc(ir, sizeof(XiImportRef));
    assert(ref != NULL);
    ref->module_path = "./missing";
    ref->member_name = "value";
    ref->resolved_mod_index = -1;
    ref->resolved_shared_slot = -1;

    XiValue *import = xi_value_new(ir, entry, XI_IMPORT_REF, &stub_string, 0);
    assert(import != NULL);
    import->aux = ref;
    import->aux_int = -1;
    xi_block_set_return(entry, NULL);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL);

    assert(had_error && "unresolved AOT imports must reject code generation");
    assert(contains(code, "(abort(), XR_NULL_VAL)") &&
           "unresolved imports must emit an abort expression");
    assert(!contains(code, "unresolved import:") &&
           "unresolved import diagnostics should not be emitted as C comments");
    assert(!contains(code, "XR_NULL_VAL /* unresolved") &&
           "unresolved imports must not emit silent null placeholders");

    printf("  Generated rejected unresolved import %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_unknown_method_symbol_fails_fast) {
    XrType stub_unit = {.kind = XR_KIND_UNIT, .id = 104, .frozen = true};
    XrType stub_string = {.kind = XR_KIND_STRING, .id = 105, .frozen = true};
    XiFunc *ir = xi_func_new("main", &stub_unit);
    assert(ir != NULL);

    XiBlock *entry = xi_block_new(ir);
    assert(entry != NULL);

    XiValue *recv = xi_const_str(ir, entry, "abc", &stub_string);
    assert(recv != NULL);

    XiValue *call = xi_value_new(ir, entry, XI_CALL_METHOD, &stub_string, 1);
    assert(call != NULL);
    call->args[0] = recv;
    call->aux = (void *) "__aot_unknown_method__";
    call->flags |= XI_FLAG_CALL_EFFECTS;
    xi_block_set_return(entry, NULL);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL);

    assert(had_error && "unknown AOT method symbols must reject code generation");
    assert(contains(code, "(abort(), XR_NULL_VAL)") &&
           "unknown methods must emit an abort expression");
    assert(!contains(code, "xrt_method_0(") &&
           "unknown methods must not fall back to method symbol zero");
    assert(!contains(code, "__aot_unknown_method__") &&
           "unknown method diagnostics should stay out of generated C comments");

    printf("  Generated rejected unknown method %zu bytes of C code\n", strlen(code));
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

TEST(cgen_direct_suspend_call_propagates_cps) {
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
    assert(!had_error && "static direct suspend calls should CPS-propagate to the caller");
    assert(contains(code, "call_frame_") &&
           "caller frame should own the child suspend frame while blocked");
    assert(contains(code, "_aot_resume") && "direct suspend call should use AOT resume entries");
    assert(nonzero_state_precedes_call(code, "_aot_resume(f->call_frame_") &&
           "direct suspend calls must publish caller state before child resume");
    assert(contains(code, "return (abort(), XR_NULL_VAL);") &&
           "suspendable functions must keep hard-failing sync wrappers");
    assert(!contains(code, "unsupported AOT sync call") &&
           "diagnostics should go to stderr, not generated C comments");

    printf("  Generated direct suspend call %zu bytes of C code\n", strlen(code));
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
    TestAotPlan plan;
    test_aot_plan_prepare(&plan, modules, 2, 1);

    XiCgenCtx *ctx = xi_cgen_ctx_new();
    assert(ctx != NULL);
    xi_cgen_ctx_set_aot_bundle(ctx, &plan.bundle);

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
    test_aot_plan_free(&plan);
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
                      "fn identity_copy(xs: Array<int>) -> Array<int> {\n"
                      "    return xs\n"
                      "}\n"
                      "let high = go(name: \"compute\") compute(5)\n"
                      "print(await high)\n"
                      "let xs = [1, 2]\n"
                      "let copied = go mutate_copy(xs)\n"
                      "print(await copied)\n"
                      "print(xs.length)\n"
                      "let roundtrip = go identity_copy(xs)\n"
                      "let ys = await roundtrip\n"
                      "print(ys.length)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT go of sync functions should generate");
    assert(contains(code, "int64_t _raw_result = test_compute_") &&
           "sync go wrapper must call typed normal function bodies");
    assert(contains(code, "int64_t _raw_result = test_mutate_copy_") &&
           "sync go wrapper must pass tagged params to typed normal functions");
    assert(contains(code, "XrValue _raw_result = test_identity_copy_") &&
           "sync go wrapper must support tagged results that alias frame params");
    assert(contains(code, "XrValue _result = XR_FROM_INT(_raw_result)") &&
           "sync go wrapper must box native scalar results for the coroutine ABI");
    assert(contains(code, "xrt_retain(_result)") &&
           "sync go wrapper must retain a result that aliases an owned frame param");
    assert(contains(code, "xrt_release(f->p0)") &&
           "sync go wrapper must release cloned tagged frame params");
    assert(contains(code, ".release_count = 1,") &&
           "sync go wrapper descriptor must report cloned tagged param releases");
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

TEST(cgen_coro_go_sync_scalar_wrapper_skips_param_roots) {
    const char *src = "fn compute(n: int, flag: bool) -> int {\n"
                      "    if (flag) { return n + 1 }\n"
                      "    return n\n"
                      "}\n"
                      "let task = go compute(3, true)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT go of scalar sync functions should generate");

    const char *frame = strstr(code, "typedef struct test_compute_");
    assert(frame != NULL && "compute sync-go frame should be emitted");
    const char *frame_end = strstr(frame, "} test_compute_");
    assert(frame_end != NULL && "compute sync-go frame should close");
    assert(count_between(frame, frame_end, "int64_t p") == 2 &&
           "scalar sync-go params should be stored unboxed in the frame");
    assert(count_between(frame, frame_end, "XrValue p") == 0 &&
           "scalar sync-go params should not use tagged frame storage");
    assert(!contains(code, "xr_aot_trace_frame_value(visitor, f->p0)") &&
           "scalar sync-go params must not be traced as roots");
    assert(!contains(code, "xr_aot_trace_frame_value(visitor, f->p1)") &&
           "scalar sync-go params must not be traced as roots");
    assert(contains(code, ".root_count = 0,") &&
           "scalar sync-go wrapper should report zero root slots");
    assert(contains(code, ".release_count = 0,") &&
           "scalar sync-go wrapper should report zero release slots");

    printf("  Generated scalar sync-go wrapper %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_sync_functions_without_go_emit_no_aot_wrappers) {
    const char *src = "fn helper(n: int) -> int {\n"
                      "    return n + 1\n"
                      "}\n"
                      "print(helper(2))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    XiCgenCoroFrameStats stats = {0};
    char *code = generate_c_with_status_and_stats(ir, "test", &had_error, &stats);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT sync-only program should generate");
    assert(stats.coroutine_count == 0 &&
           "sync-only programs should not report coroutine frame stats");
    assert(!contains(code, "_aot_desc") && "unused sync functions should not emit AOT descs");
    assert(!contains(code, "typedef struct test_helper_") &&
           "unused sync functions should not emit sync-go frame structs");

    printf("  Generated sync-only program without wrappers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_sync_go_wrappers_only_for_go_targets) {
    const char *src = "fn unused(n: int) -> int {\n"
                      "    return n + 1\n"
                      "}\n"
                      "fn used(n: int) -> int {\n"
                      "    return n * 2\n"
                      "}\n"
                      "let task = go used(3)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    XiCgenCoroFrameStats stats = {0};
    char *code = generate_c_with_status_and_stats(ir, "test", &had_error, &stats);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT go of sync function should generate");
    assert(stats.coroutine_count == 2 &&
           "frame stats should count the main coroutine and the sync-go frame");
    assert(stats.total_frame_bytes >= stats.max_frame_bytes &&
           "frame stats should preserve aggregate frame bytes");
    assert(contains(code, "int64_t _raw_result = test_used_") &&
           "go target should keep its sync-go wrapper");
    assert(!contains(code, "_raw_result = test_unused_") &&
           "non-go sync functions must not emit sync-go wrappers");
    assert(!contains(code, "typedef struct test_unused_") &&
           "non-go sync functions must not emit sync-go frame structs");

    printf("  Generated only needed sync-go wrappers %zu bytes of C code\n", strlen(code));
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
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_send(ctx,") &&
           "channel send must publish the AOT resume state before runtime blocking");
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
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_send_i64(ctx,") &&
           "scalar channel send must publish the AOT resume state before runtime blocking");
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
    assert(nonzero_state_precedes_call(code, "xr_aot_await_task(ctx,") &&
           "await must publish the AOT resume state before runtime blocking");
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

TEST(cgen_coro_scalar_await_uses_tagged_slot) {
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
           "await Task<int> should reserve a native scalar frame slot");

    const char *resume = strstr(code, "static XrAotResult test_main_plus_");
    const char *trace = resume ? strstr(resume, "static void test_main_plus_") : NULL;
    assert(resume != NULL && trace != NULL && "main_plus resume function should be emitted");
    assert(count_between(resume, trace, "xr_aot_await_task(ctx,") == 1 &&
           "initial scalar await should use the slot bridge");
    assert(count_between(resume, trace, "xr_aot_await_task_resume(ctx,") == 1 &&
           "resumed scalar await should use the slot bridge");
    assert(count_between(resume, trace, "xr_slot_aot_frame_offset") >= 2 &&
           "scalar await should pass native AOT frame slots on start and resume");
    assert(count_between(resume, trace, "xr_slot_xvalue_ptr(&") == 0 &&
           "scalar await should not pass tagged result slots");

    printf("  Generated scalar await slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_await_timeout_passes_deadline) {
    const char *src = "fn worker(ch: Channel<int>) -> int {\n"
                      "    match (ch.recv()) {\n"
                      "        Recv.Value(value) -> { return value }\n"
                      "        _ -> { return -1 }\n"
                      "    }\n"
                      "}\n"
                      "let ch = new Channel<int>(0)\n"
                      "let task = go worker(ch)\n"
                      "let result = task.awaitTimeout(25)\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT task awaitTimeout should generate");
    const char *await_call = strstr(code, "xr_aot_task_await_result(ctx,");
    assert(await_call != NULL && "task awaitTimeout must use the AOT TaskResult bridge");
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

TEST(cgen_tagged_null_equality_keeps_null_literal) {
    const char *src = "fn maybe() -> int? {\n"
                      "    return null\n"
                      "}\n"
                      "\n"
                      "let task = go maybe()\n"
                      "let picked = task\n"
                      "let result = await picked\n"
                      "print(result == null)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT null equality should generate");
    const char *eq = strstr(code, "xrt_eq(");
    assert(eq != NULL && "tagged await result should use tagged equality");
    const char *eq_end = strchr(eq, ')');
    assert(eq_end != NULL && "tagged equality call should be closed");
    assert(count_between(eq, eq_end, "XR_NULL_VAL") == 1 &&
           "tagged null equality must compare against XR_NULL_VAL");
    assert(count_between(eq, eq_end, "XR_FROM_INT") == 0 &&
           "tagged null equality must not turn null into int zero");

    printf("  Generated tagged null equality %zu bytes of C code\n", strlen(code));
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
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_recv_slot(ctx,") &&
           "channel recv must publish the AOT resume state before runtime blocking");
    assert(contains(code, "xr_aot_chan_recv_slot_resume(ctx, xr_slot_none(), true);") &&
           "channel recv resume must recover the slot from coroutine wait state and store Recv");
    assert(!contains(code, "xr_aot_chan_recv_slot_resume(ctx, _chan_recv_slot_") &&
           "channel recv resume must not depend on a local slot variable");

    printf("  Generated channel recv wait-state slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scalar_channel_recv_uses_tagged_slot) {
    const char *src = "fn recv_plus(ch: Channel<int>) -> int {\n"
                      "    let v = ch.recv()\n"
                      "    return match (v) {\n"
                      "        Recv.Value(n) -> n + 1\n"
                      "        _ -> -1\n"
                      "    }\n"
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
    assert(count_between(frame, frame_end, "XrValue v") >= 1 &&
           "channel recv should reserve a tagged frame slot for Recv<T>");

    const char *slot_ref = strstr(code, "xr_slot_aot_frame_offset");
    assert(slot_ref != NULL && "channel recv must create a backend-neutral slot ref");
    assert(strstr(slot_ref, "), 3);\n    f->state = ") != NULL &&
           strstr(slot_ref, "XrAotResult _chan_recv_") != NULL &&
           "channel recv slot should use XR_REP_TAGGED before publishing resume state");

    const char *recv_call = strstr(code, "xr_aot_chan_recv_slot(ctx,");
    assert(recv_call != NULL && "channel recv must use the AOT recv slot bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_recv_slot(ctx,") &&
           "scalar channel recv must publish the AOT resume state before runtime blocking");

    const char *resume = strstr(code, "static XrAotResult test_recv_plus_");
    const char *trace = resume ? strstr(resume, "static void test_recv_plus_") : NULL;
    assert(resume != NULL && trace != NULL && "recv_plus resume function should be emitted");
    printf("  Generated scalar channel recv slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_channel_recv_null_check_keeps_tagged_slot) {
    const char *src = "fn read_or_stop(ch: Channel<int>) {\n"
                      "    let v = ch.recv()\n"
                      "    match (v) {\n"
                      "        Recv.Value(n) -> { print(n) }\n"
                      "        Recv.Closed -> { print(\"closed\") }\n"
                      "        _ -> { print(\"other\") }\n"
                      "    }\n"
                      "}\n"
                      "let ch = new Channel<int>(1)\n"
                      "ch.send(0)\n"
                      "let task = go read_or_stop(ch)\n"
                      "await task\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT Recv channel recv should generate");

    const char *frame = strstr(code, "typedef struct test_read_or_stop_");
    assert(frame != NULL && "read_or_stop coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_read_or_stop_");
    assert(frame_end != NULL && "read_or_stop coroutine frame should close");
    assert(count_between(frame, frame_end, "XrValue v") >= 1 &&
           "Recv result should keep a tagged frame slot");

    const char *slot_ref = strstr(code, "xr_slot_aot_frame_offset");
    assert(slot_ref != NULL && "channel recv must create a backend-neutral slot ref");
    assert(strstr(slot_ref, "), 3);\n    f->state = ") != NULL &&
           strstr(slot_ref, "XrAotResult _chan_recv_") != NULL &&
           "channel recv result slot should use XR_REP_TAGGED");

    printf("  Generated Recv channel recv slot %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scalar_channel_try_recv_returns_recv_enum) {
    const char *src = "let ch = new Channel<int>(1)\n"
                      "let recv = ch.tryRecv()\n"
                      "print(recv)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT channel tryRecv should generate");
    assert(contains(code, "xr_aot_chan_try_recv(ctx,") &&
           "tryRecv must use the Recv<T> enum bridge");
    assert(!contains(code, "xr_aot_chan_try_recv_slot(ctx,") &&
           "tryRecv must not use the old typed slot bridge");
    assert(!contains(code, "_chan_try_ok_") && "tryRecv must not expose an ok bit");

    printf("  Generated channel tryRecv Recv enum %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_select_try_recv_uses_ready_bit) {
    const char *src = "let ch = new Channel<int>(0)\n"
                      "select {\n"
                      "    value from ch -> { print(value) }\n"
                      "    _ -> { print(0) }\n"
                      "}\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT select tryRecv should generate");
    assert(contains(code, "xr_aot_chan_try_recv(ctx,") &&
           "select recv probe must use the AOT Recv enum bridge");
    assert(contains(code, " = xr_aot_recv_is_value(") &&
           "select readiness must use the positive Recv.Value status projection");

    printf("  Generated select tryRecv Recv.Value readiness %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_sleep_publishes_state_before_block) {
    const char *src = "import time\n"
                      "time.sleep(5)\n"
                      "print(\"awake\")\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT sleep should generate");
    assert(contains(code, "xr_aot_sleep(ctx,") && "sleep must use the AOT sleep bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_sleep(ctx,") &&
           "sleep must publish the AOT resume state before runtime blocking");

    printf("  Generated sleep state publication %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_select_publishes_state_before_block) {
    const char *src = "let ch = new Channel<int>(0)\n"
                      "select {\n"
                      "    value from ch -> { print(value) }\n"
                      "}\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT select should generate");
    assert(contains(code, "xr_aot_select_block(ctx,") && "select must use the AOT bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_select_block(ctx,") &&
           "select must publish the AOT resume state before runtime blocking");

    printf("  Generated select state publication %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_channel_timeout_publishes_state_before_block) {
    const char *src = "let ch = new Channel<int>(0)\n"
                      "let sent = ch.sendTimeout(7, 10)\n"
                      "let recv = ch.recvTimeout(10)\n"
                      "print(sent)\n"
                      "print(recv)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT channel timeout ops should generate");
    assert(contains(code, "xr_aot_chan_send_timeout") &&
           "sendTimeout must use the AOT timeout bridge");
    assert(contains(code, "xr_aot_chan_recv_slot(ctx,") &&
           "recvTimeout must use the AOT recv slot bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_send_timeout") &&
           "sendTimeout must publish the AOT resume state before runtime blocking");
    assert(nonzero_state_precedes_call(code, "xr_aot_chan_recv_slot(ctx,") &&
           "recvTimeout must publish the AOT resume state before runtime blocking");

    printf("  Generated channel timeout state publication %zu bytes of C code\n", strlen(code));
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
    assert(nonzero_state_precedes_call(code, "xr_aot_await_all_tasks(ctx,") &&
           "await all must publish the AOT resume state before runtime blocking");
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
    assert(nonzero_state_precedes_call(code, "xr_aot_await_any_task(ctx,") &&
           "await any must publish the AOT resume state before runtime blocking");
    assert(contains(code, "xr_aot_await_any_task_resume(ctx,") &&
           "await any resume must use the aggregate AOT bridge");
    assert(contains(code, "xr_slot_aot_frame_offset") &&
           "scalar await any results should use a typed frame slot");

    printf("  Generated await any aggregate bridge %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_scope_exit_publishes_state_before_block) {
    const char *src = "fn child(ch: Channel<int>) {\n"
                      "    ch.recv()\n"
                      "}\n"
                      "fn scoped() {\n"
                      "    let ch = new Channel<int>(0)\n"
                      "    scope {\n"
                      "        go child(ch)\n"
                      "    }\n"
                      "}\n"
                      "scoped()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT scope exit should generate");
    assert(contains(code, "xr_aot_scope_exit(ctx,") && "scope exit must use the AOT bridge");
    assert(nonzero_state_precedes_call(code, "xr_aot_scope_exit(ctx,") &&
           "scope exit must publish the AOT resume state before runtime blocking");

    printf("  Generated scope exit state publication %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_channel_fields_use_aot_helpers) {
    const char *src = "let ch = new Channel<int>(2)\n"
                      "print(ch.length)\n"
                      "print(ch.capacity)\n"
                      "print(ch.isClosed)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT Channel field reads should generate");
    assert(contains(code, "xr_aot_chan_length(") &&
           "Channel.length must read through the AOT channel helper");
    assert(contains(code, "xr_aot_chan_capacity(") &&
           "Channel.capacity must read through the AOT channel helper");
    assert(contains(code, "xr_aot_chan_is_closed(") &&
           "Channel.isClosed must read through the AOT channel helper");
    assert(!contains(code, "xrt_map_get((xrt_map_t*)") &&
           "AOT Channel fields must not fall back to map property dispatch");

    printf("  Generated AOT channel field helpers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_task_status_uses_task_bridge) {
    const char *src = "fn wait_for_value(ch: Channel<int>) -> int {\n"
                      "    match (ch.recv()) {\n"
                      "        Recv.Value(value) -> { return value }\n"
                      "        _ -> { return -1 }\n"
                      "    }\n"
                      "}\n"
                      "fn quick_value(n: int) -> int {\n"
                      "    yield\n"
                      "    return n * 2\n"
                      "}\n"
                      "fn task_done(task: Task<int>) -> bool {\n"
                      "    return task.done\n"
                      "}\n"
                      "fn task_status(task: Task<int>) -> TaskStatus {\n"
                      "    return task.status\n"
                      "}\n"
                      "fn task_poll(task: Task<int>) -> TaskResult<int> {\n"
                      "    return task.poll()\n"
                      "}\n"
                      "const ch = new Channel<int>(0)\n"
                      "let blocked = go wait_for_value(ch)\n"
                      "blocked.cancel()\n"
                      "let cancelled_result = blocked.awaitResult()\n"
                      "print(blocked.done)\n"
                      "print(blocked.status)\n"
                      "print(cancelled_result)\n"
                      "let quick = go quick_value(21)\n"
                      "let quick_result = await quick\n"
                      "print(quick.done)\n"
                      "print(quick.status)\n"
                      "print(quick.poll())\n"
                      "print(quick.awaitResult())\n"
                      "print(quick_result)\n"
                      "print(task_done(quick))\n"
                      "print(task_status(quick))\n"
                      "print(task_poll(quick))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT Task status access should generate");
    assert(contains(code, "xr_aot_task_cancel(ctx,") && "Task.cancel must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_done(ctx,") && "Task.done must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_status(ctx,") && "Task.status must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_poll(ctx,") && "Task.poll must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_await_result(ctx,") &&
           "Task.awaitResult must use the AOT TaskResult bridge");
    assert(contains(code, "xr_aot_task_done(NULL,") &&
           "sync AOT Task.done must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_status(NULL,") &&
           "sync AOT Task.status must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_poll(NULL,") &&
           "sync AOT Task.poll must use the AOT Task bridge");
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
    run_cgen_stats_tracks_native_abi();
    run_cgen_module_prefix_is_c_identifier();
    run_cgen_recursive();
    run_cgen_for_loop();
    run_cgen_typed_array_uses_raw_storage_fast_path();
    run_cgen_typed_array_u8_uses_byte_storage_fast_path();
    run_cgen_bytes_methods_use_raw_memory_helpers();
    run_cgen_typed_array_i16_and_u32_use_raw_storage_fast_path();
    run_cgen_typed_array_float_and_bool_use_raw_storage_fast_path();
    run_cgen_inlined_struct_uses_native_field_storage();
    run_cgen_escaping_struct_uses_heap_native_storage();
    run_cgen_escaping_struct_string_field_uses_heap_native_storage();
    run_cgen_nested_struct_field_uses_embedded_heap_native_storage();
    run_cgen_fixed_array_struct_field_uses_embedded_heap_native_storage();
    run_cgen_shared_struct_alias_elides_tagged_hot_locals();
    run_cgen_class_method_caches_receiver_scalar_fields();
    run_cgen_class_set_length_size_sum_uses_native_arithmetic();
    run_cgen_class_set_u8_uses_typed_direct_helpers();
    run_cgen_class_map_i64_i64_uses_typed_direct_helpers();
    run_cgen_class_bool_key_map_uses_specialized_direct_helpers();
    run_cgen_class_map_bool_value_guarded_condition_uses_native();
    run_cgen_class_map_bool_value_unguarded_condition_uses_truthy();
    run_cgen_inherited_class_uses_native_base_layout();
    run_cgen_typed_array_slice_preserves_raw_storage_fast_path();
    run_cgen_typeof_as_and_slice_use_direct_drivers();
    run_cgen_range_uses_direct_aot_driver();
    run_cgen_typed_array_slice_loop_uses_guarded_unchecked_raw_load();
    run_cgen_typed_array_branchy_fill_loop_uses_preallocated_raw_store();
    run_cgen_typed_array_filter_preserves_raw_storage_fast_path();
    run_cgen_typed_array_map_uses_typed_result_storage_fast_path();
    run_cgen_typed_array_map_readonly_result_caches_data_pointer();
    run_cgen_typed_array_map_captured_callback_uses_runtime_helper();
    run_cgen_typed_array_filter_readonly_result_caches_data_pointer();
    run_cgen_typed_array_filter_captured_callback_uses_runtime_helper();
    run_cgen_typed_array_reduce_uses_native_accumulator_fast_path();
    run_cgen_typed_array_reduce_captured_callback_uses_runtime_helper();
    run_cgen_int_const_div_mod_uses_native_ops();
    run_cgen_unsupported_coroutine_ops_fail_fast();
    run_cgen_unresolved_import_fails_fast();
    run_cgen_unknown_method_symbol_fails_fast();
    run_cgen_suspendable_wrapper_aborts();
    run_cgen_direct_suspend_call_propagates_cps();
    run_cgen_suspendable_dependency_init_fails_fast();
    run_cgen_coro_frame_params_use_typed_storage();
    run_cgen_coro_frame_skips_dead_ssa_slots();
    run_cgen_runtime_managed_types_skip_arc();
    run_cgen_coro_frame_release_uses_aot_arc();
    run_cgen_coro_go_clones_tagged_args();
    run_cgen_coro_go_sync_function_uses_wrapper_desc();
    run_cgen_coro_go_sync_scalar_wrapper_skips_param_roots();
    run_cgen_sync_functions_without_go_emit_no_aot_wrappers();
    run_cgen_coro_sync_go_wrappers_only_for_go_targets();
    run_cgen_coro_channel_send_clones_value();
    run_cgen_coro_scalar_channel_send_skips_clone();
    run_cgen_coro_scalar_channel_try_send_uses_typed_bridge();
    run_cgen_coro_await_clones_tagged_result();
    run_cgen_coro_scalar_await_uses_tagged_slot();
    run_cgen_coro_await_timeout_passes_deadline();
    run_cgen_tagged_null_equality_keeps_null_literal();
    run_cgen_coro_recv_resume_uses_wait_state_slot();
    run_cgen_coro_scalar_channel_recv_uses_tagged_slot();
    run_cgen_coro_channel_recv_null_check_keeps_tagged_slot();
    run_cgen_coro_scalar_channel_try_recv_returns_recv_enum();
    run_cgen_coro_select_try_recv_uses_ready_bit();
    run_cgen_coro_sleep_publishes_state_before_block();
    run_cgen_coro_select_publishes_state_before_block();
    run_cgen_coro_channel_timeout_publishes_state_before_block();
    run_cgen_coro_recv_slot_is_traced_as_frame_root();
    run_cgen_coro_await_all_uses_aggregate_bridge();
    run_cgen_coro_await_any_uses_typed_aggregate_bridge();
    run_cgen_coro_scope_exit_publishes_state_before_block();
    run_cgen_channel_fields_use_aot_helpers();
    run_cgen_coro_task_status_uses_task_bridge();

    teardown();

    printf("\n=== %d/%d Xi CGen tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
