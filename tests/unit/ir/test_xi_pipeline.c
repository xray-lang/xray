/*
 * test_xi_pipeline.c - End-to-end tests for Xi IR compilation pipeline
 *
 * Tests the full path: source -> parse -> analyze -> lower -> verify ->
 * optimize -> emit -> XrProto, then inspects the emitted bytecode.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_pipeline.h"
#include "../../../src/ir/xi_emit.h"
#include "../../../src/ir/xi_program_semantic.h"
#include "../../../src/aot/program/xr_backend_ir.h"
#include "../../../src/execution/xr_execution.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/module/xmodule_graph.h"
#include "../../../src/module/xmodule_identity.h"
#include "../../../src/module/xmodule_resolver.h"
#include "../../../src/plan/target/xr_target_profile.h"
#include "../../../src/program/xr_program_from_xi.h"
#include "../../../src/program/xr_program_xi_projection_gen.h"
#include "../../../src/program/xr_program_verify.h"
#include "../../../src/program/xr_reference_evaluator.h"
#include "../../../src/program/xr_validated_program_internal.h"
#include "../../../src/runtime/abi/xr_runtime_target_profile.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../src/vm/xr_program_vm.h"
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

#define PIPELINE_TEST_REQUIRE(condition)                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "pipeline test assertion failed: %s (%s:%d)\n", #condition, __FILE__,  \
                    __LINE__);                                                                     \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* Pipeline assertions and their helper calls must remain active in Release. */
#ifdef NDEBUG
#undef assert
#define assert(condition) PIPELINE_TEST_REQUIRE(condition)
#endif

static XrVMRuntime *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

typedef struct XiProgramProviderBindings {
    XrProviderBinding providers[XR_RUNTIME_ABI_MAX_PROVIDERS];
    XrProviderOperationBinding operations[XR_RUNTIME_ABI_MAX_PROVIDERS]
                                         [XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    size_t count;
} XiProgramProviderBindings;

static void xi_program_provider_entry(void) {
}

static void xi_program_build_provider_bindings(const XrTargetProfile *profile,
                                               XiProgramProviderBindings *bindings) {
    memset(bindings, 0, sizeof(*bindings));
    bindings->count = xr_target_profile_provider_count(profile);
    PIPELINE_TEST_REQUIRE(bindings->count > 0u);
    PIPELINE_TEST_REQUIRE(bindings->count <= XR_RUNTIME_ABI_MAX_PROVIDERS);
    for (size_t provider_index = 0; provider_index < bindings->count; ++provider_index) {
        const XrTargetProviderContract *contract =
            xr_target_profile_provider(profile, provider_index);
        PIPELINE_TEST_REQUIRE(contract != NULL);
        XrProviderBinding *provider = &bindings->providers[provider_index];
        provider->contract_id = contract->contract_id;
        PIPELINE_TEST_REQUIRE(xr_target_provider_contract_fingerprint(
                                  contract, &provider->contract_fingerprint) == XR_RUNTIME_ABI_OK);
        provider->behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        provider->operations = bindings->operations[provider_index];
        provider->operation_count = contract->operation_count;
        PIPELINE_TEST_REQUIRE(provider->operation_count <= XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS);
        for (uint16_t operation_index = 0; operation_index < contract->operation_count;
             ++operation_index) {
            XrProviderOperationBinding *operation =
                &bindings->operations[provider_index][operation_index];
            operation->operation_id = contract->operations[operation_index].stable_id;
            operation->entry = xi_program_provider_entry;
        }
    }
}

static void setup(void) {
    if (!g_iso) {
        XrVMConfig p = {0};
        g_iso = xray_vm_new_full(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
    }
}

static char *compile_source_module_identity(void) {
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = "xi-pipeline-fixture",
    };
    char *identity = NULL;
    if (!xr_module_identity_from_logical(&authority, NULL, &identity) ||
        !xr_module_identity_valid(identity, NULL)) {
        xr_free(identity);
        return NULL;
    }
    return identity;
}

/* Compile source through the full pipeline, return proto.
 * Caller must free both proto and result. */
static XrProto *compile_source(const char *source, XiPipelineConfig *cfg) {
    PIPELINE_TEST_REQUIRE(g_iso != NULL);

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
    char *fixture_identity = NULL;
    if (!effective.module_identity) {
        fixture_identity = compile_source_module_identity();
        if (!fixture_identity) {
            fprintf(stderr, "  MODULE IDENTITY FAILED for: %s\n", source);
            xa_analyzer_free(analyzer);
            xr_program_destroy(program);
            return NULL;
        }
        effective.module_identity = fixture_identity;
    }
    XiPipelineResult res = xi_pipeline_compile_program(program, analyzer, g_iso, &effective);
    xr_free(fixture_identity);

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

typedef struct XiPipelineScalarFixture {
    XrModuleResolver *resolver;
    XrModuleGraph *graph;
    XrModuleSpec *spec;
    XaAnalyzer *analyzer;
} XiPipelineScalarFixture;

static bool xi_pipeline_fixture_analyze_source(XiPipelineScalarFixture *fixture,
                                               XrCompilerSession *session, const char *namespace_id,
                                               const char *source) {
    memset(fixture, 0, sizeof(*fixture));
    XrModuleResolverConfig resolver_config = {0};
    fixture->resolver = xr_module_resolver_new(&resolver_config);
    if (!fixture->resolver)
        return false;
    fixture->graph = xr_module_graph_new(session, fixture->resolver);
    if (!fixture->graph)
        return false;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = namespace_id,
    };
    char *error = NULL;
    if (xr_module_graph_build_source(fixture->graph, &authority, source, &error) != 0) {
        xr_free(error);
        return false;
    }
    xr_free(error);
    if (xr_module_graph_topological_sort(fixture->graph) != 0 || fixture->graph->has_cycle ||
        fixture->graph->spec_count != 1 || fixture->graph->entry_index < 0)
        return false;
    fixture->spec = &fixture->graph->specs[fixture->graph->entry_index];
    fixture->analyzer = xa_analyzer_new(session);
    if (!fixture->analyzer)
        return false;
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    xa_analyzer_analyze(fixture->analyzer, "scalar-binding.xr", fixture->spec->ast);
    int diagnostic_count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(fixture->analyzer, &diagnostic_count);
         diag; diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            return false;
    }
    XrHashMap *exports = NULL;
    if (!xa_analyzer_collect_export_symbols_checked(fixture->analyzer, fixture->spec->ast,
                                                    &exports))
        return false;
    fixture->spec->status = XR_MODSPEC_ANALYZED;
    return true;
}

