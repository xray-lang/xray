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
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../include/xray_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ========== Test Infrastructure ========== */

static XrVMRuntime *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

static void setup(void) {
    if (!g_iso) {
        XrVMConfig p;
        xray_vm_config_init(&p);
        g_iso = xray_vm_new_full(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
    }
}

/* Compile source through the full pipeline, return proto.
 * Caller must free both proto and result. */
static XrProto *compile_source(const char *source, XiPipelineConfig *cfg) {
    assert(g_iso != NULL);

    /* Create analyzer first — its type pool must be active during parsing
     * so the parser can create type annotations (function types, etc.). */
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer)
        return NULL;

    AstNode *program = xr_parse(session, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %s\n", source);
        xa_analyzer_free(analyzer);
        return NULL;
    }

    xa_analyzer_analyze(analyzer, "test.xr", program);

    /* SemanticPlan debug identities require the same canonical source file
     * that the analyzer used. Keep the caller's policy intact while making
     * this in-memory fixture equivalent to a real file compilation. */
    XiPipelineConfig effective = cfg ? *cfg : xi_pipeline_default_config();
    effective.source_file = "test.xr";
    XiPipelineResult res = xi_pipeline_compile_program(program, analyzer, g_iso, &effective);

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    if (res.status != XI_PIPE_OK) {
        fprintf(stderr, "  PIPELINE FAILED at %s: %s\n", xi_pipeline_stage_str(res.error.stage),
                res.error.detail);
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

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* ========== Constant & Arithmetic Tests ========== */

TEST(e2e_simple_const) {
    /* var x = 42
     * print(x)
     * Expect: LOADI + PRINT + RETURN */
    XrProto *p = compile_source("var x = 42\nprint(x)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_PRINT));
    xr_instruction_unit_free(p);
}

TEST(e2e_arithmetic) {
    /* var x = 1 + 2
     * var y = x * 3
     * After const folding: x=3, y=9 (both constants). */
    XrProto *p = compile_source("var x = 1 + 2\nvar y = x * 3\nprint(y)", NULL);
    assert(p != NULL);
    /* After optimization, ADD and MUL should be folded away */
    assert(!has_opcode(p, OP_ADD) && "1+2 should be folded");
    assert(!has_opcode(p, OP_MUL) && "3*3 should be folded");
    xr_instruction_unit_free(p);
}

TEST(e2e_variable_assignment) {
    /* var x = 10
     * x = x + 5
     * print(x) */
    XrProto *p = compile_source("var x = 10\nx = x + 5\nprint(x)", NULL);
    assert(p != NULL);
    /* After const folding: x=15, so no ADD */
    assert(!has_opcode(p, OP_ADD) && "10+5 should be folded");
    xr_instruction_unit_free(p);
}

/* ========== Control Flow Tests ========== */

TEST(e2e_if_else) {
    /* if (true) { print(1) } else { print(2) } */
    XrProto *p = compile_source("if (true) { print(1) } else { print(2) }", NULL);
    assert(p != NULL);
    /* With const folding, the branch may be eliminated */
    assert(has_opcode(p, OP_PRINT));
    xr_instruction_unit_free(p);
}

TEST(e2e_while_loop) {
    /* var i = 0
     * while i < 3 { i = i + 1 }
     * print(i) */
    XrProto *p = compile_source("var i = 0\nwhile (i < 3) { i = i + 1 }\nprint(i)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_JMP) && "while loop needs JMP");
    xr_instruction_unit_free(p);
}

/* ========== Pipeline Configuration Tests ========== */

TEST(e2e_no_optimize) {
    /* Verify that unoptimized path works */
    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.run_optimize = false;
    XrProto *p = compile_source("var x = 1 + 2\nprint(x)", &cfg);
    assert(p != NULL);
    /* Without optimization, constant folding doesn't run, so arithmetic remains.
     * Instruction fusion may emit ADDI instead of ADD for small constant args. */
    assert((has_opcode(p, OP_ADD) || has_opcode(p, OP_ADDI)) &&
           "unoptimized should keep ADD or ADDI");
    xr_instruction_unit_free(p);
}

TEST(e2e_with_verify) {
    /* Verify passes by default */
    XiPipelineConfig cfg = xi_pipeline_default_config();
    XrProto *p = compile_source("var x = 42\nprint(x)", &cfg);
    assert(p != NULL);
    xr_instruction_unit_free(p);
}

/* ========== Boolean & Comparison ========== */

TEST(e2e_bool_ops) {
    /* var a = true
     * var b = false
     * print(a) */
    XrProto *p = compile_source("var a = true\nvar b = false\nprint(a)", NULL);
    assert(p != NULL);
    xr_instruction_unit_free(p);
}

TEST(e2e_comparison) {
    /* var x = 5 > 3
     * print(x)
     * After const folding: x=true */
    XrProto *p = compile_source("var x = 5 > 3\nprint(x)", NULL);
    assert(p != NULL);
    xr_instruction_unit_free(p);
}

/* ========== Multiple Statements ========== */

TEST(e2e_multi_print) {
    /* print(1)
     * print(2)
     * print(3) */
    XrProto *p = compile_source("print(1)\nprint(2)\nprint(3)", NULL);
    assert(p != NULL);
    assert(count_opcode(p, OP_PRINT) == 3 && "should have 3 PRINT ops");
    xr_instruction_unit_free(p);
}

/* ========== String Literals ========== */

TEST(e2e_string_literal) {
    XrProto *p = compile_source("var s = \"hello\"\nprint(s)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_PRINT));
    xr_instruction_unit_free(p);
}

/* ========== Unary Ops ========== */

TEST(e2e_unary_neg) {
    /* var x = -42
     * After const folding: x = -42 */
    XrProto *p = compile_source("var x = -42\nprint(x)", NULL);
    assert(p != NULL);
    xr_instruction_unit_free(p);
}

/* ========== For Loop ========== */

TEST(e2e_for_loop) {
    XrProto *p = compile_source("var sum = 0\n"
                                "for (var i = 0; i < 5; i = i + 1) { sum = sum + i }\n"
                                "print(sum)",
                                NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_JMP) && "for loop needs backward JMP");
    assert(has_opcode(p, OP_PRINT));
    xr_instruction_unit_free(p);
}

/* ========== Function / Closure ========== */

TEST(e2e_function_decl) {
    /* Function declaration should emit CLOSURE opcode and have a child proto */
    XrProto *p = compile_source("fn add(a: int, b: int) -> int { return a + b }\n"
                                "print(add(1, 2))",
                                NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_CLOSURE) && "function decl needs CLOSURE");
    assert(PROTO_PROTO_COUNT(p) >= 1 && "should have child proto for add()");
    xr_instruction_unit_free(p);
}

TEST(e2e_attached_ir_is_repped) {
    XrProto *p = compile_source("fn add(a: int, b: int) -> int { return a + b }\n"
                                "print(add(1, 2))",
                                NULL);
    assert(p != NULL);
    assert(p->xi_func != NULL);
    XiFunc *root = (XiFunc *) p->xi_func;
    assert(root->stage >= XI_STAGE_REPPED);
    assert((root->invariant_mask & XI_INV_REPS_SELECTED) != 0);
    assert(PROTO_PROTO_COUNT(p) >= 1);
    XrProto *child = PROTO_PROTO(p, 0);
    assert(child != NULL);
    assert(child->xi_func != NULL);
    XiFunc *child_ir = (XiFunc *) child->xi_func;
    assert(child_ir->stage >= XI_STAGE_REPPED);
    assert((child_ir->invariant_mask & XI_INV_REPS_SELECTED) != 0);
    xr_instruction_unit_free(p);
}

TEST(e2e_recursive_func) {
    XrProto *p = compile_source("fn fib(n: int) -> int {\n"
                                "  if (n <= 1) { return n }\n"
                                "  return fib(n - 1) + fib(n - 2)\n"
                                "}\nprint(fib(5))",
                                NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_CLOSURE));
    /* Child proto should use CALLSELF for recursion */
    XrProto *child = PROTO_PROTO(p, 0);
    assert(has_opcode(child, OP_CALLSELF) && "recursive call should use CALLSELF");
    xr_instruction_unit_free(p);
}

TEST(e2e_nested_call) {
    /* Tests the register clobber fix: nested calls to same function.
     * The callee branches so the inliner (which folds straight-line
     * shared-slot helpers into the caller) keeps both calls alive. */
    XrProto *p = compile_source("fn add(a: int, b: int) -> int {\n"
                                "  if (a < 0) { return b }\n"
                                "  return a + b\n"
                                "}\n"
                                "print(add(1, add(2, 3)))",
                                NULL);
    assert(p != NULL);
    /* Main proto should have 2 call instructions (not CALLSELF).  Calls to
     * a statically known closure emit OP_CALL_STATIC; dynamic ones OP_CALL. */
    int ncalls = count_opcode(p, OP_CALL) + count_opcode(p, OP_CALL_STATIC);
    assert(ncalls >= 2 && "nested calls need >= 2 calls");
    assert(!has_opcode(p, OP_CALLSELF) && "main proto must not self-call");
    xr_instruction_unit_free(p);
}

/* ========== Constant Propagation Chain ========== */

TEST(e2e_const_prop_chain) {
    /* var a = 2; var b = a + 3; var c = b * 4; print(c)
     * After folding: a=2, b=5, c=20. No arithmetic ops. */
    XrProto *p = compile_source("var a = 2\nvar b = a + 3\nvar c = b * 4\nprint(c)", NULL);
    assert(p != NULL);
    assert(!has_opcode(p, OP_ADD) && "chain should fold ADD away");
    assert(!has_opcode(p, OP_MUL) && "chain should fold MUL away");
    xr_instruction_unit_free(p);
}

/* ========== Dead Code Elimination ========== */

TEST(e2e_dce_unused_var) {
    /* Top-level vars are stored via SETSHARED (side effect) so DCE keeps
     * them.  Test inside a function where locals are register-only.
     *   fn f() -> int { var x = 42; var y = 99; return x }
     * y is unused → LOADI 99 should be eliminated from the child proto. */
    XrProto *p =
        compile_source("fn f() -> int { var x = 42\nvar y = 99\nreturn x }\nprint(f())", NULL);
    assert(p != NULL);
    /* Child proto (f) should have only one LOADI (for x=42); y=99 is dead */
    int nch = DYNARRAY_COUNT(&p->protos);
    assert(nch >= 1 && "need at least one child proto for f()");
    XrProto *child = DYNARRAY_GET(&p->protos, 0, XrProto *);
    int loads = count_opcode(child, OP_LOADI) + count_opcode(child, OP_LOADK);
    assert(loads <= 1 && "unused y should be eliminated by DCE");
    xr_instruction_unit_free(p);
}

/* ========== Array Operations ========== */

TEST(e2e_array_literal) {
    XrProto *p = compile_source("var arr = [10, 20, 30]\nprint(arr[1])", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_NEWARRAY) && "array literal needs NEWARRAY");
    assert(has_opcode(p, OP_INDEX_GET) && "arr[1] needs INDEX_GET");
    xr_instruction_unit_free(p);
}

TEST(e2e_array_set) {
    XrProto *p = compile_source("var arr = [1, 2, 3]\narr[0] = 99\nprint(arr[0])", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_INDEX_SET) && "arr[0]=99 needs INDEX_SET");
    xr_instruction_unit_free(p);
}

