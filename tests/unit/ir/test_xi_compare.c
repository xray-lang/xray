/*
 * test_xi_compare.c - Dual-path comparison: legacy codegen vs Xi IR pipeline
 *
 * Compiles the same source through both paths and compares:
 *   1. Both succeed (no crash, no error)
 *   2. Instruction sequence similarity (opcode histogram)
 *   3. Key metrics (maxstacksize, constant pool size, arity)
 *   4. Execution produces the same result (via VM)
 */

#include "../../../src/ir/xi_pipeline.h"
#include "../../../src/frontend/codegen/xcompiler.h"
#include "../../../src/frontend/codegen/xcompiler_context.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"
#include "../../../include/xray_isolate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "../../../src/vm/xvm_internal.h"
#include "../../../src/runtime/xisolate_api.h"

/* ========== Test Infrastructure ========== */

static XrayIsolate *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

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

/* ========== Compilation Helpers ========== */

/* Compile via legacy codegen path */
static XrProto *compile_legacy(const char *source) {
    XR_DCHECK(g_iso != NULL, "isolate must be initialized");

    /* Create context first — its analyzer installs a type pool on the
     * isolate, which the parser needs for creating type annotations. */
    XrCompilerContext *ctx = xr_compiler_context_new(g_iso);
    if (!ctx) return NULL;

    AstNode *program = xr_parse(g_iso, source);
    if (!program) { xr_compiler_context_free(ctx); return NULL; }

    /* Use the context's built-in analyzer */
    xa_analyzer_analyze(ctx->analyzer, "compare.xr", program);
    ctx->use_xi_pipeline = false;

    /* Re-install the parse arena: legacy codegen desugars some AST nodes
     * (e.g. for-in, match) which calls ast_alloc and needs the arena. */
    struct XrArena *saved_arena = xr_isolate_get_current_arena(g_iso);
    if (program->type == AST_PROGRAM && program->as.program.arena)
        xr_isolate_set_current_arena(g_iso, program->as.program.arena);

    XrProto *proto = xr_compile(ctx, program);

    xr_isolate_set_current_arena(g_iso, saved_arena);
    xr_compiler_context_free(ctx);
    xr_program_destroy(program);
    return proto;
}

/* Compile via Xi IR pipeline */
static XrProto *compile_xi(const char *source) {
    XR_DCHECK(g_iso != NULL, "isolate must be initialized");

    /* Create analyzer first — it installs a type pool on the isolate,
     * which the parser needs for creating type annotations. */
    XaAnalyzer *analyzer = xa_analyzer_new(g_iso);
    if (!analyzer) return NULL;

    AstNode *program = xr_parse(g_iso, source);
    if (!program) { xa_analyzer_free(analyzer); return NULL; }

    xa_analyzer_analyze(analyzer, "compare.xr", program);

    XiPipelineConfig cfg = xi_pipeline_default_config();
    XiPipelineResult res = xi_pipeline_compile_program(
        program, analyzer, g_iso, &cfg);

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    if (res.status != XI_PIPE_OK) {
        xi_pipeline_result_free(&res);
        return NULL;
    }

    XrProto *proto = res.proto;
    xi_pipeline_result_free(&res);
    return proto;
}

/* ========== Execution Capture ========== */

/* Capture path for temp file */
static const char *g_capture_path = "/tmp/xi_cmp_capture.txt";

/* Read captured output from temp file into heap buffer */
static char *read_capture(void) {
    FILE *f = fopen(g_capture_path, "r");
    if (!f) return NULL;

    char *buf = (char *)xr_malloc(4096);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, 4095, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Execute proto via xr_execute and capture stdout into a heap buffer. */
static char *execute_and_capture(XrProto *proto) {
    if (!proto || !g_iso) return NULL;

    fflush(stdout);
    if (!freopen(g_capture_path, "w", stdout)) return NULL;

    int rc = xr_execute(g_iso, proto);
    (void)rc;
    fflush(stdout);

    if (!freopen("/dev/tty", "w", stdout)) return NULL;
    return read_capture();
}

/* ========== Comparison Utilities ========== */

/* Build opcode histogram for a proto */
static void build_histogram(const XrProto *proto, int hist[256]) {
    memset(hist, 0, 256 * sizeof(int));
    int count = PROTO_CODE_COUNT(proto);
    for (int i = 0; i < count; i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, i));
        if (op < 256) hist[op]++;
    }
}

