/*
 * test_xi_pipeline.c - End-to-end tests for Xi IR compilation pipeline
 *
 * Tests the full path: source -> parse -> analyze -> lower -> verify ->
 * optimize -> emit -> XrProto, then inspects the emitted bytecode.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_ops_gen.h"
#include "../../../src/ir/xi_pipeline.h"
#include "../../../src/ir/xi_emit.h"
#include "../../../src/ir/xi_program_semantic.h"
#include "../../../src/ir/xi_program_semantic_plan.h"
#include "../../../src/aot/program/xr_backend_ir.h"
#include "../../../src/analysis/xglobal_producer.h"
#include "../../../src/execution/xr_execution.h"
#include "../../../src/frontend/canonical/xcanon.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/frontend/analyzer/xanalyzer_mono.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/ir/xi_import_resolve.h"
#include "../../../src/module/xmodule_graph.h"
#include "../../../src/module/xmodule_identity.h"
#include "../../../src/module/xmodule_resolver.h"
#include "../../../src/plan/target/xr_target_profile.h"
#include "../../../src/program/xr_program_from_xi.h"
#include "../../../src/program/xr_program_xi_projection_gen.h"
#include "../../../src/program/xr_program_verify.h"
#include "../../../src/program/xr_reference_evaluator.h"
#include "../../../src/program/xr_validated_program_internal.h"
#include "../../../src/runtime/class/xclass_info.h"
#include "../../../src/runtime/abi/xr_runtime_target_profile.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../src/vm/xr_program_vm.h"
#include "../../../include/xray_vm.h"
#include "../test_win_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define PIPELINE_SNPRINTF(buffer, size, ...) sprintf_s((buffer), (size), __VA_ARGS__)
#else
#define PIPELINE_SNPRINTF(buffer, size, ...) snprintf((buffer), (size), __VA_ARGS__)
#endif

_Static_assert(XR_CORE_OP_CORE_PANIC_PUBLISH == 50, "panic publish stable id drifted");

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
static const char *g_source_aot_output_path = NULL;
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
        if (diag->severity == XR_DIAG_SEV_ERROR) {
            fprintf(stderr, "canonical Program analysis failed: %s\n", diag->message);
            return false;
        }
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

static bool xi_pipeline_fixture_build_global_evidence(XiPipelineScalarFixture *fixture,
                                                      XrCompilerSession *session,
                                                      XgGlobalEvidence *evidence) {
    if (!fixture || !fixture->spec || !fixture->spec->ast || !fixture->analyzer || !session ||
        !evidence)
        return false;
    XrCompilerSessionScope canon_scope;
    bool has_canon_scope =
        fixture->spec->ast->type == AST_PROGRAM && fixture->spec->ast->as.program.arena &&
        xr_compiler_session_push_arena(session, fixture->spec->ast->as.program.arena,
                                       fixture->spec->source_path, &canon_scope);
    XrCanonStatus canon = xr_canon_program(fixture->spec->ast, fixture->analyzer, session);
    if (has_canon_scope)
        xr_compiler_session_pop_arena(&canon_scope);
    if (canon != XR_CANON_OK)
        return false;

    xa_analyzer_clear_diagnostics(fixture->analyzer);
    xa_analyzer_update(fixture->analyzer, "scalar-binding.xr", fixture->spec->ast);
    int diagnostic_count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(fixture->analyzer, &diagnostic_count);
         diag; diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            return false;
    }
    if (fixture->spec->export_symbols) {
        xr_hashmap_free(fixture->spec->export_symbols);
        fixture->spec->export_symbols = NULL;
    }
    if (!xa_analyzer_collect_export_symbols_checked(fixture->analyzer, fixture->spec->ast,
                                                    &fixture->spec->export_symbols))
        return false;
    fixture->spec->status = XR_MODSPEC_ANALYZED;
    memset(evidence, 0, sizeof(*evidence));
    return xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
        evidence, fixture->graph, XG_BUILD_NATIVE_RELEASE, 0u, NULL, 0u, fixture->analyzer);
}

static XiFunc *xi_pipeline_find_function_by_xg_id(XiFunc *root, XgFuncId func_id) {
    if (!root || func_id == XG_NO_ID)
        return NULL;
    if (root->xg_body_func_id == func_id)
        return root;
    for (uint16_t child_index = 0u; child_index < root->nchildren; ++child_index) {
        XiFunc *found =
            xi_pipeline_find_function_by_xg_id(root->children[child_index], func_id);
        if (found)
            return found;
    }
    return NULL;
}

static XiFunc *xi_pipeline_find_module_function_by_xg_id(XiFunc *root, XgFuncId func_id) {
    if (!root || !root->module)
        return NULL;
    XiFunc *found = xi_pipeline_find_function_by_xg_id(root, func_id);
    for (uint16_t index = 0u; !found && index < root->module->nfuncs; ++index)
        found = xi_pipeline_find_function_by_xg_id(root->module->functions[index], func_id);
    return found;
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
        pos += (size_t) PIPELINE_SNPRINTF(buf + pos, cap - pos, "var v%d = %d + %d\n", i, i,
                                         i * 2);
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
    PIPELINE_TEST_REQUIRE(accepted.ir->semantic_plan != NULL);
    XrSemanticOperationRecord *scalar_call = NULL;
    for (size_t index = 0;
         index < xr_semantic_plan_operation_count(accepted.ir->semantic_plan); ++index) {
        XrSemanticOperationRecord *operation = (XrSemanticOperationRecord *)
            xr_semantic_plan_operation(accepted.ir->semantic_plan, index);
        if (!operation || operation->opcode != XI_CALL)
            continue;
        PIPELINE_TEST_REQUIRE(scalar_call == NULL);
        scalar_call = operation;
    }
    PIPELINE_TEST_REQUIRE(scalar_call != NULL);
    PIPELINE_TEST_REQUIRE(scalar_call->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED);
    PIPELINE_TEST_REQUIRE(scalar_call->return_parameter == -1);
    PIPELINE_TEST_REQUIRE(scalar_call->return_provenance == XR_SEM_RETURN_OWNED);
    PIPELINE_TEST_REQUIRE(scalar_call->return_complete == 1);
    PIPELINE_TEST_REQUIRE(
        xi_program_semantic_verify(accepted.ir->module, profile, error, sizeof(error)));
    uint8_t saved_result_ownership = scalar_call->result_ownership;
    scalar_call->result_ownership = XI_GEN_RESULT_OWNERSHIP_CALL_RESULT;
    PIPELINE_TEST_REQUIRE(!xi_program_semantic_plan_verify(
        accepted.ir, accepted.ir->semantic_plan, profile, error, sizeof(error)));
    scalar_call->result_ownership = saved_result_ownership;
    PIPELINE_TEST_REQUIRE(xi_program_semantic_plan_verify(
        accepted.ir, accepted.ir->semantic_plan, profile, error, sizeof(error)));
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
        {XI_CALL_BUILTIN, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_OWNER_COPY, 0u,
         XR_PROGRAM_XI_PROJECTION_OWNER_COPY},
        {XI_TUPLE_NEW, XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, 0u,
         XR_PROGRAM_XI_PROJECTION_AGGREGATE_CONSTRUCT},
        {XI_TUPLE_GET, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_AGGREGATE_PROJECT, 0u,
         XR_PROGRAM_XI_PROJECTION_AGGREGATE_PROJECT},
        {XI_AGG_GET, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_AGGREGATE_PROJECT, 0u,
         XR_PROGRAM_XI_PROJECTION_AGGREGATE_PROJECT},
        {XI_LOAD_UPVAL, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_AGGREGATE_PROJECT, 0u,
         XR_PROGRAM_XI_PROJECTION_AGGREGATE_PROJECT},
        {XI_AGG_UPDATE, XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, XR_CORE_OP_CORE_AGGREGATE_UPDATE, 0u,
         XR_PROGRAM_XI_PROJECTION_AGGREGATE_UPDATE},
        {XI_VARIANT_CONSTRUCT, XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, XR_CORE_OP_CORE_VARIANT_CONSTRUCT,
         0u, XR_PROGRAM_XI_PROJECTION_VARIANT_CONSTRUCT},
        {XI_VARIANT_TEST, XR_CORE_TYPE_BOOL, XR_CORE_OP_CORE_VARIANT_TEST, 0u,
         XR_PROGRAM_XI_PROJECTION_VARIANT_TEST},
        {XI_VARIANT_PROJECT, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_VARIANT_PROJECT, 0u,
         XR_PROGRAM_XI_PROJECTION_VARIANT_PROJECT},
        {XI_SOURCE_MOVE, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_OWNER_MOVE, 0u,
         XR_PROGRAM_XI_PROJECTION_OWNER_MOVE},
        {XI_OWNER_FORWARD, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_OWNER_MOVE, 0u,
         XR_PROGRAM_XI_PROJECTION_OWNER_MOVE},
        {XI_LOCAL_ADDR, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_PLACE_LOCAL, 0u,
         XR_PROGRAM_XI_PROJECTION_PLACE_LOCAL},
        {XI_PLACE_LOAD, XR_CORE_TYPE_I64, XR_CORE_OP_CORE_PLACE_LOAD, 0u,
         XR_PROGRAM_XI_PROJECTION_PLACE_LOAD},
        {XI_PLACE_STORE, XR_CORE_TYPE_VOID, XR_CORE_OP_CORE_PLACE_STORE, 0u,
         XR_PROGRAM_XI_PROJECTION_PLACE_STORE},
        {XI_CLOSURE_NEW, XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, XR_CORE_OP_CORE_CALLABLE_PACK, 0u,
         XR_PROGRAM_XI_PROJECTION_CALLABLE_PACK},
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
        uint32_t effects = UINT32_MAX;
        uint32_t capabilities = UINT32_MAX;
        const XrCoreOperationSpec *operation =
            xr_core_spec_operation_by_id(rows[index].core_operation);
        PIPELINE_TEST_REQUIRE(operation != NULL);
        PIPELINE_TEST_REQUIRE(
            xr_program_xi_operation_contract(rows[index].xi_operation, &effects, &capabilities));
        PIPELINE_TEST_REQUIRE(effects == operation->effect_mask);
        PIPELINE_TEST_REQUIRE(capabilities == operation->capability_mask);
    }

    XrProgramXiProjection rejected = {0};
    PIPELINE_TEST_REQUIRE(!xr_program_xi_projection(XI_ADD, XR_CORE_TYPE_BOOL, &rejected));
    PIPELINE_TEST_REQUIRE(!xr_program_xi_projection(XI_MOD, XR_CORE_TYPE_I64, &rejected));
    PIPELINE_TEST_REQUIRE(!xr_program_xi_projection(XI_ADD, XR_CORE_TYPE_I64, NULL));
    PIPELINE_TEST_REQUIRE(!xr_program_xi_value_is_materialized(XI_MOD));
    uint32_t effects = 0u;
    uint32_t capabilities = 0u;
    PIPELINE_TEST_REQUIRE(!xr_program_xi_operation_contract(XI_MOD, &effects, &capabilities));
    PIPELINE_TEST_REQUIRE(!xr_program_xi_operation_contract(XI_ADD, NULL, &capabilities));
    PIPELINE_TEST_REQUIRE(!xr_program_xi_operation_contract(XI_ADD, &effects, NULL));
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

static uint32_t validated_program_operation_count(const XrValidatedProgram *program,
                                                  uint16_t operation_id) {
    uint32_t count = 0u;
    for (uint32_t function_index = 0; function_index < program->function_count; ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        for (uint32_t block_index = 0; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0; instruction_index < block->instruction_count;
                 ++instruction_index)
                count += block->instructions[instruction_index].operation_id == operation_id;
        }
    }
    return count;
}

static bool validated_program_has_owned_storage_copy_pack(const XrValidatedProgram *program) {
    for (uint32_t function_index = 0u; program && function_index < program->function_count;
         ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 1u;
                 instruction_index < block->instruction_count; ++instruction_index) {
                const XrValidatedInstruction *pack = &block->instructions[instruction_index];
                const XrValidatedType *existential =
                    xr_validated_program_type(program, pack->result_type_id);
                if (pack->operation_id != XR_CORE_OP_CORE_EXISTENTIAL_PACK || !existential ||
                    existential->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
                    existential->interface_use_kind !=
                        XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE ||
                    pack->operand_count != 1u || !pack->operands)
                    continue;
                const XrValidatedInstruction *copy =
                    &block->instructions[instruction_index - 1u];
                if (copy->operation_id == XR_CORE_OP_CORE_OWNER_COPY &&
                    copy->result_id != XR_PROGRAM_LOCATION_NONE &&
                    copy->result_ownership == XR_CORE_IR_OWNER &&
                    pack->operands[0] == copy->result_id)
                    return true;
            }
        }
    }
    return false;
}

static bool xi_function_has_operation(const XiFunc *function, uint16_t operation_id) {
    for (uint32_t block = 0; function && block < function->nblocks; ++block) {
        const XiBlock *row = function->blocks[block];
        for (uint32_t value = 0; row && value < row->nvalues; ++value) {
            if (row->values[value] && row->values[value]->op == operation_id)
                return true;
        }
    }
    return false;
}

static bool unreachable_callable_return_preserves_program(
    XiFunc *function, XiValue *conflicting_return, const XrProgramFromXiInput *input,
    const XrProgramArtifact *baseline) {
    if (!function || !conflicting_return || !input || !baseline || !baseline->bytes ||
        baseline->size == 0u || !function->blocks || function->nblocks == 0u ||
        function->nblocks == UINT32_MAX || function->next_block_id == UINT32_MAX)
        return false;

    uint32_t saved_block_count = function->nblocks;
    uint32_t saved_next_block_id = function->next_block_id;
    uint32_t saved_block_capacity = function->blocks_cap;
    XiBlock **saved_blocks = function->blocks;
    XiBlock **scoped_blocks =
        xr_malloc(((size_t) saved_block_count + 1u) * sizeof(*scoped_blocks));
    if (!scoped_blocks)
        return false;
    memcpy(scoped_blocks, saved_blocks, (size_t) saved_block_count * sizeof(*scoped_blocks));

    XiBlock detached = {0};
    detached.id = saved_next_block_id;
    detached.func = function;
    xi_block_set_return(&detached, conflicting_return);
    scoped_blocks[saved_block_count] = &detached;
    function->blocks = scoped_blocks;
    function->blocks_cap = saved_block_count + 1u;
    function->nblocks = saved_block_count + 1u;
    function->next_block_id = saved_next_block_id + 1u;

    char diagnostic[512] = {0};
    XrProgramArtifact candidate = {0};
    XrProgramBuildStatus status =
        xr_program_write_from_xi(input, &candidate, diagnostic, sizeof(diagnostic));

    function->blocks = saved_blocks;
    function->blocks_cap = saved_block_capacity;
    function->next_block_id = saved_next_block_id;
    function->nblocks = saved_block_count;
    xr_free(scoped_blocks);

    bool unchanged = status == XR_PROGRAM_BUILD_OK && detached.npreds == 0u && candidate.bytes &&
                     candidate.size == baseline->size &&
                     xr_program_id_equal(candidate.id, baseline->id) &&
                     memcmp(candidate.bytes, baseline->bytes, baseline->size) == 0;
    xr_program_artifact_free(&candidate);
    return unchanged;
}

static bool xi_pipeline_program_write_has_status(const XrProgramFromXiInput *input,
                                                 XrProgramBuildStatus expected_status,
                                                 const char *diagnostic_fragment,
                                                 const XrProgramArtifact *expected_artifact) {
    char diagnostic[512] = {0};
    XrProgramArtifact artifact = {0};
    XrProgramBuildStatus status =
        xr_program_write_from_xi(input, &artifact, diagnostic, sizeof(diagnostic));
    bool matches = status == expected_status &&
                   (!diagnostic_fragment || strstr(diagnostic, diagnostic_fragment) != NULL);
    if (expected_status == XR_PROGRAM_BUILD_OK) {
        matches = matches && artifact.bytes && artifact.size != 0u;
        if (expected_artifact)
            matches = matches && artifact.size == expected_artifact->size &&
                      xr_program_id_equal(artifact.id, expected_artifact->id) &&
                      memcmp(artifact.bytes, expected_artifact->bytes, artifact.size) == 0;
    } else {
        matches = matches && artifact.bytes == NULL;
    }
    xr_program_artifact_free(&artifact);
    return matches;
}

static XiValue *xi_pipeline_error_check_for_region(XiFunc *function,
                                                   const XiErrorRegion *region) {
    XiValue *found = NULL;
    for (uint32_t block_index = 0u; function && block_index < function->nblocks; ++block_index) {
        XiBlock *block = function->blocks[block_index];
        XiValue *control = block ? block->control : NULL;
        if (!control || control->op != XI_ERR_CHECK || control->error_region != region)
            continue;
        if (found)
            return NULL;
        found = control;
    }
    return found;
}

static uint32_t xi_pipeline_collect_error_regions(XiFunc *function, XiErrorRegion **regions,
                                                  uint32_t capacity) {
    uint32_t count = 0u;
    for (uint32_t block_index = 0u; function && block_index < function->nblocks; ++block_index) {
        XiBlock *block = function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            XiValue *value = block->values[value_index];
            if (!value || value->op != XI_ERR_CATCH || !value->error_region)
                continue;
            bool duplicate = false;
            for (uint32_t region_index = 0u; region_index < count; ++region_index)
                duplicate |= regions[region_index] == value->error_region;
            if (duplicate)
                continue;
            if (count >= capacity)
                return capacity + 1u;
            regions[count++] = value->error_region;
        }
    }
    return count;
}

static bool xi_pipeline_block_reaches(const XiFunc *function, const XiBlock *start,
                                      const XiBlock *target) {
    if (!function || !start || !target || function->nblocks == 0u)
        return false;
    uint8_t *seen = xr_calloc(function->nblocks, sizeof(*seen));
    const XiBlock **work = xr_calloc(function->nblocks, sizeof(*work));
    if (!seen || !work) {
        xr_free(seen);
        xr_free(work);
        return false;
    }
    uint32_t start_index = UINT32_MAX;
    for (uint32_t index = 0u; index < function->nblocks; ++index)
        if (function->blocks[index] == start) {
            start_index = index;
            break;
        }
    if (start_index == UINT32_MAX) {
        xr_free(seen);
        xr_free(work);
        return false;
    }
    uint32_t head = 0u;
    uint32_t tail = 0u;
    bool found = false;
    seen[start_index] = 1u;
    work[tail++] = start;
    while (head < tail && !found) {
        const XiBlock *block = work[head++];
        found = block == target;
        for (uint32_t successor_index = 0u; successor_index < 2u && !found;
             ++successor_index) {
            const XiBlock *successor = block->succs[successor_index];
            for (uint32_t index = 0u; successor && index < function->nblocks; ++index) {
                if (function->blocks[index] != successor || seen[index])
                    continue;
                seen[index] = 1u;
                work[tail++] = successor;
                break;
            }
        }
    }
    xr_free(seen);
    xr_free(work);
    return found;
}

static bool xi_pipeline_hostile_lexical_error_regions_are_fail_closed(
    XiFunc *function, const XrProgramFromXiInput *input, const XrProgramArtifact *baseline) {
    XiErrorRegion *regions[2] = {0};
    if (!function || !input || !baseline ||
        xi_pipeline_collect_error_regions(function, regions, 2u) != 2u)
        return false;

    XiErrorRegion *earlier = NULL;
    XiErrorRegion *later = NULL;
    if (xi_pipeline_block_reaches(function, regions[0]->body_block,
                                  regions[1]->registration_block) &&
        !xi_pipeline_block_reaches(function, regions[1]->body_block,
                                   regions[0]->registration_block)) {
        earlier = regions[0];
        later = regions[1];
    } else if (xi_pipeline_block_reaches(function, regions[1]->body_block,
                                         regions[0]->registration_block) &&
               !xi_pipeline_block_reaches(function, regions[0]->body_block,
                                          regions[1]->registration_block)) {
        earlier = regions[1];
        later = regions[0];
    } else {
        return false;
    }
    XiValue *later_check = xi_pipeline_error_check_for_region(function, later);
    if (!later_check || earlier->parent || later->parent)
        return false;

    XiErrorRegion *saved_check_region = later_check->error_region;
    later_check->error_region = earlier;
    bool out_of_region_rejected = xi_pipeline_program_write_has_status(
        input, XR_PROGRAM_BUILD_INVALID_INPUT, "nearest lexical error region", NULL);
    later_check->error_region = saved_check_region;
    if (!out_of_region_rejected)
        return false;

    XiErrorRegion *saved_parent = later->parent;
    later->parent = earlier;
    bool sibling_parent_rejected = xi_pipeline_program_write_has_status(
        input, XR_PROGRAM_BUILD_INVALID_INPUT, "parent is outside its lexical region", NULL);
    later->parent = saved_parent;
    if (!sibling_parent_rejected)
        return false;

    XiErrorRegion detached_parent = {0};
    later->parent = &detached_parent;
    bool detached_parent_rejected = xi_pipeline_program_write_has_status(
        input, XR_PROGRAM_BUILD_INVALID_INPUT, "incomplete canonical structure", NULL);
    later->parent = saved_parent;
    if (!detached_parent_rejected)
        return false;

    XiBlock *saved_earlier_merge = earlier->merge_block;
    earlier->merge_block = later->merge_block;
    bool overlap_rejected = xi_pipeline_program_write_has_status(
        input, XR_PROGRAM_BUILD_INVALID_INPUT,
        "overlap without a lexical nesting relation", NULL);
    earlier->merge_block = saved_earlier_merge;
    if (!overlap_rejected)
        return false;

    earlier->merge_block = later->merge_block;
    later->parent = earlier;
    bool nested_accepted = xi_pipeline_program_write_has_status(
        input, XR_PROGRAM_BUILD_OK, NULL, baseline);
    later->parent = saved_parent;
    earlier->merge_block = saved_earlier_merge;
    return nested_accepted;
}

static bool xi_pipeline_same_successor_edges_keep_distinct_phi_arguments(
    XiFunc *edge_function, XiFunc *entry_function, const XrProgramFromXiInput *input,
    int64_t expected_result) {
    if (!edge_function || !entry_function || !input || edge_function->nparams != 3u ||
        !edge_function->params || !edge_function->params[1] || !edge_function->params[2])
        return false;
    XiBlock *branch = NULL;
    for (uint32_t block_index = 0u; block_index < edge_function->nblocks; ++block_index) {
        XiBlock *candidate = edge_function->blocks[block_index];
        if (!candidate || candidate->kind != XI_BLOCK_IF || !candidate->succs[0] ||
            !candidate->succs[1] || candidate->succs[0] == candidate->succs[1])
            continue;
        if (branch)
            return false;
        branch = candidate;
    }
    if (!branch)
        return false;
    XiBlock *target = branch->succs[0];
    XiBlock *dead = branch->succs[1];
    if (!target || !dead || target->npreds != 1u || !target->preds ||
        target->preds[0] != branch || target->phis)
        return false;

    XrProgramFromXiInput scoped_input = *input;
    scoped_input.entry_function = entry_function;
    char baseline_diagnostic[512] = {0};
    XrProgramArtifact baseline = {0};
    if (xr_program_write_from_xi(&scoped_input, &baseline, baseline_diagnostic,
                                 sizeof(baseline_diagnostic)) != XR_PROGRAM_BUILD_OK) {
        xr_program_artifact_free(&baseline);
        return false;
    }

    XiBlock *saved_first_successor = branch->succs[0];
    XiBlock *saved_second_successor = branch->succs[1];
    XiBlock **saved_target_preds = target->preds;
    uint16_t saved_target_pred_count = target->npreds;
    uint16_t saved_target_pred_capacity = target->preds_cap;
    XiPhi *saved_target_phis = target->phis;
    XiValue *saved_target_control = target->control;
    XiValue **saved_dead_values = dead->values;
    uint32_t saved_dead_value_count = dead->nvalues;
    uint32_t saved_dead_value_capacity = dead->values_cap;
    uint32_t saved_next_value_id = edge_function->next_value_id;

    XiPhi *phi = xi_phi_new(edge_function, target, edge_function->params[1]->type, 2u);
    if (!phi) {
        xr_program_artifact_free(&baseline);
        return false;
    }
    phi->value.args[0] = edge_function->params[1];
    phi->value.args[1] = edge_function->params[2];
    XiBlock *duplicate_predecessors[2] = {branch, branch};
    XiValue bogus = {0};
    bogus.id = UINT32_MAX;
    bogus.op = XI_REGEX_COMPILE;
    bogus.type = edge_function->params[1]->type;
    bogus.block = dead;
    XiValue *bogus_values[1] = {&bogus};

    branch->succs[0] = target;
    branch->succs[1] = target;
    target->preds = duplicate_predecessors;
    target->npreds = 2u;
    target->preds_cap = 2u;
    target->control = &phi->value;
    dead->values = bogus_values;
    dead->nvalues = 1u;
    dead->values_cap = 1u;

    char diagnostic[512] = {0};
    XrProgramArtifact candidate = {0};
    XrProgramBuildStatus status =
        xr_program_write_from_xi(&scoped_input, &candidate, diagnostic, sizeof(diagnostic));

    dead->values = saved_dead_values;
    dead->nvalues = saved_dead_value_count;
    dead->values_cap = saved_dead_value_capacity;
    target->control = saved_target_control;
    target->phis = saved_target_phis;
    target->preds = saved_target_preds;
    target->npreds = saved_target_pred_count;
    target->preds_cap = saved_target_pred_capacity;
    branch->succs[0] = saved_first_successor;
    branch->succs[1] = saved_second_successor;
    edge_function->next_value_id = saved_next_value_id;

    XrValidatedProgram *validated = NULL;
    XrProgramDiagnostic verify_diagnostic;
    bool valid = status == XR_PROGRAM_BUILD_OK && candidate.bytes && candidate.size != 0u &&
                 xr_program_validate(candidate.bytes, candidate.size, NULL, &validated,
                                     &verify_diagnostic) == XR_PROGRAM_VERIFY_OK &&
                 validated != NULL;
    if (valid) {
        XrReferenceOutcome reference = xr_reference_evaluate(
            validated, xr_validated_program_entry_function(validated), NULL, 0u, NULL, NULL);
        valid = reference.kind == XR_REFERENCE_OUTCOME_RETURN &&
                reference.value.kind == XR_REFERENCE_VALUE_I64 &&
                reference.value.as.i64 == expected_result;
    }
    xr_validated_program_free(validated);
    xr_program_artifact_free(&candidate);
    xr_program_artifact_free(&baseline);
    return valid;
}

typedef struct XiCanonicalProgramTestFixture {
    XrCompilerSession *original_session;
    XrCompilerSession *session;
    XrTargetProfile *profile;
    XiPipelineScalarFixture source;
    XgGlobalEvidence evidence;
    XiPipelineResult pipeline;
} XiCanonicalProgramTestFixture;

static void xi_canonical_program_test_fixture_cleanup(XiCanonicalProgramTestFixture *fixture) {
    if (!fixture)
        return;
    xi_pipeline_result_free(&fixture->pipeline);
    xg_global_evidence_free(&fixture->evidence);
    xi_pipeline_scalar_fixture_cleanup(&fixture->source);
    xr_target_profile_free(fixture->profile);
    if (fixture->session) {
        xr_compiler_session_attach_isolate(g_iso, fixture->original_session);
        xr_compiler_session_delete(fixture->session);
    }
    memset(fixture, 0, sizeof(*fixture));
}

static bool xi_canonical_program_test_fixture_build(XiCanonicalProgramTestFixture *fixture,
                                                    const char *namespace_id,
                                                    const char *source) {
    if (!fixture || !namespace_id || !source || !g_iso)
        return false;
    memset(fixture, 0, sizeof(*fixture));
    fixture->original_session = xr_compiler_session_current_for_isolate(g_iso);
    XrCompilerSessionConfig session_config = {0};
    fixture->session = xr_compiler_session_new(&session_config);
    if (!fixture->session)
        goto fail;
    if (xr_compiler_session_attach_isolate(g_iso, fixture->session) !=
        fixture->original_session)
        goto fail;
    char diagnostic[512] = {0};
    if (!xr_runtime_target_profile_build_native_hosted(&fixture->profile, diagnostic,
                                                        sizeof(diagnostic)) ||
        !xr_compiler_session_set_target_profile(fixture->session, fixture->profile) ||
        !xi_pipeline_fixture_analyze_source(&fixture->source, fixture->session, namespace_id,
                                            source) ||
        !xi_pipeline_fixture_build_global_evidence(&fixture->source, fixture->session,
                                                   &fixture->evidence))
        goto fail;

    XiPipelineConfig config = xi_pipeline_program_input_config();
    config.run_canonicalize = false;
    config.source_file = "scalar-binding.xr";
    config.module_name = namespace_id;
    config.module_identity = fixture->source.spec->canonical;
    config.global_evidence = &fixture->evidence;
    config.global_evidence_module_id = 1u;
    fixture->pipeline = xi_pipeline_compile_program(fixture->source.spec->ast,
                                                    fixture->source.analyzer, g_iso, &config);
    if (fixture->pipeline.status != XI_PIPE_OK || !fixture->pipeline.ir ||
        !fixture->pipeline.ir->module) {
        fprintf(stderr, "canonical Program Xi failed at %s: %s\n",
                xi_pipeline_stage_str(fixture->pipeline.error.stage),
                fixture->pipeline.error.detail);
        goto fail;
    }
    return true;

fail:
    xi_canonical_program_test_fixture_cleanup(fixture);
    return false;
}

static const XgBodySummary *xi_pipeline_find_xg_body(const XgGlobalEvidence *evidence,
                                                     XgFuncId func_id) {
    const XgBodySummary *found = NULL;
    for (uint32_t index = 0u; evidence && index < evidence->nbodies; ++index) {
        const XgBodySummary *candidate = &evidence->bodies[index];
        if (candidate->func_id != func_id)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static const XgInterfaceMethodSummary *xi_pipeline_find_xg_interface_method(
    const XgGlobalEvidence *evidence, XgInterfaceId interface_id,
    XgInterfaceMethodId method_id) {
    const XgInterfaceMethodSummary *found = NULL;
    for (uint32_t index = 0u; evidence && index < evidence->ninterface_methods; ++index) {
        const XgInterfaceMethodSummary *candidate = &evidence->interface_methods[index];
        if (candidate->owner_interface_id != interface_id ||
            candidate->interface_method_id != method_id)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static uint32_t xi_pipeline_count_xg_direct_targets(const XiFunc *function,
                                                    const XgGlobalEvidence *evidence,
                                                    XgFuncId target_func_id) {
    uint32_t count = 0u;
    for (uint32_t block_index = 0u; function && block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *value = block->values[value_index];
            const XgCallsiteSummary *callsite =
                value && value->xg_callsite_id != XG_NO_ID
                    ? xg_global_evidence_find_callsite(
                          evidence, (XgCallsiteId) value->xg_callsite_id)
                    : NULL;
            count += callsite && callsite->kind == XG_CALL_DIRECT_FUNC &&
                     callsite->static_target_func_id == target_func_id;
        }
    }
    return count;
}

static const XrValidatedInstruction *xi_pipeline_find_unique_validated_operation(
    const XrValidatedProgram *program, uint16_t operation_id,
    const XrValidatedFunction **owner_out) {
    const XrValidatedInstruction *found = NULL;
    const XrValidatedFunction *owner = NULL;
    for (uint32_t function_index = 0u; program && function_index < program->function_count;
         ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0u;
                 instruction_index < block->instruction_count; ++instruction_index) {
                const XrValidatedInstruction *instruction =
                    &block->instructions[instruction_index];
                if (instruction->operation_id != operation_id)
                    continue;
                if (found)
                    return NULL;
                found = instruction;
                owner = function;
            }
        }
    }
    if (owner_out)
        *owner_out = owner;
    return found;
}

static bool xi_pipeline_validated_function_drops_value(const XrValidatedFunction *function,
                                                       uint32_t value_id) {
    for (uint32_t block_index = 0u; function && block_index < function->block_count;
         ++block_index) {
        const XrValidatedBlock *block = &function->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
            if (instruction->operation_id == XR_CORE_OP_CORE_OWNER_DROP &&
                instruction->operand_count == 1u && instruction->operands[0] == value_id)
                return true;
        }
    }
    return false;
}

typedef struct XiMoveModuleFixture {
    char directory[XR_TEST_PATH_MAX];
    char library_path[XR_TEST_PATH_MAX];
    char consumer_path[XR_TEST_PATH_MAX];
    XrModuleResolver *resolver;
    XrModuleGraph *graph;
    XaAnalyzer *analyzer;
    XiPipelineResult pipelines[2];
    XiModule *modules[2];
    uint32_t library_index;
    uint32_t consumer_index;
    XgGlobalEvidence evidence;
} XiMoveModuleFixture;

static bool xi_pipeline_write_source_file(const char *path, const char *source) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    size_t size = strlen(source);
    bool written = fwrite(source, 1u, size, file) == size;
    return fclose(file) == 0 && written;
}

static void xi_move_module_fixture_cleanup(XiMoveModuleFixture *fixture) {
    if (!fixture)
        return;
    for (uint32_t index = 0u; index < 2u; ++index)
        xi_pipeline_result_free(&fixture->pipelines[index]);
    xg_global_evidence_free(&fixture->evidence);
    xa_analyzer_free(fixture->analyzer);
    xr_module_graph_free(fixture->graph);
    xr_module_resolver_free(fixture->resolver);
    if (fixture->library_path[0])
        xr_test_unlink(fixture->library_path);
    if (fixture->consumer_path[0])
        xr_test_unlink(fixture->consumer_path);
    if (fixture->directory[0])
        xr_test_rmdir(fixture->directory);
    memset(fixture, 0, sizeof(*fixture));
}

static bool xi_move_module_fixture_analyze(XiMoveModuleFixture *fixture) {
    for (int topo = 0; fixture && fixture->graph && topo < fixture->graph->topo_count; ++topo) {
        XrModuleSpec *spec = &fixture->graph->specs[fixture->graph->topo_order[topo]];
        xa_analyzer_analyze(fixture->analyzer, spec->source_path, spec->ast);
        int diagnostic_count = 0;
        for (XaDiagnostic *diagnostic =
                 xa_analyzer_get_diagnostics(fixture->analyzer, &diagnostic_count);
             diagnostic; diagnostic = diagnostic->next) {
            if (diagnostic->severity == XR_DIAG_SEV_ERROR) {
                fprintf(stderr, "MOVE module analysis failed: %s\n", diagnostic->message);
                return false;
            }
        }
        if (spec->export_symbols)
            xr_hashmap_free(spec->export_symbols);
        spec->export_symbols = NULL;
        if (!xa_analyzer_collect_export_symbols_checked(fixture->analyzer, spec->ast,
                                                        &spec->export_symbols))
            return false;
        spec->status = XR_MODSPEC_ANALYZED;
        xa_analyzer_clear_diagnostics(fixture->analyzer);
    }
    return true;
}

static bool xi_move_module_fixture_build(XiMoveModuleFixture *fixture,
                                         XrCompilerSession *session) {
    static unsigned int serial;
    static const char library_source[] =
        "export fn imported_move_target(value: i64) -> i64 {\n"
        "  return value + 1\n"
        "}\n";
    static const char consumer_source[] =
        "import { imported_move_target } from \"./library\"\n"
        "fn root() -> i64 {\n"
        "  var seed = 41\n"
        "  return later_forward(seed)\n"
        "}\n"
        "fn later_forward(value: i64) -> i64 {\n"
        "  return imported_move_target(value)\n"
        "}\n";
    if (!fixture || !session)
        return false;
    memset(fixture, 0, sizeof(*fixture));
    PIPELINE_SNPRINTF(fixture->directory, sizeof(fixture->directory),
                      "xi_program_move_modules_%u_XXXXXX", serial++);
    if (!xr_test_mkdtemp(fixture->directory))
        goto fail;
    char absolute_directory[XR_TEST_PATH_MAX] = {0};
    if (!xr_test_realpath_buf(fixture->directory, absolute_directory,
                              sizeof(absolute_directory)))
        goto fail;
    memcpy(fixture->directory, absolute_directory, strlen(absolute_directory) + 1u);
    int library_length = PIPELINE_SNPRINTF(fixture->library_path,
                                           sizeof(fixture->library_path), "%s/library.xr",
                                           fixture->directory);
    int consumer_length = PIPELINE_SNPRINTF(fixture->consumer_path,
                                            sizeof(fixture->consumer_path), "%s/consumer.xr",
                                            fixture->directory);
    if (library_length < 0 || (size_t) library_length >= sizeof(fixture->library_path) ||
        consumer_length < 0 || (size_t) consumer_length >= sizeof(fixture->consumer_path) ||
        !xi_pipeline_write_source_file(fixture->library_path, library_source) ||
        !xi_pipeline_write_source_file(fixture->consumer_path, consumer_source))
        goto fail;

    XrModuleResolverConfig resolver_config = {0};
    fixture->resolver = xr_module_resolver_new(&resolver_config);
    fixture->graph = xr_module_graph_new(session, fixture->resolver);
    if (!fixture->resolver || !fixture->graph)
        goto fail;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_SCRIPT,
        .physical_root = fixture->directory,
    };
    char *graph_error = NULL;
    if (xr_module_graph_build(fixture->graph, fixture->consumer_path, &authority, &graph_error) !=
        0) {
        fprintf(stderr, "MOVE module graph failed: %s\n",
                graph_error ? graph_error : "unknown graph error");
        xr_free(graph_error);
        goto fail;
    }
    xr_free(graph_error);
    if (xr_module_graph_topological_sort(fixture->graph) != 0 || fixture->graph->has_cycle ||
        fixture->graph->spec_count != 2 || fixture->graph->topo_count != 2 ||
        fixture->graph->entry_index < 0)
        goto fail;

    fixture->analyzer = xa_analyzer_new(session);
    if (!fixture->analyzer)
        goto fail;
    xa_analyzer_set_build_profile(fixture->analyzer, XA_ANALYZER_BUILD_PROFILE_HOSTED);
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    if (!xi_move_module_fixture_analyze(fixture))
        goto fail;
    AstNode *roots[2] = {0};
    for (int topo = 0; topo < fixture->graph->topo_count; ++topo)
        roots[topo] = fixture->graph->specs[fixture->graph->topo_order[topo]].ast;
    for (int topo = 0; topo < fixture->graph->topo_count; ++topo)
        if (!xa_mono_pass(roots[topo], roots, fixture->graph->topo_count, g_iso,
                          fixture->analyzer))
            goto fail;
    for (int topo = 0; topo < fixture->graph->topo_count; ++topo) {
        XrModuleSpec *spec = &fixture->graph->specs[fixture->graph->topo_order[topo]];
        XrCompilerSessionScope scope;
        bool has_scope = spec->ast->type == AST_PROGRAM && spec->ast->as.program.arena &&
                         xr_compiler_session_push_arena(session, spec->ast->as.program.arena,
                                                        spec->source_path, &scope);
        XrCanonStatus canon = xr_canon_program(spec->ast, fixture->analyzer, session);
        if (has_scope)
            xr_compiler_session_pop_arena(&scope);
        if (canon != XR_CANON_OK)
            goto fail;
    }
    if (!xi_move_module_fixture_analyze(fixture) ||
        !xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
            &fixture->evidence, fixture->graph, XG_BUILD_NATIVE_RELEASE, 0u, NULL, 0u,
            fixture->analyzer))
        goto fail;

    XiPipelineConfig config = xi_pipeline_program_input_config();
    config.run_canonicalize = false;
    config.module_graph = fixture->graph;
    config.graph_modules = fixture->modules;
    config.graph_module_count = 2;
    config.global_evidence = &fixture->evidence;
    for (uint32_t topo = 0u; topo < 2u; ++topo) {
        int spec_index = fixture->graph->topo_order[topo];
        XrModuleSpec *spec = &fixture->graph->specs[spec_index];
        config.source_file = spec->source_path;
        config.module_identity = spec->canonical;
        config.module_name = spec_index == fixture->graph->entry_index
                                 ? "move_consumer"
                                 : "move_library";
        config.global_evidence_module_id = topo + 1u;
        fixture->pipelines[topo] =
            xi_pipeline_compile_program(spec->ast, fixture->analyzer, g_iso, &config);
        if (fixture->pipelines[topo].status != XI_PIPE_OK || !fixture->pipelines[topo].ir ||
            !fixture->pipelines[topo].ir->module) {
            fprintf(stderr, "MOVE module Xi failed at %s: %s\n",
                    xi_pipeline_stage_str(fixture->pipelines[topo].error.stage),
                    fixture->pipelines[topo].error.detail);
            goto fail;
        }
        fixture->modules[topo] = fixture->pipelines[topo].ir->module;
        if (spec_index == fixture->graph->entry_index)
            fixture->consumer_index = topo;
    }
    fixture->library_index = fixture->consumer_index == 0u ? 1u : 0u;
    for (uint32_t topo = 0u; topo < 2u; ++topo) {
        int spec_index = fixture->graph->topo_order[topo];
        xi_resolve_imports(fixture->pipelines[topo].ir, fixture->graph,
                           fixture->graph->specs[spec_index].source_path, fixture->modules, 2);
    }
    return true;

fail:
    xi_move_module_fixture_cleanup(fixture);
    return false;
}

TEST(e2e_program_move_direct_signatures_are_published_before_bodies) {
    XrCompilerSession *original_session = xr_compiler_session_current_for_isolate(g_iso);
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    PIPELINE_TEST_REQUIRE(session != NULL);
    PIPELINE_TEST_REQUIRE(xr_compiler_session_attach_isolate(g_iso, session) == original_session);
    char diagnostic[512] = {0};
    XrTargetProfile *profile = NULL;
    PIPELINE_TEST_REQUIRE(
        xr_runtime_target_profile_build_native_hosted(&profile, diagnostic, sizeof(diagnostic)));
    PIPELINE_TEST_REQUIRE(xr_compiler_session_set_target_profile(session, profile));

    XiMoveModuleFixture fixture = {0};
    PIPELINE_TEST_REQUIRE(xi_move_module_fixture_build(&fixture, session));
    XiModule *consumer = fixture.modules[fixture.consumer_index];
    PIPELINE_TEST_REQUIRE(consumer != NULL);

    XiFunc *cross_module_caller = NULL;
    XiFunc *cross_module_target = NULL;
    XiValue *cross_module_call = NULL;
    for (uint16_t function_index = 0u; function_index < consumer->nfuncs; ++function_index) {
        XiFunc *function = consumer->functions[function_index];
        const XgBodySummary *owner_body =
            function ? xi_pipeline_find_xg_body(&fixture.evidence, function->xg_body_func_id) : NULL;
        for (uint32_t block_index = 0u; function && owner_body &&
                                         block_index < function->nblocks;
             ++block_index) {
            XiBlock *block = function->blocks[block_index];
            for (uint32_t value_index = 0u; block && value_index < block->nvalues;
                 ++value_index) {
                XiValue *value = block->values[value_index];
                const XgCallsiteSummary *callsite =
                    value && value->xg_callsite_id != XG_NO_ID
                        ? xg_global_evidence_find_callsite(
                              &fixture.evidence, (XgCallsiteId) value->xg_callsite_id)
                        : NULL;
                const XgBodySummary *target_body =
                    callsite && callsite->kind == XG_CALL_DIRECT_FUNC
                        ? xi_pipeline_find_xg_body(&fixture.evidence,
                                                   callsite->static_target_func_id)
                        : NULL;
                if (!value || value->op != XI_CALL || !target_body ||
                    target_body->module_id == owner_body->module_id)
                    continue;
                PIPELINE_TEST_REQUIRE(cross_module_caller == NULL);
                cross_module_caller = function;
                cross_module_call = value;
                cross_module_target = xi_pipeline_find_module_function_by_xg_id(
                    fixture.pipelines[fixture.library_index].ir,
                    callsite->static_target_func_id);
            }
        }
    }
    PIPELINE_TEST_REQUIRE(cross_module_caller != NULL);
    PIPELINE_TEST_REQUIRE(cross_module_target != NULL);
    PIPELINE_TEST_REQUIRE(cross_module_call != NULL && cross_module_call->nargs == 2u);
    PIPELINE_TEST_REQUIRE(cross_module_caller->nparams == 1u);
    PIPELINE_TEST_REQUIRE(cross_module_target->nparams == 1u);
    /* The regression is about signature publication order, independent of
     * source ownership diagnostics.  Scalar MOVE remains a valid canonical
     * call mode with non-owner payloads, so make the verified Xi contract
     * deliberately order-sensitive here. */
    cross_module_caller->params[0]->param_mode = XR_PARAM_MOVE;
    cross_module_target->params[0]->param_mode = XR_PARAM_MOVE;
    PIPELINE_TEST_REQUIRE(cross_module_caller->params[0]->param_mode == XR_PARAM_MOVE);
    PIPELINE_TEST_REQUIRE(cross_module_target->params[0]->param_mode == XR_PARAM_MOVE);

    XiFunc *entry = NULL;
    uint16_t entry_index = UINT16_MAX;
    uint16_t forward_target_index = UINT16_MAX;
    for (uint16_t function_index = 0u; function_index < consumer->nfuncs; ++function_index) {
        XiFunc *function = consumer->functions[function_index];
        if (function == cross_module_caller)
            forward_target_index = function_index;
        if (!function || function->has_receiver || function->nparams != 0u ||
            xi_pipeline_count_xg_direct_targets(function, &fixture.evidence,
                                                cross_module_caller->xg_body_func_id) != 1u)
            continue;
        PIPELINE_TEST_REQUIRE(entry == NULL);
        entry = function;
        entry_index = function_index;
    }
    PIPELINE_TEST_REQUIRE(entry != NULL);
    PIPELINE_TEST_REQUIRE(entry_index < forward_target_index);

    XrCoreIrKey semantic_profile = xr_core_ir_key(
        "move-direct-signature-profile", strlen("move-direct-signature-profile"));
    const XiFunc *module_roots[2] = {fixture.pipelines[0].ir, fixture.pipelines[1].ir};
    XrProgramFromXiInput input = {
        .module_roots = module_roots,
        .module_count = 2u,
        .entry_function = entry,
        .global_evidence = &fixture.evidence,
        .semantic_profile_fingerprint = semantic_profile.bytes,
    };
    XrProgramArtifact first = {0};
    XrProgramBuildStatus first_status =
        xr_program_write_from_xi(&input, &first, diagnostic, sizeof(diagnostic));
    if (first_status != XR_PROGRAM_BUILD_OK) {
        fprintf(stderr, "MOVE direct Program build failed: %s\n", diagnostic);
        fprintf(stderr, "target xg=%u consumer=%u library=%u\n",
                (unsigned) cross_module_target->xg_body_func_id,
                (unsigned) fixture.consumer_index, (unsigned) fixture.library_index);
        for (uint32_t callsite_index = 0u; callsite_index < fixture.evidence.ncallsites;
             ++callsite_index) {
            const XgCallsiteSummary *callsite = &fixture.evidence.callsites[callsite_index];
            fprintf(stderr, "callsite %u kind=%u static=%u sig=%llu targets=%u\n",
                    (unsigned) callsite->callsite_id, (unsigned) callsite->kind,
                    (unsigned) callsite->static_target_func_id,
                    (unsigned long long) callsite->callable_signature_key,
                    (unsigned) callsite->callable_target_count);
        }
    }
    PIPELINE_TEST_REQUIRE(first_status == XR_PROGRAM_BUILD_OK);
    XrProgramArtifact repeated = {0};
    PIPELINE_TEST_REQUIRE(xr_program_write_from_xi(&input, &repeated, diagnostic,
                                                   sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(first.size == repeated.size);
    PIPELINE_TEST_REQUIRE(xr_program_id_equal(first.id, repeated.id));
    PIPELINE_TEST_REQUIRE(memcmp(first.bytes, repeated.bytes, first.size) == 0);
    XrValidatedProgram *validated = NULL;
    XrProgramDiagnostic verify_diagnostic;
    PIPELINE_TEST_REQUIRE(xr_program_validate(first.bytes, first.size, NULL,
                                              &validated, &verify_diagnostic) ==
                          XR_PROGRAM_VERIFY_OK);
    PIPELINE_TEST_REQUIRE(validated != NULL);

    xr_validated_program_free(validated);
    xr_program_artifact_free(&repeated);
    xr_program_artifact_free(&first);
    xi_move_module_fixture_cleanup(&fixture);
    xr_target_profile_free(profile);
    PIPELINE_TEST_REQUIRE(xr_compiler_session_attach_isolate(g_iso, original_session) == session);
    xr_compiler_session_delete(session);
}

TEST(e2e_program_typed_error_cleanup_trampoline_reuses_error_live_in) {
    static const char source[] =
        "enum CleanupFailure { Negative { code: i64 } }\n"
        "fn maybe_cleanup_failure(value: i64) -> i64 {\n"
        "  if (value < 0) { throw CleanupFailure.Negative { code: value } }\n"
        "  return value\n"
        "}\n"
        "fn root() -> i64 {\n"
        "  var action = fn() -> i64 { return 1 }\n"
        "  try {\n"
        "    var value = maybe_cleanup_failure(-2)\n"
        "    return value + action()\n"
        "  }\n"
        "  catch (error) { return 42 }\n"
        "}\n";
    XiCanonicalProgramTestFixture fixture = {0};
    PIPELINE_TEST_REQUIRE(xi_canonical_program_test_fixture_build(
        &fixture, "xi-program-cleanup-trampoline", source));

    XiFunc *entry = NULL;
    XiValue *fallible_call = NULL;
    for (uint16_t function_index = 0u;
         function_index < fixture.pipeline.ir->module->nfuncs; ++function_index) {
        XiFunc *function = fixture.pipeline.ir->module->functions[function_index];
        for (uint32_t block_index = 0u; function && block_index < function->nblocks;
             ++block_index) {
            XiBlock *block = function->blocks[block_index];
            for (uint32_t value_index = 0u; block && value_index < block->nvalues;
                 ++value_index) {
                XiValue *value = block->values[value_index];
                const XgCallsiteSummary *callsite =
                    value && value->xg_callsite_id != XG_NO_ID
                        ? xg_global_evidence_find_callsite(
                              &fixture.evidence, (XgCallsiteId) value->xg_callsite_id)
                        : NULL;
                if (!callsite || callsite->kind != XG_CALL_DIRECT_FUNC ||
                    (callsite->flags &
                     (XG_CALL_ERROR_EFFECT_VERIFIED | XG_CALL_MAY_ERROR)) !=
                        (XG_CALL_ERROR_EFFECT_VERIFIED | XG_CALL_MAY_ERROR))
                    continue;
                PIPELINE_TEST_REQUIRE(fallible_call == NULL);
                fallible_call = value;
                entry = function;
            }
        }
    }
    PIPELINE_TEST_REQUIRE(entry != NULL);
    PIPELINE_TEST_REQUIRE(!entry->has_receiver && entry->nparams == 0u);
    PIPELINE_TEST_REQUIRE(fallible_call != NULL && fallible_call->nargs >= 1u);
    PIPELINE_TEST_REQUIRE(fallible_call->block != NULL);
    PIPELINE_TEST_REQUIRE(fallible_call->block->kind == XI_BLOCK_IF);
    PIPELINE_TEST_REQUIRE(fallible_call->block->control != NULL);
    PIPELINE_TEST_REQUIRE(fallible_call->block->control->op == XI_ERR_CHECK);
    PIPELINE_TEST_REQUIRE(fallible_call->block->succs[0] != NULL);

    XiValue *caught = NULL;
    bool found_cleanup = false;
    uint32_t cleanup_block_count = 0u;
    XiBlock *route = fallible_call->block->succs[0];
    for (uint32_t depth = 0u; route && depth <= entry->nblocks; ++depth) {
        for (uint32_t value_index = 0u; value_index < route->nvalues; ++value_index) {
            XiValue *value = route->values[value_index];
            if (value && value->op == XI_ERR_CATCH) {
                PIPELINE_TEST_REQUIRE(caught == NULL);
                caught = value;
            }
            if (value && value->op == XI_RELEASE && value->nargs == 1u && value->args) {
                PIPELINE_TEST_REQUIRE((value->flags & XI_FLAG_MAY_THROW) == 0u);
                found_cleanup = true;
            }
        }
        if (caught)
            break;
        ++cleanup_block_count;
        PIPELINE_TEST_REQUIRE(route->kind == XI_BLOCK_PLAIN);
        PIPELINE_TEST_REQUIRE(route->control == NULL);
        PIPELINE_TEST_REQUIRE(route->succs[0] != NULL && route->succs[1] == NULL);
        route = route->succs[0];
    }
    PIPELINE_TEST_REQUIRE(found_cleanup);
    PIPELINE_TEST_REQUIRE(cleanup_block_count != 0u);
    PIPELINE_TEST_REQUIRE(caught != NULL);
    PIPELINE_TEST_REQUIRE(caught->error_region == fallible_call->block->control->error_region);

    XrCoreIrKey semantic_profile = xr_core_ir_key(
        "typed-error-cleanup-profile", strlen("typed-error-cleanup-profile"));
    const XiFunc *module_roots[] = {fixture.pipeline.ir};
    XrProgramFromXiInput input = {
        .module_roots = module_roots,
        .module_count = 1u,
        .entry_function = entry,
        .global_evidence = &fixture.evidence,
        .semantic_profile_fingerprint = semantic_profile.bytes,
    };
    char diagnostic[512] = {0};
    XrProgramArtifact artifact = {0};
    XrProgramBuildStatus build_status =
        xr_program_write_from_xi(&input, &artifact, diagnostic, sizeof(diagnostic));
    if (build_status != XR_PROGRAM_BUILD_OK)
        fprintf(stderr, "cleanup trampoline Program build failed: %s\n", diagnostic);
    PIPELINE_TEST_REQUIRE(build_status == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *validated = NULL;
    XrProgramDiagnostic verify_diagnostic;
    PIPELINE_TEST_REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated,
                                              &verify_diagnostic) ==
                          XR_PROGRAM_VERIFY_OK);
    PIPELINE_TEST_REQUIRE(validated != NULL);
    PIPELINE_TEST_REQUIRE(validated_program_has_operation(
        validated, XR_CORE_OP_CORE_CALL_SEALED_INVOKE));

    xr_validated_program_free(validated);
    xr_program_artifact_free(&artifact);
    xi_canonical_program_test_fixture_cleanup(&fixture);
}