TEST(e2e_native_pointer_store_narrows_to_pointee) {
    XrProto *p = compile_source("fn write(p: ref MutPtr<u16>, value: u16) {\n"
                                "  unsafe { p[0] = value }\n"
                                "}",
                                NULL);
    assert(p != NULL && "raw pointer stores must preserve the pointee's native width");
    xr_instruction_unit_free(p);
}

TEST(e2e_inlined_ref_forwarding_remaps_place_origin) {
    XrProto *p = compile_source("fn core(cursor: ref Ptr<u8>) {\n"
                                "  cursor = cursor.offset(1)\n"
                                "}\n"
                                "fn forward(cursor: ref Ptr<u8>) {\n"
                                "  core(ref cursor)\n"
                                "}\n"
                                "fn adapter(input: Ptr<u8>) {\n"
                                "  var cursor = input\n"
                                "  forward(ref cursor)\n"
                                "}",
                                NULL);
    assert(p != NULL && "inlining must remap forwarded ref place origins into the caller");
    xr_instruction_unit_free(p);
}

/* ========== Bitwise Operations ========== */

TEST(e2e_bitwise_ops) {
    /* Variable-based bitwise ops should emit real instructions */
    XrProto *p = compile_source("var a = 12\nvar b = 10\n"
                                "print(a & b)\nprint(a | b)\nprint(a ^ b)",
                                NULL);
    assert(p != NULL);
    xr_instruction_unit_free(p);
}

