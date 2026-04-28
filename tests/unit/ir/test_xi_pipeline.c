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

/* Compile source through the full pipeline, return proto.
 * Caller must free both proto and result. */
static XrProto *compile_source(const char *source, XiPipelineConfig *cfg) {
    assert(g_iso != NULL);

    /* Create analyzer first — its type pool must be active during parsing
     * so the parser can create type annotations (function types, etc.). */
    XaAnalyzer *analyzer = xa_analyzer_new(g_iso);
    if (!analyzer) return NULL;

    AstNode *program = xr_parse(g_iso, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %s\n", source);
        xa_analyzer_free(analyzer);
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

/* ========== For Loop ========== */

TEST(e2e_for_loop) {
    XrProto *p = compile_source(
        "let sum = 0\n"
        "for (let i = 0; i < 5; i = i + 1) { sum = sum + i }\n"
        "print(sum)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_JMP) && "for loop needs backward JMP");
    assert(has_opcode(p, OP_PRINT));
    xr_vm_proto_free(p);
}

/* ========== Function / Closure ========== */

TEST(e2e_function_decl) {
    /* Function declaration should emit CLOSURE opcode and have a child proto */
    XrProto *p = compile_source(
        "fn add(a: int, b: int): int { return a + b }\n"
        "print(add(1, 2))", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_CLOSURE) && "function decl needs CLOSURE");
    assert(PROTO_PROTO_COUNT(p) >= 1 && "should have child proto for add()");
    xr_vm_proto_free(p);
}

TEST(e2e_recursive_func) {
    XrProto *p = compile_source(
        "fn fib(n: int): int {\n"
        "  if (n <= 1) { return n }\n"
        "  return fib(n - 1) + fib(n - 2)\n"
        "}\nprint(fib(5))", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_CLOSURE));
    /* Child proto should use CALLSELF for recursion */
    XrProto *child = PROTO_PROTO(p, 0);
    assert(has_opcode(child, OP_CALLSELF) && "recursive call should use CALLSELF");
    xr_vm_proto_free(p);
}

TEST(e2e_nested_call) {
    /* Tests the register clobber fix: nested calls to same function */
    XrProto *p = compile_source(
        "fn add(a: int, b: int): int { return a + b }\n"
        "print(add(1, add(2, 3)))", NULL);
    assert(p != NULL);
    /* Main proto should have 2 CALL instructions (not CALLSELF) */
    assert(count_opcode(p, OP_CALL) >= 2 && "nested calls need >= 2 CALLs");
    xr_vm_proto_free(p);
}

/* ========== Constant Propagation Chain ========== */

TEST(e2e_const_prop_chain) {
    /* let a = 2; let b = a + 3; let c = b * 4; print(c)
     * After folding: a=2, b=5, c=20. No arithmetic ops. */
    XrProto *p = compile_source(
        "let a = 2\nlet b = a + 3\nlet c = b * 4\nprint(c)", NULL);
    assert(p != NULL);
    assert(!has_opcode(p, OP_ADD) && "chain should fold ADD away");
    assert(!has_opcode(p, OP_MUL) && "chain should fold MUL away");
    xr_vm_proto_free(p);
}

/* ========== Dead Code Elimination ========== */

TEST(e2e_dce_unused_var) {
    /* let x = 42; let y = 99; print(x)
     * y is unused → should be eliminated */
    XrProto *p = compile_source(
        "let x = 42\nlet y = 99\nprint(x)", NULL);
    assert(p != NULL);
    /* Only one LOADI needed (for x=42); y=99 should be dead */
    int loads = count_opcode(p, OP_LOADI) + count_opcode(p, OP_LOADK);
    assert(loads <= 1 && "unused y should be eliminated by DCE");
    xr_vm_proto_free(p);
}

/* ========== Array Operations ========== */

TEST(e2e_array_literal) {
    XrProto *p = compile_source(
        "let arr = [10, 20, 30]\nprint(arr[1])", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_NEWARRAY) && "array literal needs NEWARRAY");
    assert(has_opcode(p, OP_INDEX_GET) && "arr[1] needs INDEX_GET");
    xr_vm_proto_free(p);
}

TEST(e2e_array_set) {
    XrProto *p = compile_source(
        "let arr = [1, 2, 3]\narr[0] = 99\nprint(arr[0])", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_INDEX_SET) && "arr[0]=99 needs INDEX_SET");
    xr_vm_proto_free(p);
}

/* ========== Bitwise Operations ========== */

TEST(e2e_bitwise_ops) {
    /* Variable-based bitwise ops should emit real instructions */
    XrProto *p = compile_source(
        "let a = 12\nlet b = 10\n"
        "print(a & b)\nprint(a | b)\nprint(a ^ b)", NULL);
    assert(p != NULL);
    xr_vm_proto_free(p);
}