static bool xi_pipeline_scalar_fixture_analyze(XiPipelineScalarFixture *fixture,
                                               XrCompilerSession *session,
                                               const char *namespace_id) {
    static const char source[] = "fn add1(value: i64) -> i64 { return value + 1 }\n"
                                 "fn root() -> i64 { return add1(41) }\n";
    return xi_pipeline_fixture_analyze_source(fixture, session, namespace_id, source);
}

static void xi_pipeline_scalar_fixture_cleanup(XiPipelineScalarFixture *fixture) {
    xa_analyzer_free(fixture->analyzer);
    xr_module_graph_free(fixture->graph);
    xr_module_resolver_free(fixture->resolver);
    memset(fixture, 0, sizeof(*fixture));
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
    assert(has_opcode(p, OP_PRINT_GROUP_FLUSH));
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
    assert(has_opcode(p, OP_PRINT_GROUP_FLUSH));
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
    /* One flush per source group: the flush is what reaches the output. */
    assert(count_opcode(p, OP_PRINT_GROUP_FLUSH) == 3 && "should have 3 print groups");
    xr_instruction_unit_free(p);
}

/* One source `print` renders as one buffered group, and exactly one of its
 * instructions touches the output capability. A verifier can check that
 * mechanically: count the flushes, not the renders. */
TEST(e2e_print_group_is_one_write) {
    XrProto *p = compile_source("print(1, 2, 3)", NULL);
    assert(p != NULL);
    assert(count_opcode(p, OP_PRINT_GROUP_NEW) == 1 && "one group buffer per source print");
    assert(count_opcode(p, OP_PRINT_GROUP_APPEND) == 3 && "one append per operand");
    assert(count_opcode(p, OP_PRINT_GROUP_FLUSH) == 1 && "one write per source print");
    xr_instruction_unit_free(p);
}

/* An empty group still owns a buffer and still writes its terminator through
 * the same single exit; arity zero is not a special case with its own path. */
TEST(e2e_print_group_zero_arity_still_flushes) {
    XrProto *p = compile_source("print()", NULL);
    assert(p != NULL);
    assert(count_opcode(p, OP_PRINT_GROUP_NEW) == 1);
    assert(count_opcode(p, OP_PRINT_GROUP_APPEND) == 0);
    assert(count_opcode(p, OP_PRINT_GROUP_FLUSH) == 1);
    xr_instruction_unit_free(p);
}

/* ========== String Literals ========== */

TEST(e2e_string_literal) {
    XrProto *p = compile_source("var s = \"hello\"\nprint(s)", NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_PRINT_GROUP_FLUSH));
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
    assert(has_opcode(p, OP_PRINT_GROUP_FLUSH));
    xr_instruction_unit_free(p);
}

/* ========== Function / Closure ========== */

TEST(e2e_function_decl) {
    /* Function declaration should emit CLOSURE opcode and have a child proto */
    XrProto *p = compile_source("fn add(a: i64, b: i64) -> i64 { return a + b }\n"
                                "print(add(1, 2))",
                                NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_CLOSURE) && "function decl needs CLOSURE");
    assert(PROTO_PROTO_COUNT(p) >= 1 && "should have child proto for add()");
    xr_instruction_unit_free(p);
}