TEST(e2e_bitwise_shift) {
    XrProto *p = compile_source("var x = 1\nprint(x << 4)\nprint(x >> 0)", NULL);
    assert(p != NULL);
    xr_instruction_unit_free(p);
}

/* ========== Compound Assignment ========== */

TEST(e2e_compound_assign) {
    XrProto *p = compile_source("var x = 10\nx += 5\nx -= 3\nx *= 2\nprint(x)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_PRINT));
    xr_instruction_unit_free(p);
}

/* ========== Increment / Decrement ========== */

TEST(e2e_inc_dec) {
    XrProto *p = compile_source("var x = 0\nx++\nx++\nx++\nx--\nprint(x)", NULL);
    assert(p != NULL);
    xr_instruction_unit_free(p);
}

/* ========== Break / Continue ========== */

TEST(e2e_break) {
    XrProto *p = compile_source("var i = 0\n"
                                "while (i < 100) {\n"
                                "  if (i == 5) { break }\n"
                                "  i = i + 1\n"
                                "}\nprint(i)",
                                NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_JMP));
    xr_instruction_unit_free(p);
}

TEST(e2e_continue) {
    XrProto *p = compile_source("var sum = 0\nvar i = 0\n"
                                "while (i < 10) {\n"
                                "  i = i + 1\n"
                                "  if (i % 2 == 0) { continue }\n"
                                "  sum = sum + i\n"
                                "}\nprint(sum)",
                                NULL);
    assert(p != NULL);
    xr_instruction_unit_free(p);
}