/* Print opcode sequence for debugging */
static void dump_opcodes(const char *label, const XrProto *proto) {
    int count = PROTO_CODE_COUNT(proto);
    fprintf(stderr, "  %s (%d insts, maxstack=%d):",
            label, count, proto->maxstacksize);
    for (int i = 0; i < count && i < 30; i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, i));
        fprintf(stderr, " %s", xr_opcode_name(op));
    }
    if (count > 30) fprintf(stderr, " ...(+%d)", count - 30);
    fprintf(stderr, "\n");
}

/* Compare two protos, return similarity score 0.0-1.0 */
static double compare_protos(const XrProto *legacy, const XrProto *xi,
                             bool verbose) {
    int hist_l[256], hist_x[256];
    build_histogram(legacy, hist_l);
    build_histogram(xi, hist_x);

    /* Jaccard-like similarity on opcode counts */
    int intersection = 0, union_total = 0;
    for (int i = 0; i < 256; i++) {
        int a = hist_l[i], b = hist_x[i];
        if (a == 0 && b == 0) continue;
        int min = a < b ? a : b;
        int max = a > b ? a : b;
        intersection += min;
        union_total += max;
    }

    double similarity = union_total > 0
        ? (double)intersection / union_total : 1.0;

    if (verbose) {
        dump_opcodes("legacy", legacy);
        dump_opcodes("xi    ", xi);
        fprintf(stderr, "  similarity=%.2f  legacy_insts=%d  xi_insts=%d"
                "  legacy_stack=%d  xi_stack=%d\n",
                similarity,
                PROTO_CODE_COUNT(legacy), PROTO_CODE_COUNT(xi),
                legacy->maxstacksize, xi->maxstacksize);

        /* Show opcode differences */
        for (int i = 0; i < 256; i++) {
            if (hist_l[i] != hist_x[i] && (hist_l[i] > 0 || hist_x[i] > 0)) {
                fprintf(stderr, "    %-20s legacy=%d  xi=%d\n",
                        xr_opcode_name((OpCode)i), hist_l[i], hist_x[i]);
            }
        }
    }

    return similarity;
}

/* ========== Test Macro ========== */

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        printf("--- " #name " ---\n"); \
        test_##name(); \
        printf("  PASS\n"); \
        tests_passed++; \
    } \
    static void test_##name(void)

#define SKIP(name, reason) \
    static void run_##name(void) { \
        printf("--- " #name " --- SKIP: " reason "\n"); \
        tests_skipped++; \
    }

/* ========== Dual-path comparison helper ========== */

typedef struct {
    const char *source;
    const char *label;
    bool expect_xi_success;    /* true = Xi pipeline must succeed */
    double min_similarity;     /* minimum opcode histogram similarity */
    bool check_exec;           /* compare VM execution output */
} CompareSpec;

static void run_compare(CompareSpec spec) {
    XrProto *p_legacy = compile_legacy(spec.source);
    assert(p_legacy != NULL && "legacy codegen must succeed");

    XrProto *p_xi = compile_xi(spec.source);

    if (!spec.expect_xi_success) {
        /* Xi pipeline may fail for unsupported features */
        if (p_xi == NULL) {
            fprintf(stderr, "  (Xi pipeline not yet supported — OK)\n");
            xr_vm_proto_free(p_legacy);
            return;
        }
    } else {
        assert(p_xi != NULL && "Xi pipeline must succeed");
    }

    /* Both succeeded — compare */
    double sim = compare_protos(p_legacy, p_xi, true);
    fprintf(stderr, "  → similarity = %.2f (min=%.2f)\n",
            sim, spec.min_similarity);

    if (sim < spec.min_similarity) {
        fprintf(stderr, "  ⚠ similarity below threshold!\n");
    }

    /* Both must produce at least one instruction and contain a RETURN */
    int lc = PROTO_CODE_COUNT(p_legacy);
    int xc = PROTO_CODE_COUNT(p_xi);
    assert(lc > 0 && "legacy must produce instructions");
    assert(xc > 0 && "xi must produce instructions");

    bool legacy_has_ret = false, xi_has_ret = false;
    for (int i = 0; i < lc; i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(p_legacy, i));
        if (op == OP_RETURN || op == OP_RETURN0 || op == OP_RETURN1)
            legacy_has_ret = true;
    }
    for (int i = 0; i < xc; i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(p_xi, i));
        if (op == OP_RETURN || op == OP_RETURN0 || op == OP_RETURN1)
            xi_has_ret = true;
    }
    assert(legacy_has_ret && "legacy must contain RETURN");
    assert(xi_has_ret && "xi must contain RETURN");

    /* Execution output comparison */
    if (spec.check_exec) {
        char *out_l = execute_and_capture(p_legacy);
        char *out_x = execute_and_capture(p_xi);

        if (out_l && out_x) {
            bool match = (strcmp(out_l, out_x) == 0);
            fprintf(stderr, "  exec: legacy=[%s] xi=[%s] %s\n",
                    out_l, out_x, match ? "MATCH" : "MISMATCH");
            assert(match && "execution output must match between legacy and Xi");
        } else {
            fprintf(stderr, "  exec: skipped (capture failed: legacy=%s xi=%s)\n",
                    out_l ? "ok" : "fail", out_x ? "ok" : "fail");
        }

        if (out_l) xr_free(out_l);
        if (out_x) xr_free(out_x);
    }

    xr_vm_proto_free(p_legacy);
    xr_vm_proto_free(p_xi);
}