TEST(e2e_attached_ir_is_repped) {
    XrProto *p = compile_source("fn add(a: i64, b: i64) -> i64 { return a + b }\n"
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
    XrProto *p = compile_source("fn fib(n: i64) -> i64 {\n"
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
    XrProto *p = compile_source("fn add(a: i64, b: i64) -> i64 {\n"
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
        compile_source("fn f() -> i64 { var x = 42\nvar y = 99\nreturn x }\nprint(f())", NULL);
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
    assert(has_opcode(p, OP_PRINT_GROUP_FLUSH));
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
    XrProto *p = compile_source("fn double(x: i64) -> i64 { return x * 2 }\n"
                                "fn negate(x: i64) -> i64 { return -x }\n"
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
    /* Canonical typed string concatenation lowers to XI_STR_CONCAT. Two parts
     * fit the VM emitter's bounded range form, so the bytecode shape is exact. */
    assert(count_opcode(p, OP_STR_CONCAT_N) == 1 && "two-part string concat uses one STR_CONCAT_N");
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
    assert(count_opcode(p, OP_STR_CONCAT_N) == 1 && "two-part template uses one STR_CONCAT_N");
    xr_instruction_unit_free(p);
}

/* ========== Nullish Coalesce ========== */

TEST(e2e_nullish_coalesce) {
    XrProto *p = compile_source("var a: i64? = null\nvar b = a ?? 42\nprint(b)", NULL);
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
                                "  var s: Slice<i64> = arr[1:3]\n"
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
    XrProto *p = compile_source("fn counter(n: i64) -> Iterator<i64> {\n"
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
    XrProto *p = compile_source("fn make() -> fn() -> i64 {\n"
                                "  fn inner() -> i64 { return 42 }\n"
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
                                 "fn func%d(x: i64) -> i64 {\n  var acc = x\n", f);
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

TEST(e2e_scalar_authority_requires_and_uses_session_profile) {
    XrCompilerSession *original_session = xr_compiler_session_current_for_isolate(g_iso);
    PIPELINE_TEST_REQUIRE(original_session != NULL);
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    PIPELINE_TEST_REQUIRE(session != NULL);
    PIPELINE_TEST_REQUIRE(xr_compiler_session_target_profile(session) == NULL);
    PIPELINE_TEST_REQUIRE(xr_compiler_session_attach_isolate(g_iso, session) == original_session);
    XiPipelineConfig config = xi_pipeline_default_config();
    config.run_emit = false;
    config.run_canonicalize = false;
    config.source_file = "scalar-binding.xr";

    XiPipelineScalarFixture missing_profile = {0};
    PIPELINE_TEST_REQUIRE(xi_pipeline_scalar_fixture_analyze(&missing_profile, session,
                                                             "xi-scalar-pipeline-missing-profile"));
    config.module_identity = missing_profile.spec->canonical;
    config.module_name = "xi_scalar_pipeline_missing_profile";
    XiPipelineResult rejected = xi_pipeline_compile_program(
        missing_profile.spec->ast, missing_profile.analyzer, g_iso, &config);
    PIPELINE_TEST_REQUIRE(rejected.status == XI_PIPE_ERR_INTERNAL);
    PIPELINE_TEST_REQUIRE(rejected.error.stage == XI_PIPE_STAGE_LOWER);
    PIPELINE_TEST_REQUIRE(strstr(rejected.error.detail, "target profile") != NULL);
    PIPELINE_TEST_REQUIRE(rejected.ir == NULL && rejected.proto == NULL);
    xi_pipeline_result_free(&rejected);
    xi_pipeline_scalar_fixture_cleanup(&missing_profile);

    char error[512] = {0};
    XrTargetProfile *profile = NULL;
    PIPELINE_TEST_REQUIRE(
        xr_runtime_target_profile_build_native_hosted(&profile, error, sizeof(error)));
    PIPELINE_TEST_REQUIRE(xr_compiler_session_set_target_profile(session, profile));

    XiPipelineScalarFixture exact_profile = {0};
    PIPELINE_TEST_REQUIRE(xi_pipeline_scalar_fixture_analyze(&exact_profile, session,
                                                             "xi-scalar-pipeline-exact-profile"));
    config.module_identity = exact_profile.spec->canonical;
    config.module_name = "xi_scalar_pipeline_exact_profile";
    XiPipelineResult accepted = xi_pipeline_compile_program(exact_profile.spec->ast,
                                                            exact_profile.analyzer, g_iso, &config);
    if (accepted.status != XI_PIPE_OK)
        fprintf(stderr, "scalar pipeline failed at %s: %s\n",
                xi_pipeline_stage_str(accepted.error.stage), accepted.error.detail);
    PIPELINE_TEST_REQUIRE(accepted.status == XI_PIPE_OK);
    PIPELINE_TEST_REQUIRE(accepted.ir != NULL && accepted.ir->module != NULL);
    PIPELINE_TEST_REQUIRE(accepted.ir->module->program_semantic_closure != NULL);
    PIPELINE_TEST_REQUIRE(accepted.ir->module->scalar_call_decision != NULL);
    PIPELINE_TEST_REQUIRE(
        xi_program_semantic_verify(accepted.ir->module, profile, error, sizeof(error)));
    xi_pipeline_result_free(&accepted);
    xi_pipeline_scalar_fixture_cleanup(&exact_profile);
    xr_target_profile_free(profile);
    PIPELINE_TEST_REQUIRE(xr_compiler_session_attach_isolate(g_iso, original_session) == session);
    xr_compiler_session_delete(session);
}

TEST(e2e_analyzer_error_stops_before_lowering) {
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    assert(analyzer != NULL);
    AstNode *program = xr_parse(session, "var x: i64 = \"not an i64\"\n");
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

TEST(e2e_program_xi_projection_is_exact_and_fail_closed) {
    static const struct {
        uint16_t xi_operation;
        uint16_t result_type;
        uint16_t core_operation;
        uint32_t immediate;
        XrProgramXiProjectionKind kind;
    } rows[] = {
        {XI_CONST, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_CONSTANT_I64, 0u,
         XR_PROGRAM_XI_PROJECTION_CONSTANT},
        {XI_CONST, XR_CORE_TYPE_BOOL, XR_CORE_OP_CORE_CONSTANT_BOOL, 0u,
         XR_PROGRAM_XI_PROJECTION_CONSTANT},
        {XI_ADD, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_ADD_I64, 1u,
         XR_PROGRAM_XI_PROJECTION_BINARY_ARITHMETIC},
        {XI_SUB, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_SUB_I64, 1u,
         XR_PROGRAM_XI_PROJECTION_BINARY_ARITHMETIC},
        {XI_MUL, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_MUL_I64, 1u,
         XR_PROGRAM_XI_PROJECTION_BINARY_ARITHMETIC},
        {XI_DIV, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_DIV_I64, 0u,
         XR_PROGRAM_XI_PROJECTION_BINARY_ARITHMETIC},
        {XI_EQ, XR_CORE_TYPE_BOOL, XR_CORE_OP_CORE_COMPARE_I64, 0u,
         XR_PROGRAM_XI_PROJECTION_COMPARE},
        {XI_NE, XR_CORE_TYPE_BOOL, XR_CORE_OP_CORE_COMPARE_I64, 1u,
         XR_PROGRAM_XI_PROJECTION_COMPARE},
        {XI_LT, XR_CORE_TYPE_BOOL, XR_CORE_OP_CORE_COMPARE_I64, 2u,
         XR_PROGRAM_XI_PROJECTION_COMPARE},
        {XI_LE, XR_CORE_TYPE_BOOL, XR_CORE_OP_CORE_COMPARE_I64, 3u,
         XR_PROGRAM_XI_PROJECTION_COMPARE},
        {XI_GT, XR_CORE_TYPE_BOOL, XR_CORE_OP_CORE_COMPARE_I64, 4u,
         XR_PROGRAM_XI_PROJECTION_COMPARE},
        {XI_GE, XR_CORE_TYPE_BOOL, XR_CORE_OP_CORE_COMPARE_I64, 5u,
         XR_PROGRAM_XI_PROJECTION_COMPARE},
        {XI_CALL, XR_CORE_TYPE_VOID, XR_CORE_OP_CORE_CALL_SEALED_DIRECT, 0u,
         XR_PROGRAM_XI_PROJECTION_SEALED_DIRECT_CALL},
        {XI_TUPLE_NEW, XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, 0u,
         XR_PROGRAM_XI_PROJECTION_AGGREGATE_CONSTRUCT},
        {XI_TUPLE_GET, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_AGGREGATE_PROJECT, 0u,
         XR_PROGRAM_XI_PROJECTION_AGGREGATE_PROJECT},
        {XI_VARIANT_CONSTRUCT, XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, XR_CORE_OP_CORE_VARIANT_CONSTRUCT,
         0u, XR_PROGRAM_XI_PROJECTION_VARIANT_CONSTRUCT},
        {XI_VARIANT_TEST, XR_CORE_TYPE_BOOL, XR_CORE_OP_CORE_VARIANT_TEST, 0u,
         XR_PROGRAM_XI_PROJECTION_VARIANT_TEST},
        {XI_VARIANT_PROJECT, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_VARIANT_PROJECT, 0u,
         XR_PROGRAM_XI_PROJECTION_VARIANT_PROJECT},
    };
    for (size_t index = 0; index < sizeof(rows) / sizeof(rows[0]); ++index) {
        XrProgramXiProjection projection = {0};
        PIPELINE_TEST_REQUIRE(xr_program_xi_projection(rows[index].xi_operation,
                                                       rows[index].result_type, &projection));
        PIPELINE_TEST_REQUIRE(projection.core_operation_id == rows[index].core_operation);
        PIPELINE_TEST_REQUIRE(projection.result_type_id == rows[index].result_type);
        PIPELINE_TEST_REQUIRE(projection.immediate_u32 == rows[index].immediate);
        PIPELINE_TEST_REQUIRE(projection.kind == rows[index].kind);
        PIPELINE_TEST_REQUIRE(xr_program_xi_value_is_materialized(rows[index].xi_operation));
    }

    XrProgramXiProjection rejected = {0};
    PIPELINE_TEST_REQUIRE(!xr_program_xi_projection(XI_ADD, XR_CORE_TYPE_BOOL, &rejected));
    PIPELINE_TEST_REQUIRE(!xr_program_xi_projection(XI_MOD, XR_CORE_TYPE_I64, &rejected));
    PIPELINE_TEST_REQUIRE(!xr_program_xi_projection(XI_ADD, XR_CORE_TYPE_I64, NULL));
    PIPELINE_TEST_REQUIRE(!xr_program_xi_value_is_materialized(XI_MOD));
}

static void require_program_input_tree(const XiFunc *function) {
    PIPELINE_TEST_REQUIRE(function != NULL);
    PIPELINE_TEST_REQUIRE(function->stage == XI_STAGE_OPTIMIZED);
    PIPELINE_TEST_REQUIRE(function->semantic_plan == NULL);
    for (uint16_t child_index = 0; child_index < function->nchildren; child_index++)
        require_program_input_tree(function->children[child_index]);
}

static bool validated_program_has_operation(const XrValidatedProgram *program,
                                            uint16_t operation_id) {
    for (uint32_t function_index = 0; function_index < program->function_count; ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        for (uint32_t block_index = 0; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0; instruction_index < block->instruction_count;
                 ++instruction_index) {
                if (block->instructions[instruction_index].operation_id == operation_id)
                    return true;
            }
        }
    }
    return false;
}

TEST(e2e_program_input_stops_before_legacy_semantic_and_backend_owners) {
    XrCompilerSession *original_session = xr_compiler_session_current_for_isolate(g_iso);
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    PIPELINE_TEST_REQUIRE(session != NULL);
    PIPELINE_TEST_REQUIRE(xr_compiler_session_attach_isolate(g_iso, session) == original_session);
    char error[512] = {0};
    XrTargetProfile *profile = NULL;
    PIPELINE_TEST_REQUIRE(
        xr_runtime_target_profile_build_native_hosted(&profile, error, sizeof(error)));
    PIPELINE_TEST_REQUIRE(xr_compiler_session_set_target_profile(session, profile));

    XiPipelineScalarFixture fixture = {0};
    static const char program_source[] = "enum Packet { Data { code: i64, flag: bool }, Empty }\n"
                                         "enum Nested<T> { Value { pair: (T, bool) }, Empty }\n"
                                         "fn sum_to(limit: i64) -> i64 {\n"
                                         "  var index: i64 = 0\n"
                                         "  var total: i64 = 0\n"
                                         "  while (index < limit) {\n"
                                         "    total = total + index\n"
                                         "    index = index + 1\n"
                                         "  }\n"
                                         "  return total\n"
                                         "}\n"
                                         "fn choose(value: i64) -> i64 {\n"
                                         "  if (value < 0) { return 0 - value }\n"
                                         "  return sum_to(value)\n"
                                         "}\n"
                                         "fn scalar_matrix(left: i64, right: i64) -> i64 {\n"
                                         "  var value: i64 = (left * right) / right\n"
                                         "  if (left == right) { value = value + 100 }\n"
                                         "  if (left != right) { value = value + 1 }\n"
                                         "  if (left <= right) { value = value + 2 }\n"
                                         "  if (left > right) { value = value + 4 }\n"
                                         "  if (left >= right) { value = value + 8 }\n"
                                         "  return value\n"
                                         "}\n"
                                         "fn choose_bool(flag: bool) -> i64 {\n"
                                         "  if (flag) { return 1 }\n"
                                         "  return 2\n"
                                         "}\n"
                                         "fn make_pair(value: i64, flag: bool) -> (i64, bool) {\n"
                                         "  return (value, flag)\n"
                                         "}\n"
                                         "fn pair_value() -> i64 {\n"
                                         "  var pair = make_pair(40, true)\n"
                                         "  return pair.0\n"
                                         "}\n"
                                         "fn packet_value() -> i64 {\n"
                                         "  var packet = Packet.Data { flag: true, code: 29 }\n"
                                         "  return match (packet) {\n"
                                         "    Packet.Data { code } -> code,\n"
                                         "    Packet.Empty -> 0\n"
                                         "  }\n"
                                         "}\n"
                                         "fn accepts_nested(value: Nested<i64>) -> i64 {\n"
                                         "  return 1\n"
                                         "}\n"
                                         "fn root() -> i64 {\n"
                                         "  return choose(10) + scalar_matrix(10, 2) + "
                                         "choose_bool(true) + choose_bool(false) + pair_value() + "
                                         "packet_value()\n"
                                         "}\n";
    PIPELINE_TEST_REQUIRE(
        xi_pipeline_fixture_analyze_source(&fixture, session, "xi-program-input", program_source));

    XiPipelineConfig config = xi_pipeline_program_input_config();
    config.source_file = "scalar-binding.xr";
    config.module_name = "program_input";
    config.module_identity = fixture.spec->canonical;

    XiPipelineResult result =
        xi_pipeline_compile_program(fixture.spec->ast, fixture.analyzer, g_iso, &config);
    if (result.status != XI_PIPE_OK)
        fprintf(stderr, "program input failed at %s: %s\n",
                xi_pipeline_stage_str(result.error.stage), result.error.detail);
    PIPELINE_TEST_REQUIRE(result.status == XI_PIPE_OK);
    PIPELINE_TEST_REQUIRE(result.ir != NULL);
    PIPELINE_TEST_REQUIRE(result.proto == NULL);
    PIPELINE_TEST_REQUIRE(result.ir->module != NULL);
    PIPELINE_TEST_REQUIRE(result.ir->module->program_semantic_closure == NULL);
    PIPELINE_TEST_REQUIRE(result.ir->module->scalar_call_decision == NULL);
    require_program_input_tree(result.ir);

    XrCoreIrKey semantic_profile =
        xr_core_ir_key("xi-program-input-profile", strlen("xi-program-input-profile"));
    const XiFunc *module_roots[] = {result.ir};
    const XiFunc *entry = NULL;
    XiFunc *packet_function = NULL;
    for (uint16_t function = 0; function < result.ir->module->nfuncs; ++function) {
        XiFunc *candidate = result.ir->module->functions[function];
        if (candidate && candidate->name && strcmp(candidate->name, "root") == 0)
            entry = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "packet_value") == 0)
            packet_function = candidate;
    }
    PIPELINE_TEST_REQUIRE(entry != NULL);
    PIPELINE_TEST_REQUIRE(packet_function != NULL);
    XrProgramFromXiInput producer_input = {
        .module_roots = module_roots,
        .module_count = 1u,
        .entry_function = entry,
        .semantic_profile_fingerprint = semantic_profile.bytes,
    };
    XrProgramArtifact artifact = {0};
    char producer_diagnostic[512] = {0};
    XrProgramBuildStatus producer_status = xr_program_write_from_xi(
        &producer_input, &artifact, producer_diagnostic, sizeof(producer_diagnostic));
    if (producer_status != XR_PROGRAM_BUILD_OK)
        fprintf(stderr, "program producer failed: %s: %s\n",
                xr_program_build_status_name(producer_status), producer_diagnostic);
    PIPELINE_TEST_REQUIRE(producer_status == XR_PROGRAM_BUILD_OK);

    XrProgramArtifact repeated_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &repeated_artifact, producer_diagnostic,
                                 sizeof(producer_diagnostic)) == XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(repeated_artifact.size == artifact.size);
    PIPELINE_TEST_REQUIRE(memcmp(repeated_artifact.bytes, artifact.bytes, artifact.size) == 0);
    xr_program_artifact_free(&repeated_artifact);

    XiEnumData *packet_schema = NULL;
    for (uint16_t slot = 0; slot < result.ir->module->nslots; ++slot) {
        XiEnumData *candidate =
            result.ir->module->slot_enums ? result.ir->module->slot_enums[slot] : NULL;
        if (candidate && candidate->name && strcmp(candidate->name, "Packet") == 0) {
            PIPELINE_TEST_REQUIRE(packet_schema == NULL || packet_schema == candidate);
            packet_schema = candidate;
        }
    }
    PIPELINE_TEST_REQUIRE(packet_schema != NULL);
    PIPELINE_TEST_REQUIRE(packet_schema->member_count == 2u);
    PIPELINE_TEST_REQUIRE(packet_schema->members[0].payload_count == 2);

    const char *saved_field_name = packet_schema->members[0].payload_names[0];
    packet_schema->members[0].payload_names[0] = "status";
    XrProgramArtifact renamed_field_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &renamed_field_artifact, producer_diagnostic,
                                 sizeof(producer_diagnostic)) == XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(!xr_program_id_equal(artifact.id, renamed_field_artifact.id));
    xr_program_artifact_free(&renamed_field_artifact);
    packet_schema->members[0].payload_names[0] = saved_field_name;

    const char *saved_second_field_name = packet_schema->members[0].payload_names[1];
    packet_schema->members[0].payload_names[1] = packet_schema->members[0].payload_names[0];
    XrProgramArtifact ambiguous_field_artifact = {0};
    PIPELINE_TEST_REQUIRE(xr_program_write_from_xi(
                              &producer_input, &ambiguous_field_artifact, producer_diagnostic,
                              sizeof(producer_diagnostic)) == XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE);
    PIPELINE_TEST_REQUIRE(ambiguous_field_artifact.bytes == NULL);
    packet_schema->members[0].payload_names[1] = saved_second_field_name;

    XiValue *variant_construct = NULL;
    XiValue *variant_project = NULL;
    for (uint32_t block = 0; block < packet_function->nblocks; ++block) {
        XiBlock *row = packet_function->blocks[block];
        for (uint32_t value = 0; row && value < row->nvalues; ++value) {
            XiValue *operation = row->values[value];
            if (operation->op == XI_VARIANT_CONSTRUCT)
                variant_construct = operation;
            else if (operation->op == XI_VARIANT_PROJECT)
                variant_project = operation;
        }
    }
    PIPELINE_TEST_REQUIRE(variant_construct != NULL);
    PIPELINE_TEST_REQUIRE(variant_project != NULL);

    int64_t saved_construct_ordinal = variant_construct->aux_int;
    variant_construct->aux_int = INT64_C(99);
    XrProgramArtifact invalid_variant_artifact = {0};
    PIPELINE_TEST_REQUIRE(xr_program_write_from_xi(
                              &producer_input, &invalid_variant_artifact, producer_diagnostic,
                              sizeof(producer_diagnostic)) == XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE);
    PIPELINE_TEST_REQUIRE(invalid_variant_artifact.bytes == NULL);
    variant_construct->aux_int = saved_construct_ordinal;

    int64_t saved_projection = variant_project->aux_int;
    variant_project->aux_int = xi_variant_pack_projection(0u, 99u);
    XrProgramArtifact invalid_projection_artifact = {0};
    PIPELINE_TEST_REQUIRE(xr_program_write_from_xi(
                              &producer_input, &invalid_projection_artifact, producer_diagnostic,
                              sizeof(producer_diagnostic)) == XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE);
    PIPELINE_TEST_REQUIRE(invalid_projection_artifact.bytes == NULL);
    variant_project->aux_int = saved_projection;

    XrType *saved_projection_type = variant_project->type;
    variant_project->type = variant_construct->type;
    XrProgramArtifact invalid_projection_type_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &invalid_projection_type_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE);
    PIPELINE_TEST_REQUIRE(invalid_projection_type_artifact.bytes == NULL);
    variant_project->type = saved_projection_type;

    XrProgramFromXiInput invalid_entry_input = producer_input;
    invalid_entry_input.entry_function = result.ir;
    XrProgramArtifact invalid_entry_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&invalid_entry_input, &invalid_entry_artifact, producer_diagnostic,
                                 sizeof(producer_diagnostic)) == XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(invalid_entry_artifact.bytes == NULL);

    XrValidatedProgram *validated = NULL;
    XrProgramDiagnostic verify_diagnostic;
    PIPELINE_TEST_REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated,
                                              &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    PIPELINE_TEST_REQUIRE(validated != NULL);
    bool found_nested_generic_variant = false;
    for (uint32_t type_index = 0; type_index < validated->type_count; ++type_index) {
        const XrValidatedType *variant = &validated->types[type_index];
        if (variant->kind != XR_CORE_IR_TYPE_VARIANT || variant->variant_count != 2u ||
            !variant->variants || variant->variants[0].payload_count != 1u ||
            !variant->variants[0].payload_types)
            continue;
        uint16_t payload_type = variant->variants[0].payload_types[0];
        const XrValidatedType *aggregate = NULL;
        for (uint32_t candidate = 0; candidate < validated->type_count; ++candidate) {
            if (validated->types[candidate].type_id == payload_type) {
                aggregate = &validated->types[candidate];
                break;
            }
        }
        if (!aggregate || aggregate->kind != XR_CORE_IR_TYPE_AGGREGATE ||
            aggregate->field_count != 2u || !aggregate->field_types)
            continue;
        if (aggregate->field_types[0] == XR_CORE_TYPE_I64 &&
            aggregate->field_types[1] == XR_CORE_TYPE_BOOL)
            found_nested_generic_variant = true;
    }
    PIPELINE_TEST_REQUIRE(found_nested_generic_variant);
    static const uint16_t required_source_operations[] = {
        XR_CORE_OP_CORE_CONSTANT_I64,
        XR_CORE_OP_CORE_CONSTANT_BOOL,
        XR_CORE_OP_CORE_ADD_I64,
        XR_CORE_OP_CORE_SUB_I64,
        XR_CORE_OP_CORE_MUL_I64,
        XR_CORE_OP_CORE_DIV_I64,
        XR_CORE_OP_CORE_COMPARE_I64,
        XR_CORE_OP_CORE_BLOCK_ARGUMENT,
        XR_CORE_OP_CORE_BRANCH,
        XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
        XR_CORE_OP_CORE_RETURN,
        XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
        XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
        XR_CORE_OP_CORE_AGGREGATE_PROJECT,
        XR_CORE_OP_CORE_VARIANT_CONSTRUCT,
        XR_CORE_OP_CORE_VARIANT_TEST,
        XR_CORE_OP_CORE_VARIANT_PROJECT,
    };
    for (size_t index = 0;
         index < sizeof(required_source_operations) / sizeof(required_source_operations[0]);
         ++index) {
        PIPELINE_TEST_REQUIRE(
            validated_program_has_operation(validated, required_source_operations[index]));
    }
    XrReferenceOutcome reference = xr_reference_evaluate(
        validated, xr_validated_program_entry_function(validated), NULL, 0u, NULL, NULL);
    PIPELINE_TEST_REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
    PIPELINE_TEST_REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_I64);
    PIPELINE_TEST_REQUIRE(reference.value.as.i64 == 140);

    XiProgramProviderBindings bindings;
    xi_program_build_provider_bindings(profile, &bindings);
    XrExecutionBindingInput execution_input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = validated,
        .profile = profile,
        .providers = bindings.providers,
        .provider_count = bindings.count,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    PIPELINE_TEST_REQUIRE(xr_execution_instance_create(&execution_input, &instance,
                                                       &execution_diagnostic) == XR_EXECUTION_OK);
    PIPELINE_TEST_REQUIRE(instance != NULL);

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    PIPELINE_TEST_REQUIRE(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic) ==
                          XR_VM_CODE_OK);
    XrVmOutcome vm = xr_vm_code_execute(vm_code, instance,
                                        xr_validated_program_entry_function(validated), NULL, 0u);
    PIPELINE_TEST_REQUIRE(vm.kind == XR_VM_OUTCOME_RETURN);
    PIPELINE_TEST_REQUIRE(vm.value.kind == XR_VM_VALUE_I64);
    PIPELINE_TEST_REQUIRE(vm.value.as.i64 == reference.value.as.i64);
    xr_vm_code_free(vm_code);

    XrBackendIR *backend_ir = NULL;
    XrBackendDiagnostic backend_diagnostic;
    XrBackendOptions backend_options = xr_backend_default_options();
    PIPELINE_TEST_REQUIRE(xr_backend_ir_build(instance, &backend_options, &backend_ir,
                                              &backend_diagnostic) == XR_BACKEND_OK);
    PIPELINE_TEST_REQUIRE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    PIPELINE_TEST_REQUIRE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    PIPELINE_TEST_REQUIRE(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic) ==
                          XR_BACKEND_OK);
    PIPELINE_TEST_REQUIRE(generated.bytes != NULL && generated.size != 0u);
    PIPELINE_TEST_REQUIRE(strstr(generated.bytes, "int main(void)") != NULL);
    PIPELINE_TEST_REQUIRE(strstr(generated.bytes, "XrProto") == NULL);
    PIPELINE_TEST_REQUIRE(strstr(generated.bytes, "SemanticPlan") == NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);

    PIPELINE_TEST_REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) ==
                          XR_EXECUTION_OK);
    PIPELINE_TEST_REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) ==
                          XR_EXECUTION_OK);
    PIPELINE_TEST_REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) ==
                          XR_EXECUTION_OK);
    xr_validated_program_free(validated);
    xr_program_artifact_free(&artifact);

    xi_pipeline_result_free(&result);
    xi_pipeline_scalar_fixture_cleanup(&fixture);
    xr_target_profile_free(profile);
    PIPELINE_TEST_REQUIRE(xr_compiler_session_attach_isolate(g_iso, original_session) == session);
    xr_compiler_session_delete(session);
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
    const char *source = "class Router {\n"
                         "    add<T>(value: T) -> i64 { return this.addRoute(value) }\n"
                         "    addRoute<T>(value: T) -> i64 { return 7 }\n"
                         "}\n"
                         "var router = Router()\n"
                         "print(router.add(1))\n";
    XrProto *p = compile_source(source, NULL);
    assert(p != NULL);
    assert(has_opcode(p, OP_PRINT_GROUP_FLUSH));
    xr_instruction_unit_free(p);
}