TEST(e2e_program_witness_move_operands_are_consumed_once) {
    static const char source[] =
        "interface MoveDirect { move consume() -> i64 }\n"
        "class DirectConsumer implements MoveDirect {\n"
        "  move consume() -> i64 { return 1 }\n"
        "}\n"
        "enum DispatchFailure { Failed }\n"
        "interface MoveInvoke { move consume() -> i64 }\n"
        "class InvokeConsumer implements MoveInvoke {\n"
        "  move consume() -> i64 {\n"
        "    throw DispatchFailure.Failed\n"
        "  }\n"
        "}\n"
        "fn direct_dispatch(receiver: move MoveDirect) -> i64 {\n"
        "  return (move receiver).consume()\n"
        "}\n"
        "fn invoke_dispatch(receiver: move MoveInvoke) -> i64 {\n"
        "  try { return (move receiver).consume() }\n"
        "  catch (error) { return 2 }\n"
        "}\n"
        "fn root() -> i64 {\n"
        "  return 0\n"
        "}\n";
    XiCanonicalProgramTestFixture fixture = {0};
    PIPELINE_TEST_REQUIRE(xi_canonical_program_test_fixture_build(
        &fixture, "xi-program-witness-move", source));

    XiFunc *direct_owner = NULL;
    XiFunc *invoke_owner = NULL;
    XiValue *direct_call = NULL;
    XiValue *invoke_call = NULL;
    for (uint16_t function_index = 0u;
         function_index < fixture.pipeline.ir->module->nfuncs; ++function_index) {
        XiFunc *function = fixture.pipeline.ir->module->functions[function_index];
        for (uint32_t block_index = 0u; function && block_index < function->nblocks;
             ++block_index) {
            XiBlock *block = function->blocks[block_index];
            for (uint32_t value_index = 0u; block && value_index < block->nvalues;
                 ++value_index) {
                XiValue *value = block->values[value_index];
                if (!value || (value->xg_existential_kind != XI_EXISTENTIAL_WITNESS_DIRECT &&
                               value->xg_existential_kind != XI_EXISTENTIAL_WITNESS_INVOKE))
                    continue;
                const XgCallsiteSummary *callsite =
                    xg_global_evidence_find_callsite(
                        &fixture.evidence, (XgCallsiteId) value->xg_callsite_id);
                const XgInterfaceMethodSummary *method =
                    callsite && callsite->kind == XG_CALL_INTERFACE
                        ? xi_pipeline_find_xg_interface_method(
                              &fixture.evidence, value->xg_interface_id,
                              (XgInterfaceMethodId) value->xg_method_id)
                        : NULL;
                PIPELINE_TEST_REQUIRE(callsite != NULL);
                PIPELINE_TEST_REQUIRE(method != NULL && method->contract_complete);
                PIPELINE_TEST_REQUIRE(method->receiver_mode == XR_PARAM_MOVE);
                PIPELINE_TEST_REQUIRE(method->parameter_count == 0u);
                PIPELINE_TEST_REQUIRE(value->nargs == 1u && value->args != NULL);
                if (value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_DIRECT) {
                    PIPELINE_TEST_REQUIRE(direct_call == NULL);
                    direct_call = value;
                    direct_owner = function;
                } else {
                    PIPELINE_TEST_REQUIRE(invoke_call == NULL);
                    invoke_call = value;
                    invoke_owner = function;
                }
            }
        }
    }
    PIPELINE_TEST_REQUIRE(direct_call != NULL && direct_owner != NULL);
    PIPELINE_TEST_REQUIRE(invoke_call != NULL && invoke_owner != NULL);
    PIPELINE_TEST_REQUIRE(direct_call->xg_interface_use_kind == XI_INTERFACE_USE_MOVE);
    PIPELINE_TEST_REQUIRE(invoke_call->xg_interface_use_kind == XI_INTERFACE_USE_MOVE);

    XiFunc *entry = NULL;
    for (uint16_t function_index = 0u;
         function_index < fixture.pipeline.ir->module->nfuncs; ++function_index) {
        XiFunc *function = fixture.pipeline.ir->module->functions[function_index];
        if (!function || function->has_receiver || function->nparams != 0u)
            continue;
        PIPELINE_TEST_REQUIRE(entry == NULL);
        entry = function;
    }
    PIPELINE_TEST_REQUIRE(entry != NULL);

    XrCoreIrKey semantic_profile =
        xr_core_ir_key("witness-move-profile", strlen("witness-move-profile"));
    const XiFunc *module_roots[] = {fixture.pipeline.ir};
    XrProgramFromXiInput input = {
        .module_roots = module_roots,
        .module_count = 1u,
        .entry_function = entry,
        .global_evidence = &fixture.evidence,
        .semantic_profile_fingerprint = semantic_profile.bytes,
    };
    char diagnostic[512] = {0};
    XrProgramArtifact artifact = {0};
    XrProgramBuildStatus build_status =
        xr_program_write_from_xi(&input, &artifact, diagnostic, sizeof(diagnostic));
    if (build_status != XR_PROGRAM_BUILD_OK) {
        fprintf(stderr, "witness MOVE Program build failed: %s\n", diagnostic);
        xi_func_dump(entry, stderr);
        xi_func_dump(direct_owner, stderr);
        xi_func_dump(invoke_owner, stderr);
    }
    PIPELINE_TEST_REQUIRE(build_status == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *validated = NULL;
    XrProgramDiagnostic verify_diagnostic;
    PIPELINE_TEST_REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated,
                                              &verify_diagnostic) ==
                          XR_PROGRAM_VERIFY_OK);
    PIPELINE_TEST_REQUIRE(validated != NULL);

    const XrValidatedFunction *direct_validated_owner = NULL;
    const XrValidatedInstruction *direct_witness = xi_pipeline_find_unique_validated_operation(
        validated, XR_CORE_OP_CORE_CALL_WITNESS_DIRECT, &direct_validated_owner);
    const XrValidatedFunction *invoke_validated_owner = NULL;
    const XrValidatedInstruction *invoke_witness = xi_pipeline_find_unique_validated_operation(
        validated, XR_CORE_OP_CORE_CALL_WITNESS_INVOKE, &invoke_validated_owner);
    PIPELINE_TEST_REQUIRE(direct_witness != NULL && direct_validated_owner != NULL);
    PIPELINE_TEST_REQUIRE(invoke_witness != NULL && invoke_validated_owner != NULL);
    PIPELINE_TEST_REQUIRE(direct_witness->operand_count == 1u);
    PIPELINE_TEST_REQUIRE(invoke_witness->operand_count == 1u);
    for (uint32_t operand = 0u; operand < 1u; ++operand) {
        uint32_t direct_value = direct_witness->operands[operand];
        uint32_t invoke_value = invoke_witness->operands[operand];
        PIPELINE_TEST_REQUIRE(direct_value < direct_validated_owner->value_count);
        PIPELINE_TEST_REQUIRE(invoke_value < invoke_validated_owner->value_count);
        PIPELINE_TEST_REQUIRE(direct_validated_owner->value_ownerships[direct_value] ==
                              XR_CORE_IR_OWNER);
        PIPELINE_TEST_REQUIRE(invoke_validated_owner->value_ownerships[invoke_value] ==
                              XR_CORE_IR_OWNER);
        PIPELINE_TEST_REQUIRE(
            !xi_pipeline_validated_function_drops_value(direct_validated_owner, direct_value));
        PIPELINE_TEST_REQUIRE(
            !xi_pipeline_validated_function_drops_value(invoke_validated_owner, invoke_value));
    }

    xr_validated_program_free(validated);
    xr_program_artifact_free(&artifact);
    xi_canonical_program_test_fixture_cleanup(&fixture);
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
    static const char program_source[] = "interface ReadValue { read() -> i64 }\n"
                                         "class ReadClass implements ReadValue {\n"
                                         "  read() -> i64 { return 1 }\n"
                                         "}\n"
                                         "struct ReadStruct implements ReadValue {\n"
                                         "  read() -> i64 { return 2 }\n"
                                         "}\n"
                                         "enum ReadEnum implements ReadValue {\n"
                                         "  One, Two\n"
                                         "  read() -> i64 { return 3 }\n"
                                         "}\n"
                                         "fn read_class(value: ReadClass) -> i64 {\n"
                                         "  var erased: ReadValue = value\n"
                                         "  return erased.read()\n"
                                         "}\n"
                                         "fn read_struct(value: ReadStruct) -> i64 {\n"
                                         "  var erased: ReadValue = value\n"
                                         "  return erased.read()\n"
                                         "}\n"
                                         "fn read_enum(value: ReadEnum) -> i64 {\n"
                                         "  var erased: ReadValue = value\n"
                                         "  return erased.read()\n"
                                         "}\n"
                                         "fn witness_only(value: ReadValue) -> i64 {\n"
                                         "  return value.read()\n"
                                         "}\n"
                                         "fn is_read_class(value: ReadValue) -> bool {\n"
                                         "  return value is ReadClass\n"
                                         "}\n"
                                         "fn project_read_class(value: ReadValue) -> ReadClass? {\n"
                                         "  return value as ReadClass?\n"
                                         "}\n"
                                         "fn existential_value() -> i64 {\n"
                                         "  return read_class(ReadClass()) + "
                                         "read_struct(ReadStruct{}) + read_enum(ReadEnum.One)\n"
                                         "}\n"
                                         "enum ReadFailure { Failed }\n"
                                         "interface FallibleRead { read_fallible() -> i64 }\n"
                                         "class FallibleReader implements FallibleRead {\n"
                                         "  read_fallible() -> i64 { throw ReadFailure.Failed }\n"
                                         "}\n"
                                         "fn witness_invoke(value: FallibleRead) -> i64 {\n"
                                         "  try { return value.read_fallible() }\n"
                                         "  catch (error: ReadFailure) { return 4 }\n"
                                         "}\n"
                                         "fn witness_invoke_value() -> i64 {\n"
                                         "  return witness_invoke(FallibleReader())\n"
                                         "}\n"
                                         "struct Point {\n"
                                         "  x: i64\n"
                                         "  y: i64\n"
                                         "}\n"
                                         "enum Packet { Data { code: i64, flag: bool }, Empty }\n"
                                         "enum Nested<T> { Value { pair: (T, bool) }, Empty }\n"
                                         "enum ComputeError { Negative { code: i64 } }\n"
                                         "fn maybe_error(value: i64) -> i64 {\n"
                                         "  if (value < 0) {\n"
                                         "    throw ComputeError.Negative { code: value }\n"
                                         "  }\n"
                                         "  return value + 1\n"
                                         "}\n"
                                         "fn invoke_value() -> i64 {\n"
                                         "  return maybe_error(4)\n"
                                         "}\n"
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
                                         "  return copy(value)\n"
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
                                         "fn update_point(input: move Point) -> i64 {\n"
                                         "  var point = input\n"
                                         "  point.x = point.x + 2\n"
                                         "  return point.x\n"
                                         "}\n"
                                         "fn write_ref(value: ref i64) -> i64 {\n"
                                         "  value = 42\n"
                                         "  return value\n"
                                         "}\n"
                                         "fn ref_value() -> i64 {\n"
                                         "  var value: i64 = 40\n"
                                         "  write_ref(ref value)\n"
                                         "  return value\n"
                                         "}\n"
                                         "fn make_adder(base: i64) -> fn(i64) -> i64 {\n"
                                         "  return fn(value: i64) -> i64 { return base + value }\n"
                                         "}\n"
                                         "fn escaped_callable_value() -> i64 {\n"
                                         "  var action = make_adder(40)\n"
                                         "  return action(2)\n"
                                         "}\n"
                                         "fn callable_multiuse() -> i64 {\n"
                                         "  var base = 10\n"
                                         "  var action = fn(value: i64) -> i64 { return base + value }\n"
                                         "  return action(1) + action(2)\n"
                                         "}\n"
                                         "fn callable_cross_cfg(flag: bool) -> i64 {\n"
                                         "  var base = 10\n"
                                         "  var action = fn(value: i64) -> i64 { return base + value }\n"
                                         "  var selected: i64 = 0\n"
                                         "  if (flag) { selected = action(1) }\n"
                                         "  else { selected = action(2) }\n"
                                         "  return selected + action(3)\n"
                                         "}\n"
                                         "fn callable_loop() -> i64 {\n"
                                         "  var base = 10\n"
                                         "  var action = fn(value: i64) -> i64 { return base + value }\n"
                                         "  var index: i64 = 0\n"
                                         "  var total: i64 = 0\n"
                                         "  while (index < 3) {\n"
                                         "    total = total + action(index)\n"
                                         "    index = index + 1\n"
                                         "  }\n"
                                         "  return total + action(3)\n"
                                         "}\n"
                                         "fn callable_copy() -> i64 {\n"
                                         "  var base = 10\n"
                                         "  var action = fn(value: i64) -> i64 { return base + value }\n"
                                         "  var duplicate = copy(action)\n"
                                         "  return action(1) + duplicate(2)\n"
                                         "}\n"
                                         "fn callable_value() -> i64 {\n"
                                         "  var captured = 5\n"
                                         "  var action = fn() -> i64 { return captured + 42 }\n"
                                         "  return action()\n"
                                         "}\n"
                                         "fn callable_error_value(value: i64) -> i64 {\n"
                                         "  var delta = 1\n"
                                         "  var action = fn(value: i64) -> i64 {\n"
                                         "    return maybe_error(value + delta)\n"
                                         "  }\n"
                                         "  try {\n"
                                         "    return action(value)\n"
                                         "  } catch (e: ComputeError) {\n"
                                         "    return 70\n"
                                         "  }\n"
                                         "}\n"
                                         "fn sibling_error_regions(value: i64) -> i64 {\n"
                                         "  var current = value\n"
                                         "  try {\n"
                                         "    current = maybe_error(current)\n"
                                         "  } catch (first: ComputeError) {\n"
                                         "    current = 1\n"
                                         "  }\n"
                                         "  try {\n"
                                         "    current = maybe_error(current)\n"
                                         "  } catch (second: ComputeError) {\n"
                                         "    current = 2\n"
                                         "  }\n"
                                         "  return current\n"
                                         "}\n"
                                         "fn edge_choice(flag: bool, left: i64, right: i64) -> i64 {\n"
                                         "  if (flag) { return left }\n"
                                         "  return right\n"
                                         "}\n"
                                         "fn edge_choice_entry() -> i64 {\n"
                                         "  return edge_choice(true, 7, 9) + edge_choice(false, 7, 9)\n"
                                         "}\n"
                                         "fn root() -> i64 {\n"
                                         "  return choose(10) + scalar_matrix(10, 2) + "
                                         "choose_bool(true) + choose_bool(false) + pair_value() + "
                                         "packet_value() + ref_value()\n"
                                         "    + invoke_value() + callable_value() + "
                                         "callable_error_value(3) + callable_error_value(-2)\n"
                                         "    + escaped_callable_value() + callable_multiuse()\n"
                                         "    + callable_cross_cfg(true) + callable_loop() + "
                                         "callable_copy() + existential_value() + "
                                         "witness_invoke_value()\n"
                                         "}\n";
    PIPELINE_TEST_REQUIRE(
        xi_pipeline_fixture_analyze_source(&fixture, session, "xi-program-input", program_source));

    XgGlobalEvidence global_evidence = {0};
    PIPELINE_TEST_REQUIRE(
        xi_pipeline_fixture_build_global_evidence(&fixture, session, &global_evidence));

    XiPipelineConfig config = xi_pipeline_program_input_config();
    config.run_canonicalize = false;
    config.source_file = "scalar-binding.xr";
    config.module_name = "program_input";
    config.module_identity = fixture.spec->canonical;
    config.global_evidence = &global_evidence;
    config.global_evidence_module_id = 1u;

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
    XiFunc *update_function = NULL;
    XiFunc *ref_function = NULL;
    XiFunc *maybe_error_function = NULL;
    XiFunc *callable_function = NULL;
    XiFunc *callable_error_function = NULL;
    XiFunc *make_adder_function = NULL;
    XiFunc *escaped_callable_function = NULL;
    XiFunc *multiuse_callable_function = NULL;
    XiFunc *cross_cfg_callable_function = NULL;
    XiFunc *loop_callable_function = NULL;
    XiFunc *copy_callable_function = NULL;
    XiFunc *existential_function = NULL;
    XiFunc *sibling_error_regions_function = NULL;
    XiFunc *edge_choice_function = NULL;
    XiFunc *edge_choice_entry_function = NULL;
    for (uint16_t function = 0; function < result.ir->module->nfuncs; ++function) {
        XiFunc *candidate = result.ir->module->functions[function];
        if (candidate && candidate->name && strcmp(candidate->name, "root") == 0)
            entry = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "packet_value") == 0)
            packet_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "update_point") == 0)
            update_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "ref_value") == 0)
            ref_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "maybe_error") == 0)
            maybe_error_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "callable_value") == 0)
            callable_function = candidate;
        if (candidate && candidate->name &&
            strcmp(candidate->name, "callable_error_value") == 0)
            callable_error_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "make_adder") == 0)
            make_adder_function = candidate;
        if (candidate && candidate->name &&
            strcmp(candidate->name, "escaped_callable_value") == 0)
            escaped_callable_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "callable_multiuse") == 0)
            multiuse_callable_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "callable_cross_cfg") == 0)
            cross_cfg_callable_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "callable_loop") == 0)
            loop_callable_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "callable_copy") == 0)
            copy_callable_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "existential_value") == 0)
            existential_function = candidate;
        if (candidate && candidate->name &&
            strcmp(candidate->name, "sibling_error_regions") == 0)
            sibling_error_regions_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "edge_choice") == 0)
            edge_choice_function = candidate;
        if (candidate && candidate->name && strcmp(candidate->name, "edge_choice_entry") == 0)
            edge_choice_entry_function = candidate;
    }
    PIPELINE_TEST_REQUIRE(entry != NULL);
    PIPELINE_TEST_REQUIRE(packet_function != NULL);
    PIPELINE_TEST_REQUIRE(update_function != NULL);
    PIPELINE_TEST_REQUIRE(ref_function != NULL);
    PIPELINE_TEST_REQUIRE(maybe_error_function != NULL);
    PIPELINE_TEST_REQUIRE(callable_function != NULL);
    PIPELINE_TEST_REQUIRE(callable_error_function != NULL);
    PIPELINE_TEST_REQUIRE(make_adder_function != NULL);
    PIPELINE_TEST_REQUIRE(escaped_callable_function != NULL);
    PIPELINE_TEST_REQUIRE(multiuse_callable_function != NULL);
    PIPELINE_TEST_REQUIRE(cross_cfg_callable_function != NULL);
    PIPELINE_TEST_REQUIRE(loop_callable_function != NULL);
    PIPELINE_TEST_REQUIRE(copy_callable_function != NULL);
    PIPELINE_TEST_REQUIRE(existential_function != NULL);
    PIPELINE_TEST_REQUIRE(sibling_error_regions_function != NULL);
    PIPELINE_TEST_REQUIRE(edge_choice_function != NULL);
    PIPELINE_TEST_REQUIRE(edge_choice_entry_function != NULL);
    PIPELINE_TEST_REQUIRE(cross_cfg_callable_function->nblocks > 1u);
    PIPELINE_TEST_REQUIRE(loop_callable_function->nblocks > 1u);
    PIPELINE_TEST_REQUIRE(xi_function_has_operation(copy_callable_function, XI_CALL_BUILTIN));
    PIPELINE_TEST_REQUIRE(!xi_function_has_operation(copy_callable_function, XI_CHECKTYPE));
    PIPELINE_TEST_REQUIRE(xi_function_has_operation(update_function, XI_AGG_GET));
    PIPELINE_TEST_REQUIRE(xi_function_has_operation(update_function, XI_AGG_UPDATE));
    PIPELINE_TEST_REQUIRE(!xi_function_has_operation(update_function, XI_AGG_SET));
    PIPELINE_TEST_REQUIRE(xi_function_has_operation(ref_function, XI_LOCAL_ADDR));
    PIPELINE_TEST_REQUIRE(make_adder_function->return_type != NULL);
    PIPELINE_TEST_REQUIRE(make_adder_function->return_type->kind == XR_KIND_FUNCTION);
    PIPELINE_TEST_REQUIRE(xr_type_function_is_no_throw(make_adder_function->return_type));
    PIPELINE_TEST_REQUIRE(!xi_function_has_operation(escaped_callable_function, XI_CHECKTYPE));

    const XgInterfaceImplSummary *read_enum_conformance = NULL;
    for (uint32_t index = 0u; index < global_evidence.ninterface_impls; ++index) {
        const XgInterfaceImplSummary *candidate = &global_evidence.interface_impls[index];
        if (candidate->implementor_kind != XG_DECL_ENUM)
            continue;
        PIPELINE_TEST_REQUIRE(read_enum_conformance == NULL);
        read_enum_conformance = candidate;
    }
    PIPELINE_TEST_REQUIRE(read_enum_conformance != NULL);
    PIPELINE_TEST_REQUIRE(read_enum_conformance->implementor_kind == XG_DECL_ENUM);
    PIPELINE_TEST_REQUIRE(read_enum_conformance->implementor_class_id == XG_NO_ID);
    PIPELINE_TEST_REQUIRE(read_enum_conformance->implementor_decl_id != XG_NO_ID);
    PIPELINE_TEST_REQUIRE(read_enum_conformance->nominal_key != 0u);

    const XgInterfaceWitnessSummary *read_enum_witness =
        xg_global_evidence_find_interface_witness(
            &global_evidence, read_enum_conformance->conformance_id, 0u);
    XiFunc *read_enum_target =
        read_enum_witness
            ? xi_pipeline_find_module_function_by_xg_id(
                  result.ir, read_enum_witness->implementation_func_id)
            : NULL;
    PIPELINE_TEST_REQUIRE(read_enum_target != NULL);
    PIPELINE_TEST_REQUIRE(read_enum_target->analyzer_effect_id != XA_EFFECT_NONE);
    const XaEffectSummary *read_enum_effect = xa_effect_db_get(
        read_enum_target->analyzer->effect_db, read_enum_target->analyzer_effect_id);
    PIPELINE_TEST_REQUIRE(read_enum_effect != NULL);
    PIPELINE_TEST_REQUIRE(xa_effect_summary_is_complete(read_enum_effect));
    PIPELINE_TEST_REQUIRE(xa_effect_summary_is_nothrow(read_enum_effect));
    PIPELINE_TEST_REQUIRE(read_enum_target->analyzer_effect_complete);
    PIPELINE_TEST_REQUIRE(read_enum_target->error_effect_nothrow);
    PIPELINE_TEST_REQUIRE(read_enum_target->has_receiver);
    PIPELINE_TEST_REQUIRE(read_enum_target->nparams != 0u);
    PIPELINE_TEST_REQUIRE(read_enum_target->params != NULL);
    PIPELINE_TEST_REQUIRE(read_enum_target->params[0] != NULL);
    XrType *read_enum_receiver_type = read_enum_target->params[0]->type;
    PIPELINE_TEST_REQUIRE(read_enum_receiver_type != NULL);
    PIPELINE_TEST_REQUIRE(read_enum_receiver_type->kind == XR_KIND_ENUM);
    PIPELINE_TEST_REQUIRE(read_enum_receiver_type->enum_type.layout != NULL);
    PIPELINE_TEST_REQUIRE(read_enum_receiver_type->enum_type.layout_id != 0u);
    PIPELINE_TEST_REQUIRE(read_enum_receiver_type->enum_type.layout_id ==
                          read_enum_receiver_type->enum_type.layout->layout_id);
    XrClassInfo *read_enum_info = read_enum_receiver_type->enum_type.nominal_ref;
    PIPELINE_TEST_REQUIRE(read_enum_info != NULL);
    PIPELINE_TEST_REQUIRE(read_enum_info->xg_decl_id ==
                          read_enum_conformance->implementor_decl_id);
    PIPELINE_TEST_REQUIRE(read_enum_info->xg_nominal_key ==
                          read_enum_conformance->nominal_key);

    const XgDeclSummary *read_enum_decl = NULL;
    for (uint32_t index = 0u; index < global_evidence.ndecls; ++index) {
        const XgDeclSummary *candidate = &global_evidence.decls[index];
        if (candidate->decl_id != read_enum_conformance->implementor_decl_id)
            continue;
        PIPELINE_TEST_REQUIRE(read_enum_decl == NULL);
        read_enum_decl = candidate;
    }
    PIPELINE_TEST_REQUIRE(read_enum_decl != NULL);
    PIPELINE_TEST_REQUIRE(read_enum_decl->kind == XG_DECL_ENUM);
    PIPELINE_TEST_REQUIRE(read_enum_decl->nominal_key == read_enum_conformance->nominal_key);

    XiEnumData *read_enum_schema = NULL;
    for (uint16_t index = 0u; index < result.ir->module->nslots; ++index) {
        XiEnumData *candidate =
            result.ir->module->slot_enums ? result.ir->module->slot_enums[index] : NULL;
        if (!candidate || candidate->layout_id != read_enum_receiver_type->enum_type.layout_id)
            continue;
        PIPELINE_TEST_REQUIRE(read_enum_schema == NULL || read_enum_schema == candidate);
        read_enum_schema = candidate;
    }
    PIPELINE_TEST_REQUIRE(read_enum_schema != NULL);
    PIPELINE_TEST_REQUIRE(read_enum_schema->member_count ==
                          read_enum_receiver_type->enum_type.layout->variant_count);

    XgInterfaceId fallible_interface_id = XG_NO_ID;
    const XgInterfaceMethodSummary *fallible_interface_method = NULL;
    for (uint32_t index = 0u; index < global_evidence.ninterface_methods; ++index) {
        const XgInterfaceMethodSummary *candidate = &global_evidence.interface_methods[index];
        if (!candidate->contract_complete || candidate->error_type_key == 0u ||
            (candidate->effect_bits & XG_BODY_MAY_ERROR) == 0u)
            continue;
        PIPELINE_TEST_REQUIRE(fallible_interface_method == NULL);
        fallible_interface_method = candidate;
        fallible_interface_id = candidate->owner_interface_id;
    }
    PIPELINE_TEST_REQUIRE(fallible_interface_method != NULL);
    PIPELINE_TEST_REQUIRE(fallible_interface_id != XG_NO_ID);

    const XgInterfaceImplSummary *fallible_reader_conformance = NULL;
    for (uint32_t index = 0u; index < global_evidence.ninterface_impls; ++index) {
        const XgInterfaceImplSummary *candidate = &global_evidence.interface_impls[index];
        if (candidate->interface_id != fallible_interface_id)
            continue;
        PIPELINE_TEST_REQUIRE(fallible_reader_conformance == NULL);
        fallible_reader_conformance = candidate;
    }
    PIPELINE_TEST_REQUIRE(fallible_reader_conformance != NULL);
    PIPELINE_TEST_REQUIRE(fallible_reader_conformance->implementor_kind == XG_DECL_CLASS);
    PIPELINE_TEST_REQUIRE(fallible_reader_conformance->implementor_class_id != XG_NO_ID);
    PIPELINE_TEST_REQUIRE(fallible_reader_conformance->implementor_decl_id != XG_NO_ID);
    PIPELINE_TEST_REQUIRE(fallible_reader_conformance->nominal_key != 0u);

    const XgInterfaceWitnessSummary *fallible_reader_witness =
        xg_global_evidence_find_interface_witness(
            &global_evidence, fallible_reader_conformance->conformance_id, 0u);
    XiFunc *fallible_reader_target =
        fallible_reader_witness
            ? xi_pipeline_find_module_function_by_xg_id(
                  result.ir, fallible_reader_witness->implementation_func_id)
            : NULL;
    PIPELINE_TEST_REQUIRE(fallible_reader_target != NULL);
    const XaEffectSummary *fallible_reader_effect =
        xa_effect_db_get(fallible_reader_target->analyzer->effect_db,
                         fallible_reader_target->analyzer_effect_id);
    PIPELINE_TEST_REQUIRE(fallible_reader_effect != NULL);
    PIPELINE_TEST_REQUIRE(fallible_reader_effect->error_set_completeness == XA_EFFECT_COMPLETE);
    PIPELINE_TEST_REQUIRE(fallible_reader_effect->error_unknown_reasons == XA_UNKNOWN_NONE);
    PIPELINE_TEST_REQUIRE(fallible_reader_effect->escaping.count == 1u);
    XrType *fallible_error_type = xa_effect_db_error_type_handle(
        fallible_reader_target->analyzer->effect_db,
        fallible_reader_effect->escaping.types[0].type_id);
    PIPELINE_TEST_REQUIRE(fallible_error_type != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_type->kind == XR_KIND_ENUM);
    PIPELINE_TEST_REQUIRE(fallible_error_type->enum_type.layout != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_type->enum_type.layout_id != 0u);
    PIPELINE_TEST_REQUIRE(fallible_error_type->enum_type.layout_id ==
                          fallible_error_type->enum_type.layout->layout_id);
    XrClassInfo *fallible_error_info = fallible_error_type->enum_type.nominal_ref;
    PIPELINE_TEST_REQUIRE(fallible_error_info != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_info->nominal_kind == XA_NOMINAL_ENUM);
    PIPELINE_TEST_REQUIRE(fallible_error_info->declaration_symbol != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_info->xg_decl_id != XG_NO_ID);
    PIPELINE_TEST_REQUIRE(fallible_error_info->xg_nominal_key != 0u);
    const XgDeclSummary *fallible_error_decl = NULL;
    for (uint32_t index = 0u; index < global_evidence.ndecls; ++index) {
        const XgDeclSummary *candidate = &global_evidence.decls[index];
        if (candidate->decl_id != fallible_error_info->xg_decl_id)
            continue;
        PIPELINE_TEST_REQUIRE(fallible_error_decl == NULL);
        fallible_error_decl = candidate;
    }
    PIPELINE_TEST_REQUIRE(fallible_error_decl != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_decl->kind == XG_DECL_ENUM);
    PIPELINE_TEST_REQUIRE(fallible_error_decl->nominal_key ==
                          fallible_error_info->xg_nominal_key);
    PIPELINE_TEST_REQUIRE(fallible_error_decl->type_key ==
                          fallible_interface_method->error_type_key);
    PIPELINE_TEST_REQUIRE(fallible_reader_target->has_receiver);
    PIPELINE_TEST_REQUIRE(fallible_reader_target->nparams != 0u);
    PIPELINE_TEST_REQUIRE(fallible_reader_target->params != NULL);
    PIPELINE_TEST_REQUIRE(fallible_reader_target->params[0] != NULL);
    XrType *fallible_reader_receiver_type = fallible_reader_target->params[0]->type;
    PIPELINE_TEST_REQUIRE(fallible_reader_receiver_type != NULL);
    PIPELINE_TEST_REQUIRE(fallible_reader_receiver_type->kind == XR_KIND_INSTANCE ||
                          fallible_reader_receiver_type->kind == XR_KIND_CLASS);
    XrClassInfo *fallible_reader_info = fallible_reader_receiver_type->instance.class_ref;
    PIPELINE_TEST_REQUIRE(fallible_reader_info != NULL);
    PIPELINE_TEST_REQUIRE(fallible_reader_info->xg_class_id ==
                          fallible_reader_conformance->implementor_class_id);
    PIPELINE_TEST_REQUIRE(fallible_reader_info->xg_decl_id ==
                          fallible_reader_conformance->implementor_decl_id);
    PIPELINE_TEST_REQUIRE(fallible_reader_info->xg_nominal_key ==
                          fallible_reader_conformance->nominal_key);

    XiClassData *fallible_reader_schema = NULL;
    for (uint16_t index = 0u; index < result.ir->module->nclasses; ++index) {
        XiClassData *candidate =
            result.ir->module->classes ? result.ir->module->classes[index] : NULL;
        if (!candidate ||
            candidate->xg_class_id != fallible_reader_conformance->implementor_class_id)
            continue;
        PIPELINE_TEST_REQUIRE(fallible_reader_schema == NULL);
        fallible_reader_schema = candidate;
    }
    PIPELINE_TEST_REQUIRE(fallible_reader_schema != NULL);
    PIPELINE_TEST_REQUIRE(fallible_reader_schema->class_info != NULL);
    PIPELINE_TEST_REQUIRE(fallible_reader_schema->class_info->xg_decl_id ==
                          fallible_reader_conformance->implementor_decl_id);
    PIPELINE_TEST_REQUIRE(fallible_reader_schema->class_info->xg_nominal_key ==
                          fallible_reader_conformance->nominal_key);

    XrProgramFromXiInput producer_input = {
        .module_roots = module_roots,
        .module_count = 1u,
        .entry_function = entry,
        .global_evidence = &global_evidence,
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
    PIPELINE_TEST_REQUIRE(xi_pipeline_hostile_lexical_error_regions_are_fail_closed(
        sibling_error_regions_function, &producer_input, &artifact));
    PIPELINE_TEST_REQUIRE(xi_pipeline_same_successor_edges_keep_distinct_phi_arguments(
        edge_choice_function, edge_choice_entry_function, &producer_input, 16));

    uint32_t saved_read_enum_effect_id = read_enum_target->analyzer_effect_id;
    bool saved_read_enum_effect_complete = read_enum_target->analyzer_effect_complete;
    bool saved_read_enum_nothrow = read_enum_target->error_effect_nothrow;
    read_enum_target->analyzer_effect_id = XA_EFFECT_NONE;
    read_enum_target->analyzer_effect_complete = false;
    read_enum_target->error_effect_nothrow = false;
    XrProgramArtifact missing_read_enum_effect_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_read_enum_effect_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(missing_read_enum_effect_artifact.bytes == NULL);
    read_enum_target->analyzer_effect_id = saved_read_enum_effect_id;
    read_enum_target->analyzer_effect_complete = saved_read_enum_effect_complete;
    read_enum_target->error_effect_nothrow = saved_read_enum_nothrow;

    const XrEnumLayout *saved_fallible_error_layout = fallible_error_type->enum_type.layout;
    fallible_error_type->enum_type.layout = NULL;
    XrProgramArtifact missing_fallible_error_layout_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_fallible_error_layout_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(missing_fallible_error_layout_artifact.bytes == NULL);
    fallible_error_type->enum_type.layout = saved_fallible_error_layout;

    fallible_error_type->enum_type.nominal_ref = NULL;
    XrProgramArtifact missing_fallible_error_nominal_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_fallible_error_nominal_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(missing_fallible_error_nominal_artifact.bytes == NULL);
    fallible_error_type->enum_type.nominal_ref = fallible_error_info;

    uint32_t saved_read_enum_layout_id = read_enum_schema->layout_id;
    read_enum_schema->layout_id = 0u;
    XrProgramArtifact missing_read_enum_schema_identity_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_read_enum_schema_identity_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) !=
        XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(missing_read_enum_schema_identity_artifact.bytes == NULL);
    read_enum_schema->layout_id = saved_read_enum_layout_id;

    read_enum_receiver_type->enum_type.nominal_ref = NULL;
    XrProgramArtifact missing_read_enum_nominal_identity_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_read_enum_nominal_identity_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(missing_read_enum_nominal_identity_artifact.bytes == NULL);
    read_enum_receiver_type->enum_type.nominal_ref = read_enum_info;

    const XrEnumLayout *saved_read_enum_layout = read_enum_receiver_type->enum_type.layout;
    read_enum_receiver_type->enum_type.layout = NULL;
    XrProgramArtifact missing_read_enum_receiver_layout_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_read_enum_receiver_layout_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(missing_read_enum_receiver_layout_artifact.bytes == NULL);
    read_enum_receiver_type->enum_type.layout = saved_read_enum_layout;

    XgClassId saved_fallible_reader_class_id = fallible_reader_schema->xg_class_id;
    fallible_reader_schema->xg_class_id = XG_NO_ID;
    XrProgramArtifact missing_fallible_reader_schema_identity_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input,
                                 &missing_fallible_reader_schema_identity_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) !=
        XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(missing_fallible_reader_schema_identity_artifact.bytes == NULL);
    fallible_reader_schema->xg_class_id = saved_fallible_reader_class_id;

    XiValue *empty_class_allocation = NULL;
    XiValue *empty_struct_literal = NULL;
    XiValue *unit_enum_literal = NULL;
    XiBlock *empty_class_error_block = NULL;
    XgCallsiteSummary *empty_class_callsite = NULL;
    for (uint32_t block_index = 0u;
         block_index < existential_function->nblocks &&
         (!empty_class_allocation || !empty_struct_literal || !unit_enum_literal);
         ++block_index) {
        XiBlock *block = existential_function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            XiValue *value = block->values[value_index];
            if (value && value->op == XI_CALL_METHOD && xi_value_is_constructor_call(value))
                empty_struct_literal = value;
            if (value && value->op == XI_LOAD_FIELD && value->nargs == 1u && value->args &&
                value->args[0] && value->args[0]->op == XI_GET_SHARED &&
                value->args[0]->aux_int >= 0 && value->type &&
                value->type->kind == XR_KIND_ENUM && result.ir->module->slot_enums &&
                value->args[0]->aux_int < result.ir->module->nslots &&
                result.ir->module->slot_enums[value->args[0]->aux_int])
                unit_enum_literal = value;
            if (!value || value->op != XI_CALL || value->xg_callsite_id == XG_NO_ID)
                continue;
            XgCallsiteSummary *row = NULL;
            for (uint32_t callsite_index = 0u; callsite_index < global_evidence.ncallsites;
                 ++callsite_index) {
                if (global_evidence.callsites[callsite_index].callsite_id ==
                    value->xg_callsite_id) {
                    row = &global_evidence.callsites[callsite_index];
                    break;
                }
            }
            if (!row || row->kind != XG_CALL_CLASS_ALLOC)
                continue;
            empty_class_allocation = value;
            empty_class_callsite = row;
            empty_class_error_block = block->succs[0];
            break;
        }
    }
    PIPELINE_TEST_REQUIRE(empty_class_allocation != NULL);
    PIPELINE_TEST_REQUIRE(empty_struct_literal != NULL);
    PIPELINE_TEST_REQUIRE(unit_enum_literal != NULL);
    PIPELINE_TEST_REQUIRE(empty_class_callsite != NULL);
    PIPELINE_TEST_REQUIRE(empty_class_error_block != NULL);
    PIPELINE_TEST_REQUIRE(empty_class_error_block->kind == XI_BLOCK_RETURN);

    XgClassId saved_allocation_class_id = empty_class_callsite->receiver_static_class_id;
    empty_class_callsite->receiver_static_class_id = XG_NO_ID;
    XrProgramArtifact missing_allocation_identity_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_allocation_identity_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) !=
        XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(missing_allocation_identity_artifact.bytes == NULL);
    empty_class_callsite->receiver_static_class_id = saved_allocation_class_id;

    XiBlockKind saved_error_block_kind = empty_class_error_block->kind;
    empty_class_error_block->kind = XI_BLOCK_PLAIN;
    XrProgramArtifact nonmechanical_allocation_error_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &nonmechanical_allocation_error_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) !=
        XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(nonmechanical_allocation_error_artifact.bytes == NULL);
    empty_class_error_block->kind = saved_error_block_kind;

    uint32_t saved_struct_lowering_flags = empty_struct_literal->lowering_flags;
    empty_struct_literal->lowering_flags &= ~XI_LOWERING_FLAG_CONSTRUCTOR_CALL;
    XrProgramArtifact missing_struct_constructor_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_struct_constructor_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) !=
        XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(missing_struct_constructor_artifact.bytes == NULL);
    empty_struct_literal->lowering_flags = saved_struct_lowering_flags;

    int64_t saved_unit_enum_symbol = unit_enum_literal->aux_int;
    unit_enum_literal->aux_int = -1;
    XrProgramArtifact missing_unit_enum_symbol_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_unit_enum_symbol_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) !=
        XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(missing_unit_enum_symbol_artifact.bytes == NULL);
    unit_enum_literal->aux_int = saved_unit_enum_symbol;

    XiValue *source_existential_pack = NULL;
    for (uint16_t function_index = 0u;
         function_index < result.ir->module->nfuncs && !source_existential_pack;
         ++function_index) {
        XiFunc *source_function = result.ir->module->functions[function_index];
        for (uint32_t block_index = 0u;
             source_function && block_index < source_function->nblocks &&
             !source_existential_pack;
             ++block_index) {
            XiBlock *source_block = source_function->blocks[block_index];
            for (uint32_t value_index = 0u; source_block && value_index < source_block->nvalues;
                 ++value_index) {
                XiValue *candidate = source_block->values[value_index];
                if (candidate && candidate->xg_existential_kind == XI_EXISTENTIAL_PACK) {
                    source_existential_pack = candidate;
                    break;
                }
            }
        }
    }
    PIPELINE_TEST_REQUIRE(source_existential_pack != NULL);
    PIPELINE_TEST_REQUIRE(source_existential_pack->op == XI_COPY);
    PIPELINE_TEST_REQUIRE(source_existential_pack->xg_interface_object_use_id != XG_NO_ID);
    PIPELINE_TEST_REQUIRE(source_existential_pack->xg_type_contract_complete == 1u);

    uint8_t saved_frozen_ownership = source_existential_pack->xg_implementor_ownership;
    source_existential_pack->xg_implementor_ownership =
        saved_frozen_ownership == XG_NOMINAL_OWNERSHIP_AFFINE
            ? XG_NOMINAL_OWNERSHIP_TRIVIAL
            : XG_NOMINAL_OWNERSHIP_AFFINE;
    XrProgramArtifact mismatched_frozen_contract_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &mismatched_frozen_contract_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(mismatched_frozen_contract_artifact.bytes == NULL);
    source_existential_pack->xg_implementor_ownership = saved_frozen_ownership;

    uint32_t saved_object_use_id = source_existential_pack->xg_interface_object_use_id;
    source_existential_pack->xg_interface_object_use_id = XG_NO_ID;
    XrProgramArtifact missing_object_use_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_object_use_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(missing_object_use_artifact.bytes == NULL);
    source_existential_pack->xg_interface_object_use_id = saved_object_use_id;

    XgInterfaceImplSummary *source_conformance = NULL;
    for (uint32_t index = 0u; index < global_evidence.ninterface_impls; ++index) {
        if (global_evidence.interface_impls[index].conformance_id ==
            source_existential_pack->xg_conformance_id) {
            source_conformance = &global_evidence.interface_impls[index];
            break;
        }
    }
    PIPELINE_TEST_REQUIRE(source_conformance != NULL);
    const XgInterfaceWitnessSummary *source_witness =
        xg_global_evidence_find_interface_witness(&global_evidence,
                                                  source_conformance->conformance_id, 0u);
    XiFunc *source_witness_target =
        source_witness ? xi_pipeline_find_module_function_by_xg_id(
                             result.ir, source_witness->implementation_func_id)
                       : NULL;
    XgInterfaceMethodSummary *source_interface_method = NULL;
    for (uint32_t index = 0u; source_witness && index < global_evidence.ninterface_methods;
         ++index) {
        if (global_evidence.interface_methods[index].interface_method_id ==
            source_witness->interface_method_id) {
            source_interface_method = &global_evidence.interface_methods[index];
            break;
        }
    }
    PIPELINE_TEST_REQUIRE(source_witness_target != NULL);
    PIPELINE_TEST_REQUIRE(source_interface_method != NULL);
    PIPELINE_TEST_REQUIRE(source_interface_method->result_ownership.complete);
    PIPELINE_TEST_REQUIRE(source_interface_method->result_ownership.kind ==
                          XG_RETURN_OWNERSHIP_OWNED);
    PIPELINE_TEST_REQUIRE(source_witness_target->arc_return_ownership.complete);
    PIPELINE_TEST_REQUIRE(source_witness_target->arc_return_ownership.kind ==
                          XI_RETURN_OWNERSHIP_OWNED);
    PIPELINE_TEST_REQUIRE(source_witness_target->arc_return_ownership.param_index ==
                          source_interface_method->result_ownership.param_index);
    PIPELINE_TEST_REQUIRE(source_witness_target->has_receiver);
    PIPELINE_TEST_REQUIRE(source_witness_target->nparams != 0u);
    PIPELINE_TEST_REQUIRE(source_witness_target->params != NULL);
    PIPELINE_TEST_REQUIRE(source_witness_target->params[0] != NULL);
    XrType *source_receiver_type = source_witness_target->params[0]->type;
    PIPELINE_TEST_REQUIRE(source_receiver_type != NULL);
    PIPELINE_TEST_REQUIRE(source_receiver_type->kind == XR_KIND_CLASS ||
                          source_receiver_type->kind == XR_KIND_INSTANCE ||
                          source_receiver_type->kind == XR_KIND_ENUM);
    XrClassInfo *source_receiver_info =
        source_receiver_type->kind == XR_KIND_ENUM
            ? source_receiver_type->enum_type.nominal_ref
            : source_receiver_type->instance.class_ref;
    PIPELINE_TEST_REQUIRE(source_receiver_info != NULL);
    PIPELINE_TEST_REQUIRE(source_receiver_info->xg_decl_id ==
                          source_conformance->implementor_decl_id);
    PIPELINE_TEST_REQUIRE(source_receiver_info->xg_nominal_key ==
                          source_conformance->nominal_key);

    if (source_receiver_type->kind == XR_KIND_CLASS ||
        source_receiver_type->kind == XR_KIND_INSTANCE) {
        source_receiver_type->instance.class_ref = NULL;
        XrProgramArtifact missing_receiver_identity_artifact = {0};
        PIPELINE_TEST_REQUIRE(
            xr_program_write_from_xi(&producer_input, &missing_receiver_identity_artifact,
                                     producer_diagnostic, sizeof(producer_diagnostic)) ==
            XR_PROGRAM_BUILD_INVALID_INPUT);
        PIPELINE_TEST_REQUIRE(missing_receiver_identity_artifact.bytes == NULL);
        source_receiver_type->instance.class_ref = source_receiver_info;
    } else {
        PIPELINE_TEST_REQUIRE(source_receiver_type->kind == XR_KIND_ENUM);
        source_receiver_type->enum_type.nominal_ref = NULL;
        XrProgramArtifact missing_receiver_identity_artifact = {0};
        PIPELINE_TEST_REQUIRE(
            xr_program_write_from_xi(&producer_input, &missing_receiver_identity_artifact,
                                     producer_diagnostic, sizeof(producer_diagnostic)) ==
            XR_PROGRAM_BUILD_INVALID_INPUT);
        PIPELINE_TEST_REQUIRE(missing_receiver_identity_artifact.bytes == NULL);
        source_receiver_type->enum_type.nominal_ref = source_receiver_info;
    }

    uint8_t saved_type_contract_complete = source_conformance->type_contract_complete;
    source_conformance->type_contract_complete = 0u;
    XrProgramArtifact incomplete_type_contract_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &incomplete_type_contract_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(incomplete_type_contract_artifact.bytes == NULL);
    source_conformance->type_contract_complete = saved_type_contract_complete;

    PIPELINE_TEST_REQUIRE(global_evidence.ninterface_witnesses != 0u);
    XgFuncId saved_witness_target = global_evidence.interface_witnesses[0].implementation_func_id;
    global_evidence.interface_witnesses[0].implementation_func_id = XG_NO_ID;
    XrProgramArtifact missing_witness_target_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_witness_target_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) !=
        XR_PROGRAM_BUILD_OK);
    PIPELINE_TEST_REQUIRE(missing_witness_target_artifact.bytes == NULL);
    global_evidence.interface_witnesses[0].implementation_func_id = saved_witness_target;

    PIPELINE_TEST_REQUIRE(global_evidence.ninterface_methods != 0u);
    uint32_t saved_method_result_key = global_evidence.interface_methods[0].result_type_key;
    global_evidence.interface_methods[0].result_type_key ^= UINT32_C(0x80000000);
    XrProgramArtifact mismatched_method_result_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &mismatched_method_result_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(mismatched_method_result_artifact.bytes == NULL);
    global_evidence.interface_methods[0].result_type_key = saved_method_result_key;

    uint8_t saved_method_result_ownership = source_interface_method->result_ownership.kind;
    source_interface_method->result_ownership.kind = XG_RETURN_OWNERSHIP_BORROWED_STATIC;
    XrProgramArtifact mismatched_method_ownership_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &mismatched_method_ownership_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(mismatched_method_ownership_artifact.bytes == NULL);
    source_interface_method->result_ownership.kind = saved_method_result_ownership;

    XrProgramFromXiInput missing_evidence_input = producer_input;
    missing_evidence_input.global_evidence = NULL;
    XrProgramArtifact missing_evidence_artifact = {0};
    PIPELINE_TEST_REQUIRE(xr_program_write_from_xi(&missing_evidence_input,
                                                   &missing_evidence_artifact, producer_diagnostic,
                                                   sizeof(producer_diagnostic)) ==
                          XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(missing_evidence_artifact.bytes == NULL);

    XiValue *indirect_call = NULL;
    XgCallsiteSummary *indirect_callsite = NULL;
    for (uint32_t block = 0; block < callable_function->nblocks && !indirect_call; ++block) {
        XiBlock *row = callable_function->blocks[block];
        for (uint32_t value_index = 0; row && value_index < row->nvalues; ++value_index) {
            XiValue *value = row->values[value_index];
            if (!value || value->op != XI_CALL || value->xg_callsite_id == XG_NO_ID)
                continue;
            for (uint32_t callsite_index = 0; callsite_index < global_evidence.ncallsites;
                 ++callsite_index) {
                XgCallsiteSummary *candidate = &global_evidence.callsites[callsite_index];
                if (candidate->callsite_id == value->xg_callsite_id &&
                    candidate->kind == XG_CALL_CLOSURE) {
                    indirect_call = value;
                    indirect_callsite = candidate;
                    break;
                }
            }
        }
    }
    PIPELINE_TEST_REQUIRE(indirect_call != NULL);
    PIPELINE_TEST_REQUIRE(indirect_callsite != NULL);
    PIPELINE_TEST_REQUIRE((indirect_callsite->flags & XG_CALL_ERROR_EFFECT_VERIFIED) != 0u);
    PIPELINE_TEST_REQUIRE((indirect_callsite->flags & XG_CALL_MAY_ERROR) == 0u);
    PIPELINE_TEST_REQUIRE((indirect_callsite->flags & XG_CALL_TARGET_SET_VERIFIED) != 0u);
    PIPELINE_TEST_REQUIRE(indirect_callsite->static_target_func_id == XG_NO_ID);
    PIPELINE_TEST_REQUIRE(indirect_callsite->callable_target_count != 0u);
    PIPELINE_TEST_REQUIRE(indirect_call->xg_callable_target_start ==
                          indirect_callsite->callable_target_start);
    PIPELINE_TEST_REQUIRE(indirect_call->xg_callable_target_count ==
                          indirect_callsite->callable_target_count);
    PIPELINE_TEST_REQUIRE(indirect_call->xg_callable_signature_key ==
                          indirect_callsite->callable_signature_key);

    XiValue *fallible_indirect_call = NULL;
    XgCallsiteSummary *fallible_indirect_callsite = NULL;
    XiBlock *fallible_indirect_call_block = NULL;
    for (uint32_t block = 0; block < callable_error_function->nblocks && !fallible_indirect_call;
         ++block) {
        XiBlock *row = callable_error_function->blocks[block];
        for (uint32_t value_index = 0; row && value_index < row->nvalues; ++value_index) {
            XiValue *value = row->values[value_index];
            if (!value || value->op != XI_CALL || value->xg_callsite_id == XG_NO_ID)
                continue;
            for (uint32_t callsite_index = 0; callsite_index < global_evidence.ncallsites;
                 ++callsite_index) {
                XgCallsiteSummary *candidate = &global_evidence.callsites[callsite_index];
                if (candidate->callsite_id == value->xg_callsite_id &&
                    candidate->kind == XG_CALL_CLOSURE) {
                    fallible_indirect_call = value;
                    fallible_indirect_callsite = candidate;
                    fallible_indirect_call_block = row;
                    break;
                }
            }
        }
    }
    PIPELINE_TEST_REQUIRE(fallible_indirect_call != NULL);
    PIPELINE_TEST_REQUIRE(fallible_indirect_callsite != NULL);
    PIPELINE_TEST_REQUIRE(fallible_indirect_call_block != NULL);
    PIPELINE_TEST_REQUIRE((fallible_indirect_callsite->flags & XG_CALL_ERROR_EFFECT_VERIFIED) !=
                          0u);
    PIPELINE_TEST_REQUIRE((fallible_indirect_callsite->flags & XG_CALL_MAY_ERROR) != 0u);
    PIPELINE_TEST_REQUIRE((fallible_indirect_callsite->flags & XG_CALL_TARGET_SET_VERIFIED) != 0u);
    PIPELINE_TEST_REQUIRE(fallible_indirect_callsite->static_target_func_id == XG_NO_ID);
    PIPELINE_TEST_REQUIRE(fallible_indirect_callsite->callable_target_count != 0u);

    uint32_t saved_callsite_id = indirect_call->xg_callsite_id;
    indirect_call->xg_callsite_id = XG_NO_ID;
    XrProgramArtifact missing_callsite_artifact = {0};
    XrProgramBuildStatus missing_callsite_status =
        xr_program_write_from_xi(&producer_input, &missing_callsite_artifact, producer_diagnostic,
                                 sizeof(producer_diagnostic));
    PIPELINE_TEST_REQUIRE(missing_callsite_status == XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE);
    PIPELINE_TEST_REQUIRE(strstr(producer_diagnostic, "unresolved callable target set") != NULL);
    PIPELINE_TEST_REQUIRE(missing_callsite_artifact.bytes == NULL);
    indirect_call->xg_callsite_id = saved_callsite_id;

    uint32_t saved_callsite_flags = indirect_callsite->flags;
    indirect_callsite->flags &= ~XG_CALL_ERROR_EFFECT_VERIFIED;
    XrProgramArtifact unverified_callsite_artifact = {0};
    PIPELINE_TEST_REQUIRE(xr_program_write_from_xi(
                              &producer_input, &unverified_callsite_artifact, producer_diagnostic,
                              sizeof(producer_diagnostic)) ==
                          XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(unverified_callsite_artifact.bytes == NULL);
    indirect_callsite->flags = saved_callsite_flags;

    uint32_t saved_fallible_callsite_flags = fallible_indirect_callsite->flags;
    fallible_indirect_callsite->flags &= ~XG_CALL_MAY_ERROR;
    XrProgramArtifact missing_fallible_effect_artifact = {0};
    PIPELINE_TEST_REQUIRE(xr_program_write_from_xi(
                              &producer_input, &missing_fallible_effect_artifact,
                              producer_diagnostic, sizeof(producer_diagnostic)) ==
                          XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(missing_fallible_effect_artifact.bytes == NULL);
    fallible_indirect_callsite->flags = saved_fallible_callsite_flags;

    uint32_t saved_fallible_target_count = fallible_indirect_call->xg_callable_target_count;
    fallible_indirect_call->xg_callable_target_count = saved_fallible_target_count + 1u;
    XrProgramArtifact missing_fallible_target_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &missing_fallible_target_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(missing_fallible_target_artifact.bytes == NULL);
    fallible_indirect_call->xg_callable_target_count = saved_fallible_target_count;

    uint32_t saved_fallible_target_flags = fallible_indirect_callsite->flags;
    fallible_indirect_callsite->flags &= ~XG_CALL_TARGET_SET_VERIFIED;
    XrProgramArtifact rebound_fallible_target_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &rebound_fallible_target_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(rebound_fallible_target_artifact.bytes == NULL);
    fallible_indirect_callsite->flags = saved_fallible_target_flags;

    XiValue *fallible_error_check = fallible_indirect_call_block->control;
    PIPELINE_TEST_REQUIRE(fallible_error_check != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_check->op == XI_ERR_CHECK);
    PIPELINE_TEST_REQUIRE(fallible_error_check->error_region != NULL);
    XiValue *fallible_error_catch = NULL;
    XiValue *fallible_error_type_test = NULL;
    for (uint32_t block_index = 0u; block_index < callable_error_function->nblocks;
         ++block_index) {
        XiBlock *block = callable_error_function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            XiValue *value = block->values[value_index];
            if (value && value->op == XI_ERR_CATCH &&
                value->error_region == fallible_error_check->error_region) {
                PIPELINE_TEST_REQUIRE(fallible_error_catch == NULL);
                fallible_error_catch = value;
                continue;
            }
            if (value && value->op == XI_IS &&
                value->xg_existential_kind == XI_EXISTENTIAL_NONE && value->nargs == 2u &&
                value->args && value->args[0] == fallible_error_catch)
                fallible_error_type_test = value;
        }
    }
    PIPELINE_TEST_REQUIRE(fallible_error_catch != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_type_test != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_type_test->nargs == 2u);
    PIPELINE_TEST_REQUIRE(fallible_error_type_test->args != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_type_test->args[0] == fallible_error_catch);
    PIPELINE_TEST_REQUIRE(fallible_error_type_test->args[1] != NULL);
    PIPELINE_TEST_REQUIRE(fallible_error_type_test->args[1]->op == XI_GET_SHARED);
    PIPELINE_TEST_REQUIRE(fallible_error_type_test->args[1]->aux_int >= 0);
    PIPELINE_TEST_REQUIRE(fallible_error_type_test->aux != NULL);

    PIPELINE_TEST_REQUIRE(fallible_error_catch->error_region ==
                          fallible_error_check->error_region);
    XiErrorRegion *saved_fallible_error_region = fallible_error_check->error_region;
    XiErrorRegion mismatched_fallible_error_region = *saved_fallible_error_region;
    fallible_error_check->error_region = &mismatched_fallible_error_region;
    memset(producer_diagnostic, 0, sizeof(producer_diagnostic));
    XrProgramArtifact mismatched_fallible_error_region_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &mismatched_fallible_error_region_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(mismatched_fallible_error_region_artifact.bytes == NULL);
    PIPELINE_TEST_REQUIRE(strstr(producer_diagnostic, "error region") != NULL);
    fallible_error_check->error_region = saved_fallible_error_region;

    const XrType *saved_fallible_error_test_nominal = fallible_error_type_test->aux;
    PIPELINE_TEST_REQUIRE(saved_fallible_error_test_nominal != fallible_error_type);
    fallible_error_type_test->aux = fallible_error_type;
    memset(producer_diagnostic, 0, sizeof(producer_diagnostic));
    XrProgramArtifact mismatched_fallible_error_token_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &mismatched_fallible_error_token_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(mismatched_fallible_error_token_artifact.bytes == NULL);
    PIPELINE_TEST_REQUIRE(strstr(producer_diagnostic, "typed catch") != NULL);
    fallible_error_type_test->aux = (void *) saved_fallible_error_test_nominal;

    XrType *saved_fallible_error_type = fallible_error_catch->type;
    fallible_error_catch->type = fallible_indirect_call->type;
    memset(producer_diagnostic, 0, sizeof(producer_diagnostic));
    XrProgramArtifact mismatched_fallible_error_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&producer_input, &mismatched_fallible_error_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_INVALID_INPUT);
    PIPELINE_TEST_REQUIRE(mismatched_fallible_error_artifact.bytes == NULL);
    PIPELINE_TEST_REQUIRE(strstr(producer_diagnostic, "block argument") != NULL);
    fallible_error_catch->type = saved_fallible_error_type;

    XiValue *conflicting_callable_return = NULL;
    for (uint32_t block_index = 0u;
         block_index < callable_function->nblocks && !conflicting_callable_return;
         ++block_index) {
        XiBlock *block = callable_function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            XiValue *value = block->values[value_index];
            if (value && value->op == XI_CLOSURE_NEW && value->type &&
                value->type->kind == XR_KIND_FUNCTION &&
                value->type->function.param_count !=
                    make_adder_function->return_type->function.param_count &&
                !xr_type_equals(value->type, make_adder_function->return_type)) {
                conflicting_callable_return = value;
                break;
            }
        }
    }
    PIPELINE_TEST_REQUIRE(conflicting_callable_return != NULL);
    PIPELINE_TEST_REQUIRE(unreachable_callable_return_preserves_program(
        make_adder_function, conflicting_callable_return, &producer_input, &artifact));

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

    XiValue *aggregate_update = NULL;
    for (uint32_t block = 0; block < update_function->nblocks; ++block) {
        XiBlock *row = update_function->blocks[block];
        for (uint32_t value = 0; row && value < row->nvalues; ++value) {
            XiValue *operation = row->values[value];
            if (operation->op == XI_AGG_UPDATE)
                aggregate_update = operation;
        }
    }
    PIPELINE_TEST_REQUIRE(aggregate_update != NULL);

    int64_t saved_update_ordinal = aggregate_update->aux_int;
    aggregate_update->aux_int = INT64_C(99);
    XrProgramArtifact invalid_update_artifact = {0};
    PIPELINE_TEST_REQUIRE(xr_program_write_from_xi(
                              &producer_input, &invalid_update_artifact, producer_diagnostic,
                              sizeof(producer_diagnostic)) == XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE);
    PIPELINE_TEST_REQUIRE(invalid_update_artifact.bytes == NULL);
    aggregate_update->aux_int = saved_update_ordinal;

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
    bool found_point_aggregate = false;
    bool found_capture_aggregate = false;
    bool found_effectful_callable = false;
    bool found_fallible_callable = false;
    for (uint32_t type_index = 0; type_index < validated->type_count; ++type_index) {
        const XrValidatedType *variant = &validated->types[type_index];
        if (variant->kind == XR_CORE_IR_TYPE_CALLABLE &&
            variant->signature_id < validated->signature_count &&
            validated->signatures[variant->signature_id].effect_mask == XR_CORE_EFFECT_TRAP)
            found_effectful_callable = true;
        if (variant->kind == XR_CORE_IR_TYPE_CALLABLE &&
            variant->signature_id < validated->signature_count &&
            validated->signatures[variant->signature_id].error_type_id != XR_CORE_TYPE_VOID &&
            (validated->signatures[variant->signature_id].effect_mask & XR_CORE_EFFECT_ERROR) != 0u)
            found_fallible_callable = true;
        if (variant->kind == XR_CORE_IR_TYPE_AGGREGATE && variant->field_count == 2u &&
            variant->field_types && variant->field_types[0] == XR_CORE_TYPE_I64 &&
            variant->field_types[1] == XR_CORE_TYPE_I64)
            found_point_aggregate = true;
        if (variant->kind == XR_CORE_IR_TYPE_AGGREGATE && variant->field_count == 1u &&
            variant->field_types && variant->field_types[0] == XR_CORE_TYPE_I64 &&
            variant->ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE &&
            variant->copy_contract == XR_CORE_IR_COPY_EXPLICIT)
            found_capture_aggregate = true;
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
    PIPELINE_TEST_REQUIRE(found_point_aggregate);
    PIPELINE_TEST_REQUIRE(found_capture_aggregate);
    PIPELINE_TEST_REQUIRE(found_effectful_callable);
    PIPELINE_TEST_REQUIRE(found_fallible_callable);
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
        XR_CORE_OP_CORE_CALL_SEALED_INVOKE,
        XR_CORE_OP_CORE_ERROR_PUBLISH,
        XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
        XR_CORE_OP_CORE_AGGREGATE_PROJECT,
        XR_CORE_OP_CORE_AGGREGATE_UPDATE,
        XR_CORE_OP_CORE_VARIANT_CONSTRUCT,
        XR_CORE_OP_CORE_VARIANT_TEST,
        XR_CORE_OP_CORE_VARIANT_PROJECT,
        XR_CORE_OP_CORE_OWNER_COPY,
        XR_CORE_OP_CORE_PLACE_LOCAL,
        XR_CORE_OP_CORE_PLACE_LOAD,
        XR_CORE_OP_CORE_PLACE_STORE,
        XR_CORE_OP_CORE_CALLABLE_PACK,
        XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT,
        XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE,
        XR_CORE_OP_CORE_OWNER_DROP,
        XR_CORE_OP_CORE_EXISTENTIAL_PACK,
        XR_CORE_OP_CORE_EXISTENTIAL_TEST,
        XR_CORE_OP_CORE_EXISTENTIAL_PROJECT,
        XR_CORE_OP_CORE_CALL_WITNESS_DIRECT,
        XR_CORE_OP_CORE_CALL_WITNESS_INVOKE,
    };
    for (size_t index = 0;
         index < sizeof(required_source_operations) / sizeof(required_source_operations[0]);
         ++index) {
        if (!validated_program_has_operation(validated, required_source_operations[index]))
            fprintf(stderr, "canonical source fixture omitted CoreSpec operation %u\n",
                    required_source_operations[index]);
        PIPELINE_TEST_REQUIRE(
            validated_program_has_operation(validated, required_source_operations[index]));
    }
    PIPELINE_TEST_REQUIRE(
        validated_program_operation_count(validated, XR_CORE_OP_CORE_CALLABLE_PACK) >= 7u);
    PIPELINE_TEST_REQUIRE(validated_program_operation_count(
                              validated, XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT) >= 11u);
    PIPELINE_TEST_REQUIRE(
        validated_program_operation_count(validated, XR_CORE_OP_CORE_OWNER_COPY) >= 2u);
    PIPELINE_TEST_REQUIRE(validated_program_has_owned_storage_copy_pack(validated));
    PIPELINE_TEST_REQUIRE(
        validated_program_operation_count(validated, XR_CORE_OP_CORE_OWNER_DROP) >= 6u);
    XrReferenceOutcome reference = xr_reference_evaluate(
        validated, xr_validated_program_entry_function(validated), NULL, 0u, NULL, NULL);
    PIPELINE_TEST_REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
    PIPELINE_TEST_REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_I64);
    PIPELINE_TEST_REQUIRE(reference.value.as.i64 == 477);

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
    PIPELINE_TEST_REQUIRE(strstr(generated.bytes, "out_error") != NULL);
    PIPELINE_TEST_REQUIRE(strstr(generated.bytes, "xr_aot_alloc") != NULL);
    PIPELINE_TEST_REQUIRE(strstr(generated.bytes, "XrProto") == NULL);
    PIPELINE_TEST_REQUIRE(strstr(generated.bytes, "SemanticPlan") == NULL);
    if (g_source_aot_output_path) {
        FILE *generated_file = fopen(g_source_aot_output_path, "wb");
        PIPELINE_TEST_REQUIRE(generated_file != NULL);
        PIPELINE_TEST_REQUIRE(fwrite(generated.bytes, 1u, generated.size, generated_file) ==
                              generated.size);
        PIPELINE_TEST_REQUIRE(fclose(generated_file) == 0);
    }
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

    XiPipelineScalarFixture witness_only_fixture = {0};
    static const char witness_only_source[] =
        "interface WitnessOnly { read() -> i64 }\n"
        "class WitnessOnlyClass implements WitnessOnly { read() -> i64 { return 7 } }\n"
        "fn dispatch_only(value: WitnessOnly) -> i64 { return value.read() }\n"
        "fn root() -> i64 { return 42 }\n";
    PIPELINE_TEST_REQUIRE(xi_pipeline_fixture_analyze_source(
        &witness_only_fixture, session, "xi-witness-only-program", witness_only_source));
    XgGlobalEvidence witness_only_evidence = {0};
    PIPELINE_TEST_REQUIRE(xi_pipeline_fixture_build_global_evidence(
        &witness_only_fixture, session, &witness_only_evidence));
    XiPipelineConfig witness_only_config = xi_pipeline_program_input_config();
    witness_only_config.run_canonicalize = false;
    witness_only_config.source_file = "scalar-binding.xr";
    witness_only_config.module_name = "witness_only";
    witness_only_config.module_identity = witness_only_fixture.spec->canonical;
    witness_only_config.global_evidence = &witness_only_evidence;
    witness_only_config.global_evidence_module_id = 1u;
    XiPipelineResult witness_only_result = xi_pipeline_compile_program(
        witness_only_fixture.spec->ast, witness_only_fixture.analyzer, g_iso,
        &witness_only_config);
    if (witness_only_result.status != XI_PIPE_OK)
        fprintf(stderr, "witness-only Xi failed at %s: %s\n",
                xi_pipeline_stage_str(witness_only_result.error.stage),
                witness_only_result.error.detail);
    PIPELINE_TEST_REQUIRE(witness_only_result.status == XI_PIPE_OK);
    PIPELINE_TEST_REQUIRE(witness_only_result.ir != NULL);
    const XiFunc *witness_only_entry = NULL;
    bool witness_only_has_call = false;
    bool witness_only_has_pack = false;
    for (uint16_t function_index = 0u;
         function_index < witness_only_result.ir->module->nfuncs; ++function_index) {
        XiFunc *candidate = witness_only_result.ir->module->functions[function_index];
        if (candidate && candidate->name && strcmp(candidate->name, "root") == 0)
            witness_only_entry = candidate;
        witness_only_has_call |=
            xi_function_has_operation(candidate, XI_CALL_METHOD) ||
            xi_function_has_operation(candidate, XI_CALL_METHOD_DIRECT);
        for (uint32_t block_index = 0u; candidate && block_index < candidate->nblocks;
             ++block_index) {
            XiBlock *candidate_block = candidate->blocks[block_index];
            for (uint32_t value_index = 0u;
                 candidate_block && value_index < candidate_block->nvalues; ++value_index) {
                XiValue *candidate_value = candidate_block->values[value_index];
                witness_only_has_pack |= candidate_value &&
                                         candidate_value->xg_existential_kind ==
                                             XI_EXISTENTIAL_PACK;
            }
        }
    }
    PIPELINE_TEST_REQUIRE(witness_only_entry != NULL);
    PIPELINE_TEST_REQUIRE(witness_only_has_call);
    PIPELINE_TEST_REQUIRE(!witness_only_has_pack);
    const XiFunc *witness_only_roots[] = {witness_only_result.ir};
    XrProgramFromXiInput witness_only_input = {
        .module_roots = witness_only_roots,
        .module_count = 1u,
        .entry_function = witness_only_entry,
        .global_evidence = &witness_only_evidence,
        .semantic_profile_fingerprint = semantic_profile.bytes,
    };
    XrProgramArtifact witness_only_artifact = {0};
    PIPELINE_TEST_REQUIRE(
        xr_program_write_from_xi(&witness_only_input, &witness_only_artifact,
                                 producer_diagnostic, sizeof(producer_diagnostic)) ==
        XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *witness_only_validated = NULL;
    PIPELINE_TEST_REQUIRE(
        xr_program_validate(witness_only_artifact.bytes, witness_only_artifact.size, NULL,
                            &witness_only_validated, &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    PIPELINE_TEST_REQUIRE(witness_only_validated != NULL);
    PIPELINE_TEST_REQUIRE(witness_only_validated->interface_count == 1u);
    PIPELINE_TEST_REQUIRE(witness_only_validated->conformance_count == 1u);
    PIPELINE_TEST_REQUIRE(validated_program_has_operation(
        witness_only_validated, XR_CORE_OP_CORE_CALL_WITNESS_DIRECT));
    PIPELINE_TEST_REQUIRE(!validated_program_has_operation(
        witness_only_validated, XR_CORE_OP_CORE_EXISTENTIAL_PACK));
    xr_validated_program_free(witness_only_validated);
    xr_program_artifact_free(&witness_only_artifact);
    xi_pipeline_result_free(&witness_only_result);
    xg_global_evidence_free(&witness_only_evidence);
    xi_pipeline_scalar_fixture_cleanup(&witness_only_fixture);

    xi_pipeline_result_free(&result);
    xg_global_evidence_free(&global_evidence);
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

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--source-aot-c") == 0)
        g_source_aot_output_path = argv[2];
    else if (argc != 1)
        return 2;
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
    run_e2e_program_move_direct_signatures_are_published_before_bodies();
    run_e2e_program_typed_error_cleanup_trampoline_reuses_error_live_in();
    run_e2e_program_witness_move_operands_are_consumed_once();
    run_e2e_program_input_stops_before_legacy_semantic_and_backend_owners();
    run_e2e_time_sleep_uses_dedicated_vm_suspend();
    run_e2e_generic_this_method_call_uses_frozen_member_identity();

    teardown();

    printf("\n=== %d/%d Xi Pipeline tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