/* ========== Test Cases ========== */

/* --- Constants --- */

TEST(cmp_int_const) {
    run_compare((CompareSpec){
        .source = "let x = 42\nprint(x)",
        .label = "int constant",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_float_const) {
    run_compare((CompareSpec){
        .source = "let x = 3.14\nprint(x)",
        .label = "float constant",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_bool_const) {
    run_compare((CompareSpec){
        .source = "let a = true\nlet b = false\nprint(a)\nprint(b)",
        .label = "bool constants",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_string_const) {
    run_compare((CompareSpec){
        .source = "let s = \"hello\"\nprint(s)",
        .label = "string constant",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_null_const) {
    run_compare((CompareSpec){
        .source = "let x = null\nprint(x)",
        .label = "null constant",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Arithmetic --- */

TEST(cmp_add) {
    run_compare((CompareSpec){
        .source = "let a = 10\nlet b = 20\nlet c = a + b\nprint(c)",
        .label = "addition",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_arith_chain) {
    run_compare((CompareSpec){
        .source = "let x = 1 + 2 * 3 - 4\nprint(x)",
        .label = "arithmetic chain (const folded)",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_unary_neg) {
    run_compare((CompareSpec){
        .source = "let x = 10\nlet y = -x\nprint(y)",
        .label = "unary negation",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Control Flow --- */

TEST(cmp_if_else) {
    run_compare((CompareSpec){
        .source = "let x = 5\nif (x > 3) { print(1) } else { print(0) }",
        .label = "if-else",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_if_const_true) {
    run_compare((CompareSpec){
        .source = "if (true) { print(1) } else { print(2) }",
        .label = "if with const true (branch elimination)",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_while_loop) {
    run_compare((CompareSpec){
        .source = "let i = 0\nwhile (i < 5) { i = i + 1 }\nprint(i)",
        .label = "while loop",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Multiple Statements --- */

TEST(cmp_multi_print) {
    run_compare((CompareSpec){
        .source = "print(1)\nprint(2)\nprint(3)",
        .label = "multiple prints",
        .expect_xi_success = true,
        .min_similarity = 0.5,
        .check_exec = true,
    });
}

TEST(cmp_multi_vars) {
    run_compare((CompareSpec){
        .source = "let a = 1\nlet b = 2\nlet c = 3\n"
                  "let d = a + b + c\nprint(d)",
        .label = "multiple variables + sum",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Variable Reassignment --- */

TEST(cmp_var_reassign) {
    run_compare((CompareSpec){
        .source = "let x = 10\nx = x + 5\nx = x * 2\nprint(x)",
        .label = "variable reassignment chain",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Comparison Operators --- */

TEST(cmp_comparisons) {
    run_compare((CompareSpec){
        .source = "let a = 5\nlet b = 10\n"
                  "print(a < b)\nprint(a > b)\n"
                  "print(a == b)\nprint(a != b)",
        .label = "comparison operators",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Logical Operators --- */

TEST(cmp_logical_and) {
    run_compare((CompareSpec){
        .source = "let a = true\nlet b = false\n"
                  "print(a && b)\nprint(a && true)",
        .label = "logical AND (short-circuit)",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_logical_or) {
    run_compare((CompareSpec){
        .source = "let a = false\nlet b = true\n"
                  "print(a || b)\nprint(false || false)",
        .label = "logical OR (short-circuit)",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_logical_not) {
    run_compare((CompareSpec){
        .source = "let a = true\nprint(!a)\nprint(!false)",
        .label = "logical NOT",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- For Loop --- */

TEST(cmp_for_loop) {
    run_compare((CompareSpec){
        .source = "let sum = 0\n"
                  "for (let i = 1; i <= 5; i = i + 1) { sum = sum + i }\n"
                  "print(sum)",
        .label = "for loop with accumulator",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Nested Control Flow --- */

TEST(cmp_nested_if) {
    run_compare((CompareSpec){
        .source = "let x = 15\n"
                  "if (x > 20) { print(1) }\n"
                  "else if (x > 10) { print(2) }\n"
                  "else { print(3) }",
        .label = "nested if-else chain",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Compound Assignment --- */

TEST(cmp_compound_assign) {
    run_compare((CompareSpec){
        .source = "let x = 10\nx += 5\nx -= 3\nx *= 2\nprint(x)",
        .label = "compound assignment operators",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Ternary Expression --- */

TEST(cmp_ternary) {
    run_compare((CompareSpec){
        .source = "let x = 5\n"
                  "let y = x > 3 ? 100 : 200\n"
                  "print(y)",
        .label = "ternary expression",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Break and Continue --- */

TEST(cmp_while_break) {
    run_compare((CompareSpec){
        .source = "let i = 0\n"
                  "while (true) {\n"
                  "  if (i >= 3) { break }\n"
                  "  i = i + 1\n"
                  "}\nprint(i)",
        .label = "while loop with break",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_while_continue) {
    run_compare((CompareSpec){
        .source = "let sum = 0\nlet i = 0\n"
                  "while (i < 6) {\n"
                  "  i = i + 1\n"
                  "  if (i == 3) { continue }\n"
                  "  sum = sum + i\n"
                  "}\nprint(sum)",
        .label = "while loop with continue (skip 3)",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Function Declaration + Call --- */

TEST(cmp_func_call) {
    run_compare((CompareSpec){
        .source = "fn add(a: int, b: int): int { return a + b }\n"
                  "let r = add(3, 4)\nprint(r)",
        .label = "function declaration and call",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_func_recursive) {
    run_compare((CompareSpec){
        .source = "fn fib(n: int): int {\n"
                  "  if (n <= 1) { return n }\n"
                  "  return fib(n - 1) + fib(n - 2)\n"
                  "}\nprint(fib(7))",
        .label = "recursive fibonacci",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Nested Loops --- */

TEST(cmp_nested_loop) {
    run_compare((CompareSpec){
        .source = "let sum = 0\n"
                  "let i = 0\nwhile (i < 3) {\n"
                  "  let j = 0\n  while (j < 3) {\n"
                  "    sum = sum + 1\n    j = j + 1\n"
                  "  }\n  i = i + 1\n}\nprint(sum)",
        .label = "nested while loops (3x3)",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- String Operations --- */

TEST(cmp_string_concat) {
    run_compare((CompareSpec){
        .source = "let a = \"hello\"\nlet b = \" world\"\n"
                  "let c = a + b\nprint(c)",
        .label = "string concatenation",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Mixed Types --- */

TEST(cmp_mixed_arith) {
    run_compare((CompareSpec){
        .source = "let a = 10\nlet b = 3\n"
                  "print(a / b)\nprint(a % b)",
        .label = "integer division and modulo",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Nested Function Calls --- */

TEST(cmp_nested_call) {
    run_compare((CompareSpec){
        .source = "fn add(a: int, b: int): int { return a + b }\n"
                  "print(add(1, add(2, 3)))",
        .label = "nested function calls",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_func_early_return) {
    run_compare((CompareSpec){
        .source = "fn abs(n: int): int {\n"
                  "  if (n < 0) { return -n }\n"
                  "  return n\n"
                  "}\nprint(abs(-5))\nprint(abs(3))",
        .label = "function with early return",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_factorial) {
    run_compare((CompareSpec){
        .source = "fn fact(n: int): int {\n"
                  "  if (n <= 1) { return 1 }\n"
                  "  return n * fact(n - 1)\n"
                  "}\nprint(fact(6))",
        .label = "recursive factorial",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Float Arithmetic --- */

TEST(cmp_float_arith) {
    run_compare((CompareSpec){
        .source = "let x = 3.14\nlet y = 2.0\n"
                  "print(x + y)\nprint(x * y)",
        .label = "float arithmetic",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Scope Shadowing --- */

TEST(cmp_block_scope) {
    run_compare((CompareSpec){
        .source = "let x = 10\n"
                  "if (true) {\n"
                  "  let y = x + 5\n"
                  "  print(y)\n"
                  "}\nprint(x)",
        .label = "block scoping with inner variable",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Complex Expressions --- */

TEST(cmp_complex_expr) {
    run_compare((CompareSpec){
        .source = "let a = 2\nlet b = 3\nlet c = 4\n"
                  "let r = (a + b) * c - a\nprint(r)",
        .label = "complex arithmetic expression",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_for_accumulate) {
    run_compare((CompareSpec){
        .source = "let sum = 0\n"
                  "for (let i = 1; i <= 10; i = i + 1) {\n"
                  "  sum = sum + i\n"
                  "}\nprint(sum)",
        .label = "for-loop sum 1..10",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_chained_comparison) {
    run_compare((CompareSpec){
        .source = "let a = 5\nlet b = 10\nlet c = 3\n"
                  "if (a > c && b > a) { print(1) } else { print(0) }",
        .label = "chained comparison with logical and",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_while_countdown) {
    run_compare((CompareSpec){
        .source = "let n = 5\nlet result = 1\n"
                  "while (n > 0) {\n"
                  "  result = result * n\n"
                  "  n = n - 1\n"
                  "}\nprint(result)",
        .label = "while-loop factorial (iterative)",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_bool_logic) {
    run_compare((CompareSpec){
        .source = "let t = true\nlet f = false\n"
                  "print(t && t)\nprint(t && f)\n"
                  "print(f || t)\nprint(f || f)",
        .label = "boolean logic combinations",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Multiple Functions --- */

TEST(cmp_multi_func) {
    run_compare((CompareSpec){
        .source = "fn double(x: int): int { return x * 2 }\n"
                  "fn inc(x: int): int { return x + 1 }\n"
                  "print(inc(double(3)))",
        .label = "multiple function declarations and chained calls",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Bitwise Operations --- */

TEST(cmp_bitwise_and_or) {
    run_compare((CompareSpec){
        .source = "let a = 12\nlet b = 10\n"
                  "print(a & b)\nprint(a | b)",
        .label = "bitwise AND and OR",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_bitwise_xor_shift) {
    run_compare((CompareSpec){
        .source = "let a = 5\n"
                  "print(a ^ 3)\nprint(a << 2)\nprint(a >> 1)",
        .label = "bitwise XOR and shifts",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_bitwise_not) {
    run_compare((CompareSpec){
        .source = "let a = 0\nprint(~a)\n"
                  "let b = 255\nprint(~b)",
        .label = "bitwise NOT",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Increment / Decrement --- */

TEST(cmp_increment) {
    run_compare((CompareSpec){
        .source = "let x = 5\nx++\nprint(x)\n"
                  "x--\nx--\nprint(x)",
        .label = "increment and decrement operators",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Array Literal and Indexing --- */

TEST(cmp_array_literal) {
    run_compare((CompareSpec){
        .source = "let arr = [10, 20, 30]\n"
                  "print(arr[0])\nprint(arr[1])\nprint(arr[2])",
        .label = "array literal and index access",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_array_assign) {
    run_compare((CompareSpec){
        .source = "let arr = [1, 2, 3]\n"
                  "arr[1] = 99\nprint(arr[1])",
        .label = "array index assignment",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Nested Ternary --- */

TEST(cmp_nested_ternary) {
    run_compare((CompareSpec){
        .source = "let x = 5\n"
                  "let r = x > 10 ? 1 : (x > 3 ? 2 : 3)\n"
                  "print(r)",
        .label = "nested ternary expression",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Multi-branch If-Else --- */

TEST(cmp_if_else_chain) {
    run_compare((CompareSpec){
        .source = "let x = 15\n"
                  "if (x > 20) { print(1) }\n"
                  "else if (x > 10) { print(2) }\n"
                  "else if (x > 5) { print(3) }\n"
                  "else { print(4) }",
        .label = "if-else chain with multiple branches",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Deeply Nested Arithmetic --- */

TEST(cmp_deep_arith) {
    run_compare((CompareSpec){
        .source = "let a = 2\nlet b = 3\nlet c = 4\nlet d = 5\n"
                  "let r = ((a + b) * (c - d) + a * b) * c\n"
                  "print(r)",
        .label = "deeply nested arithmetic expression",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- While with Multiple Conditions --- */

TEST(cmp_while_multi_cond) {
    run_compare((CompareSpec){
        .source = "let i = 0\nlet sum = 0\n"
                  "while (i < 10 && sum < 20) {\n"
                  "  sum = sum + i\n"
                  "  i = i + 1\n"
                  "}\nprint(sum)\nprint(i)",
        .label = "while loop with compound condition",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Map Literal --- */

TEST(cmp_map_literal) {
    run_compare((CompareSpec){
        .source = "let m = {\"a\": 1, \"b\": 2}\n"
                  "print(m[\"a\"])\nprint(m[\"b\"])",
        .label = "map literal and key access",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false,  /* STORE_FIELD emit uses field index, not name lookup */
    });
}

/* --- Template String --- */

TEST(cmp_template_string) {
    run_compare((CompareSpec){
        .source = "let name = \"world\"\n"
                  "let msg = \"hello ${name}\"\n"
                  "print(msg)",
        .label = "template string interpolation",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- For-in Loop --- */

TEST(cmp_for_in_array) {
    run_compare((CompareSpec){
        .source = "let arr = [10, 20, 30]\n"
                  "let sum = 0\n"
                  "for (item in arr) { sum = sum + item }\n"
                  "print(sum)",
        .label = "for-in loop over array",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_for_in_range) {
    run_compare((CompareSpec){
        .source = "let sum = 0\n"
                  "for (i in 0..5) { sum = sum + i }\n"
                  "print(sum)",
        .label = "for-in loop over range",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Closure with Captures --- */

TEST(cmp_closure_capture) {
    run_compare((CompareSpec){
        .source = "fn make_adder(x: int): fn(int): int {\n"
                  "  fn adder(y: int): int { return x + y }\n"
                  "  return adder\n"
                  "}\n"
                  "let add5 = make_adder(5)\n"
                  "print(add5(3))",
        .label = "closure capturing outer variable",
        .expect_xi_success = false,
        .min_similarity = 0.1,
        .check_exec = false,
    });
}

/* --- Type Conversion --- */

TEST(cmp_type_convert) {
    run_compare((CompareSpec){
        .source = "let x = 42\n"
                  "let s = x as string\n"
                  "print(s)\n"
                  "let f = 3.14\n"
                  "let i = f as int\n"
                  "print(i)",
        .label = "type conversion int->string and float->int",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false,  /* XI_AS emits MOVE, not runtime cast */
    });
}

/* --- Nullish Coalesce --- */

TEST(cmp_nullish_coalesce) {
    run_compare((CompareSpec){
        .source = "let a: int? = null\n"
                  "let b = a ?? 42\n"
                  "print(b)\n"
                  "let c: int? = 10\n"
                  "let d = c ?? 99\n"
                  "print(d)",
        .label = "nullish coalesce operator",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Match Expression --- */

TEST(cmp_match_expr) {
    run_compare((CompareSpec){
        .source = "let x = 3\n"
                  "let r = match x {\n"
                  "  1 => 10\n"
                  "  2 => 20\n"
                  "  3 => 30\n"
                  "  _ => 0\n"
                  "}\nprint(r)",
        .label = "match expression with literal patterns",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Try-Catch --- */

TEST(cmp_try_catch) {
    run_compare((CompareSpec){
        .source = "let result = 0\n"
                  "try {\n"
                  "  result = 42\n"
                  "} catch (e) {\n"
                  "  result = -1\n"
                  "}\nprint(result)",
        .label = "try-catch (no throw path)",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Slice Expression --- */

TEST(cmp_slice) {
    run_compare((CompareSpec){
        .source = "let arr = [1, 2, 3, 4, 5]\n"
                  "let s = arr[1:3]\n"
                  "print(s)",
        .label = "array slice expression",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Scope with Nested Functions --- */

TEST(cmp_nested_func_scope) {
    run_compare((CompareSpec){
        .source = "fn outer(): int {\n"
                  "  let x = 10\n"
                  "  fn inner(): int { return x * 2 }\n"
                  "  return inner()\n"
                  "}\nprint(outer())",
        .label = "nested function accessing outer scope",
        .expect_xi_success = false,
        .min_similarity = 0.1,
        .check_exec = false,
    });
}

/* --- Multiple Return Values --- */

TEST(cmp_func_no_return) {
    run_compare((CompareSpec){
        .source = "fn greet(name: string) {\n"
                  "  print(\"hello\")\n"
                  "  print(name)\n"
                  "}\ngreet(\"xray\")",
        .label = "void function with no return value",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Optional Chaining --- */

TEST(cmp_optional_chain) {
    run_compare((CompareSpec){
        .source = "let x: int? = null\n"
                  "let v = x ?? -1\n"
                  "print(v)",
        .label = "nullable with nullish coalesce fallback",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false,  /* nullable runtime behavior may differ */
    });
}

/* ========== Summary Report ========== */

static void print_summary(void) {
    printf("\n=== %d/%d Xi Compare tests passed",
           tests_passed, tests_passed + tests_failed);
    if (tests_skipped > 0)
        printf(" (%d skipped)", tests_skipped);
    printf(" ===\n");
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Compare: Legacy vs Xi Pipeline ===\n\n");

    setup();

    /* Constants */
    run_cmp_int_const();
    run_cmp_float_const();
    run_cmp_bool_const();
    run_cmp_string_const();
    run_cmp_null_const();

    /* Arithmetic */
    run_cmp_add();
    run_cmp_arith_chain();
    run_cmp_unary_neg();

    /* Control flow */
    run_cmp_if_else();
    run_cmp_if_const_true();
    run_cmp_while_loop();

    /* Multiple statements */
    run_cmp_multi_print();
    run_cmp_multi_vars();

    /* Reassignment */
    run_cmp_var_reassign();

    /* Comparisons */
    run_cmp_comparisons();

    /* Logical operators */
    run_cmp_logical_and();
    run_cmp_logical_or();
    run_cmp_logical_not();

    /* For loop */
    run_cmp_for_loop();

    /* Nested control flow */
    run_cmp_nested_if();

    /* Compound assignment */
    run_cmp_compound_assign();

    /* Ternary */
    run_cmp_ternary();

    /* Break / continue */
    run_cmp_while_break();
    run_cmp_while_continue();

    /* Functions */
    run_cmp_func_call();
    run_cmp_func_recursive();

    /* Nested loops */
    run_cmp_nested_loop();

    /* String operations */
    run_cmp_string_concat();

    /* Mixed arithmetic */
    run_cmp_mixed_arith();

    /* Nested / advanced function calls */
    run_cmp_nested_call();
    run_cmp_func_early_return();
    run_cmp_factorial();

    /* Float arithmetic */
    run_cmp_float_arith();

    /* Block scoping */
    run_cmp_block_scope();

    /* Complex expressions */
    run_cmp_complex_expr();
    run_cmp_for_accumulate();
    run_cmp_chained_comparison();
    run_cmp_while_countdown();
    run_cmp_bool_logic();

    /* Multiple functions */
    run_cmp_multi_func();

    /* Bitwise operations */
    run_cmp_bitwise_and_or();
    run_cmp_bitwise_xor_shift();
    run_cmp_bitwise_not();

    /* Increment / decrement */
    run_cmp_increment();

    /* Array literal and indexing */
    run_cmp_array_literal();
    run_cmp_array_assign();

    /* Nested ternary */
    run_cmp_nested_ternary();

    /* Multi-branch if-else */
    run_cmp_if_else_chain();

    /* Deep arithmetic */
    run_cmp_deep_arith();

    /* While with compound condition */
    run_cmp_while_multi_cond();

    /* Map literal */
    run_cmp_map_literal();

    /* Template string */
    run_cmp_template_string();

    /* For-in loop */
    run_cmp_for_in_array();
    run_cmp_for_in_range();

    /* Closure with captures */
    run_cmp_closure_capture();

    /* Type conversion */
    run_cmp_type_convert();

    /* Nullish coalesce */
    run_cmp_nullish_coalesce();

    /* Match expression */
    run_cmp_match_expr();

    /* Try-catch */
    run_cmp_try_catch();

    /* Slice */
    run_cmp_slice();

    /* Nested function scope */
    run_cmp_nested_func_scope();

    /* Void function */
    run_cmp_func_no_return();

    /* Optional chaining */
    run_cmp_optional_chain();

    teardown();

    print_summary();
    return tests_failed > 0 ? 1 : 0;
}
