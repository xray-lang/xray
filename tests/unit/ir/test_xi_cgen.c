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
#include "../../../src/ir/xi_arc.h"
#include "../../../src/ir/xi_escape.h"
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

    AstNode *program = xr_parse(xr_compiler_session_current_for_isolate(g_iso), source);
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

static unsigned max_cell_ref_id(const char *code) {
    unsigned max_id = 0;
    bool found = false;
    const char *p = code;
    while ((p = strstr(p, "cell_")) != NULL) {
        char *end = NULL;
        unsigned long id = strtoul(p + 5, &end, 10);
        if (end != p + 5) {
            if (!found || id > max_id)
                max_id = (unsigned) id;
            found = true;
            p = end;
        } else {
            p += 5;
        }
    }
    return found ? max_id : 0;
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

static const char *next_static_after(const char *fn) {
    assert(fn != NULL);
    const char *next = strstr(fn + 1, "\nstatic ");
    assert(next != NULL && "generated function should be followed by another static declaration");
    return next;
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

    assert(contains(code, "printf(\"%lld\", (long long)") &&
           "native int print should use direct printf");
    assert(!contains(code, "xrt_println(") && !contains(code, "xrt_print(") &&
           "native int print should not call the generic tagged printer");
    assert(!contains(code, "XR_FROM_INT(v") &&
           "print-only int boxes should be elided from generated C");
    /* Should have a main function that accepts script arguments. */
    assert(contains(code, "int main(int argc, char **argv)") && "should have main()");
    /* Should include xrt.h */
    assert(contains(code, "#include \"xrt.h\"") && "should include xrt.h");

    printf("  Generated %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_skips_unused_process_builtin_init) {
    const char *src = "print(1 + 2)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "simple AOT program should generate");
    assert(!contains(code, "xrt_process_new(") &&
           "AOT entry must not construct process.args when process is unused");
    assert(!contains(code, "xrt_builtins[5] =") &&
           "unused process builtin slot must not be initialized");

    printf("  Generated process-free entry %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_initializes_used_process_builtin) {
    const char *src = "print(process.args.length)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "process.args AOT program should generate");
    assert(contains(code, "xrt_builtins[5] = xrt_process_new(") &&
           "used process builtin slot must be initialized");
    assert(contains(code, "xrt_process_new(\"test.xr\",") &&
           "process builtin must receive the entry source path");
    assert(contains(code, "argc > 1 ? argc - 1 : 0") &&
           "process.args must still receive script arguments");

    printf("  Generated process-aware entry %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_initializes_file_dir_builtins_from_entry_source) {
    const char *src = "print(__file__)\n"
                      "print(__dir__)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    assert(ir->module != NULL && "lowering should attach module metadata");
    ir->module->path = "/tmp/xray/main.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "main", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "__file__/__dir__ AOT program should generate");
    assert(contains(code, "xrt_builtins[6] = xr_box_str(\"/tmp/xray/main.xr\")") &&
           "__file__ must be initialized from the entry source path");
    assert(contains(code, "xrt_builtins[7] = xr_box_str(\"/tmp/xray\")") &&
           "__dir__ must be initialized from the entry source directory");
    assert(!contains(code, "xray_isolate_set_script_info") &&
           "AOT generated C must not route script info through VM isolate");

    printf("  Generated file/dir-aware entry %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_runtime_file_dir_stays_runtime_owned) {
    const char *src = "import time\n"
                      "print(__file__)\n"
                      "print(__dir__)\n"
                      "time.sleep(0)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "main", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "runtime-backed __file__/__dir__ AOT program should generate");
    assert(contains(code, "runtime_cfg.file = \"test.xr\";") &&
           "runtime-backed generated main must pass script info to XrAotRuntimeConfig");
    assert(contains(code, "xrt_builtins[6] = xr_box_str(\"test.xr\")") &&
           "sync helpers may still initialize standalone __file__");
    assert(!contains(code, "xr_aot_runtime_set_builtin(rt, 6") &&
           "runtime-owned __file__ must not be overwritten with an xrt string");
    assert(!contains(code, "xr_aot_runtime_set_builtin(rt, 7") &&
           "runtime-owned __dir__ must not be overwritten with an xrt string");
    assert(!contains(code, "xray_isolate_set_script_info") &&
           "AOT generated C must not route script info through VM isolate");

    printf("  Generated runtime-owned file/dir entry %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_standalone_prelude_enum_globals_generate_static_members) {
    const char *src = "fn sent(r: SendResult) -> bool {\n"
                      "    return match (r) {\n"
                      "        SendResult.Sent -> true\n"
                      "        _ -> false\n"
                      "    }\n"
                      "}\n"
                      "fn empty(r: Recv<int>) -> bool {\n"
                      "    return match (r) {\n"
                      "        Recv.Empty -> true\n"
                      "        _ -> false\n"
                      "    }\n"
                      "}\n"
                      "print(sent(SendResult.Sent))\n"
                      "print(empty(Recv.Empty))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "standalone AOT prelude enum globals should generate");
    assert(contains(code, "xrt_map_new(4)") &&
           "standalone builtin enum globals should use lightweight enum maps");
    assert(contains(code, "_ev_SendResult_Sent") &&
           "no-payload SendResult members must be stable enum keys");
    assert(contains(code, "_ev_Recv_Empty") && "no-payload Recv members must be stable enum keys");
    assert(!contains(code, "xr_aot_load_builtin_field(ctx,") &&
           "standalone AOT must not require a coroutine isolate for prelude enum fields");

    printf("  Generated standalone prelude enum globals %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_cancelled_builtin_generates_false) {
    const char *src = "print(cancelled())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT cancelled() should generate");
    assert(contains(code, "XR_FROM_BOOL(false)") && "cancelled() currently lowers to false");
    assert(!contains(code, "unknown builtin") &&
           "cancelled() must not be treated as a named builtin");

    printf("  Generated cancelled() builtin %zu bytes of C code\n", strlen(code));
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
    assert(contains(code, "printf(\"%lld\", (long long)") && contains(code, "putchar('\\n')") &&
           "native int print should use direct printf/putchar");
    assert(!contains(code, "xrt_println(") && !contains(code, "xrt_print(") &&
           "native int print should not call the generic tagged printer");

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
    assert(contains(code, "printf(\"%lld\", (long long)") &&
           "native int print should use direct printf");

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

TEST(cgen_str_concat_uses_single_allocation_helper) {
#define CHECK_CGEN_STR_CONCAT(cond, msg)                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", (msg));                                                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    const char *src = "enum Kind { Ready }\n"
                      "let s = \"a\" + string(1) + \"b\" + string(2) + true + null + Kind.Ready\n"
                      "print(s)\n"
                      "print(\"q\" + string(3))\n"
                      "print(\"${42}\")\n";

    XiFunc *ir = compile_to_ir(src);
    CHECK_CGEN_STR_CONCAT(ir != NULL, "IR compilation failed");

    char *code = generate_c(ir, "test");
    CHECK_CGEN_STR_CONCAT(code != NULL, "C code generation failed");

    size_t code_len = strlen(code);
    const char *code_end = code + code_len;
    size_t add_calls = count_between(code, code_end, "xrt_add(");
    size_t strbuf_new_calls = count_between(code, code_end, "xrt_strbuf_new()");
    size_t part_init_calls = count_between(code, code_end, "xrt_strpart_init(");
    size_t concat_parts_calls = count_between(code, code_end, "xrt_str_concat_parts(");
    CHECK_CGEN_STR_CONCAT(add_calls == 0 && !contains(code, "xrt_add("),
                          "AOT STR_CONCAT must not lower to nested binary xrt_add calls");
    CHECK_CGEN_STR_CONCAT(strbuf_new_calls == 0,
                          "multi-part concat sites should not allocate temporary StringBuilder");
    CHECK_CGEN_STR_CONCAT(part_init_calls >= 8,
                          "AOT STR_CONCAT should materialize every part exactly once");
    CHECK_CGEN_STR_CONCAT(concat_parts_calls >= 2,
                          "AOT STR_CONCAT should use the single-allocation concat helper");

    printf("  Generated single-allocation string concat %zu bytes of C code "
           "(add=%zu strbuf_new=%zu parts=%zu concat=%zu)\n",
           code_len, add_calls, strbuf_new_calls, part_init_calls, concat_parts_calls);
    xr_free(code);
    xi_func_free(ir);

#undef CHECK_CGEN_STR_CONCAT
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

TEST(cgen_c_export_emits_public_c_abi_wrapper) {
    const char *src = "@c_export(\"xr_add\")\n"
                      "fn add(a: int32, b: int32) -> int32 {\n"
                      "    return a + b\n"
                      "}\n"
                      "print(add(3, 4))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "@c_export scalar function should generate");
    assert(contains(code, "\nint32_t xr_add(int32_t p0, int32_t p1);") &&
           "@c_export should emit a public C ABI forward declaration");
    assert(contains(code, "\nint32_t xr_add(int32_t p0, int32_t p1) {") &&
           "@c_export should emit a public C ABI wrapper definition");
    assert(!contains(code, "static int32_t xr_add(") &&
           "@c_export wrapper must not be file-static");
    assert(!contains(code, "xr_add(xrt_closure_t") &&
           "@c_export wrapper must not expose Xray's hidden closure parameter");
    assert(contains(code, "test_add_") && contains(code, "(NULL, (int64_t)p0, (int64_t)p1)") &&
           "@c_export wrapper should call the internal Xray function with a NULL closure");

    printf("  Generated C export wrapper %zu bytes of C code\n", strlen(code));
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
    assert(stats.boxed_adapters == 0 &&
           "direct-only native function should not expose a boxed adapter");
    assert(!contains(code, "xrt_closure_new((void*)test_inc_") &&
           "direct-only shared function should not allocate a runtime closure");

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
    assert(contains(code, "_1127_coro_numeric_prefix_compute_") &&
           "numeric module prefixes must be emitted as legal C identifiers");
    assert(!contains(code, " 1127_coro_numeric_prefix_compute_") &&
           "numeric module prefixes must not be emitted raw");

    printf("  Generated numeric-prefix module %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_emits_source_line_directives) {
    const char *src = "print(1)\n"
                      "print(2)\n"
                      "print(3)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    assert(ir->module != NULL && "pipeline should produce module metadata");
    ir->module->path = "debug_map.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "source-line directive test should generate");
    assert(contains(code, "#line 1 \"debug_map.xr\"") &&
           "first source statement should be mapped to the module path");
    assert(contains(code, "#line 2 \"debug_map.xr\"") &&
           "second source statement should be mapped to the module path");
    assert(contains(code, "#line 3 \"debug_map.xr\"") &&
           "print statement should be mapped to the module path");

    printf("  Generated source-line mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_emits_debug_source_var_slots) {
    const char *src = "fn compute(seed: int) -> int {\n"
                      "    let answer = seed + 1\n"
                      "    let doubled = answer * 2\n"
                      "    let ratio = doubled / 2.0\n"
                      "    let ok = ratio == 21.0\n"
                      "    if (!ok) { return 0 }\n"
                      "    return doubled\n"
                      "}\n"
                      "print(compute(20))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "debug source-var slot test should generate");
    assert(contains(code, "#if defined(XRAY_AOT_DEBUG_LOCALS)") &&
           "debug source locals should be guarded by the debug-local define");
    assert(contains(code, "int64_t seed = 0;") && "source parameter should get a debug local");
    assert(contains(code, "int64_t answer = 0;") && "source local should get a debug local");
    assert(contains(code, "int64_t doubled = 0;") &&
           "second source local should get a debug local");
    assert(contains(code, "double ratio = 0;") && "float source local should get a debug local");
    assert(contains(code, "uint8_t ok = 0;") && "bool source local should get a debug local");
    assert(contains(code, "seed = (int64_t)") && "source parameter should be synchronized");
    assert(contains(code, "answer = (int64_t)") && "answer should be synchronized");
    assert(contains(code, "doubled = (int64_t)") && "doubled should be synchronized");
    assert(contains(code, "ratio = (double)") && "ratio should be synchronized");
    assert(contains(code, "ok = (uint8_t)") && "ok should be synchronized");

    printf("  Generated debug source-var mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_emits_shadowed_debug_source_var_slots) {
    const char *src = "fn compute(seed: int) -> int {\n"
                      "    let answer = seed + 1\n"
                      "    if (answer > 0) {\n"
                      "        let shadowSeed = answer + 10\n"
                      "        let answer = shadowSeed\n"
                      "        print(answer)\n"
                      "    }\n"
                      "    return answer\n"
                      "}\n"
                      "print(compute(20))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "shadowed debug source-var slot test should generate");
    assert(contains(code, "int64_t answer = 0;") &&
           "first source variable with a repeated name should keep its source name");
    assert(contains(code, "int64_t _xray_dbg_shadow_1_answer = 0;") &&
           "shadowed source variable should get a stable debug fallback name");
    assert(contains(code, "answer = (int64_t)") && "outer answer should be synchronized");
    assert(contains(code, "_xray_dbg_shadow_1_answer = (int64_t)") &&
           "shadowed answer should be synchronized");

    printf("  Generated shadowed debug source-var mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_struct_debug_source_var_slots_use_typed_pointers) {
    const char *src = "@repr(C)\n"
                      "struct Point {\n"
                      "    x: int32\n"
                      "    y: int32\n"
                      "}\n"
                      "fn make(seed: int32) -> Point {\n"
                      "    if (seed < 0) { return Point{x: 0, y: 0} }\n"
                      "    let p = Point{x: seed + 1, y: seed + 2}\n"
                      "    let q = p\n"
                      "    return q\n"
                      "}\n"
                      "let out = make(20)\n"
                      "print(out.x)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "struct debug source-var slot test should generate");
    assert(contains(code, "#if defined(XRAY_AOT_DEBUG_LOCALS)") &&
           "struct debug source locals should be guarded by the debug-local define");
    assert(contains(code, "xrt_struct_test_") &&
           "test module should emit native struct storage types");
    assert(contains(code, "* p = 0;") && "struct source local p should use a typed pointer");
    assert(contains(code, "* q = 0;") && "struct source local q should use a typed pointer");
    assert(contains(code, "p = (xrt_struct_test_") &&
           "struct source local p should synchronize from the native heap pointer");
    assert(contains(code, "q = (xrt_struct_test_") &&
           "struct source local q should synchronize from the native heap pointer");
    assert(!contains(code, "XrValue p = XR_NULL_VAL;") &&
           "struct source local p should not degrade to an opaque XrValue debug slot");
    assert(!contains(code, "XrValue q = XR_NULL_VAL;") &&
           "struct source local q should not degrade to an opaque XrValue debug slot");

    printf("  Generated struct debug source-var mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_emits_source_line_directives) {
    const char *src = "fn worker(n: int) -> int {\n"
                      "    yield\n"
                      "    return n + 1\n"
                      "}\n"
                      "let task = go worker(41)\n"
                      "let result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    assert(ir->module != NULL && "pipeline should produce module metadata");
    ir->module->path = "debug_coro.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "coroutine source-line directive test should generate");
    assert(contains(code, "_aot_resume") && "test source should emit a coroutine resume body");
    assert(contains(code, "#line 2 \"debug_coro.xr\"") &&
           "yield should be mapped inside the coroutine resume body");
    assert(contains(code, "#line 3 \"debug_coro.xr\"") &&
           "coroutine return should be mapped to the source return line");
    assert(contains(code, "#line 6 \"debug_coro.xr\"") &&
           "await should be mapped inside the entry coroutine resume body");
    assert(contains(code, "#line 7 \"debug_coro.xr\"") &&
           "post-await print should be mapped inside the entry coroutine resume body");
    assert(contains(code, "#line 2 \"debug_coro.xr\"\n"
                          "    return xr_aot_yielded();\n"
                          "#line 1 \"<xray-generated>\"") &&
           "yield should reset generated control flow after the source-mapped yield");
    assert(contains(code, "#line 5 \"debug_coro.xr\"\n"
                          "    void *_child_frame_") &&
           "go should map only its source operation, not later state-machine control flow");
    assert(contains(code, ");\n"
                          "#line 1 \"<xray-generated>\"\n"
                          "    XrAotSpawnResult _spawn_") &&
           "go frame setup should reset before generated spawn bookkeeping");
    assert(contains(code, "#line 6 \"debug_coro.xr\"\n"
                          "    XrAotResult _await_") &&
           "await helper call should carry the await source line");
    assert(contains(code, "false);\n"
                          "#line 1 \"<xray-generated>\"\n"
                          "    if (_await_") &&
           "await helper should reset before generated blocked/error checks");

    printf("  Generated coroutine source-line mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_emits_debug_source_var_slots) {
    const char *src = "fn worker(seed: int) -> int {\n"
                      "    let answer = seed + 1\n"
                      "    yield\n"
                      "    let doubled = answer * 2\n"
                      "    let ratio = doubled / 2.0\n"
                      "    let ok = ratio == 21.0\n"
                      "    if (!ok) { return 0 }\n"
                      "    return doubled\n"
                      "}\n"
                      "let task = go worker(20)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    assert(ir->module != NULL && "pipeline should produce module metadata");
    ir->module->path = "debug_coro_locals.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "coroutine debug source-var slot test should generate");
    assert(contains(code, "_aot_resume") && "test source should emit a coroutine resume body");
    assert(contains(code, "#line 2 \"debug_coro_locals.xr\"") &&
           "pre-suspend local initializer should carry its source line");
    assert(contains(code, "#line 4 \"debug_coro_locals.xr\"") &&
           "post-resume local initializer should carry its source line");
    assert(contains(code, "#line 8 \"debug_coro_locals.xr\"") &&
           "coroutine return terminator should carry its source line");
    assert(contains(code, "#if defined(XRAY_AOT_DEBUG_LOCALS)") &&
           "coroutine debug source locals should be guarded by the debug-local define");
    assert(contains(code, "int64_t seed = 0;") &&
           "coroutine source parameter should get a debug local");
    assert(contains(code, "int64_t answer = 0;") &&
           "coroutine source local before suspension should get a debug local");
    assert(contains(code, "int64_t doubled = 0;") &&
           "coroutine source local after suspension should get a debug local");
    assert(contains(code, "double ratio = 0;") &&
           "coroutine float source local should get a debug local");
    assert(contains(code, "uint8_t ok = 0;") &&
           "coroutine bool source local should get a debug local");
    assert(contains(code, "seed = (int64_t)") &&
           "coroutine source parameter should be synchronized from the frame");
    assert(contains(code, "answer = (int64_t)") &&
           "coroutine source local should be synchronized after assignment/resume");
    assert(contains(code, "doubled = (int64_t)") &&
           "post-resume source local should be synchronized after assignment");
    assert(contains(code, "ratio = (double)") &&
           "coroutine float source local should be synchronized after assignment");
    assert(contains(code, "ok = (uint8_t)") &&
           "coroutine bool source local should be synchronized after assignment");

    printf("  Generated coroutine debug source-var mapped C %zu bytes\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_syncs_helper_result_debug_source_vars) {
    const char *src = "fn produce() -> int {\n"
                      "    return 41\n"
                      "}\n"
                      "fn worker(ch: Channel<int>) -> int {\n"
                      "    let task = go produce()\n"
                      "    let result = await task\n"
                      "    ch.send(result)\n"
                      "    let received = ch.recv()\n"
                      "    return received + 1\n"
                      "}\n"
                      "shared const ch: Channel<int> = new Channel(1)\n"
                      "let task = go worker(ch)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");
    assert(ir->module != NULL && "pipeline should produce module metadata");
    ir->module->path = "debug_coro_helper_results.xr";

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "coroutine helper debug source-var test should generate");
    assert(contains(code, "_aot_resume") && "test source should emit coroutine resume bodies");
    assert(contains(code, "xr_aot_await_task") &&
           "test should exercise the await helper result path");
    assert(contains(code, "xr_aot_chan_recv_slot") &&
           "test should exercise the channel recv helper result path");
    assert(contains(code, "int64_t result = 0;") &&
           "await result source variable should get a debug local");
    assert(contains(code, "int64_t received = 0;") &&
           "recv result source variable should get a debug local");
    assert(contains(code, "result = (int64_t)") &&
           "await helper result should be synchronized into the source debug local");
    assert(contains(code, "received = (int64_t)") &&
           "recv helper result should be synchronized into the source debug local");

    printf("  Generated coroutine helper debug source-var mapped C %zu bytes\n", strlen(code));
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
    const char *fn_body = strchr(fn, '{');
    const char *fn_end = fn_body ? strstr(fn_body, "\n}\n\nstatic") : NULL;
    assert(fn_body != NULL && fn_end != NULL && fn_body < fn_end &&
           "run function body should be bounded");

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
    assert(contains(code, "struct { int64_t x; double y; uint8_t ok; uint8_t byte; }") &&
           "inlined struct must use native C field storage");
    assert(contains(code, "(uint8_t)") && "sub-width struct stores must narrow to storage width");
    assert(!contains(code, "XrValue x") && "inlined scalar struct fields must not be boxed");
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
    assert(contains(code, "->x") && contains(code, "->y") && contains(code, "->ok") &&
           contains(code, "->byte") && "escaping primitive struct fields must use direct access");
    assert(!contains(code, "xrt_map_new(") && "primitive struct must not allocate runtime map");
    assert(!contains(code, "xrt_map_get(") && "primitive struct reads must not use runtime map");
    assert(!contains(code, "xrt_map_set(") && "primitive struct writes must not use runtime map");
    assert(!contains(code, "xrt_call_method(") &&
           "escaping struct path must not call an undeclared constructor helper");

    printf("  Generated escaping struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_same_shape_structs_keep_distinct_source_field_names) {
    const char *src = "struct Point {\n"
                      "    x: int\n"
                      "    y: int\n"
                      "}\n"
                      "struct Pair {\n"
                      "    a: int\n"
                      "    b: int\n"
                      "}\n"
                      "let p = Point{x: 1, y: 2}\n"
                      "let q = Pair{a: 3, b: 4}\n"
                      "print(p.x + p.y + q.a + q.b)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "same-shape source-named structs should generate");
    const char *code_end = code + strlen(code);
    assert(count_between(code, code_end, "typedef struct xrt_struct_test_") >= 2 &&
           "same-shape structs with different source field names must not share one typedef");
    assert(contains(code, "int64_t x; int64_t y;") &&
           "Point native layout must preserve source field names");
    assert(contains(code, "int64_t a; int64_t b;") &&
           "Pair native layout must preserve source field names");
    assert(contains(code, "->x") && contains(code, "->y") && contains(code, "->a") &&
           contains(code, "->b") && "field access must use each struct's source field names");

    printf("  Generated distinct same-shape struct layouts %zu bytes of C code\n", strlen(code));
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
    assert(contains(code, "XrValue name") &&
           "string struct field must be stored as a tagged immutable reference field");
    assert(contains(code, "XR_TAG_STRUCT_REF") &&
           "mixed scalar/string struct must allocate as an AOT struct reference");
    assert(contains(code, "->count") && contains(code, "->name") &&
           "mixed scalar/string struct fields must use direct access");
    assert(!contains(code, "xrt_map_new(") && "mixed scalar/string struct must not allocate map");
    assert(!contains(code, "xrt_map_get(") && "mixed scalar/string struct reads must not use map");
    assert(!contains(code, "xrt_map_set(") && "mixed scalar/string struct writes must not use map");

    printf("  Generated string-field struct heap-native path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_repr_c_struct_omits_native_header) {
    const char *src = "@repr(C)\n"
                      "struct CPair {\n"
                      "    a: int32\n"
                      "    b: uint8\n"
                      "}\n"
                      "let p = CPair{a: 41, b: 1}\n"
                      "p.a = p.a + 1\n"
                      "print(p.a + p.b)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "@repr(C) struct path should generate");

    const char *typedef_start = strstr(code, "typedef struct xrt_struct_test_");
    assert(typedef_start != NULL && "@repr(C) struct must emit a native typedef");
    const char *typedef_end = strstr(typedef_start, ";\n");
    assert(typedef_end != NULL && "typedef should be bounded");
    assert(count_between(typedef_start, typedef_end, "_size") == 0 &&
           "@repr(C) typedef must not include the Xray size header");
    assert(count_between(typedef_start, typedef_end, "_layout") == 0 &&
           "@repr(C) typedef must not include the Xray layout header");
    assert(count_between(typedef_start, typedef_end, "int32_t a") == 1 &&
           "@repr(C) int32 field must be placed at payload offset 0");
    assert(count_between(typedef_start, typedef_end, "uint8_t b") == 1 &&
           "@repr(C) uint8 field must be emitted as raw C storage");
    assert(contains(code, "xr_struct_ref(_s, (uint16_t)sizeof(") &&
           "@repr(C) struct refs must carry storage size outside the payload");

    printf("  Generated @repr(C) headerless struct path %zu bytes of C code\n", strlen(code));
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
    assert(contains(code, "xrt_struct_test_") && contains(code, " p;") &&
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
    const char *fn_body = strchr(fn, '{');
    const char *fn_end = fn_body ? strstr(fn_body, "\n}\n\nstatic") : NULL;
    assert(fn_body != NULL && fn_end != NULL && fn_body < fn_end &&
           "run function body should be bounded");

    assert(contains(code, "uint8_t data[4]") &&
           "fixed array field must be embedded in the native heap layout");
    assert(contains(code, "xrt_fixed_array_copy") &&
           "fixed array field initialization must copy into embedded storage");
    assert(count_between(fn_body, fn_end, "data[") > 0 &&
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
    const char *fn_body = strchr(fn, '{');
    const char *fn_end = fn_body ? strstr(fn_body, "\n}\n\nstatic") : NULL;
    assert(fn_body != NULL && fn_end != NULL && fn_body < fn_end &&
           "run function body should be bounded");

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
    const char *fn_end = next_static_after(fn);

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

TEST(cgen_local_class_direct_native_methods_omit_boxed_adapters) {
    const char *src = "class Counter {\n"
                      "    value: int\n"
                      "    constructor(init: int) { this.value = init }\n"
                      "    bump(n: int) -> int {\n"
                      "        this.value = this.value + n\n"
                      "        return this.value\n"
                      "    }\n"
                      "    get() -> int { return this.value }\n"
                      "}\n"
                      "fn run() -> int {\n"
                      "    let c = Counter(2)\n"
                      "    let a = c.bump(5)\n"
                      "    return a + c.get()\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "local direct native class method path should generate");

    assert(contains(code, "typedef struct xrt_native_test_Counter") &&
           "local class should still use a native receiver layout");
    assert(contains(code, "int64_t test_bump_") && contains(code, "int64_t test_get_") &&
           "direct native methods should keep typed entry points");
    assert(!contains(code, "static XrValue test_bump_") &&
           !contains(code, "static XrValue test_get_") &&
           "pure local native class flow must not emit dead boxed method adapters");

    const char *run = strstr(code, "static int64_t test_run_");
    assert(run != NULL && "run function should use typed ABI");
    run = strstr(run + 1, "static int64_t test_run_");
    assert(run != NULL && "run function definition should follow its declaration");
    const char *run_body = strchr(run, '{');
    const char *run_end = run_body ? strstr(run_body, "\n}\n\nstatic") : NULL;
    assert(run_body != NULL && run_end != NULL && run_body < run_end &&
           "run function body should be bounded");
    assert(count_between(run, run_end, "xrt_map_new(") == 0 &&
           count_between(run, run_end, "xrt_map_get(") == 0 &&
           count_between(run, run_end, "xrt_map_set(") == 0 &&
           "pure local native class flow must stay out of the map boundary");

    printf("  Generated local direct native class method path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_class_constructor_returns_heap_native_instance) {
    const char *src = "class Counter {\n"
                      "    value: int\n"
                      "    constructor(init: int) { this.value = init }\n"
                      "    bump(n: int) -> int {\n"
                      "        this.value = this.value + n\n"
                      "        return this.value\n"
                      "    }\n"
                      "    get() -> int { return this.value }\n"
                      "}\n"
                      "fn make(n: int) -> Counter {\n"
                      "    return Counter(n)\n"
                      "}\n"
                      "fn touch(c: Counter) -> int {\n"
                      "    c.value = c.value + 1\n"
                      "    return c.get()\n"
                      "}\n"
                      "fn run() -> int {\n"
                      "    let c = make(2)\n"
                      "    let a = c.bump(5)\n"
                      "    let b = c.value\n"
                      "    let d = touch(c)\n"
                      "    return a + b + d + c.get()\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "heap native class instance path should generate");

    assert(contains(code, "xrt_obj_alloc((uint16_t)") &&
           "escaping native class constructor should allocate a typed heap object");
    assert(!contains(code, "xrt_box_obj(_inst)") &&
           "native class constructor values should stay as pointers inside typed AOT code");
    assert(contains(code, "heap_type == XR_TINSTANCE") &&
           "boxed method and field paths should discriminate native class instances");
    assert(contains(code, "xrt_instanceof(") &&
           "boxed native class paths should guard the concrete class id");

    const char *make_sig = "static xrt_native_test_Counter * test_make_";
    const char *make = strstr(code, make_sig);
    assert(make != NULL && "make function should return a native class pointer");
    make = strstr(make + 1, make_sig);
    assert(make != NULL && "make function definition should follow its declaration");
    const char *make_end = next_static_after(make);
    assert(count_between(make, make_end, "xrt_obj_alloc(") == 1 &&
           "make should allocate exactly one heap native instance");
    assert(count_between(make, make_end, "xrt_box_obj(_inst)") == 0 &&
           "typed native class return should not box inside the producer");
    assert(count_between(make, make_end, "xrt_map_new(") == 0 &&
           "escaping native class constructor must not allocate a map instance");
    assert(!contains(code, "static XrValue test_make_") &&
           "direct-only native class producer should not emit a boxed adapter");

    const char *touch = strstr(code, "static int64_t test_touch_");
    assert(touch != NULL && "touch function should use typed scalar return ABI");
    touch = strstr(touch + 1, "static int64_t test_touch_");
    assert(touch != NULL && "touch function definition should follow its declaration");
    const char *touch_end = next_static_after(touch);
    assert(count_between(touch, touch_end, "xrt_getprop_name(") == 0 &&
           count_between(touch, touch_end, "xrt_setprop_name(") == 0 &&
           count_between(touch, touch_end, "xrt_map_get(") == 0 &&
           count_between(touch, touch_end, "xrt_map_set(") == 0 &&
           "native class pointer parameters must not fall back to Map field access");
    assert(count_between(touch, touch_end, "test_get_") >= 1 &&
           count_between(touch, touch_end, "test_get_3_boxed(") == 0 &&
           "native class pointer method calls should use the typed method entry directly");

    assert(!contains(code, "static XrValue test_bump_") &&
           "pure typed native class flow should not keep a dead boxed method adapter");

    printf("  Generated heap native class instance path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_native_class_ref_field_constructor_result_uses_ptr_storage) {
    const char *src = "class IntBag {\n"
                      "    values: Array<int>\n"
                      "    constructor(values: Array<int>) { this.values = values }\n"
                      "    scan(rounds: int) -> int {\n"
                      "        let r = 0\n"
                      "        let sum = 0\n"
                      "        while (r < rounds) {\n"
                      "            let i = 0\n"
                      "            while (i < this.values.length) {\n"
                      "                sum = sum + this.values[i]\n"
                      "                i = i + 1\n"
                      "            }\n"
                      "            r = r + 1\n"
                      "        }\n"
                      "        return sum\n"
                      "    }\n"
                      "}\n"
                      "fn make_values(n: int) -> Array<int> {\n"
                      "    let values: Array<int> = []\n"
                      "    let i = 0\n"
                      "    while (i < n) {\n"
                      "        values.push(i * 3 + 1)\n"
                      "        i = i + 1\n"
                      "    }\n"
                      "    return values\n"
                      "}\n"
                      "fn run(rounds: int) -> int {\n"
                      "    let bag = IntBag(make_values(8))\n"
                      "    return bag.scan(rounds)\n"
                      "}\n"
                      "print(run(10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "native class ref-field constructor path should generate");

    const char *run = strstr(code, "static int64_t test_run_");
    assert(run != NULL && "run function should use typed scalar return ABI");
    run = strstr(run + 1, "static int64_t test_run_");
    assert(run != NULL && "run function definition should follow its declaration");
    const char *run_end = next_static_after(run);
    assert(count_between(run, run_end, "xrt_obj_alloc(") == 0 &&
           "non-escaping ref-field native class should be stack-constructed");
    assert(count_between(run, run_end, "xrt_native_test_IntBag _ci") == 1 &&
           count_between(run, run_end, "xrt_native_test_IntBag_dtor(&_ci") == 0 &&
           "stack-constructed collection-only ref field class should skip no-op destructor calls");
    assert(count_between(run, run_end, "xrt_box_obj(_inst)") == 0 &&
           "local native class constructor result must not be boxed before direct method calls");
    assert(count_between(run, run_end, "test_scan_") >= 1 &&
           count_between(run, run_end, "test_scan_2_boxed(") == 0 &&
           "local native class method call should target the typed receiver entry");
    assert(count_between(run, run_end, "XR_FROM_INT(test_scan_") == 0 &&
           "stack-return path should keep the method result native until return");
    assert(count_between(run, run_end, "xrt_getprop_name(") == 0 &&
           count_between(run, run_end, "xrt_setprop_name(") == 0 &&
           count_between(run, run_end, "xrt_map_get(") == 0 &&
           count_between(run, run_end, "xrt_map_set(") == 0 &&
           "native-layout class ref-field flow must not fall back to Map storage");

    printf("  Generated native class ref-field constructor ptr path %zu bytes of C code\n",
           strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_native_class_collection_ref_fields_skip_noop_arc) {
    const char *src = "class Bag {\n"
                      "    values: Array<int>\n"
                      "    constructor(values: Array<int>) {\n"
                      "        this.values = values\n"
                      "    }\n"
                      "    replace(next: Array<int>) -> int {\n"
                      "        this.values = next\n"
                      "        return this.values.length\n"
                      "    }\n"
                      "}\n"
                      "fn make(values: Array<int>) -> Bag {\n"
                      "    return Bag(values)\n"
                      "}\n"
                      "fn swap(b: Bag, next: Array<int>) -> int {\n"
                      "    b.values = next\n"
                      "    return b.values.length\n"
                      "}\n"
                      "fn local(values: Array<int>) -> int {\n"
                      "    let bag = Bag(values)\n"
                      "    return bag.values.length\n"
                      "}\n"
                      "fn run() -> int {\n"
                      "    let a = [1]\n"
                      "    let b = [2, 3]\n"
                      "    let bag = make(a)\n"
                      "    return bag.replace(b) + swap(bag, a) + local(a)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "native class ref field ownership path should generate");

    assert(!contains(code, "static void xrt_native_test_Bag_dtor(void *obj)") &&
           "AOT collection ref fields are not prepend-header ARC-managed and need no destructor");
    assert(!contains(code, "xrt_release(xr_mkptr((self)->f0, XR_TAG_ARRAY));") &&
           "collection ref field destructors should not emit no-op releases");
    assert(contains(code, "xrt_type_register(\"Bag\", 0, NULL, 0, NULL, "
                          "(uint32_t)sizeof(xrt_native_test_Bag))") &&
           "class type registration should not wire a destructor for collection-only refs");

    const char *replace = strstr(code, "static int64_t test_replace_");
    assert(replace != NULL && "replace method should use typed ABI");
    replace = strstr(replace + 1, "static int64_t test_replace_");
    assert(replace != NULL && "replace method definition should follow its declaration");
    const char *replace_end = next_static_after(replace);
    const char *assign = strstr(replace, "(p0)->f0 = (xrt_array_t *)_new.ptr");
    assert(assign && assign < replace_end &&
           count_between(replace, replace_end, "xrt_retain(_new);") == 0 &&
           count_between(replace, replace_end, "xrt_release(xr_mkptr((p0)->f0, XR_TAG_ARRAY));") ==
               0 &&
           "direct native receiver collection ref stores should assign without no-op ARC calls");

    const char *swap = strstr(code, "static int64_t test_swap_");
    assert(swap != NULL && "swap function should use typed scalar return ABI");
    swap = strstr(swap + 1, "static int64_t test_swap_");
    assert(swap != NULL && "swap function definition should follow its declaration");
    const char *swap_end = next_static_after(swap);
    assert(count_between(swap, swap_end, "heap_type == XR_TINSTANCE") == 0 &&
           count_between(swap, swap_end, "xrt_getprop_name(") == 0 &&
           count_between(swap, swap_end, "xrt_setprop_name(") == 0 &&
           count_between(swap, swap_end, "xrt_map_get(") == 0 &&
           count_between(swap, swap_end, "xrt_map_set(") == 0 &&
           "native class pointer parameters should access ref fields without Map fallback");
    assign = strstr(swap, "(_native)->f0 = (xrt_array_t *)_new.ptr");
    assert(assign && assign < swap_end && count_between(swap, swap_end, "xrt_retain(_new);") == 0 &&
           count_between(swap, swap_end, "xrt_release(xr_mkptr((_native)->f0, XR_TAG_ARRAY));") ==
               0 &&
           "heap native instance collection ref stores should assign without no-op ARC calls");

    const char *local = strstr(code, "static int64_t test_local_");
    assert(local != NULL && "local function should use typed scalar return ABI");
    local = strstr(local + 1, "static int64_t test_local_");
    assert(local != NULL && "local function definition should follow its declaration");
    const char *local_end = next_static_after(local);
    assert(count_between(local, local_end, "xrt_obj_alloc(") == 1 &&
           count_between(local, local_end, "_ci") == 0 &&
           "ref-field native classes should not stack-inline without stack destructors");

    printf("  Generated native class collection ref field path %zu bytes of C code\n",
           strlen(code));
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
    const char *fn_end = next_static_after(fn);

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
    assert(count_between(fn, fn_end, "(int64_t)((uint64_t)(") > 0 &&
           count_between(fn, fn_end, "xrt_i64_add(") == 0 &&
           "Set<int>.length + size should emit inline native wrap arithmetic");

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
    const char *fill_end = next_static_after(fill);
    const char *scan = strstr(code, "static int64_t test_scan_");
    assert(scan != NULL && "scan method should use typed ABI");
    scan = strstr(scan + 1, "static int64_t test_scan_");
    assert(scan != NULL && "scan method definition should follow its declaration");
    const char *scan_end = next_static_after(scan);
    const char *prune = strstr(code, "static int64_t test_prune_");
    assert(prune != NULL && "prune method should use typed ABI");
    prune = strstr(prune + 1, "static int64_t test_prune_");
    assert(prune != NULL && "prune method definition should follow its declaration");
    const char *prune_end = next_static_after(prune);

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
    const char *fill_end = next_static_after(fill);
    const char *scan = strstr(code, "static int64_t test_scan_");
    assert(scan != NULL && "scan method should use typed ABI");
    scan = strstr(scan + 1, "static int64_t test_scan_");
    assert(scan != NULL && "scan method definition should follow its declaration");
    const char *scan_end = next_static_after(scan);
    const char *prune = strstr(code, "static int64_t test_prune_");
    assert(prune != NULL && "prune method should use typed ABI");
    prune = strstr(prune + 1, "static int64_t test_prune_");
    assert(prune != NULL && "prune method definition should follow its declaration");
    const char *prune_end = next_static_after(prune);

    assert(contains(code, "xrt_map_new_typed(0, XR_ELEM_I64, XR_ELEM_I64)") &&
           "Map<int,int> class field constructor should use typed map storage");
    assert(count_between(fill, fill_end, "xrt_map_set_i64_i64_typed(") == 1 &&
           "Map<int,int>.set should use the typed integer direct helper");
    assert(count_between(scan, scan_end, "xrt_map_has_i64_typed(") == 0 &&
           "Map<int,int>.has guarding a get fuses into the get's find, not a separate has probe");
    assert(count_between(scan, scan_end, "xrt_map_find_i64_typed(") == 1 &&
           count_between(scan, scan_end, "xrt_map_get_i64_value_typed(") == 1 &&
           "Map<int,int>.has+get guarded should share one typed find plus one typed value load");
    assert(count_between(scan, scan_end, "_mf") >= 2 &&
           "the fused has should write _mf and the guarded get should read it back");
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
    /* Map<bool,i64|f32> lowers to the 2-slot boolmap representation: created via
     * xrt_boolmap_new_typed and probed with the dispatch-free boolmap helpers. */
    assert(contains(code, "xrt_boolmap_new_typed(0, XR_ELEM_F32)"));
    assert(count_between(code, code_end, "xrt_map_set_bool_f32_typed(") == 2 &&
           count_between(code, code_end, "xrt_map_delete_bool_f32_typed(") == 1);
    assert(contains(code, "xrt_boolmap_new_typed(0, XR_ELEM_I64)"));
    assert(count_between(code, code_end, "xrt_map_set_bool_i64_typed(") == 2 &&
           count_between(code, code_end, "xrt_map_delete_bool_i64_typed(") == 1);
    /* Each guarded get loads a slot value through the boolmap value helpers,
     * regardless of has+get probe fusion (a fused has writes the slot index the
     * get reads back; an unfused has keeps its own boolmap find). */
    assert(count_between(code, code_end, "xrt_boolmap_value_f64(") == 2 &&
           count_between(code, code_end, "xrt_boolmap_value_i64(") == 2 &&
           "bool-key Map.get should load values via the boolmap helpers");
    assert(count_between(code, code_end, "xrt_boolmap_find(") >= 2 &&
           "guarded bool-key gets should probe via the boolmap find helper");
    assert(!contains(code, "xrt_map_has(") && !contains(code, "xrt_map_get(") &&
           "bool-key Map hot methods must not fall back to boxed map helpers");
    assert(!contains(code, "xrt_map_find_bool_typed(") &&
           !contains(code, "xrt_map_get_i64_value_typed(") &&
           !contains(code, "xrt_map_get_f64_value_typed(") &&
           "Map<bool,i64|f32> must use the boolmap representation, not the generic typed probes");
    assert(!contains(code, "xrt_map_set_i64_i64_typed(") &&
           !contains(code, "xrt_map_find_i64_typed(") &&
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
    const char *count_end = next_static_after(count);
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
    const char *count_end = next_static_after(count);
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
    const char *area_end = next_static_after(area);
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
    const char *kind_end = next_static_after(kind);
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
    const char *score_end = next_static_after(score);
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
                      "let ri = 2..=6\n"
                      "print(r)\n"
                      "print(ri)\n"
                      "print(typeof(r))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "range direct driver should generate");

    assert(contains(code, "xrt_range_from_i64(") &&
           "range expression must use the direct AOT range helper");
    assert(contains(code, ", false)") && "half-open range must pass inclusive=false");
    assert(contains(code, ", true)") && "inclusive range must pass inclusive=true");
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
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "pure inlined Array<uint8>.filter must not emit a dead boxed callback adapter");
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
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "pure inlined Array<int>.map must not emit a dead boxed callback adapter");
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
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "read-only pure Array<int>.map must not emit a dead boxed callback adapter");
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

TEST(cgen_dynamic_uncaptured_callback_keeps_boxed_adapter) {
    const char *src = "fn apply(f: (int) -> int, x: int) -> int {\n"
                      "    return f(x)\n"
                      "}\n"
                      "fn run() -> int {\n"
                      "    return apply(fn(n: int) -> int { return n + 1 }, 41)\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "dynamic uncaptured callback should generate");
    assert(contains(code, "xrt_closure_new((void*)test___anonymous__") &&
           "dynamic uncaptured callback must allocate a closure value");
    assert(contains(code, "static XrValue test___anonymous__") &&
           "dynamic uncaptured typed callback must keep its boxed adapter");

    printf("  Generated dynamic uncaptured callback path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_closure_cell_var_id_above_255) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 900, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 901, .frozen = true};

    XiFunc *ir = xi_func_new("manual_high_cell", &func_type);
    assert(ir != NULL);
    XiBlock *entry = xi_block_new(ir);
    entry->sealed = true;

    XiValue *captured = xi_const_int(ir, entry, 7, &int_type);
    captured->var_id = 300;

    XiFunc *child = xi_func_new("manual_child", &int_type);
    assert(child != NULL);
    child->parent_func = ir;
    XiBlock *child_entry = xi_block_new(child);
    child_entry->sealed = true;
    XiValue *child_ret = xi_const_int(child, child_entry, 1, &int_type);
    xi_block_set_return(child_entry, child_ret);
    child->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_REG,
        .needs_cell = true,
        .type = &int_type,
        .value = captured,
        .name = "captured",
    };
    child->ncaptures = 1;

    ir->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(ir->children != NULL);
    ir->children[0] = child;
    ir->children_cap = 1;
    ir->nchildren = 1;

    XiValue *closure = xi_value_new(ir, entry, XI_CLOSURE_NEW, &func_type, 1);
    assert(closure != NULL);
    closure->aux = (void *) child;
    closure->aux_int = 0;
    closure->args[0] = captured;
    xi_block_set_return(entry, closure);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "high var_id closure cell should generate");
    assert(contains(code, "xrt_cell_new(") && "mutable capture should allocate a cell");
    assert(max_cell_ref_id(code) > 255 && "cell var_id should exceed the old 8-bit wall");

    printf("  Generated high-var closure cell path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_stack_alloc_direct_closure_uses_stack_runtime) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 910, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 911, .frozen = true};

    XiFunc *ir = xi_func_new("manual_stack_closure", &int_type);
    assert(ir != NULL);
    XiBlock *entry = xi_block_new(ir);
    entry->sealed = true;

    XiValue *captured = xi_const_int(ir, entry, 7, &int_type);

    XiFunc *child = xi_func_new("manual_stack_child", &int_type);
    assert(child != NULL);
    child->parent_func = ir;
    XiBlock *child_entry = xi_block_new(child);
    child_entry->sealed = true;
    XiValue *ret = xi_const_int(child, child_entry, 1, &int_type);
    xi_block_set_return(child_entry, ret);
    child->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_REG,
        .needs_cell = false,
        .type = &int_type,
        .value = captured,
        .name = "captured",
    };
    child->ncaptures = 1;

    ir->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(ir->children != NULL);
    ir->children[0] = child;
    ir->children_cap = 1;
    ir->nchildren = 1;

    XiValue *closure = xi_value_new(ir, entry, XI_CLOSURE_NEW, &func_type, 1);
    assert(closure != NULL);
    closure->aux = (void *) child;
    closure->args[0] = captured;
    XiValue *call = xi_value_new(ir, entry, XI_CALL, &int_type, 1);
    assert(call != NULL);
    call->args[0] = closure;
    xi_block_set_return(entry, call);

    xi_escape_analyze(ir);
    xi_stack_alloc_rewrite(ir);
    assert(closure->op == XI_STACK_ALLOC && "direct closure should stack allocate");
    xi_arc_insert(ir);
    xi_arc_elim(ir);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "stack closure should generate");
    assert(contains(code, "xrt_closure_stack_new((void*)") &&
           "direct no-escape closure should use stack closure runtime");
    assert(!contains(code, "xrt_closure_new((void*)") &&
           "direct no-escape closure must not allocate a heap closure");
    assert(contains(code, "xrt_release(") && contains(code, "XR_TAG_CLOSURE") &&
           "stack closure should still run ARC destruction at its death point");

    printf("  Generated stack closure path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_stack_alloc_closure_preserves_cell_capture) {
    XrType int_type = {.kind = XR_KIND_INT, .id = 912, .frozen = true};
    XrType func_type = {.kind = XR_KIND_FUNCTION, .id = 913, .frozen = true};

    XiFunc *ir = xi_func_new("manual_stack_cell_closure", &int_type);
    assert(ir != NULL);
    XiBlock *entry = xi_block_new(ir);
    entry->sealed = true;

    XiValue *captured = xi_const_int(ir, entry, 7, &int_type);
    captured->var_id = 37;

    XiFunc *child = xi_func_new("manual_stack_cell_child", &int_type);
    assert(child != NULL);
    child->parent_func = ir;
    XiBlock *child_entry = xi_block_new(child);
    child_entry->sealed = true;
    XiValue *ret = xi_const_int(child, child_entry, 1, &int_type);
    xi_block_set_return(child_entry, ret);
    child->captures[0] = (XiCapture) {
        .source = XI_CAPTURE_SRC_REG,
        .needs_cell = true,
        .type = &int_type,
        .value = captured,
        .name = "captured",
    };
    child->ncaptures = 1;

    ir->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(ir->children != NULL);
    ir->children[0] = child;
    ir->children_cap = 1;
    ir->nchildren = 1;

    XiValue *closure = xi_value_new(ir, entry, XI_CLOSURE_NEW, &func_type, 1);
    assert(closure != NULL);
    closure->aux = (void *) child;
    closure->args[0] = captured;
    XiValue *call = xi_value_new(ir, entry, XI_CALL, &int_type, 1);
    assert(call != NULL);
    call->args[0] = closure;
    xi_block_set_return(entry, call);

    xi_escape_analyze(ir);
    xi_stack_alloc_rewrite(ir);
    assert(closure->op == XI_STACK_ALLOC && "direct closure should stack allocate");
    xi_arc_insert(ir);
    xi_arc_elim(ir);

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "stack closure cell capture should generate");
    assert(contains(code, "xrt_closure_stack_new((void*)") &&
           "direct no-escape closure should use stack closure runtime");
    assert(contains(code, "xrt_cell_new(") &&
           "mutable capture for stack closure must allocate a cell");
    assert(contains(code, "_c->upvals[0] = cell_37") &&
           "stack closure upvalue must receive the mutable capture cell");

    printf("  Generated stack closure cell path %zu bytes of C code\n", strlen(code));
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
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "read-only pure Array<uint8>.filter must not emit a dead boxed callback adapter");
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
    assert(!contains(code, "static XrValue test___anonymous__") &&
           "pure inlined Array<int>.reduce must not emit a dead boxed callback adapter");
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
    assert(contains(code, " / INT64_C(5)") && contains(code, " % INT64_C(7)") &&
           "constant div/mod RHS must stay literal so C compilers can strength-reduce it");

    printf("  Generated integer div/mod fast path %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_defer_isolates_existing_pending_error) {
    const char *src = "enum E { Bad(code: int) }\n"
                      "fn run() -> int {\n"
                      "    let log: Array<int> = []\n"
                      "    fn body() {\n"
                      "        defer { log.push(100) }\n"
                      "        throw E.Bad(1)\n"
                      "    }\n"
                      "    try { body() } catch (e) { log.push(1) }\n"
                      "    return log[0] + log[1]\n"
                      "}\n"
                      "print(run())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "defer over pending error should generate");

    assert(contains(code, "XrValue _xrt_defer_saved_error_") &&
           "defer emission must save an existing pending error");
    assert(contains(code, "int _xrt_defer_had_error_") &&
           "defer emission must remember whether the error channel was active");
    assert(contains(code, "xrt_pending_error = XR_NULL_VAL;") &&
           "defer body must run with the old pending error temporarily cleared");
    assert(contains(code, "xrt_release(_xrt_defer_saved_error_") &&
           "a defer-raised replacement error must release the old pending error");

    printf("  Generated defer pending-error isolation %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_err_return_stops_unreachable_tail) {
    const char *src = "enum E { Msg(s: string) }\n"
                      "fn failing() -> int {\n"
                      "    throw E.Msg(\"boom\")\n"
                      "    return 0\n"
                      "}\n"
                      "print(failing())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "throw plus unreachable return should generate");

    const char *fn = strstr(code, "static int64_t test_failing_");
    assert(fn != NULL && "failing function should be generated with native int ABI");
    const char *fn_end = next_static_after(fn);
    assert(count_between(fn, fn_end, "xrt_pending_error =") == 1 &&
           "throw outside try should lower to exactly one pending-error write");
    assert(count_between(fn, fn_end, "return 0;") == 1 &&
           "throw outside try should return the int ABI default once");
    assert(count_between(fn, fn_end, "return 0;\n    v") == 0 &&
           "AOT must not emit unreachable SSA statements after ERR_RETURN");
    assert(count_between(fn, fn_end, "return (int64_t)(v") == 0 &&
           "AOT must not emit the unreachable source return after ERR_RETURN");

    printf("  Generated ERR_RETURN tail stop %zu bytes of C code\n", strlen(code));
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
    assert(contains(code, "_aot_frame_init(f->call_frame_") &&
           "scalar direct suspend call frames should be reset and reused after synchronous "
           "completion");
    assert(contains(code, "    f->state = 0;\n") &&
           "reusable AOT frame init should reset entry state without clearing cached child frames");
    assert(contains(code, "    memset(f, 0, sizeof(*f));\n    if (!") &&
           "fresh AOT frame allocation should still clear owned frame storage once");
    assert(contains(code, "return (abort(), XR_NULL_VAL);") &&
           "suspendable functions must keep hard-failing sync wrappers");
    assert(!contains(code, "unsupported AOT sync call") &&
           "diagnostics should go to stderr, not generated C comments");

    printf("  Generated direct suspend call %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_direct_suspend_method_call_propagates_cps) {
    const char *src = "class Box {\n"
                      "    bump(n: int) -> int {\n"
                      "        yield\n"
                      "        return n + 1\n"
                      "    }\n"
                      "}\n"
                      "fn main() -> int {\n"
                      "    let box = new Box()\n"
                      "    return box.bump(41)\n"
                      "}\n"
                      "print(main())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "static direct suspend method calls should CPS-propagate to the caller");
    assert(contains(code, "call_frame_") &&
           "caller frame should own the child method frame while blocked");
    assert(contains(code, "_aot_resume(f->call_frame_") &&
           "direct suspend method calls must resume through the child frame");
    assert(contains(code, "_aot_frame_new(") &&
           "direct suspend method calls must allocate a child frame");
    assert(!contains(code, "unsupported AOT sync method call") &&
           "method suspendability must be represented as CPS, not a sync-wrapper failure");

    printf("  Generated direct suspend method call %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_shared_static_function_retain_is_elided) {
    const char *src = "fn inc(n: int) -> int {\n"
                      "    return n + 1\n"
                      "}\n"
                      "let result = inc(41)\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "static direct calls should generate without cgen errors");
    assert(contains(code, "test_inc_") && "static function should be emitted directly");
    assert(!contains(code, "xrt_retain(v") &&
           "direct calls to uncaptured shared functions should not retain the callee closure");

    printf("  Generated shared static call %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_shared_static_function_retain_is_elided) {
    const char *src = "fn inc(n: int) -> int {\n"
                      "    return n + 1\n"
                      "}\n"
                      "fn worker(n: int) -> int {\n"
                      "    yield\n"
                      "    return inc(n)\n"
                      "}\n"
                      "let task = go worker(41)\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine static calls should generate without cgen errors");
    assert(contains(code, "test_inc_") && "static function should be emitted directly");
    assert(!contains(code, "xrt_retain(v") && "AOT coroutine direct calls to uncaptured shared "
                                              "functions should not retain callee closure");

    printf("  Generated coroutine shared static call %zu bytes of C code\n", strlen(code));
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

TEST(cgen_coro_loop_tail_phi_survives_poll_suspend) {
    const char *src = "fn worker(ch: Channel<int>, timeout_ms: int, value: int) -> bool {\n"
                      "    return match (ch.sendTimeout(value, timeout_ms)) {\n"
                      "        SendResult.Timeout -> true\n"
                      "        _ -> false\n"
                      "    }\n"
                      "}\n"
                      "fn run_once(count: int, timeout_ms: int) -> int {\n"
                      "    shared const ch = new Channel(1)\n"
                      "    ch.send(1)\n"
                      "    let tasks: Array<Task> = []\n"
                      "    for (let i = 0; i < count; i++) {\n"
                      "        tasks.push(go worker(ch, timeout_ms, i + 2))\n"
                      "    }\n"
                      "    let total = 0\n"
                      "    for (task in tasks) {\n"
                      "        if (await task) {\n"
                      "            total++\n"
                      "        }\n"
                      "    }\n"
                      "    let _drain = ch.recv()\n"
                      "    ch.close()\n"
                      "    return total\n"
                      "}\n"
                      "print(run_once(3, 10))\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT coroutine loop-tail phi should generate");

    const char *frame = strstr(code, "typedef struct test_run_once_");
    assert(frame != NULL && "run_once coroutine frame should be emitted");
    const char *frame_end = strstr(frame, "} test_run_once_");
    assert(frame_end != NULL && "run_once coroutine frame should have an end marker");
    assert(count_between(frame, frame_end, "\n    int64_t phi") >= 4 &&
           "loop-tail accumulator phi must be stored in the frame across poll suspend");

    const char *resume = strstr(frame_end, "test_run_once_");
    resume = resume ? strstr(resume, "_aot_resume(void *raw_frame") : NULL;
    const char *trace = resume ? strstr(resume, "_aot_trace(void *frame") : NULL;
    assert(resume != NULL && trace != NULL && "run_once resume/trace functions should exist");
    assert(count_between(resume, trace, "\n    int64_t phi") == 0 &&
           "cross-suspend phi values must not be resume-local zeroed temporaries");

    printf("  Generated loop-tail phi frame %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_runtime_managed_types_skip_arc) {
    XrType task_type = {.kind = XR_KIND_INSTANCE};
    XrType channel_type = {.kind = XR_KIND_CHANNEL};
    XrType string_type = {.kind = XR_KIND_STRING};

    task_type.instance.class_name = "Task";

    /* Only Task/Coroutine are runtime-managed (executor-owned): the compiler
     * skips ARC for them. Channel is pure cross-coroutine shared DATA with no
     * executor owner, so it uses the atomic shared-RC like a string — the
     * compiler DOES track it (last drop frees), which is what stops channels
     * created and discarded in a loop from leaking. */
    assert(!xi_own_type_is_rc(&task_type) && "Task is owned by the coroutine runtime");
    assert(xi_own_type_is_rc(&channel_type) && "Channel is atomic shared-RC, compiler-tracked");
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
    assert(contains(code, "_aot_release(void *frame, struct XrCoroHeap *heap)") &&
           "frame release must receive coroutine heap context");
    assert(contains(code, "xrt_release(xr_str_value_from_ptr(f->v") &&
           "AOT ARC string ptr slot should be released after restoring XrValue tag");
    assert(contains(code, "xr_aot_trace_frame_value(visitor, xr_str_value_from_ptr(f->v") &&
           "AOT ARC string ptr slot should be traced after restoring XrValue tag");
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
    assert(contains(code, "void * _raw_result = test_identity_copy_") &&
           "sync go wrapper must support pointer results that alias frame params");
    assert(contains(code, "XrValue _result = XR_FROM_INT(_raw_result)") &&
           "sync go wrapper must box native scalar results for the coroutine ABI");
    assert(contains(code, "XrValue _result = xr_mkptr(_raw_result, XR_TAG_ARRAY)") &&
           "sync go wrapper must box native pointer results for the coroutine ABI");
    assert(contains(code, "xrt_retain(_result)") &&
           "sync go wrapper must retain a result that aliases an owned frame param");
    assert(contains(code, "xrt_release(xr_mkptr(f->p0, XR_TAG_ARRAY))") &&
           "sync go wrapper must release cloned pointer frame params after restoring the tag");
    assert(contains(code, ".release_count = 1,") &&
           "sync go wrapper descriptor must report cloned tagged param releases");
    assert(contains(code, "xr_aot_done(_result)") &&
           "sync go wrapper must complete through the AOT coroutine result ABI");
    assert(contains(code, "xr_aot_trace_frame_value(visitor, xr_mkptr(f->p0, XR_TAG_ARRAY))") &&
           "sync go wrapper pointer params must remain traceable while queued");
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

TEST(cgen_coro_go_zero_state_sync_wrapper_has_nonempty_frame) {
    const char *src = "fn one() -> int {\n"
                      "    return 1\n"
                      "}\n"
                      "let task = go one()\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    XiCgenCoroFrameStats stats = {0};
    char *code = generate_c_with_status_and_stats(ir, "test", &had_error, &stats);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT go of zero-state sync function should generate");
    assert(contains(code, "uint8_t _empty;") &&
           "zero-state sync-go frame must be non-empty and C-portable");
    assert(contains(code, "xr_aot_frame_alloc(sizeof(*f))") &&
           "zero-state sync-go wrapper must still allocate a backend frame");
    assert(stats.coroutine_count == 2 &&
           "frame stats should count the main coroutine and zero-state sync-go frame");
    assert(stats.max_frame_bytes >= 1 && "zero-state sync-go frame must report non-zero size");

    printf("  Generated zero-state sync-go wrapper %zu bytes of C code\n", strlen(code));
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

TEST(cgen_coro_unit_match_send_omits_void_phi) {
    const char *src = "fn recv_timeout_until_close(ch: Channel<int>, done: Channel<int>) {\n"
                      "    match (ch.recvTimeout(1)) {\n"
                      "        Recv.Value(value) -> done.send(value)\n"
                      "        _ -> done.send(-1)\n"
                      "    }\n"
                      "}\n"
                      "let ch = new Channel<int>(0)\n"
                      "let done = new Channel<int>(1)\n"
                      "go recv_timeout_until_close(ch, done)\n"
                      "ch.close()\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT unit match coroutine should generate");
    assert(contains(code, "xr_aot_chan_send_i64(ctx,") &&
           "unit match branches should still emit typed channel sends");
    assert(!contains(code, "phi") && "unit/void phi values should not materialize in C");

    printf("  Generated unit match send without void phi %zu bytes of C code\n", strlen(code));
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
    assert(!contains(code, "xr_aot_poll_yield(ctx)") &&
           "nonblocking trySend must not own a suspend/poll state");
    assert(!contains(code, "xr_aot_chan_try_send(ctx,") &&
           "scalar channel trySend must not re-box at the generated call site");
    assert(count_between(code, code + strlen(code), "XR_FROM_INT(") == 1 &&
           "scalar channel trySend should not emit a dead boxed send operand");
    assert(!contains(code, "xrt_value_clone_for_coro(") &&
           "scalar channel trySend values must not call the deep-copy helper");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(") &&
           "trySend returns a native no-payload SendResult enum and must not be bridged");

    printf("  Generated scalar channel trySend %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_builtin_no_payload_enum_fields_skip_bridge) {
    const char *src = "fn read_builtin_enums() -> int {\n"
                      "    yield\n"
                      "    let sent = SendResult.Sent\n"
                      "    let closed = Recv.Closed\n"
                      "    let pending = TaskResult.Pending\n"
                      "    let status = TaskStatus.Success\n"
                      "    print(sent)\n"
                      "    print(closed)\n"
                      "    print(pending)\n"
                      "    print(status)\n"
                      "    return 1\n"
                      "}\n"
                      "let task = go read_builtin_enums()\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT builtin enum field coroutine should generate");
    assert(contains(code, "xr_aot_load_builtin_field(ctx,") &&
           "builtin enum fields should use the AOT builtin field helper");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(_builtin_field_") &&
           "no-payload builtin enum fields must be returned as native AOT enum keys");

    printf("  Generated builtin no-payload enum field bridge skip %zu bytes of C code\n",
           strlen(code));
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

    const char *main_plus = strstr(code, "test_main_plus_");
    const char *resume =
        main_plus ? strstr(main_plus, "_aot_resume(void *raw_frame, const XrAotContext *ctx) {")
                  : NULL;
    const char *trace = resume ? strstr(resume, "test_main_plus_") : NULL;
    trace = trace ? strstr(trace, "_aot_trace(void *frame") : NULL;
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

TEST(cgen_coro_fused_scalar_channel_recv_uses_typed_pair_bridge) {
    const char *src = "fn recv_or(ch: Channel<int>, fallback: int) -> int {\n"
                      "    return match (ch.recv()) {\n"
                      "        Recv.Value(n) -> n\n"
                      "        _ -> fallback\n"
                      "    }\n"
                      "}\n"
                      "let ch = new Channel<int>(1)\n"
                      "ch.send(9)\n"
                      "let task = go recv_or(ch, -1)\n"
                      "let result = await task\n"
                      "print(result)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT fused scalar channel recv should generate");
    assert(contains(code, "xr_aot_chan_recv_pair_i64(ctx,") &&
           "fused scalar channel recv must use the typed pair bridge");
    assert(!contains(code, "xr_aot_chan_recv_pair(ctx,") &&
           "fused scalar channel recv must not call the generic pair bridge");
    assert(contains(code, "S1:;\n    f->state = 0;\n    _chan_recv_slot_") &&
           "fused channel recv resume must rebuild the frame slot after jumping to the label");

    printf("  Generated fused scalar channel recv %zu bytes of C code\n", strlen(code));
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

    const char *recv_plus = strstr(code, "test_recv_plus_");
    const char *resume =
        recv_plus ? strstr(recv_plus, "_aot_resume(void *raw_frame, const XrAotContext *ctx) {")
                  : NULL;
    const char *trace = resume ? strstr(resume, "test_recv_plus_") : NULL;
    trace = trace ? strstr(trace, "_aot_trace(void *frame") : NULL;
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
    assert(!contains(code, "xr_aot_poll_yield(ctx)") &&
           "nonblocking tryRecv must not own a suspend/poll state");
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

TEST(cgen_runtime_needed_main_uses_aot_runtime) {
    const char *src = "import time\n"
                      "time.sleep(5)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT sleep should generate");
    assert(contains(code, "XrAotRuntimeConfig runtime_cfg;") &&
           "runtime-needed generated main must create an AOT runtime config");
    assert(contains(code, "XrAotRuntime *rt = xr_aot_runtime_new(&runtime_cfg);") &&
           "runtime-needed generated main must create XrAotRuntime directly");
    assert(contains(code, "runtime_cfg.file = \"test.xr\";") &&
           "runtime-needed generated main must pass entry source path to AOT runtime");
    assert(contains(code, "xrt_global_ctx.runtime = rt;") &&
           "generated sync helpers must see the AOT runtime owner");
    assert(contains(code, "xr_aot_run_main(rt,") &&
           "generated coroutine main must call the final runtime API");
    assert(contains(code, "xr_aot_runtime_delete(rt);") &&
           "generated main must tear down XrAotRuntime directly");
    assert(!contains(code, "XrayIsolateParams") &&
           "generated main must not construct VM isolate params");
    assert(!contains(code, "xray_isolate_new(") &&
           "generated main must not construct a VM isolate");
    assert(!contains(code, "xr_multicore_init(") &&
           "generated main must not initialize scheduler through isolate");
    assert(!contains(code, "xr_aot_run_main_vm_bridge(") &&
           "generated main must not use the VM bridge entry");
    assert(!contains(code, "xray_isolate_delete(") &&
           "generated main must not tear down a VM isolate");

    printf("  Generated AOT runtime main %zu bytes of C code\n", strlen(code));
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
    const char *send_timeout = strstr(code, "XrAotResult _chan_send_timeout_");
    const char *recv_timeout =
        send_timeout ? strstr(send_timeout, "XrAotResult _chan_recv_timeout_") : NULL;
    assert(send_timeout != NULL && recv_timeout != NULL && "timeout send/recv blocks should exist");
    assert(count_between(send_timeout, recv_timeout, "xr_aot_bridge_value_to_xrt(") == 0 &&
           "sendTimeout result is a native no-payload SendResult enum and must not be bridged");
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

TEST(cgen_sync_go_channel_try_methods_use_aot_helpers) {
    const char *src = "fn recv_value(r: Recv<int>) -> int {\n"
                      "    return match (r) {\n"
                      "        Recv.Value(v) -> v\n"
                      "        _ -> 0\n"
                      "    }\n"
                      "}\n"
                      "fn producer(ch: Channel<int>) -> int {\n"
                      "    ch.trySend(1)\n"
                      "    ch.trySend(2)\n"
                      "    return 0\n"
                      "}\n"
                      "fn consumer(ch: Channel<int>) -> int {\n"
                      "    return recv_value(ch.tryRecv())\n"
                      "}\n"
                      "fn close_and_check(ch: Channel<int>) -> bool {\n"
                      "    ch.close()\n"
                      "    return ch.isClosed()\n"
                      "}\n"
                      "shared const ch = new Channel<int>(2)\n"
                      "let p = go producer(ch)\n"
                      "print(await p)\n"
                      "let c = go consumer(ch)\n"
                      "print(await c)\n"
                      "let closed = go close_and_check(ch)\n"
                      "print(await closed)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT sync-go Channel nonblocking methods should generate");
    assert(contains(code, "xr_aot_chan_try_send_sync_i64(") &&
           "sync-go Channel.trySend should use the scalar sync AOT bridge");
    assert(contains(code, "xr_aot_chan_try_recv_sync(") &&
           "sync-go Channel.tryRecv should use the sync AOT bridge");
    assert(contains(code, "xr_aot_chan_close_sync(") &&
           "sync-go Channel.close should use the sync AOT bridge");
    assert(contains(code, "xr_aot_chan_is_closed_sync(") &&
           "sync-go Channel.isClosed should use the sync AOT bridge");
    assert(!contains(code, "xrt_method_0(") && !contains(code, "xrt_method_1(") &&
           "Channel nonblocking methods must not fall back to dynamic method dispatch");

    printf("  Generated sync-go channel method helpers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_work_queue_resume_rebuilds_slot_and_traces_task) {
    const char *src = "shared const queue: WorkQueue<int> = WorkQueue<int>(1, 4)\n"
                      "fn consumer() -> int {\n"
                      "    let item = queue.pop(0)\n"
                      "    return item ?? 0\n"
                      "}\n"
                      "let task = go consumer()\n"
                      "assert_true(queue.push(7, 0))\n"
                      "print(await task)\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT WorkQueue pop should generate");
    assert(contains(code, "xr_aot_work_queue_pop(ctx,") &&
           "initial WorkQueue.pop must use the AOT bridge");
    assert(contains(code, "S1:;\n    f->state = 1;\n    _wq_pop_slot_") &&
           "WorkQueue.pop resume must rebuild the frame slot after jumping to the label");
    assert(contains(code, "xr_aot_work_queue_pop_resume(ctx, _wq_pop_slot_") &&
           "WorkQueue.pop resume must use the rebuilt slot");
    assert(contains(code, "xr_aot_trace_frame_value(visitor, f->v") &&
           "go-created Task values kept across spawn continuation must be traced");

    printf("  Generated WorkQueue resume slot rebuild %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_work_queue_native_methods_use_aot_helpers) {
    const char *src = "shared const queue: WorkQueue<int> = WorkQueue<int>(4, 2)\n"
                      "fn use_queue() -> int {\n"
                      "    assert_true(queue.push(1, 0))\n"
                      "    let (value, ok) = queue.tryPop(0)\n"
                      "    if (!ok) { return -1 }\n"
                      "    if (queue.isClosed) { return -2 }\n"
                      "    return value + queue.length + queue.shardCount\n"
                      "}\n"
                      "print(use_queue())\n";

    XiFunc *ir = compile_to_ir(src);
    assert(ir != NULL && "IR compilation failed");

    bool had_error = false;
    char *code = generate_c_with_status(ir, "test", &had_error);
    assert(code != NULL && "C code generation failed");
    assert(!had_error && "AOT WorkQueue native methods should generate");
    assert(contains(code, "xr_aot_work_queue_push_sync(") &&
           "WorkQueue.push should use the sync AOT bridge outside suspendable code");
    assert(contains(code, "xr_aot_work_queue_try_pop_sync(") &&
           "WorkQueue.tryPop should use the sync AOT bridge outside suspendable code");
    assert(contains(code, "xr_aot_work_queue_length(") &&
           "WorkQueue.length should read through the AOT helper");
    assert(contains(code, "xr_aot_work_queue_shard_count(") &&
           "WorkQueue.shardCount should read through the AOT helper");
    assert(contains(code, "xr_aot_work_queue_is_closed(") &&
           "WorkQueue.isClosed should read through the AOT helper");
    assert(contains(code, "runtime_cfg.caps = XR_AOT_CAP_WORK_QUEUE;") &&
           "sync WorkQueue main must create a work-queue-capable AOT runtime");
    assert(contains(code, "xrt_global_ctx.runtime = rt;") &&
           "sync WorkQueue helpers must receive a runtime-backed global context");
    assert(!contains(code, "xray_isolate_new(") && "sync WorkQueue main must not use a VM isolate");
    assert(!contains(code, "xrt_method_0(") && !contains(code, "xrt_method_1(") &&
           "WorkQueue native methods must not fall back to dynamic method dispatch");

    printf("  Generated WorkQueue native method helpers %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

TEST(cgen_coro_task_status_uses_native_enum_status) {
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
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(xr_aot_task_status") &&
           "Task.status must already return AOT-native enum values");
    assert(contains(code, "xr_aot_task_poll(ctx,") && "Task.poll must use the AOT Task bridge");
    assert(contains(code, "xr_aot_task_await_result(ctx,") &&
           "Task.awaitResult must use the AOT TaskResult bridge");
    assert(contains(code, "xr_aot_task_done(&xrt_global_ctx,") &&
           "sync AOT Task.done must use the global AOT context");
    assert(contains(code, "xr_aot_task_status(&xrt_global_ctx,") &&
           "sync AOT Task.status must use the global AOT context");
    assert(!contains(code, "xr_aot_bridge_value_to_xrt(xr_aot_task_status(&xrt_global_ctx") &&
           "sync Task.status must already return AOT-native enum values");
    assert(contains(code, "xr_aot_task_poll(&xrt_global_ctx,") &&
           "sync AOT Task.poll must use the global AOT context");
    assert(!contains(code, "xr_aot_task_poll(NULL,") &&
           "sync AOT Task.poll must not lose runtime/builtin context");
    assert(!contains(code, "xrt_method_0(v") &&
           "Task.cancel must not fall back to AOT dynamic method dispatch");
    assert(!contains(code, "xrt_getprop(v") &&
           "Task fields must not fall back to AOT dynamic property dispatch");

    printf("  Generated native Task.status helper %zu bytes of C code\n", strlen(code));
    xr_free(code);
    xi_func_free(ir);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi CGen Unit Tests ===\n\n");

    setup();

    run_cgen_simple_arith();
    run_cgen_skips_unused_process_builtin_init();
    run_cgen_initializes_used_process_builtin();
    run_cgen_initializes_file_dir_builtins_from_entry_source();
    run_cgen_runtime_file_dir_stays_runtime_owned();
    run_cgen_standalone_prelude_enum_globals_generate_static_members();
    run_cgen_cancelled_builtin_generates_false();
    run_cgen_variable_and_print();
    run_cgen_if_else();
    run_cgen_multi_print();
    run_cgen_while_loop();
    run_cgen_string_literal();
    run_cgen_str_concat_uses_single_allocation_helper();
    run_cgen_function_call();
    run_cgen_c_export_emits_public_c_abi_wrapper();
    run_cgen_stats_tracks_native_abi();
    run_cgen_module_prefix_is_c_identifier();
    run_cgen_emits_source_line_directives();
    run_cgen_emits_debug_source_var_slots();
    run_cgen_emits_shadowed_debug_source_var_slots();
    run_cgen_struct_debug_source_var_slots_use_typed_pointers();
    run_cgen_coro_emits_source_line_directives();
    run_cgen_coro_emits_debug_source_var_slots();
    run_cgen_coro_syncs_helper_result_debug_source_vars();
    run_cgen_recursive();
    run_cgen_for_loop();
    run_cgen_typed_array_uses_raw_storage_fast_path();
    run_cgen_typed_array_u8_uses_byte_storage_fast_path();
    run_cgen_bytes_methods_use_raw_memory_helpers();
    run_cgen_typed_array_i16_and_u32_use_raw_storage_fast_path();
    run_cgen_typed_array_float_and_bool_use_raw_storage_fast_path();
    run_cgen_inlined_struct_uses_native_field_storage();
    run_cgen_escaping_struct_uses_heap_native_storage();
    run_cgen_same_shape_structs_keep_distinct_source_field_names();
    run_cgen_escaping_struct_string_field_uses_heap_native_storage();
    run_cgen_repr_c_struct_omits_native_header();
    run_cgen_nested_struct_field_uses_embedded_heap_native_storage();
    run_cgen_fixed_array_struct_field_uses_embedded_heap_native_storage();
    run_cgen_shared_struct_alias_elides_tagged_hot_locals();
    run_cgen_class_method_caches_receiver_scalar_fields();
    run_cgen_local_class_direct_native_methods_omit_boxed_adapters();
    run_cgen_class_constructor_returns_heap_native_instance();
    run_cgen_native_class_ref_field_constructor_result_uses_ptr_storage();
    run_cgen_native_class_collection_ref_fields_skip_noop_arc();
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
    run_cgen_dynamic_uncaptured_callback_keeps_boxed_adapter();
    run_cgen_closure_cell_var_id_above_255();
    run_cgen_stack_alloc_direct_closure_uses_stack_runtime();
    run_cgen_stack_alloc_closure_preserves_cell_capture();
    run_cgen_typed_array_filter_readonly_result_caches_data_pointer();
    run_cgen_typed_array_filter_captured_callback_uses_runtime_helper();
    run_cgen_typed_array_reduce_uses_native_accumulator_fast_path();
    run_cgen_typed_array_reduce_captured_callback_uses_runtime_helper();
    run_cgen_int_const_div_mod_uses_native_ops();
    run_cgen_defer_isolates_existing_pending_error();
    run_cgen_err_return_stops_unreachable_tail();
    run_cgen_unsupported_coroutine_ops_fail_fast();
    run_cgen_unresolved_import_fails_fast();
    run_cgen_unknown_method_symbol_fails_fast();
    run_cgen_suspendable_wrapper_aborts();
    run_cgen_direct_suspend_call_propagates_cps();
    run_cgen_direct_suspend_method_call_propagates_cps();
    run_cgen_shared_static_function_retain_is_elided();
    run_cgen_coro_shared_static_function_retain_is_elided();
    run_cgen_suspendable_dependency_init_fails_fast();
    run_cgen_coro_frame_params_use_typed_storage();
    run_cgen_coro_frame_skips_dead_ssa_slots();
    run_cgen_coro_loop_tail_phi_survives_poll_suspend();
    run_cgen_runtime_managed_types_skip_arc();
    run_cgen_coro_frame_release_uses_aot_arc();
    run_cgen_coro_go_clones_tagged_args();
    run_cgen_coro_go_sync_function_uses_wrapper_desc();
    run_cgen_coro_go_sync_scalar_wrapper_skips_param_roots();
    run_cgen_coro_go_zero_state_sync_wrapper_has_nonempty_frame();
    run_cgen_sync_functions_without_go_emit_no_aot_wrappers();
    run_cgen_coro_sync_go_wrappers_only_for_go_targets();
    run_cgen_coro_channel_send_clones_value();
    run_cgen_coro_scalar_channel_send_skips_clone();
    run_cgen_coro_unit_match_send_omits_void_phi();
    run_cgen_coro_scalar_channel_try_send_uses_typed_bridge();
    run_cgen_coro_builtin_no_payload_enum_fields_skip_bridge();
    run_cgen_coro_await_clones_tagged_result();
    run_cgen_coro_scalar_await_uses_tagged_slot();
    run_cgen_coro_await_timeout_passes_deadline();
    run_cgen_tagged_null_equality_keeps_null_literal();
    run_cgen_coro_recv_resume_uses_wait_state_slot();
    run_cgen_coro_fused_scalar_channel_recv_uses_typed_pair_bridge();
    run_cgen_coro_scalar_channel_recv_uses_tagged_slot();
    run_cgen_coro_channel_recv_null_check_keeps_tagged_slot();
    run_cgen_coro_scalar_channel_try_recv_returns_recv_enum();
    run_cgen_coro_select_try_recv_uses_ready_bit();
    run_cgen_coro_sleep_publishes_state_before_block();
    run_cgen_runtime_needed_main_uses_aot_runtime();
    run_cgen_coro_select_publishes_state_before_block();
    run_cgen_coro_channel_timeout_publishes_state_before_block();
    run_cgen_coro_recv_slot_is_traced_as_frame_root();
    run_cgen_coro_await_all_uses_aggregate_bridge();
    run_cgen_coro_await_any_uses_typed_aggregate_bridge();
    run_cgen_coro_scope_exit_publishes_state_before_block();
    run_cgen_channel_fields_use_aot_helpers();
    run_cgen_sync_go_channel_try_methods_use_aot_helpers();
    run_cgen_coro_work_queue_resume_rebuilds_slot_and_traces_task();
    run_cgen_work_queue_native_methods_use_aot_helpers();
    run_cgen_coro_task_status_uses_native_enum_status();

    teardown();

    printf("\n=== %d/%d Xi CGen tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