/* ========== Multi-branch If-Else ========== */

TEST(e2e_if_else_chain) {
    XrProto *p = compile_source("var x = 7\n"
                                "if (x > 10) { print(1) }\n"
                                "else if (x > 5) { print(2) }\n"
                                "else { print(3) }",
                                NULL);
    assert(p != NULL);
    /* Multiple branches means multiple conditional jumps */
    xr_instruction_unit_free(p);
}

/* ========== Float Constants ========== */

TEST(e2e_float_arith) {
    /* var x = 1.5 + 2.5 → folded to 4.0 */
    XrProto *p = compile_source("var x = 1.5 + 2.5\nprint(x)", NULL);
    assert(p != NULL);
    assert(!has_opcode(p, OP_ADD) && "1.5+2.5 should be folded");
    xr_instruction_unit_free(p);
}

/* ========== Ternary ========== */

TEST(e2e_ternary) {
    XrProto *p = compile_source("var x = 5\nvar r = x > 3 ? 1 : 0\nprint(r)", NULL);
    assert(p != NULL);
    xr_instruction_unit_free(p);
}

/* ========== Logical Short-Circuit ========== */

TEST(e2e_short_circuit) {
    /* Pure boolean operands may be speculated eagerly as BAND/BOR. */
    XrProto *p = compile_source("var a = true\nvar b = false\n"
                                "if (a && b) { print(1) }\n"
                                "if (a || b) { print(2) }",
                                NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_BAND) && "pure && should use eager BAND");
    assert(has_opcode(p, OP_BOR) && "pure || should use eager BOR");
    xr_instruction_unit_free(p);
}