TEST(e2e_bitwise_shift) {
    XrProto *p = compile_source(
        "let x = 1\nprint(x << 4)\nprint(x >> 0)", NULL);
    assert(p != NULL);
    xr_vm_proto_free(p);
}

/* ========== Compound Assignment ========== */

TEST(e2e_compound_assign) {
    XrProto *p = compile_source(
        "let x = 10\nx += 5\nx -= 3\nx *= 2\nprint(x)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_PRINT));
    xr_vm_proto_free(p);
}

/* ========== Increment / Decrement ========== */

TEST(e2e_inc_dec) {
    XrProto *p = compile_source(
        "let x = 0\nx++\nx++\nx++\nx--\nprint(x)", NULL);
    assert(p != NULL);
    xr_vm_proto_free(p);
}

/* ========== Break / Continue ========== */

TEST(e2e_break) {
    XrProto *p = compile_source(
        "let i = 0\n"
        "while (i < 100) {\n"
        "  if (i == 5) { break }\n"
        "  i = i + 1\n"
        "}\nprint(i)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_JMP));
    xr_vm_proto_free(p);
}

TEST(e2e_continue) {
    XrProto *p = compile_source(
        "let sum = 0\nlet i = 0\n"
        "while (i < 10) {\n"
        "  i = i + 1\n"
        "  if (i % 2 == 0) { continue }\n"
        "  sum = sum + i\n"
        "}\nprint(sum)", NULL);
    assert(p != NULL);
    xr_vm_proto_free(p);
}

/* ========== Multi-branch If-Else ========== */

TEST(e2e_if_else_chain) {
    XrProto *p = compile_source(
        "let x = 7\n"
        "if (x > 10) { print(1) }\n"
        "else if (x > 5) { print(2) }\n"
        "else { print(3) }", NULL);
    assert(p != NULL);
    /* Multiple branches means multiple conditional jumps */
    xr_vm_proto_free(p);
}

/* ========== Float Constants ========== */

TEST(e2e_float_arith) {
    /* let x = 1.5 + 2.5 → folded to 4.0 */
    XrProto *p = compile_source(
        "let x = 1.5 + 2.5\nprint(x)", NULL);
    assert(p != NULL);
    assert(!has_opcode(p, OP_ADD) && "1.5+2.5 should be folded");
    xr_vm_proto_free(p);
}

/* ========== Ternary ========== */

TEST(e2e_ternary) {
    XrProto *p = compile_source(
        "let x = 5\nlet r = x > 3 ? 1 : 0\nprint(r)", NULL);
    assert(p != NULL);
    xr_vm_proto_free(p);
}

/* ========== Logical Short-Circuit ========== */

TEST(e2e_short_circuit) {
    /* Short-circuit AND/OR produce conditional jumps, not BAND/BOR */
    XrProto *p = compile_source(
        "let a = true\nlet b = false\n"
        "if (a && b) { print(1) }\n"
        "if (a || b) { print(2) }", NULL);
    assert(p != NULL);
    /* Should NOT have BAND/BOR — these are logical ops with short-circuit */
    assert(!has_opcode(p, OP_BAND) && "&& should not emit BAND");
    assert(!has_opcode(p, OP_BOR) && "|| should not emit BOR");
    xr_vm_proto_free(p);
}

/* ========== Multiple Functions ========== */

TEST(e2e_multi_func) {
    XrProto *p = compile_source(
        "fn double(x: int): int { return x * 2 }\n"
        "fn negate(x: int): int { return -x }\n"
        "print(negate(double(3)))", NULL);
    assert(p != NULL);
    assert(PROTO_PROTO_COUNT(p) >= 2 && "should have 2 child protos");
    assert(count_opcode(p, OP_CLOSURE) >= 2 && "need 2 CLOSUREs");
    xr_vm_proto_free(p);
}

/* ========== String Concatenation ========== */

TEST(e2e_string_concat) {
    XrProto *p = compile_source(
        "let a = \"hello\"\nlet b = \" world\"\n"
        "let c = a + b\nprint(c)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_ADD) && "string concat uses ADD");
    xr_vm_proto_free(p);
}

/* ========== Map Literal ========== */

TEST(e2e_map_literal) {
    XrProto *p = compile_source(
        "let m = {\"a\": 1, \"b\": 2}\nprint(m)", NULL);
    assert(p != NULL);
    /* Map creation should emit NEWMAP or NEWJSON + field stores */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 3 && "map literal needs multiple instructions");
    xr_vm_proto_free(p);
}

/* ========== Template String ========== */

TEST(e2e_template_string) {
    XrProto *p = compile_source(
        "let x = \"world\"\nlet s = \"hello ${x}\"\nprint(s)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_STRBUF_NEW) && "template uses STRBUF pipeline");
    assert(has_opcode(p, OP_STRBUF_APPEND));
    assert(has_opcode(p, OP_STRBUF_FINISH));
    xr_vm_proto_free(p);
}

/* ========== Nullish Coalesce ========== */

TEST(e2e_nullish_coalesce) {
    XrProto *p = compile_source(
        "let a: int? = null\nlet b = a ?? 42\nprint(b)", NULL);
    assert(p != NULL);
    /* ?? lowers to ISNULL + conditional branch; verify enough instructions */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 3 && "nullish coalesce needs branch logic");
    xr_vm_proto_free(p);
}

/* ========== Match Expression ========== */

TEST(e2e_match_expr) {
    XrProto *p = compile_source(
        "let x = 2\n"
        "let r = match (x) {\n"
        "  1 => 10,\n"
        "  2 => 20,\n"
        "  _ => 0\n"
        "}\nprint(r)", NULL);
    assert(p != NULL);
    /* Match lowers to comparisons + branches; verify enough instructions */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 5 && "match needs comparison + branch logic");
    xr_vm_proto_free(p);
}

/* ========== Try-Catch ========== */

TEST(e2e_try_catch) {
    XrProto *p = compile_source(
        "try { print(1) } catch (e) { print(e) }", NULL);
    assert(p != NULL);
    /* Try-catch should emit SETUP_TRY + POP_TRY or similar */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 3 && "try-catch requires setup/body/handler");
    xr_vm_proto_free(p);
}

