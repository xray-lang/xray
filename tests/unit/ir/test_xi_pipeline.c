/*
 * test_xi_pipeline.c - End-to-end tests for Xi IR compilation pipeline
 *
 * Tests the full path: source -> parse -> analyze -> lower -> verify ->
 * optimize -> emit -> XrProto, then inspects the emitted bytecode.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_pipeline.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/base/xmalloc.h"
#include "../../../include/xray_isolate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Test Infrastructure ========== */

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

/* Compile source through the full pipeline, return proto.
 * Caller must free both proto and result. */
static XrProto *compile_source(const char *source, XiPipelineConfig *cfg) {
    assert(g_iso != NULL);

    AstNode *program = xr_parse(g_iso, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %s\n", source);
        return NULL;
    }

    XaAnalyzer *analyzer = xa_analyzer_new(g_iso);
    if (!analyzer) {
        xr_program_destroy(program);
        return NULL;
    }
    xa_analyzer_analyze(analyzer, "test.xr", program);

    XiPipelineResult res = xi_pipeline_compile_program(
        program, analyzer, g_iso, cfg);

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    if (res.status != XI_PIPE_OK) {
        fprintf(stderr, "  PIPELINE FAILED: %s (%s)\n",
                xi_pipe_status_str(res.status),
                res.error_msg ? res.error_msg : "no detail");
        xi_pipeline_result_free(&res);
        return NULL;
    }

    XrProto *proto = res.proto;
    xi_pipeline_result_free(&res);
    return proto;
}

/* Check that the proto contains at least one instruction with the given opcode */
static bool has_opcode(const XrProto *proto, OpCode op) {
    int count = PROTO_CODE_COUNT(proto);
    for (int i = 0; i < count; i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == op)
            return true;
    }
    return false;
}

/* Count occurrences of an opcode */
static int count_opcode(const XrProto *proto, OpCode op) {
    int n = 0;
    int count = PROTO_CODE_COUNT(proto);
    for (int i = 0; i < count; i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == op)
            n++;
    }
    return n;
}

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        printf("--- " #name " ---\n"); \
        test_##name(); \
        printf("  PASS\n"); \
        tests_passed++; \
    } \
    static void test_##name(void)

/* ========== Constant & Arithmetic Tests ========== */

TEST(e2e_simple_const) {
    /* let x = 42
     * print(x)
     * Expect: LOADI + PRINT + RETURN */
    XrProto *p = compile_source("let x = 42\nprint(x)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_PRINT));
    xr_vm_proto_free(p);
}

TEST(e2e_arithmetic) {
    /* let x = 1 + 2
     * let y = x * 3
     * After const folding: x=3, y=9 (both constants). */
    XrProto *p = compile_source("let x = 1 + 2\nlet y = x * 3\nprint(y)", NULL);
    assert(p != NULL);
    /* After optimization, ADD and MUL should be folded away */
    assert(!has_opcode(p, OP_ADD) && "1+2 should be folded");
    assert(!has_opcode(p, OP_MUL) && "3*3 should be folded");
    xr_vm_proto_free(p);
}

TEST(e2e_variable_assignment) {
    /* let x = 10
     * x = x + 5
     * print(x) */
    XrProto *p = compile_source("let x = 10\nx = x + 5\nprint(x)", NULL);
    assert(p != NULL);
    /* After const folding: x=15, so no ADD */
    assert(!has_opcode(p, OP_ADD) && "10+5 should be folded");
    xr_vm_proto_free(p);
}

/* ========== Control Flow Tests ========== */

TEST(e2e_if_else) {
    /* if (true) { print(1) } else { print(2) } */
    XrProto *p = compile_source("if (true) { print(1) } else { print(2) }", NULL);
    assert(p != NULL);
    /* With const folding, the branch may be eliminated */
    assert(has_opcode(p, OP_PRINT));
    xr_vm_proto_free(p);
}

TEST(e2e_while_loop) {
    /* let i = 0
     * while i < 3 { i = i + 1 }
     * print(i) */
    XrProto *p = compile_source(
        "let i = 0\nwhile (i < 3) { i = i + 1 }\nprint(i)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_JMP) && "while loop needs JMP");
    xr_vm_proto_free(p);
}

/* ========== Pipeline Configuration Tests ========== */

TEST(e2e_no_optimize) {
    /* Verify that unoptimized path works */
    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.run_optimize = false;
    XrProto *p = compile_source("let x = 1 + 2\nprint(x)", &cfg);
    assert(p != NULL);
    /* Without optimization, constant folding doesn't run, so arithmetic remains.
     * Instruction fusion may emit ADDI instead of ADD for small constant args. */
    assert((has_opcode(p, OP_ADD) || has_opcode(p, OP_ADDI)) &&
           "unoptimized should keep ADD or ADDI");
    xr_vm_proto_free(p);
}

TEST(e2e_with_verify) {
    /* Verify passes by default */
    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.run_verify = true;
    XrProto *p = compile_source("let x = 42\nprint(x)", &cfg);
    assert(p != NULL);
    xr_vm_proto_free(p);
}

/* ========== Boolean & Comparison ========== */

TEST(e2e_bool_ops) {
    /* let a = true
     * let b = false
     * print(a) */
    XrProto *p = compile_source("let a = true\nlet b = false\nprint(a)", NULL);
    assert(p != NULL);
    xr_vm_proto_free(p);
}

TEST(e2e_comparison) {
    /* let x = 5 > 3
     * print(x)
     * After const folding: x=true */
    XrProto *p = compile_source("let x = 5 > 3\nprint(x)", NULL);
    assert(p != NULL);
    xr_vm_proto_free(p);
}

/* ========== Multiple Statements ========== */

TEST(e2e_multi_print) {
    /* print(1)
     * print(2)
     * print(3) */
    XrProto *p = compile_source("print(1)\nprint(2)\nprint(3)", NULL);
    assert(p != NULL);
    assert(count_opcode(p, OP_PRINT) == 3 && "should have 3 PRINT ops");
    xr_vm_proto_free(p);
}

/* ========== String Literals ========== */

TEST(e2e_string_literal) {
    XrProto *p = compile_source("let s = \"hello\"\nprint(s)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_PRINT));
    xr_vm_proto_free(p);
}

/* ========== Unary Ops ========== */

TEST(e2e_unary_neg) {
    /* let x = -42
     * After const folding: x = -42 */
    XrProto *p = compile_source("let x = -42\nprint(x)", NULL);
    assert(p != NULL);
    xr_vm_proto_free(p);
}

/* ========== Pipeline Status API ========== */

TEST(e2e_status_str) {
    assert(strcmp(xi_pipe_status_str(XI_PIPE_OK), "OK") == 0);
    assert(strcmp(xi_pipe_status_str(XI_PIPE_ERR_LOWER), "AST lowering failed") == 0);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Pipeline E2E Tests ===\n\n");

    setup();

    /* Constants & arithmetic */
    run_e2e_simple_const();
    run_e2e_arithmetic();
    run_e2e_variable_assignment();

    /* Control flow */
    run_e2e_if_else();
    run_e2e_while_loop();

    /* Configuration */
    run_e2e_no_optimize();
    run_e2e_with_verify();

    /* Boolean & comparison */
    run_e2e_bool_ops();
    run_e2e_comparison();

    /* Multiple statements */
    run_e2e_multi_print();

    /* String */
    run_e2e_string_literal();

    /* Unary */
    run_e2e_unary_neg();

    /* API */
    run_e2e_status_str();

    teardown();

    printf("\n=== %d/%d Xi Pipeline tests passed ===\n",
           tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