/* ========== Multiple Functions ========== */

TEST(e2e_multi_func) {
    XrProto *p = compile_source("fn double(x: int) -> int { return x * 2 }\n"
                                "fn negate(x: int) -> int { return -x }\n"
                                "print(negate(double(3)))",
                                NULL);
    assert(p != NULL);
    assert(PROTO_PROTO_COUNT(p) >= 2 && "should have 2 child protos");
    assert(count_opcode(p, OP_CLOSURE) >= 2 && "need 2 CLOSUREs");
    xr_instruction_unit_free(p);
}

/* ========== String Concatenation ========== */

TEST(e2e_string_concat) {
    XrProto *p = compile_source("var a = \"hello\"\nvar b = \" world\"\n"
                                "var c = a + b\nprint(c)",
                                NULL);
    assert(p != NULL);
    /* Xi pipeline uses STRBUF optimization for typed string concat,
     * or falls back to ADD when types are unknown. Accept either. */
    assert((has_opcode(p, OP_ADD) || has_opcode(p, OP_STRBUF_FINISH)) &&
           "string concat uses ADD or STRBUF");
    xr_instruction_unit_free(p);
}

/* ========== Map Literal ========== */

TEST(e2e_map_literal) {
    XrProto *p = compile_source("var m = {\"a\": 1, \"b\": 2}\nprint(m)", NULL);
    assert(p != NULL);
    /* Map creation should emit NEWMAP or NEWJSON + field stores */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 3 && "map literal needs multiple instructions");
    xr_instruction_unit_free(p);
}

/* ========== Template String ========== */

TEST(e2e_template_string) {
    XrProto *p = compile_source("var x = \"world\"\nvar s = \"hello ${x}\"\nprint(s)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_STRBUF_NEW) && "template uses STRBUF pipeline");
    assert(has_opcode(p, OP_STRBUF_APPEND));
    assert(has_opcode(p, OP_STRBUF_FINISH));
    xr_instruction_unit_free(p);
}

/* ========== Nullish Coalesce ========== */

TEST(e2e_nullish_coalesce) {
    XrProto *p = compile_source("var a: int? = null\nvar b = a ?? 42\nprint(b)", NULL);
    assert(p != NULL);
    /* ?? lowers to ISNULL + conditional branch; verify enough instructions */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 3 && "nullish coalesce needs branch logic");
    xr_instruction_unit_free(p);
}

/* ========== Match Expression ========== */

TEST(e2e_match_expr) {
    XrProto *p = compile_source("var x = 2\n"
                                "var r = match (x) {\n"
                                "  1 -> 10,\n"
                                "  2 -> 20,\n"
                                "  _ -> 0\n"
                                "}\nprint(r)",
                                NULL);
    assert(p != NULL);
    /* Match lowers to comparisons + branches; verify enough instructions */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 5 && "match needs comparison + branch logic");
    xr_instruction_unit_free(p);
}

/* ========== Try-Catch ========== */

TEST(e2e_try_catch) {
    XrProto *p = compile_source("try { print(1) } catch (e) { print(e) }", NULL);
    assert(p != NULL);
    /* Try-catch should emit SETUP_TRY + POP_TRY or similar */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 3 && "try-catch requires setup/body/handler");
    xr_instruction_unit_free(p);
}

/* ========== Slice ========== */

TEST(e2e_slice) {
    XrProto *p = compile_source("fn useSlice() {\n"
                                "  var arr = [1, 2, 3, 4, 5]\n"
                                "  var s: Slice<int> = arr[1:3]\n"
                                "  print(s)\n"
                                "}\n"
                                "useSlice()",
                                NULL);
    assert(p != NULL);
    assert(PROTO_PROTO_COUNT(p) >= 1);
    assert(has_opcode(PROTO_PROTO(p, 0), OP_SLICE) && "slice expression needs OP_SLICE");
    xr_instruction_unit_free(p);
}

