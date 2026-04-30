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
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../src/coro/xcoroutine.h"
#include "../../../src/coro/xworker.h"
#include "../../../src/runtime/closure/xclosure.h"

/* ========== Test Infrastructure ========== */

static XrayIsolate *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

/* Protos passed to xr_execute create GC-managed closures that reference
 * the proto.  Freeing the proto while closures remain on the GC heap is
 * a use-after-free.  Defer proto frees until after isolate teardown. */
#define DEFERRED_PROTO_CAP 512
static XrProto *g_deferred_protos[DEFERRED_PROTO_CAP];
static int g_deferred_count = 0;

static void defer_proto_free(XrProto *p) {
    if (p && g_deferred_count < DEFERRED_PROTO_CAP)
        g_deferred_protos[g_deferred_count++] = p;
}

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
    /* Free deferred protos after isolate (and its GC heap) is gone */
    for (int i = 0; i < g_deferred_count; i++)
        xr_vm_proto_free(g_deferred_protos[i]);
    g_deferred_count = 0;
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

/* Execute proto and capture stdout into a heap buffer.
 * Uses xr_coro_reset_for_call (like xcmd_test.c) instead of
 * xr_execute, which doesn't fully reset coro state between
 * sequential runs on the same isolate. */
static char *execute_and_capture(XrProto *proto) {
    if (!proto || !g_iso) return NULL;

    XrCoroutine *main_coro = xr_isolate_get_main_coro(g_iso);
    if (!main_coro) return NULL;
    XrClosure *closure = xr_closure_new(g_iso, proto, main_coro);
    if (!closure) return NULL;
    xr_coro_reset_for_call(main_coro, g_iso, closure);

    fflush(stdout);
    if (!freopen(g_capture_path, "w", stdout)) return NULL;

    xr_main_thread_run(g_iso, main_coro);
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

    if (spec.check_exec) {
        /* GC-managed closures created by xr_execute still reference these
         * protos; freeing now would be a use-after-free on the next GC scan.
         * Defer until after isolate teardown destroys the GC heap. */
        defer_proto_free(p_legacy);
        defer_proto_free(p_xi);
    } else {
        xr_vm_proto_free(p_legacy);
        xr_vm_proto_free(p_xi);
    }
}

/* Check if a proto (or its sub-protos) contains at least one instance of
 * an opcode.  Recursive to handle fn-in-fn patterns. */
static bool proto_has_opcode(const XrProto *proto, OpCode target) {
    int count = PROTO_CODE_COUNT(proto);
    for (int i = 0; i < count; i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == target)
            return true;
    }
    int nchildren = DYNARRAY_COUNT(&proto->protos);
    for (int i = 0; i < nchildren; i++) {
        XrProto *child = DYNARRAY_GET(&proto->protos, i, XrProto *);
        if (proto_has_opcode(child, target))
            return true;
    }
    return false;
}

/* Compile via Xi and assert a specific fused opcode is present.
 * Also runs execution comparison if check_exec is set. */
typedef struct {
    const char *source;
    const char *label;
    OpCode expect_op;      /* opcode that must appear in Xi output */
    bool check_exec;
} FusionSpec;