/* ========== Main ========== */

/* Release strips assert(), so a case whose only checks are asserts reports PASS
 * in a Release build without having checked anything.  The emission check below
 * is the one thing standing behind "a group publishes all of itself or none of
 * it", so it is checked in both builds. */
#define PIPE_CHECK(cond, what)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);                            \
            tests_failed++;                                                                        \
        }                                                                                          \
    } while (0)

/* The emission check must reject a group that writes twice, and it must be
 * observed rejecting it rather than assumed to.
 *
 * The mutation turns an append into a second flush, and the case asserts the
 * instruction was not already a flush before doing so.  Without that step a
 * mutation can land on an instruction that already held the target opcode: the
 * assignment changes nothing, the verifier correctly accepts an untampered
 * program, and the case reads as "tamper detection is broken" while nothing was
 * ever tampered with. */
TEST(e2e_print_group_second_write_is_refused) {
    XrProto *p = compile_source("print(1, 2, 3)", NULL);
    PIPE_CHECK(p != NULL, "print fixture compiles");
    if (!p)
        return;

    char err[256];
    PIPE_CHECK(xi_emit_verify_print_groups(p, err, sizeof(err)),
               "an unmutated group passes the emission check");

    XrInstruction *code = PROTO_CODE_BASE(p);
    uint32_t n = (uint32_t) PROTO_CODE_COUNT(p);
    uint32_t at = n;
    for (uint32_t i = 0; i < n; i++) {
        if (GET_OPCODE(code[i]) == OP_PRINT_GROUP_APPEND) {
            at = i;
            break;
        }
    }
    PIPE_CHECK(at < n, "the fixture contains an append to mutate");
    if (at >= n) {
        xr_instruction_unit_free(p);
        return;
    }
    PIPE_CHECK(GET_OPCODE(code[at]) != OP_PRINT_GROUP_FLUSH,
               "the mutation must change the opcode rather than restate it");

    code[at] = CREATE_ABC(OP_PRINT_GROUP_FLUSH, GETARG_A(code[at]), 0, 0);
    PIPE_CHECK(GET_OPCODE(code[at]) == OP_PRINT_GROUP_FLUSH, "the mutation took effect");
    PIPE_CHECK(!xi_emit_verify_print_groups(p, err, sizeof(err)),
               "a group that reaches the output capability twice is refused");
    /* Printing the reason is what makes a silent pass distinguishable from a
     * check that never ran: an empty reason means nothing was refused. */
    printf("    refused with: %s\n", err);

    xr_instruction_unit_free(p);
}