TEST(e2e_generator_completion_has_no_normal_return_value) {
    XrProto *p = compile_source("fn counter(n: int) -> Iterator<int> {\n"
                                "  yield n\n"
                                "}\n"
                                "for (x in counter(1)) {\n"
                                "  print(x)\n"
                                "}",
                                NULL);
    assert(p != NULL);
    assert(PROTO_PROTO_COUNT(p) >= 1);
    assert(has_opcode(PROTO_PROTO(p, 0), OP_GEN_YIELD));
    xr_instruction_unit_free(p);
}

/* ========== Closure (nested function) ========== */

TEST(e2e_closure) {
    XrProto *p = compile_source("fn make() -> fn() -> int {\n"
                                "  fn inner() -> int { return 42 }\n"
                                "  return inner\n"
                                "}\nvar f = make()\nprint(f())",
                                NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_CLOSURE) && "nested func needs OP_CLOSURE");
    assert(PROTO_PROTO_COUNT(p) >= 1 && "should have child proto");
    xr_instruction_unit_free(p);
}

/* ========== Type Conversion ========== */

TEST(e2e_type_convert) {
    XrProto *p = compile_source("var x = 42\nvar s = x as string\nprint(s)", NULL);
    assert(p != NULL);
    /* XI_AS lowers to MOVE; just verify pipeline succeeds */
    int total = PROTO_CODE_COUNT(p);
    assert(total >= 2 && "type conversion pipeline must produce instructions");
    xr_instruction_unit_free(p);
}

/* ========== Range ========== */

TEST(e2e_range) {
    XrProto *p = compile_source("var r = 0..10\nprint(r)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_NEWRANGE) && "range expression needs OP_NEWRANGE");
    xr_instruction_unit_free(p);

    p = compile_source("var r = 0..=10\nprint(r)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_NEWRANGE_INCLUSIVE) &&
           "inclusive range expression needs OP_NEWRANGE_INCLUSIVE");
    xr_instruction_unit_free(p);
}

/* ========== Budget Stress Tests ========== */

static char *gen_large_sequential(int nstmts) {
    size_t cap = (size_t) nstmts * 40 + 256;
    char *buf = (char *) malloc(cap);
    if (!buf)
        return NULL;
    size_t pos = 0;
    for (int i = 0; i < nstmts; i++)
        pos += (size_t) snprintf(buf + pos, cap - pos, "var v%d = %d + %d\n", i, i, i * 2);
    pos += (size_t) snprintf(buf + pos, cap - pos, "print(v%d)\n", nstmts - 1);
    return buf;
}

static char *gen_nested_loops(int depth, int body_stmts) {
    size_t cap = (size_t) (depth * 80 + body_stmts * 40 + 256);
    char *buf = (char *) malloc(cap);
    if (!buf)
        return NULL;
    size_t pos = 0;
    pos += (size_t) snprintf(buf + pos, cap - pos, "var result = 0\n");
    for (int d = 0; d < depth; d++)
        pos += (size_t) snprintf(buf + pos, cap - pos,
                                 "for (var i%d = 0; i%d < 3; i%d = i%d + 1) {\n", d, d, d, d);
    for (int s = 0; s < body_stmts; s++)
        pos += (size_t) snprintf(buf + pos, cap - pos, "result = result + %d\n", s + 1);
    for (int d = 0; d < depth; d++)
        pos += (size_t) snprintf(buf + pos, cap - pos, "}\n");
    pos += (size_t) snprintf(buf + pos, cap - pos, "print(result)\n");
    return buf;
}

static char *gen_many_functions(int nfuncs, int body_size) {
    size_t cap = (size_t) (nfuncs * (body_size * 40 + 120) + 256);
    char *buf = (char *) malloc(cap);
    if (!buf)
        return NULL;
    size_t pos = 0;
    for (int f = 0; f < nfuncs; f++) {
        pos += (size_t) snprintf(buf + pos, cap - pos,
                                 "fn func%d(x: int) -> int {\n  var acc = x\n", f);
        for (int s = 0; s < body_size; s++)
            pos += (size_t) snprintf(buf + pos, cap - pos, "  acc = acc + %d\n", s + 1);
        pos += (size_t) snprintf(buf + pos, cap - pos, "  return acc\n}\n");
    }
    pos += (size_t) snprintf(buf + pos, cap - pos, "var r = func0(1)\n");
    for (int f = 1; f < nfuncs; f++)
        pos += (size_t) snprintf(buf + pos, cap - pos, "r = func%d(r)\n", f);
    pos += (size_t) snprintf(buf + pos, cap - pos, "print(r)\n");
    return buf;
}

static uint64_t clock_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t) (counter.QuadPart * 1000000000ULL / (uint64_t) freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
#endif
}