static void run_fusion(FusionSpec spec) {
    XrProto *p_legacy = compile_legacy(spec.source);
    assert(p_legacy != NULL && "legacy codegen must succeed");

    XrProto *p_xi = compile_xi(spec.source);
    assert(p_xi != NULL && "Xi pipeline must succeed");

    bool has_op = proto_has_opcode(p_xi, spec.expect_op);
    fprintf(stderr, "  xi has %s: %s\n",
            xr_opcode_name(spec.expect_op), has_op ? "yes" : "NO");
    assert(has_op && "expected fused opcode not found in Xi output");

    if (spec.check_exec) {
        char *out_l = execute_and_capture(p_legacy);
        char *out_x = execute_and_capture(p_xi);
        if (out_l && out_x) {
            bool match = (strcmp(out_l, out_x) == 0);
            fprintf(stderr, "  exec: legacy=[%s] xi=[%s] %s\n",
                    out_l, out_x, match ? "MATCH" : "MISMATCH");
            assert(match && "execution output must match");
        }
        if (out_l) xr_free(out_l);
        if (out_x) xr_free(out_x);
    }

    if (spec.check_exec) {
        defer_proto_free(p_legacy);
        defer_proto_free(p_xi);
    } else {
        xr_vm_proto_free(p_legacy);
        xr_vm_proto_free(p_xi);
    }
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

TEST(cmp_string_concat_chain) {
    /* Multi-operand chain: "a" + "b" + "c" + "d" flattened to single STRBUF */
    run_compare((CompareSpec){
        .source = "let a = \"hello\"\n"
                  "let b = \" \"\n"
                  "let c = \"world\"\n"
                  "let d = \"!\"\n"
                  "let result = a + b + c + d\n"
                  "print(result)",
        .label = "string concat chain: 4-way STRBUF flatten",
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
        .check_exec = true,
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
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Type Assertion (as / as?) --- */

TEST(cmp_type_convert) {
    /* as cast: type matches -> value passes through */
    run_compare((CompareSpec){
        .source = "let x: Json = 42\n"
                  "let y = x as int\n"
                  "print(y)",
        .label = "as cast: Json(int) as int succeeds",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_as_safe_match) {
    /* as? (safe cast): type matches -> value passes through */
    run_compare((CompareSpec){
        .source = "let x: Json = \"hello\"\n"
                  "let y = x as string?\n"
                  "print(y)",
        .label = "as? safe cast: Json(string) as string? succeeds",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_as_safe_mismatch) {
    /* as? (safe cast): type mismatch -> null */
    run_compare((CompareSpec){
        .source = "let x: Json = \"hello\"\n"
                  "let y = x as int?\n"
                  "print(y)",
        .label = "as? safe cast: Json(string) as int? -> null",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_as_unsafe_mismatch) {
    /* as (unsafe cast): type mismatch -> throw (both legacy and Xi throw) */
    run_compare((CompareSpec){
        .source = "let x: Json = \"hello\"\n"
                  "let y = x as int\n"
                  "print(y)",
        .label = "as unsafe cast: Json(string) as int -> throw",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
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
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
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
        .check_exec = true,
    });
}

/* --- Method Calls on Builtins --- */

TEST(cmp_array_push) {
    run_compare((CompareSpec){
        .source = "let arr = [10, 20]\n"
                  "arr.push(30)\n"
                  "print(arr.length)\nprint(arr[2])",
        .label = "array push and length",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_string_method) {
    run_compare((CompareSpec){
        .source = "let s = \"hello\"\n"
                  "print(s.length)\n"
                  "print(s.toUpperCase())",
        .label = "string length and toUpperCase",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Higher-Order Functions --- */

TEST(cmp_higher_order) {
    run_compare((CompareSpec){
        .source = "fn makeAdder(x: int): fn(int): int {\n"
                  "    return fn(y: int): int { return x + y }\n"
                  "}\n"
                  "let add5 = makeAdder(5)\n"
                  "print(add5(3))\n"
                  "print(add5(10))",
        .label = "higher-order function (closure factory)",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Nested For-In --- */

TEST(cmp_nested_for_in) {
    run_compare((CompareSpec){
        .source = "let total = 0\n"
                  "let matrix = [[1, 2], [3, 4]]\n"
                  "for (row in matrix) {\n"
                  "    for (val in row) {\n"
                  "        total += val\n"
                  "    }\n"
                  "}\n"
                  "print(total)",
        .label = "nested for-in over 2D array",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- For-In with String --- */

TEST(cmp_for_in_string) {
    run_compare((CompareSpec){
        .source = "let count = 0\n"
                  "for (c in \"hello\") {\n"
                  "    count += 1\n"
                  "}\n"
                  "print(count)",
        .label = "for-in over string characters",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Array Map/Filter-like Patterns --- */

TEST(cmp_array_sum_func) {
    run_compare((CompareSpec){
        .source = "fn sum(arr: Array<int>): int {\n"
                  "    let total = 0\n"
                  "    for (x in arr) {\n"
                  "        total += x\n"
                  "    }\n"
                  "    return total\n"
                  "}\n"
                  "print(sum([1, 2, 3, 4, 5]))",
        .label = "function taking array param with for-in",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Multiple Closures --- */

TEST(cmp_multi_closure) {
    run_compare((CompareSpec){
        .source = "let x = 10\n"
                  "let add = fn(a: int): int { return a + x }\n"
                  "let mul = fn(a: int): int { return a * x }\n"
                  "print(add(5))\n"
                  "print(mul(3))",
        .label = "multiple closures capturing same variable",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Fibonacci (complex recursion) --- */

TEST(cmp_fibonacci) {
    run_compare((CompareSpec){
        .source = "fn fib(n: int): int {\n"
                  "    if (n <= 1) { return n }\n"
                  "    return fib(n - 1) + fib(n - 2)\n"
                  "}\n"
                  "print(fib(10))",
        .label = "fibonacci recursive function",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Transitive Closure Capture (3 levels deep) --- */

TEST(cmp_transitive_capture) {
    run_compare((CompareSpec){
        .source = "fn outer(): int {\n"
                  "    let x = 10\n"
                  "    fn middle(): int {\n"
                  "        fn inner(): int { return x + 1 }\n"
                  "        return inner()\n"
                  "    }\n"
                  "    return middle()\n"
                  "}\n"
                  "print(outer())",
        .label = "transitive closure capture (3 levels)",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Closure Counter (mutable capture via cell) --- */

TEST(cmp_closure_counter) {
    run_compare((CompareSpec){
        .source = "fn counter(): fn(): int {\n"
                  "    let n = 0\n"
                  "    return fn(): int { n += 1; return n }\n"
                  "}\n"
                  "let c = counter()\n"
                  "print(c())\n"
                  "print(c())\n"
                  "print(c())",
        .label = "closure counter with mutable capture",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Function Composition (captures two function params) --- */

TEST(cmp_compose) {
    run_compare((CompareSpec){
        .source = "fn compose(f: fn(int): int, g: fn(int): int): fn(int): int {\n"
                  "    return fn(x: int): int { return f(g(x)) }\n"
                  "}\n"
                  "fn add1(x: int): int { return x + 1 }\n"
                  "fn mul2(x: int): int { return x * 2 }\n"
                  "let h = compose(add1, mul2)\n"
                  "print(h(5))",
        .label = "function composition capturing two params",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Callback / Higher-Order Apply --- */

TEST(cmp_apply_fn) {
    run_compare((CompareSpec){
        .source = "fn apply(f: fn(int): int, x: int): int {\n"
                  "    return f(x)\n"
                  "}\n"
                  "fn double(x: int): int { return x * 2 }\n"
                  "fn square(x: int): int { return x * x }\n"
                  "print(apply(double, 5))\n"
                  "print(apply(square, 4))",
        .label = "higher-order apply with function params",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Find Max in Array --- */

TEST(cmp_find_max) {
    run_compare((CompareSpec){
        .source = "let nums = [5, 3, 8, 1, 9, 2]\n"
                  "let max = nums[0]\n"
                  "for (n in nums) {\n"
                  "    if (n > max) { max = n }\n"
                  "}\n"
                  "print(max)",
        .label = "find max in array using for-in",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Mutual Recursion (even/odd) --- */

TEST(cmp_mutual_recursion) {
    run_compare((CompareSpec){
        .source = "fn is_even(n: int): bool {\n"
                  "    if (n == 0) { return true }\n"
                  "    return is_odd(n - 1)\n"
                  "}\n"
                  "fn is_odd(n: int): bool {\n"
                  "    if (n == 0) { return false }\n"
                  "    return is_even(n - 1)\n"
                  "}\n"
                  "print(is_even(10))\n"
                  "print(is_odd(7))",
        .label = "mutual recursion (even/odd)",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Power Function (recursive) --- */

TEST(cmp_power) {
    run_compare((CompareSpec){
        .source = "fn power(base: int, exp: int): int {\n"
                  "    if (exp == 0) { return 1 }\n"
                  "    return base * power(base, exp - 1)\n"
                  "}\n"
                  "print(power(2, 10))",
        .label = "recursive power function",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- GCD (Euclidean algorithm) --- */

TEST(cmp_gcd) {
    run_compare((CompareSpec){
        .source = "fn gcd(a: int, b: int): int {\n"
                  "    if (b == 0) { return a }\n"
                  "    return gcd(b, a % b)\n"
                  "}\n"
                  "print(gcd(48, 18))",
        .label = "recursive GCD (Euclidean)",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* ========== OOP Tests ========== */

/* --- Basic class instantiation and field access --- */

TEST(cmp_class_basic) {
    run_compare((CompareSpec){
        .source = "class Point {\n"
                  "    x: int\n"
                  "    y: int\n"
                  "    constructor(x: int, y: int) {\n"
                  "        this.x = x\n"
                  "        this.y = y\n"
                  "    }\n"
                  "}\n"
                  "let p = new Point(3, 4)\n"
                  "print(p.x)\n"
                  "print(p.y)",
        .label = "class basic: constructor + field access",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Class with method --- */

TEST(cmp_class_method) {
    run_compare((CompareSpec){
        .source = "class Box {\n"
                  "    value: int\n"
                  "    constructor(v) {\n"
                  "        this.value = v\n"
                  "    }\n"
                  "}\n"
                  "let b = new Box(42)\n"
                  "print(b.value)\n"
                  "b.value = 99\n"
                  "print(b.value)",
        .label = "class field: read + write",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Struct literal --- */

TEST(cmp_struct_literal) {
    run_compare((CompareSpec){
        .source = "let obj = { name: \"Alice\", age: 30 }\n"
                  "print(obj[\"name\"])\n"
                  "print(obj[\"age\"])",
        .label = "object literal with fields",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Class inheritance --- */

TEST(cmp_class_inherit) {
    run_compare((CompareSpec){
        .source = "class Animal {\n"
                  "    name: string\n"
                  "    constructor(name: string) {\n"
                  "        this.name = name\n"
                  "    }\n"
                  "}\n"
                  "class Dog extends Animal {\n"
                  "    breed: string\n"
                  "    constructor(name: string, breed: string) {\n"
                  "        super(name)\n"
                  "        this.breed = breed\n"
                  "    }\n"
                  "}\n"
                  "let d = new Dog(\"Rex\", \"Labrador\")\n"
                  "print(d.name)\n"
                  "print(d.breed)",
        .label = "class inheritance + method override",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* ========== Enum Tests ========== */

TEST(cmp_enum_basic) {
    run_compare((CompareSpec){
        .source = "enum Color {\n"
                  "    Red,\n"
                  "    Green,\n"
                  "    Blue\n"
                  "}\n"
                  "let c = Color.Red\n"
                  "print(c.name)\n"
                  "print(c.value)",
        .label = "enum basic: access name and value",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* ========== For-in Map Tests ========== */

TEST(cmp_for_in_map) {
    run_compare((CompareSpec){
        .source = "let m = { a: 1, b: 2, c: 3 }\n"
                  "let sum = 0\n"
                  "for (k, v in m) {\n"
                  "    sum += v\n"
                  "}\n"
                  "print(sum)",
        .label = "for-in map with key-value",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* ========== Additional Closure Tests ========== */

TEST(cmp_closure_adder) {
    run_compare((CompareSpec){
        .source = "fn make_adder(n: int): fn(int): int {\n"
                  "    return fn(x: int): int { return x + n }\n"
                  "}\n"
                  "let add5 = make_adder(5)\n"
                  "let add10 = make_adder(10)\n"
                  "print(add5(3))\n"
                  "print(add10(3))",
        .label = "closure factory: make_adder",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_closure_accumulator) {
    run_compare((CompareSpec){
        .source = "fn make_acc(): fn(int): int {\n"
                  "    let total = 0\n"
                  "    return fn(n: int): int { total += n; return total }\n"
                  "}\n"
                  "let acc = make_acc()\n"
                  "print(acc(5))\n"
                  "print(acc(3))\n"
                  "print(acc(2))",
        .label = "closure accumulator with mutable capture",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* ========== Misc Pattern Tests ========== */

TEST(cmp_nested_closure) {
    run_compare((CompareSpec){
        .source = "fn outer(): fn(): fn(): int {\n"
                  "    let val = 42\n"
                  "    return fn(): fn(): int {\n"
                  "        return fn(): int { return val }\n"
                  "    }\n"
                  "}\n"
                  "print(outer()()())",
        .label = "triple-nested closure transitive capture",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_string_for_in) {
    run_compare((CompareSpec){
        .source = "let count = 0\n"
                  "for (ch in \"hello\") {\n"
                  "    count += 1\n"
                  "}\n"
                  "print(count)",
        .label = "for-in string char count",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* ========== Destructuring / Multi-assign / Collection Tests ========== */

TEST(cmp_destructure_array) {
    run_compare((CompareSpec){
        .source = "let arr = [10, 20, 30]\n"
                  "let [a, b, c] = arr\n"
                  "print(a)\n"
                  "print(b)\n"
                  "print(c)",
        .label = "destructure array declaration",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_destructure_object) {
    run_compare((CompareSpec){
        .source = "let { name, age } = { name: \"alice\", age: 30 }\n"
                  "print(name)\n"
                  "print(age)",
        .label = "destructure object declaration",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_multi_var_decl) {
    run_compare((CompareSpec){
        .source = "fn pair(): (int, int) { return 10, 20 }\n"
                  "let x, y = pair()\n"
                  "print(x)\n"
                  "print(y)",
        .label = "multi-value var declaration",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_multi_assign) {
    run_compare((CompareSpec){
        .source = "let x = 1\n"
                  "let y = 2\n"
                  "x, y = y, x\n"
                  "print(x)\n"
                  "print(y)",
        .label = "multi-value swap assignment",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_set_literal) {
    run_compare((CompareSpec){
        .source = "let s = #[1, 2, 3, 2, 1]\n"
                  "print(s.size())",
        .label = "set literal with duplicates",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_object_literal) {
    run_compare((CompareSpec){
        .source = "let obj = { name: \"alice\", age: 30 }\n"
                  "print(obj[\"name\"])\n"
                  "print(obj[\"age\"])",
        .label = "object literal bracket access",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true, /* Xi uses NEWMAP, legacy uses NEWJSON — bracket access works on both */
    });
}

/* ========== Coroutine Tests ========== */

TEST(cmp_defer_simple) {
    /* Legacy analyzer doesn't recognize 'print' as a builtin inside defer
     * expressions, so use a user-defined cleanup function instead. */
    run_compare((CompareSpec){
        .source =
            "fn cleanup() { print(\"deferred\") }\n"
            "fn f() {\n"
            "  defer cleanup()\n"
            "  print(\"body\")\n"
            "}\n"
            "f()",
        .label = "defer: cleanup after function body",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_defer_lifo) {
    run_compare((CompareSpec){
        .source =
            "fn first() { print(\"first\") }\n"
            "fn second() { print(\"second\") }\n"
            "fn f() {\n"
            "  defer first()\n"
            "  defer second()\n"
            "  print(\"body\")\n"
            "}\n"
            "f()",
        .label = "defer: LIFO ordering",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_yield_basic) {
    /* yield in main should be a no-op (no other coroutines) */
    run_compare((CompareSpec){
        .source = "print(\"before\")\nyield\nprint(\"after\")",
        .label = "yield: no-op without other coroutines",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_chan_new_unbuf) {
    /* Channel() creates an unbuffered channel; just type-check */
    run_compare((CompareSpec){
        .source = "const ch = Channel()\nprint(typeof(ch))",
        .label = "Channel(): unbuffered channel construction",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_chan_new_buffered) {
    run_compare((CompareSpec){
        .source = "const ch: Channel<int> = Channel(4)\nprint(typeof(ch))",
        .label = "Channel(N): buffered channel construction",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_chan_send_recv_buffered) {
    /* Buffered channel: send then recv on same coro works without scheduling */
    run_compare((CompareSpec){
        .source =
            "const ch: Channel<int> = Channel(2)\n"
            "ch.send(10)\n"
            "ch.send(20)\n"
            "print(ch.recv())\n"
            "print(ch.recv())",
        .label = "Channel(2): single-coro send+recv",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_go_simple) {
    /* go fn() — Xi uses OP_GO, legacy uses OP_SPAWN_CONT (different scheduling).
     * Output ordering may differ; only verify Xi compiles and runs. */
    run_compare((CompareSpec){
        .source =
            "fn worker() { print(\"worker\") }\n"
            "let task = go worker()\n"
            "await task\n"
            "print(\"done\")",
        .label = "go fn(): basic spawn + await",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false, /* OP_GO vs OP_SPAWN_CONT scheduling differs */
    });
}

TEST(cmp_go_with_chan) {
    /* go + channel: producer/consumer pattern */
    run_compare((CompareSpec){
        .source =
            "fn producer(ch: Channel<int>) { ch.send(42) }\n"
            "const ch: Channel<int> = Channel(1)\n"
            "let task = go producer(ch)\n"
            "print(ch.recv())\n"
            "await task",
        .label = "go + channel: producer/consumer",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false, /* scheduling differs */
    });
}

TEST(cmp_cancelled) {
    /* cancelled() returns false in main coroutine */
    run_compare((CompareSpec){
        .source = "print(cancelled())",
        .label = "cancelled(): false in main",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_scope_basic) {
    /* scope { body } — basic structured concurrency block */
    run_compare((CompareSpec){
        .source = "scope {\n"
                  "  print(\"inside\")\n"
                  "}\n"
                  "print(\"after\")",
        .label = "scope: basic block with print",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_select_recv) {
    /* select with channel recv */
    run_compare((CompareSpec){
        .source = "fn producer(ch: Channel<int>) {\n"
                  "  ch.send(42)\n"
                  "}\n"
                  "const ch: Channel<int> = Channel(1)\n"
                  "go producer(ch)\n"
                  "select {\n"
                  "  msg from ch => {\n"
                  "    print(msg)\n"
                  "  }\n"
                  "}",
        .label = "select: recv from channel",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false, /* scheduling differs */
    });
}

TEST(cmp_await_all) {
    /* await [t1, t2] — wait for all */
    run_compare((CompareSpec){
        .source = "fn double(x: int): int {\n"
                  "  return x * 2\n"
                  "}\n"
                  "let t1 = go double(10)\n"
                  "let t2 = go double(20)\n"
                  "let r = await [t1, t2]\n"
                  "print(r)",
        .label = "await.all: wait for multiple tasks",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false, /* scheduling differs */
    });
}

TEST(cmp_await_any) {
    /* await.any [t1, t2] — wait for first */
    run_compare((CompareSpec){
        .source = "fn double(x: int): int {\n"
                  "  return x * 2\n"
                  "}\n"
                  "let t1 = go double(10)\n"
                  "let t2 = go double(20)\n"
                  "let r = await.any [t1, t2]\n"
                  "print(r)",
        .label = "await.any: wait for first task",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false, /* scheduling differs */
    });
}

/* ========== Instruction Fusion Tests ========== */

TEST(fusion_addi) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return x + 1 }\nprint(f(10))",
        .label = "ADDI: x + small_const",
        .expect_op = OP_ADDI,
        .check_exec = true,
    });
}

TEST(fusion_addi_commutative) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return 3 + x }\nprint(f(10))",
        .label = "ADDI commutative: small_const + x",
        .expect_op = OP_ADDI,
        .check_exec = true,
    });
}

TEST(fusion_subi) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return x - 3 }\nprint(f(10))",
        .label = "SUBI: x - small_const",
        .expect_op = OP_SUBI,
        .check_exec = true,
    });
}

TEST(fusion_muli) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return x * 7 }\nprint(f(5))",
        .label = "MULI: x * small_const",
        .expect_op = OP_MULI,
        .check_exec = true,
    });
}

TEST(fusion_muli_commutative) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return 7 * x }\nprint(f(5))",
        .label = "MULI commutative: small_const * x",
        .expect_op = OP_MULI,
        .check_exec = true,
    });
}

TEST(fusion_addk) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return x + 1000 }\nprint(f(10))",
        .label = "ADDK: x + large_const",
        .expect_op = OP_ADDK,
        .check_exec = true,
    });
}

TEST(fusion_addk_commutative) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return 1000 + x }\nprint(f(10))",
        .label = "ADDK commutative: large_const + x",
        .expect_op = OP_ADDK,
        .check_exec = true,
    });
}

TEST(fusion_subk) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return x - 500 }\nprint(f(2000))",
        .label = "SUBK: x - large_const",
        .expect_op = OP_SUBK,
        .check_exec = true,
    });
}

TEST(fusion_mulk) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return x * 1000 }\nprint(f(3))",
        .label = "MULK: x * large_const",
        .expect_op = OP_MULK,
        .check_exec = true,
    });
}

TEST(fusion_mulk_commutative) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return 1000 * x }\nprint(f(3))",
        .label = "MULK commutative: large_const * x",
        .expect_op = OP_MULK,
        .check_exec = true,
    });
}

TEST(fusion_divk) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return x / 500 }\nprint(f(5000))",
        .label = "DIVK: x / large_const",
        .expect_op = OP_DIVK,
        .check_exec = true,
    });
}

TEST(fusion_modk) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int): int { return x % 1000 }\nprint(f(12345))",
        .label = "MODK: x % large_const",
        .expect_op = OP_MODK,
        .check_exec = true,
    });
}

TEST(fusion_lti_branch) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int) { if (x < 10) { print(1) } else { print(0) } }\nf(5)",
        .label = "LTI: branch x < small_const",
        .expect_op = OP_LTI,
        .check_exec = true,
    });
}

TEST(fusion_eqi_branch) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int) { if (x == 5) { print(1) } else { print(0) } }\nf(5)",
        .label = "EQI: branch x == small_const",
        .expect_op = OP_EQI,
        .check_exec = true,
    });
}

TEST(fusion_lei_branch) {
    run_fusion((FusionSpec){
        .source = "fn f(x: int) { if (x <= 10) { print(1) } else { print(0) } }\nf(10)",
        .label = "LEI: branch x <= small_const",
        .expect_op = OP_LEI,
        .check_exec = true,
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
    run_cmp_string_concat_chain();

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

    /* Type assertion (as / as?) */
    run_cmp_type_convert();
    run_cmp_as_safe_match();
    run_cmp_as_safe_mismatch();
    run_cmp_as_unsafe_mismatch();

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

    /* Method calls on builtins */
    run_cmp_array_push();
    run_cmp_string_method();

    /* Higher-order functions */
    run_cmp_higher_order();

    /* Nested for-in */
    run_cmp_nested_for_in();

    /* For-in with string */
    run_cmp_for_in_string();

    /* Array sum function */
    run_cmp_array_sum_func();

    /* Multiple closures */
    run_cmp_multi_closure();

    /* Fibonacci */
    run_cmp_fibonacci();

    /* Transitive closure capture */
    run_cmp_transitive_capture();

    /* Closure counter (mutable capture) */
    run_cmp_closure_counter();

    /* Function composition */
    run_cmp_compose();

    /* Higher-order apply */
    run_cmp_apply_fn();

    /* Find max in array */
    run_cmp_find_max();

    /* Mutual recursion */
    run_cmp_mutual_recursion();

    /* Recursive power */
    run_cmp_power();

    /* GCD */
    run_cmp_gcd();

    /* OOP: class basic */
    run_cmp_class_basic();
    run_cmp_class_method();
    run_cmp_struct_literal();
    run_cmp_class_inherit();

    /* Enum */
    run_cmp_enum_basic();

    /* For-in map */
    run_cmp_for_in_map();

    /* Additional closures */
    run_cmp_closure_adder();
    run_cmp_closure_accumulator();

    /* Misc patterns */
    run_cmp_nested_closure();
    run_cmp_string_for_in();

    /* Destructuring / multi-assign / collections */
    run_cmp_destructure_array();
    run_cmp_destructure_object();
    run_cmp_multi_var_decl();
    run_cmp_multi_assign();
    run_cmp_set_literal();
    run_cmp_object_literal();

    /* Coroutine */
    run_cmp_defer_simple();
    run_cmp_defer_lifo();
    run_cmp_yield_basic();
    run_cmp_chan_new_unbuf();
    run_cmp_chan_new_buffered();
    run_cmp_chan_send_recv_buffered();
    run_cmp_go_simple();
    run_cmp_go_with_chan();
    run_cmp_cancelled();
    run_cmp_scope_basic();
    run_cmp_select_recv();
    run_cmp_await_all();
    run_cmp_await_any();

    /* Instruction fusion */
    run_fusion_addi();
    run_fusion_addi_commutative();
    run_fusion_subi();
    run_fusion_muli();
    run_fusion_muli_commutative();
    run_fusion_addk();
    run_fusion_addk_commutative();
    run_fusion_subk();
    run_fusion_mulk();
    run_fusion_mulk_commutative();
    run_fusion_divk();
    run_fusion_modk();
    run_fusion_lti_branch();
    run_fusion_eqi_branch();
    run_fusion_lei_branch();

    teardown();

    print_summary();
    return tests_failed > 0 ? 1 : 0;
}