/* A group that never flushes is refused for the mirror-image reason: its buffer
 * would be discarded with the rendered text still inside. */
TEST(e2e_print_group_without_write_is_refused) {
    XrProto *p = compile_source("print(1, 2, 3)", NULL);
    PIPE_CHECK(p != NULL, "print fixture compiles");
    if (!p)
        return;

    XrInstruction *code = PROTO_CODE_BASE(p);
    uint32_t n = (uint32_t) PROTO_CODE_COUNT(p);
    uint32_t at = n;
    for (uint32_t i = 0; i < n; i++) {
        if (GET_OPCODE(code[i]) == OP_PRINT_GROUP_FLUSH) {
            at = i;
            break;
        }
    }
    PIPE_CHECK(at < n, "the fixture contains a flush to remove");
    if (at >= n) {
        xr_instruction_unit_free(p);
        return;
    }
    PIPE_CHECK(GET_OPCODE(code[at]) != OP_PRINT_GROUP_APPEND,
               "the mutation must change the opcode rather than restate it");

    char err[256];
    code[at] = CREATE_ABC(OP_PRINT_GROUP_APPEND, GETARG_A(code[at]), 0, 0);
    PIPE_CHECK(GET_OPCODE(code[at]) == OP_PRINT_GROUP_APPEND, "the mutation took effect");
    PIPE_CHECK(!xi_emit_verify_print_groups(p, err, sizeof(err)),
               "a group that never reaches the output capability is refused");
    printf("    refused with: %s\n", err);

    xr_instruction_unit_free(p);
}

int main(void) {
    printf("=== Xi Pipeline E2E Tests ===\n\n");

    setup();

    /* The first pipeline KAT installs the exact session profile consumed by
     * every later source compilation in this process. */
    run_e2e_scalar_authority_requires_and_uses_session_profile();

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
    run_e2e_print_group_is_one_write();
    run_e2e_print_group_zero_arity_still_flushes();
    run_e2e_print_group_second_write_is_refused();
    run_e2e_print_group_without_write_is_refused();

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
    run_e2e_program_xi_projection_is_exact_and_fail_closed();
    run_e2e_program_input_stops_before_legacy_semantic_and_backend_owners();
    run_e2e_time_sleep_uses_dedicated_vm_suspend();
    run_e2e_generic_this_method_call_uses_frozen_member_identity();

    teardown();

    printf("\n=== %d/%d Xi Pipeline tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