TEST(stress_large_sequential_with_budget) {
    char *src = gen_large_sequential(200);
    assert(src != NULL);

    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.budget_ns = 2ULL * 1000 * 1000; /* 2 ms */

    uint64_t t0 = clock_ns();
    XrProto *p = compile_source(src, &cfg);
    uint64_t elapsed_ms = (clock_ns() - t0) / 1000000;

    assert(p != NULL);
    assert(elapsed_ms < 5000 && "budget stress should finish in < 5s");
    printf("  200 stmts, 2ms budget -> %llu ms\n", (unsigned long long) elapsed_ms);

    xr_instruction_unit_free(p);
    free(src);
}

TEST(stress_large_sequential_no_budget) {
    char *src = gen_large_sequential(200);
    assert(src != NULL);

    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.budget_ns = 0; /* unlimited */

    uint64_t t0 = clock_ns();
    XrProto *p = compile_source(src, &cfg);
    uint64_t elapsed_ms = (clock_ns() - t0) / 1000000;

    assert(p != NULL);
    assert(elapsed_ms < 10000 && "no-budget stress should finish in < 10s");
    printf("  200 stmts, no budget -> %llu ms\n", (unsigned long long) elapsed_ms);

    xr_instruction_unit_free(p);
    free(src);
}

TEST(stress_nested_loops_with_budget) {
    char *src = gen_nested_loops(3, 20);
    assert(src != NULL);

    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.budget_ns = 5ULL * 1000 * 1000; /* 5 ms */

    uint64_t t0 = clock_ns();
    XrProto *p = compile_source(src, &cfg);
    uint64_t elapsed_ms = (clock_ns() - t0) / 1000000;

    assert(p != NULL);
    assert(elapsed_ms < 5000 && "nested-loop stress should finish in < 5s");
    printf("  3-deep loops, 5ms budget -> %llu ms\n", (unsigned long long) elapsed_ms);

    xr_instruction_unit_free(p);
    free(src);
}

TEST(stress_many_functions_with_budget) {
    char *src = gen_many_functions(20, 15);
    assert(src != NULL);

    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.budget_ns = 3ULL * 1000 * 1000; /* 3 ms */

    uint64_t t0 = clock_ns();
    XrProto *p = compile_source(src, &cfg);
    uint64_t elapsed_ms = (clock_ns() - t0) / 1000000;

    assert(p != NULL);
    assert(elapsed_ms < 5000 && "many-func stress should finish in < 5s");
    printf("  20 funcs x 15 stmts, 3ms budget -> %llu ms\n", (unsigned long long) elapsed_ms);

    xr_instruction_unit_free(p);
    free(src);
}

static char *gen_large_reuse(int nstmts) {
    size_t cap = (size_t) nstmts * 30 + 256;
    char *buf = (char *) malloc(cap);
    if (!buf)
        return NULL;
    size_t pos = 0;
    pos += (size_t) snprintf(buf + pos, cap - pos, "var acc = 0\n");
    for (int i = 0; i < nstmts; i++)
        pos += (size_t) snprintf(buf + pos, cap - pos, "acc = acc + %d\n", i + 1);
    pos += (size_t) snprintf(buf + pos, cap - pos, "print(acc)\n");
    return buf;
}

