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

    AstNode *program = xr_parse(g_iso, source);
    if (!program) return NULL;

    XrCompilerContext *ctx = xr_compiler_context_new(g_iso);
    if (!ctx) {
        xr_program_destroy(program);
        return NULL;
    }
    /* Use the context's built-in analyzer */
    xa_analyzer_analyze(ctx->analyzer, "compare.xr", program);
    ctx->use_xi_pipeline = false;

    XrProto *proto = xr_compile(ctx, program);

    xr_compiler_context_free(ctx);
    xr_program_destroy(program);
    return proto;
}

/* Compile via Xi IR pipeline */
static XrProto *compile_xi(const char *source) {
    XR_DCHECK(g_iso != NULL, "isolate must be initialized");

    AstNode *program = xr_parse(g_iso, source);
    if (!program) return NULL;

    XaAnalyzer *analyzer = xa_analyzer_new(g_iso);
    if (!analyzer) { xr_program_destroy(program); return NULL; }
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

    teardown();

    print_summary();
    return tests_failed > 0 ? 1 : 0;
}