/* ========== Slice ========== */

TEST(e2e_slice) {
    XrProto *p = compile_source(
        "let arr = [1, 2, 3, 4, 5]\nlet s = arr[1:3]\nprint(s)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_SLICE) && "slice expression needs OP_SLICE");
    xr_vm_proto_free(p);
}

/* ========== Closure (nested function) ========== */

TEST(e2e_closure) {
    XrProto *p = compile_source(
        "fn make(): fn(): int {\n"
        "  fn inner(): int { return 42 }\n"
        "  return inner\n"
        "}\nlet f = make()\nprint(f())", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_CLOSURE) && "nested func needs OP_CLOSURE");
    assert(PROTO_PROTO_COUNT(p) >= 1 && "should have child proto");
    xr_vm_proto_free(p);
}

/* ========== Type Conversion ========== */

TEST(e2e_type_convert) {
    XrProto *p = compile_source(
        "let x = 42\nlet s = x as string\nprint(s)", NULL);
    assert(p != NULL);
    /* XI_AS lowers to MOVE; just verify pipeline succeeds */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 2 && "type conversion pipeline must produce instructions");
    xr_vm_proto_free(p);
}

/* ========== Range ========== */

TEST(e2e_range) {
    XrProto *p = compile_source(
        "let r = 0..10\nprint(r)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_NEWRANGE) && "range expression needs OP_NEWRANGE");
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

    /* For loop */
    run_e2e_for_loop();

    /* Functions / closures */
    run_e2e_function_decl();
    run_e2e_recursive_func();
    run_e2e_nested_call();

    /* Constant propagation chain */
    run_e2e_const_prop_chain();

    /* Dead code elimination */
    run_e2e_dce_unused_var();

    /* Array operations */
    run_e2e_array_literal();
    run_e2e_array_set();

    /* Bitwise operations */
    run_e2e_bitwise_ops();
    run_e2e_bitwise_shift();

    /* Compound assignment */
    run_e2e_compound_assign();

    /* Increment / decrement */
    run_e2e_inc_dec();

    /* Break / continue */
    run_e2e_break();
    run_e2e_continue();

    /* Multi-branch if-else */
    run_e2e_if_else_chain();

    /* Float arithmetic */
    run_e2e_float_arith();

    /* Ternary */
    run_e2e_ternary();

    /* Logical short-circuit */
    run_e2e_short_circuit();

    /* Multiple functions */
    run_e2e_multi_func();

    /* String concatenation */
    run_e2e_string_concat();

    /* Map literal */
    run_e2e_map_literal();

    /* Template string */
    run_e2e_template_string();

    /* Nullish coalesce */
    run_e2e_nullish_coalesce();

    /* Match expression */
    run_e2e_match_expr();

    /* Try-catch */
    run_e2e_try_catch();

    /* Slice */
    run_e2e_slice();

    /* Closure (nested function) */
    run_e2e_closure();

    /* Type conversion */
    run_e2e_type_convert();

    /* Range */
    run_e2e_range();

    /* API */
    run_e2e_status_str();

    teardown();

    printf("\n=== %d/%d Xi Pipeline tests passed ===\n",
           tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