TEST(stress_budget_truncation_still_valid) {
    char *src = gen_large_reuse(400);
    assert(src != NULL);

    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.budget_ns = 1ULL * 1000 * 1000; /* 1 ms: very tight */

    XrProto *p = compile_source(src, &cfg);
    assert(p != NULL);

    int total = PROTO_CODE_COUNT(p);
    assert(total > 0 && "truncated pipeline must still emit bytecode");

    xr_instruction_unit_free(p);
    free(src);
}

/* ========== Pipeline Status API ========== */

TEST(e2e_analyzer_error_stops_before_lowering) {
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    assert(analyzer != NULL);
    AstNode *program = xr_parse(session, "var x: int = \"not an int\"\n");
    assert(program != NULL);
    xa_analyzer_analyze(analyzer, "invalid.xr", program);

    XiPipelineResult res = xi_pipeline_compile_program(program, analyzer, g_iso, NULL);
    assert(res.status == XI_PIPE_ERR_ANALYZE);
    assert(res.error.stage == XI_PIPE_STAGE_ANALYZE);
    assert(res.error.code == XI_VERIFY_EXECUTABLE_TYPE);
    assert(res.error.detail[0] != '\0');
    assert(res.ir == NULL && res.proto == NULL);

    xi_pipeline_result_free(&res);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
}

TEST(e2e_status_str) {
    assert(strcmp(xi_pipe_status_str(XI_PIPE_OK), "OK") == 0);
    assert(strcmp(xi_pipe_status_str(XI_PIPE_ERR_ANALYZE), "semantic analysis failed") == 0);
    assert(strcmp(xi_pipe_status_str(XI_PIPE_ERR_LOWER), "AST lowering failed") == 0);
    assert(strcmp(xi_pipeline_stage_str(XI_PIPE_STAGE_OPTIMIZE), "optimize") == 0);
}

TEST(e2e_time_sleep_uses_dedicated_vm_suspend) {
    XrProto *p = compile_source("import time\ntime.sleep(1)\nprint(7)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_SLEEP));
    assert(xr_entry_plan_derive(p));
    assert(p->entry_plan.root_representation == XR_ROOT_RESUMABLE_FRAME);
    assert(p->entry_plan.scheduler_mode == XR_SCHED_SINGLE);
    xr_instruction_unit_free(p);
}

TEST(e2e_generic_this_method_call_uses_frozen_member_identity) {
    const char *source =
        "class Router {\n"
        "    add<T>(value: T) -> int { return this.addRoute(value) }\n"
        "    addRoute<T>(value: T) -> int { return 7 }\n"
        "}\n"
        "var router = Router()\n"
        "print(router.add(1))\n";
    XrProto *p = compile_source(source, NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_PRINT));
    xr_instruction_unit_free(p);
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
    run_e2e_attached_ir_is_repped();
    run_e2e_recursive_func();
    run_e2e_nested_call();

    /* Constant propagation chain */
    run_e2e_const_prop_chain();

    /* Dead code elimination */
    run_e2e_dce_unused_var();

    /* Array operations */
    run_e2e_array_literal();
    run_e2e_array_set();
    run_e2e_native_pointer_store_narrows_to_pointee();
    run_e2e_inlined_ref_forwarding_remaps_place_origin();

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

    /* Generator completion */
    run_e2e_generator_completion_has_no_normal_return_value();

    /* Closure (nested function) */
    run_e2e_closure();

    /* Type conversion */
    run_e2e_type_convert();

    /* Range */
    run_e2e_range();

    /* Budget stress tests */
    printf("\n--- Budget Stress Tests ---\n");
    run_stress_large_sequential_with_budget();
    run_stress_large_sequential_no_budget();
    run_stress_nested_loops_with_budget();
    run_stress_many_functions_with_budget();
    run_stress_budget_truncation_still_valid();

    /* API */
    run_e2e_analyzer_error_stops_before_lowering();
    run_e2e_status_str();
    run_e2e_time_sleep_uses_dedicated_vm_suspend();
    run_e2e_generic_this_method_call_uses_frozen_member_identity();

    teardown();

    printf("\n=== %d/%d Xi Pipeline tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
