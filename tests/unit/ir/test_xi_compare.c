/*
 * test_xi_compare.c - Production compiler wrapper vs direct Xi pipeline
 *
 * Compiles the same source through both paths and compares:
 *   1. Both succeed (no crash, no error)
 *   2. Instruction sequence similarity (opcode histogram)
 *   3. Key metrics (maxstacksize, constant pool size, arity)
 *   4. Execution produces the same result (via VM)
 */

#include "../../../src/frontend/analyzer/xanalyzer_ast_visitor.h"
#include "../../../src/frontend/analyzer/xa_node_table.h"
#include "../../../src/vm/xvm_dispatch_helpers.h"
#include "../../../src/runtime/class/xclass_lookup.h"
#include "../../../src/runtime/class/xinstance.h"
#include "../test_helper.h"
#include "../../../src/ir/xi_pipeline.h"
#include "../../../src/frontend/codegen/xcompiler.h"
#include "../../../src/frontend/codegen/xcompiler_context.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../include/xray_vm.h"

#include "../../../src/analysis/xglobal_producer.h"
#include "../../../src/module/xmodule_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../../../src/vm/xvm_internal.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../src/coro/xcoroutine.h"
#include "../../../src/coro/xworker.h"
#include "../../../src/runtime/closure/xclosure.h"

/* ========== Test Infrastructure ========== */

static XrVMRuntime *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

/* Set by CHECK/REQUIRE when the running test violates an expectation;
 * cleared before each test by the TEST macro. */
static bool g_test_failed = false;

/* Release builds define NDEBUG, which turns assert() into a no-op.  Every
 * expectation in this file must therefore be checked explicitly: an
 * assert-guarded NULL proto is not a guard at all in Release, and the
 * comparison helpers below go on to dereference it and die on a signal
 * instead of reporting which snippet failed to compile.
 *
 * CHECK records the failure and keeps going, so one snippet can report
 * every way it diverged.  REQUIRE also returns, for expectations whose
 * failure makes the rest of the test meaningless (typically a NULL proto);
 * callers must release anything they own before it can fire. */
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: ");                                                           \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            g_test_failed = true;                                                                  \
        }                                                                                          \
    } while (0)

#define REQUIRE(cond, ...)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: ");                                                           \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            g_test_failed = true;                                                                  \
            return;                                                                                \
        }                                                                                          \
    } while (0)

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
        XrVMConfig p = {0};
        g_iso = xray_vm_new_full(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
    }
    /* Free deferred protos after isolate (and its GC heap) is gone */
    for (int i = 0; i < g_deferred_count; i++)
        xr_instruction_unit_free(g_deferred_protos[i]);
    g_deferred_count = 0;
}

/* ========== Compilation Helpers ========== */

static bool compare_operation_begin(XrCompilerSession *session,
                                    XrCompilerSessionOperationScope *scope) {
    const XrCompileUnitIdentity identity = {
        .kind = XR_COMPILE_UNIT_MEMORY,
        .module_identity = "memory-module-v1:id=21:xi-compare-fixture-v1",
    };
    if (!xr_compiler_session_operation_begin(session, scope))
        return false;
    if (xr_compiler_session_set_compile_unit_identity(session, &identity))
        return true;
    (void) xr_compiler_session_operation_fail(scope, XR_COMPILER_SESSION_OPERATION_FATAL);
    return false;
}

static XrProto *compare_operation_finish(XrCompilerSessionOperationScope *scope, XrProto *proto) {
    if (!proto) {
        (void) xr_compiler_session_operation_fail(scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return NULL;
    }
    if (xr_compiler_session_operation_succeed(scope))
        return proto;
    xr_instruction_unit_free(proto);
    return NULL;
}

/* Compile through the production compiler wrapper. */
static XrProto *compile_wrapper(const char *source) {
    XR_DCHECK(g_iso != NULL, "isolate must be initialized");

    /* Create context first — its analyzer installs the current type pool
     * that the parser needs for creating type annotations. */
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    XrCompilerSessionOperationScope operation;
    if (!compare_operation_begin(session, &operation))
        return NULL;
    XrCompilerContext *ctx = xr_compiler_context_new(session);
    if (!ctx)
        return compare_operation_finish(&operation, NULL);
    /* xr_compile passes ctx->source_file to the analyzer as the file being
     * analyzed, and file identity selects the nominal owner that declaration
     * analysis records for enums.  Leaving it NULL makes the wrapper path
     * analyze an unnamed file while the Xi path below names it, so the two
     * paths disagree about enum ownership.  Name both the same. */
    ctx->source_file = "compare.xr";
    ctx->source_content = source;
    /* Mirror the production compile path: bind the session module graph so
     * declaration analysis (e.g. enums) resolves the same way as the CLI. */
    xa_analyzer_set_graph(ctx->analyzer, xr_compiler_session_module_graph(session));

    AstNode *program = xr_parse_with_source(session, source, ctx->source_file);
    if (!program) {
        xr_compiler_context_free(ctx);
        return compare_operation_finish(&operation, NULL);
    }

    /* Re-enter the parse arena: the production wrapper desugars some AST nodes
     * (e.g. for-in, match) which calls ast_alloc and needs the arena. */
    XrCompilerSessionScope ast_scope;
    bool has_ast_scope = program->type == AST_PROGRAM && program->as.program.arena &&
                         xr_compiler_session_push_arena(session, program->as.program.arena,
                                                        "compare.xr", &ast_scope);

    XrProto *proto = xr_compile(ctx, program);

    if (has_ast_scope)
        xr_compiler_session_pop_arena(&ast_scope);
    xr_compiler_context_free(ctx);
    xr_program_destroy(program);
    return compare_operation_finish(&operation, proto);
}


/* Compile via Xi IR pipeline */
static XrProto *compile_xi(const char *source) {
    XR_DCHECK(g_iso != NULL, "isolate must be initialized");

    /* Create analyzer first — it installs the current type pool for this
     * compiler session before parsing type annotations. */
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    XrCompilerSessionOperationScope operation;
    if (!compare_operation_begin(session, &operation))
        return NULL;
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer)
        return compare_operation_finish(&operation, NULL);
    /* Mirror the production compile path: bind the session module graph so
     * declaration analysis (e.g. enums) resolves the same way as the CLI. */
    xa_analyzer_set_graph(analyzer, xr_compiler_session_module_graph(session));

    AstNode *program = xr_parse_with_source(session, source, "compare.xr");
    if (!program) {
        xa_analyzer_free(analyzer);
        return compare_operation_finish(&operation, NULL);
    }

    xa_analyzer_analyze(analyzer, "compare.xr", program);

    XgGlobalEvidence evidence = {0};
    int topo_index = 0;
    XrModuleSpec spec = {0};
    XrModuleGraph graph = {0};
    spec.canonical = "memory-module-v1:id=21:xi-compare-fixture-v1";
    spec.source_path = "compare.xr";
    spec.kind = XR_MOD_MEMORY;
    spec.authority.kind = XR_MODULE_IDENTITY_MEMORY;
    spec.ast = program;
    xr_module_source_fingerprint(source, &spec.source_content_fingerprint);
    graph.specs = &spec;
    graph.spec_count = graph.spec_capacity = 1;
    graph.topo_order = &topo_index;
    graph.topo_count = 1;
    graph.entry_index = 0;
    if (!xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
        &evidence, &graph, XG_BUILD_DEV, 0, NULL, 0, analyzer)) {
        fprintf(stderr, "[compare diagnostic] direct graph evidence rejected exact fixture\n");
        xg_global_evidence_free(&evidence);
        xa_analyzer_free(analyzer);
        xr_program_destroy(program);
        return compare_operation_finish(&operation, NULL);
    }

    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.module_identity = "memory-module-v1:id=21:xi-compare-fixture-v1";
    cfg.source_file = "compare.xr";
    cfg.global_evidence = &evidence;
    cfg.global_evidence_module_id = 1;
    XiPipelineResult res = xi_pipeline_compile_program(program, analyzer, g_iso, &cfg);

    xg_global_evidence_free(&evidence);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    if (res.status != XI_PIPE_OK) {
        fprintf(stderr, "[compare diagnostic] status=%u stage=%u detail=%s\n",
            (unsigned) res.status, (unsigned) res.error.stage, res.error.detail);
        xi_pipeline_result_free(&res);
        return compare_operation_finish(&operation, NULL);
    }

    XrProto *proto = res.proto;
    xi_pipeline_result_free(&res);
    return compare_operation_finish(&operation, proto);
}

/* ========== Execution Capture ========== */

/* Capturing stdout used to go through:
 *   freopen("/tmp/xi_cmp_capture.txt", "w", stdout) ... freopen("/dev/tty", ...)
 * which silently breaks on Windows: there is no /tmp by default, and
 * /dev/tty does not exist at all. Worse, when freopen fails it has
 * already closed the original stdout, leaving every subsequent printf
 * writing into a corrupted CRT FILE* -- which surfaces as a /GS stack
 * cookie failure (STATUS_STACK_BUFFER_OVERRUN, 0xC0000409) the next
 * time the runtime walks an internal stdio buffer.
 *
 * The portable approach is dup() + tmpfile() + dup2() -> save the
 * original stdout fd, point stdout at an anonymous temp file, run the
 * payload, then restore. tmpfile() handles platform tempdir lookup
 * and auto-deletes on fclose. */
#ifdef _WIN32
#if defined(_MSC_VER)
#include <corecrt_io.h>
#else
#include <io.h>
#endif
#define xi_cmp_dup _dup
#define xi_cmp_dup2 _dup2
#define xi_cmp_close _close
#define xi_cmp_fileno _fileno
#else
#include <unistd.h>
#define xi_cmp_dup dup
#define xi_cmp_dup2 dup2
#define xi_cmp_close close
#define xi_cmp_fileno fileno
#endif

/* Execute proto and capture stdout into a heap buffer.
 *
 * This must go through xr_execute -- the same entry point xr_isolate_dostring
 * uses -- rather than driving the coroutine by hand.  Hand-rolling
 * "closure_new + reset_for_call + main_thread_run" skips four preconditions
 * that xr_execute establishes, and every one of them is silent when missed:
 *
 *   - xr_vm_bind_proto_shared_slots: module-level `var`s live in shared
 *     slots, so SETSHARED/GETSHARED read garbage without it.
 *   - the entry plan: a root with XR_ROOT_ELIDED runs on the native stack
 *     directly via xr_vm_interpret_proto and never touches a coroutine.
 *     Snippets this small are exactly the elided case, so pushing them
 *     through the scheduler path ran nothing at all.
 *   - xr_isolate_multicore_init: xr_main_thread_run needs isolate->vm.scheduler,
 *     which xray_vm_new_full leaves NULL.
 *   - the main coroutine: xray_vm_new_full also leaves isolate->main_coro
 *     NULL (it is created on demand), so xr_isolate_get_main_coro returns
 *     NULL on a fresh isolate.
 *
 * That last one made execute_and_capture bail out before running anything,
 * which is why every comparison logged "capture failed" while the test
 * still reported PASS.  Bootstrapping the coroutine alone is not enough:
 * it only moves the silent failure from "captured nothing" to "captured an
 * empty string from a VM that never ran".  Delegating to xr_execute keeps
 * all four in step with the production path.
 *
 * *out_rc receives the xr_execute return code (0 = success) so callers can
 * assert that both pipelines agree on success, not merely on output text. */
static char *execute_and_capture(XrProto *proto, int *out_rc) {
    if (out_rc)
        *out_rc = -1;
    if (!proto || !g_iso)
        return NULL;

    /* xr_compile leaves entry_plan zeroed; only xr_compile_source_with_path
     * derives it afterwards, and neither compile helper here goes through
     * that wrapper.  Without this, xr_execute rejects every proto with
     * "bytecode has no verified entry plan" before running a single
     * instruction.  Deriving per path is also the honest comparison: the
     * plan is scanned out of the emitted opcodes, so the wrapper and direct Xi paths each get
     * the plan their own bytecode earns. */
    if (!xr_entry_plan_derive(proto))
        return NULL;

    fflush(stdout);

    int saved_fd = xi_cmp_dup(xi_cmp_fileno(stdout));
    if (saved_fd < 0)
        return NULL;

    FILE *tmp = tmpfile();
    if (!tmp) {
        xi_cmp_close(saved_fd);
        return NULL;
    }

    if (xi_cmp_dup2(xi_cmp_fileno(tmp), xi_cmp_fileno(stdout)) < 0) {
        fclose(tmp);
        xi_cmp_close(saved_fd);
        return NULL;
    }

    int rc = xr_execute(g_iso, proto);
    if (out_rc)
        *out_rc = rc;
    fflush(stdout);

    /* Restore stdout before reading -- so any error path below can
     * still write diagnostics through the real terminal. */
    xi_cmp_dup2(saved_fd, xi_cmp_fileno(stdout));
    xi_cmp_close(saved_fd);

    rewind(tmp);
    char *buf = (char *) xr_malloc(4096);
    if (!buf) {
        fclose(tmp);
        return NULL;
    }
    size_t n = fread(buf, 1, 4095, tmp);
    buf[n] = '\0';
    fclose(tmp); /* tmpfile() deletes on close */
    return buf;
}

/* ========== Comparison Utilities ========== */

/* Build opcode histogram for a proto */
static void build_histogram(const XrProto *proto, int hist[256]) {
    memset(hist, 0, 256 * sizeof(int));
    int count = PROTO_CODE_COUNT(proto);
    for (int i = 0; i < count; i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, i));
        if (op < 256)
            hist[op]++;
    }
}

/* Print opcode sequence for debugging */
static void dump_opcodes(const char *label, const XrProto *proto) {
    int count = PROTO_CODE_COUNT(proto);
    fprintf(stderr, "  %s (%d insts, maxstack=%d):", label, count, proto->maxstacksize);
    for (int i = 0; i < count && i < 30; i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, i));
        fprintf(stderr, " %s", xr_opcode_name(op));
    }
    if (count > 30)
        fprintf(stderr, " ...(+%d)", count - 30);
    fprintf(stderr, "\n");
}

/* Compare two protos, return similarity score 0.0-1.0 */
static double compare_protos(const XrProto *wrapper, const XrProto *xi, bool verbose) {
    int hist_l[256], hist_x[256];
    build_histogram(wrapper, hist_l);
    build_histogram(xi, hist_x);

    /* Jaccard-like similarity on opcode counts */
    int intersection = 0, union_total = 0;
    for (int i = 0; i < 256; i++) {
        int a = hist_l[i], b = hist_x[i];
        if (a == 0 && b == 0)
            continue;
        int min = a < b ? a : b;
        int max = a > b ? a : b;
        intersection += min;
        union_total += max;
    }

    double similarity = union_total > 0 ? (double) intersection / union_total : 1.0;

    if (verbose) {
        dump_opcodes("wrapper", wrapper);
        dump_opcodes("xi    ", xi);
        fprintf(stderr,
                "  similarity=%.2f  wrapper_insts=%d  xi_insts=%d"
                "  wrapper_stack=%d  xi_stack=%d\n",
                similarity, PROTO_CODE_COUNT(wrapper), PROTO_CODE_COUNT(xi), wrapper->maxstacksize,
                xi->maxstacksize);

        /* Show opcode differences */
        for (int i = 0; i < 256; i++) {
            if (hist_l[i] != hist_x[i] && (hist_l[i] > 0 || hist_x[i] > 0)) {
                fprintf(stderr, "    %-20s wrapper=%d  xi=%d\n", xr_opcode_name((OpCode) i),
                        hist_l[i], hist_x[i]);
            }
        }
    }

    return similarity;
}

/* ========== Test Macro ========== */

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        /* Each compare snippet is self-contained; recreate the isolate so a                       \
         * fresh compiler session/type pool is used per test (the runtime/session                  \
         * split makes accumulated cross-snippet state unsafe to share). */                        \
        teardown();                                                                                \
        setup();                                                                                   \
        g_test_failed = false;                                                                     \
        test_##name();                                                                             \
        if (g_test_failed) {                                                                       \
            printf("  FAIL\n");                                                                    \
            tests_failed++;                                                                        \
        } else {                                                                                   \
            printf("  PASS\n");                                                                    \
            tests_passed++;                                                                        \
        }                                                                                          \
    }                                                                                              \
    static void test_##name(void)

/* ========== Dual-path comparison helper ========== */

typedef struct {
    const char *source;
    const char *label;
    bool expect_xi_success;      /* true = Xi pipeline must succeed */
    double min_similarity;       /* minimum opcode histogram similarity */
    bool check_exec;             /* compare VM execution output */
    bool expect_runtime_failure; /* both compiled programs must fail execution */
    const char *expected_stdout; /* independent result when supplied */
} CompareSpec;

static void run_compare(CompareSpec spec) {
    XrProto *p_wrapper = compile_wrapper(spec.source);
    REQUIRE(p_wrapper != NULL, "compiler wrapper returned NULL for '%s'", spec.label);
    REQUIRE(spec.expect_xi_success, "comparison fallback is forbidden for '%s'", spec.label);

    XrProto *p_xi = compile_xi(spec.source);
    if (p_xi == NULL) {
        xr_instruction_unit_free(p_wrapper);
        REQUIRE(false, "Xi pipeline returned NULL for '%s'", spec.label);
    }

    /* Both succeeded — compare */
    double sim = compare_protos(p_wrapper, p_xi, true);
    fprintf(stderr, "  → similarity = %.2f (min=%.2f)\n", sim, spec.min_similarity);

    if (sim < spec.min_similarity) {
        fprintf(stderr, "  ⚠ similarity below threshold!\n");
    }

    /* Both must produce at least one instruction and contain a RETURN */
    int lc = PROTO_CODE_COUNT(p_wrapper);
    int xc = PROTO_CODE_COUNT(p_xi);
    CHECK(lc > 0, "compiler wrapper produced no instructions for '%s'", spec.label);
    CHECK(xc > 0, "xi produced no instructions for '%s'", spec.label);

    bool wrapper_has_ret = false, xi_has_ret = false;
    for (int i = 0; i < lc; i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(p_wrapper, i));
        if (op == OP_RETURN || op == OP_RETURN0 || op == OP_RETURN1)
            wrapper_has_ret = true;
    }
    for (int i = 0; i < xc; i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(p_xi, i));
        if (op == OP_RETURN || op == OP_RETURN0 || op == OP_RETURN1)
            xi_has_ret = true;
    }
    CHECK(wrapper_has_ret, "compiler wrapper contains no RETURN for '%s'", spec.label);
    CHECK(xi_has_ret, "xi contains no RETURN for '%s'", spec.label);

    /* Execution output comparison */
    if (spec.check_exec) {
        int rc_l = -1, rc_x = -1;
        char *out_l = execute_and_capture(p_wrapper, &rc_l);
        char *out_x = execute_and_capture(p_xi, &rc_x);

        /* A failed capture used to log "skipped" and fall through to PASS,
         * which is how this comparison stayed dead for so long.  Treat it as
         * the failure it is. */
        CHECK(out_l && out_x, "execution capture failed for '%s' (wrapper=%s xi=%s)", spec.label,
              out_l ? "ok" : "fail", out_x ? "ok" : "fail");

        if (out_l && out_x) {
            if (spec.expected_stdout) {
                CHECK(strcmp(out_l, spec.expected_stdout) == 0,
                      "wrapper stdout differs from independent expectation for '%s'", spec.label);
                CHECK(strcmp(out_x, spec.expected_stdout) == 0,
                      "xi stdout differs from independent expectation for '%s'", spec.label);
            }
            bool match = (strcmp(out_l, out_x) == 0);
            fprintf(stderr, "  exec: rc=%d/%d wrapper=[%s] xi=[%s] %s\n", rc_l, rc_x, out_l, out_x,
                    match ? "MATCH" : "MISMATCH");
            CHECK(match, "execution output differs for '%s': wrapper=[%s] xi=[%s]", spec.label,
                  out_l, out_x);
        }

        CHECK(rc_l == rc_x, "execution status differs for '%s': wrapper=%d xi=%d", spec.label, rc_l,
              rc_x);
        if (spec.expect_runtime_failure) {
            CHECK(rc_l != 0, "compiler wrapper execution unexpectedly succeeded for '%s'",
                  spec.label);
            CHECK(rc_x != 0, "xi execution unexpectedly succeeded for '%s'", spec.label);
        } else {
            /* Equal output is only meaningful if the code actually ran: two
             * failed runs both produce "".  Require success from both paths,
             * so a regression that stops execution cannot masquerade as a
             * match. */
            CHECK(rc_l == 0, "compiler wrapper execution failed (rc=%d) for '%s'", rc_l,
                  spec.label);
            CHECK(rc_x == 0, "xi execution failed (rc=%d) for '%s'", rc_x, spec.label);
        }

        if (out_l)
            xr_free(out_l);
        if (out_x)
            xr_free(out_x);
    }

    if (spec.check_exec) {
        /* GC-managed closures created by xr_execute still reference these
         * protos; freeing now would be a use-after-free on the next GC scan.
         * Defer until after isolate teardown destroys the GC heap. */
        defer_proto_free(p_wrapper);
        defer_proto_free(p_xi);
    } else {
        xr_instruction_unit_free(p_wrapper);
        xr_instruction_unit_free(p_xi);
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

static int proto_opcode_count(const XrProto *proto, OpCode target) {
    int total = 0;
    int count = PROTO_CODE_COUNT(proto);
    for (int i = 0; i < count; i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == target)
            total++;
    }
    int nchildren = DYNARRAY_COUNT(&proto->protos);
    for (int i = 0; i < nchildren; i++) {
        XrProto *child = DYNARRAY_GET(&proto->protos, i, XrProto *);
        total += proto_opcode_count(child, target);
    }
    return total;
}

/* Compile via Xi and assert a specific fused opcode is present.
 * Also runs execution comparison if check_exec is set. */
typedef struct {
    const char *source;
    const char *label;
    OpCode expect_op; /* opcode that must appear in Xi output */
    bool check_exec;
} FusionSpec;

static void run_fusion(FusionSpec spec) {
    XrProto *p_wrapper = compile_wrapper(spec.source);
    REQUIRE(p_wrapper != NULL, "compiler wrapper returned NULL for '%s'", spec.label);

    XrProto *p_xi = compile_xi(spec.source);
    if (p_xi == NULL) {
        xr_instruction_unit_free(p_wrapper);
        REQUIRE(false, "Xi pipeline returned NULL for '%s'", spec.label);
    }

    bool has_op = proto_has_opcode(p_xi, spec.expect_op);
    fprintf(stderr, "  xi has %s: %s\n", xr_opcode_name(spec.expect_op), has_op ? "yes" : "NO");
    CHECK(has_op, "expected fused opcode %s not found in Xi output for '%s'",
          xr_opcode_name(spec.expect_op), spec.label);

    if (spec.check_exec) {
        int rc_l = -1, rc_x = -1;
        char *out_l = execute_and_capture(p_wrapper, &rc_l);
        char *out_x = execute_and_capture(p_xi, &rc_x);
        CHECK(out_l && out_x, "execution capture failed for '%s' (wrapper=%s xi=%s)", spec.label,
              out_l ? "ok" : "fail", out_x ? "ok" : "fail");
        if (out_l && out_x) {
            bool match = (strcmp(out_l, out_x) == 0);
            fprintf(stderr, "  exec: rc=%d/%d wrapper=[%s] xi=[%s] %s\n", rc_l, rc_x, out_l, out_x,
                    match ? "MATCH" : "MISMATCH");
            CHECK(match, "execution output differs for '%s': wrapper=[%s] xi=[%s]", spec.label,
                  out_l, out_x);
        }
        CHECK(rc_l == 0, "compiler wrapper execution failed (rc=%d) for '%s'", rc_l, spec.label);
        CHECK(rc_x == 0, "xi execution failed (rc=%d) for '%s'", rc_x, spec.label);
        if (out_l)
            xr_free(out_l);
        if (out_x)
            xr_free(out_x);
    }

    if (spec.check_exec) {
        defer_proto_free(p_wrapper);
        defer_proto_free(p_xi);
    } else {
        xr_instruction_unit_free(p_wrapper);
        xr_instruction_unit_free(p_xi);
    }
}

/* ========== Test Cases ========== */

/* --- Constants --- */

TEST(cmp_int_const) {
    run_compare((CompareSpec) {
        .source = "var x = 42\nprint(x)",
        .label = "i64 constant",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_float_const) {
    run_compare((CompareSpec) {
        .source = "var x = 3.14\nprint(x)",
        .label = "f64 constant",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_bool_const) {
    run_compare((CompareSpec) {
        .source = "var a = true\nvar b = false\nprint(a)\nprint(b)",
        .label = "bool constants",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_string_const) {
    run_compare((CompareSpec) {
        .source = "var s = \"hello\"\nprint(s)",
        .label = "string constant",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_null_const) {
    run_compare((CompareSpec) {
        .source = "var x = null\nprint(x)",
        .label = "null constant",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Arithmetic --- */

TEST(cmp_add) {
    run_compare((CompareSpec) {
        .source = "var a = 10\nvar b = 20\nvar c = a + b\nprint(c)",
        .label = "addition",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_arith_chain) {
    run_compare((CompareSpec) {
        .source = "var x = 1 + 2 * 3 - 4\nprint(x)",
        .label = "arithmetic chain (const folded)",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_unary_neg) {
    run_compare((CompareSpec) {
        .source = "var x = 10\nvar y = -x\nprint(y)",
        .label = "unary negation",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Control Flow --- */

TEST(cmp_if_else) {
    run_compare((CompareSpec) {
        .source = "var x = 5\nif (x > 3) { print(1) } else { print(0) }",
        .label = "if-else",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_if_const_true) {
    run_compare((CompareSpec) {
        .source = "if (true) { print(1) } else { print(2) }",
        .label = "if with const true (branch elimination)",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_while_loop) {
    run_compare((CompareSpec) {
        .source = "var i = 0\nwhile (i < 5) { i = i + 1 }\nprint(i)",
        .label = "while loop",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Multiple Statements --- */

TEST(cmp_multi_print) {
    run_compare((CompareSpec) {
        .source = "print(1)\nprint(2)\nprint(3)",
        .label = "multiple prints",
        .expect_xi_success = true,
        .min_similarity = 0.5,
        .check_exec = true,
    });
}

TEST(cmp_multi_vars) {
    run_compare((CompareSpec) {
        .source = "var a = 1\nvar b = 2\nvar c = 3\n"
                  "var d = a + b + c\nprint(d)",
        .label = "multiple variables + sum",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Variable Reassignment --- */

TEST(cmp_var_reassign) {
    run_compare((CompareSpec) {
        .source = "var x = 10\nx = x + 5\nx = x * 2\nprint(x)",
        .label = "variable reassignment chain",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Comparison Operators --- */

TEST(cmp_comparisons) {
    run_compare((CompareSpec) {
        .source = "var a = 5\nvar b = 10\n"
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
    run_compare((CompareSpec) {
        .source = "var a = true\nvar b = false\n"
                  "print(a && b)\nprint(a && true)",
        .label = "logical AND (short-circuit)",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_logical_or) {
    run_compare((CompareSpec) {
        .source = "var a = false\nvar b = true\n"
                  "print(a || b)\nprint(false || false)",
        .label = "logical OR (short-circuit)",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_logical_not) {
    run_compare((CompareSpec) {
        .source = "var a = true\nprint(!a)\nprint(!false)",
        .label = "logical NOT",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- For Loop --- */

TEST(cmp_for_loop) {
    run_compare((CompareSpec) {
        .source = "var sum = 0\n"
                  "for (var i = 1; i <= 5; i = i + 1) { sum = sum + i }\n"
                  "print(sum)",
        .label = "for loop with accumulator",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Nested Control Flow --- */

TEST(cmp_nested_if) {
    run_compare((CompareSpec) {
        .source = "var x = 15\n"
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
    run_compare((CompareSpec) {
        .source = "var x = 10\nx += 5\nx -= 3\nx *= 2\nprint(x)",
        .label = "compound assignment operators",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Ternary Expression --- */

TEST(cmp_ternary) {
    run_compare((CompareSpec) {
        .source = "var x = 5\n"
                  "var y = x > 3 ? 100 : 200\n"
                  "print(y)",
        .label = "ternary expression",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Break and Continue --- */

TEST(cmp_while_break) {
    run_compare((CompareSpec) {
        .source = "var i = 0\n"
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
    run_compare((CompareSpec) {
        .source = "var sum = 0\nvar i = 0\n"
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
    run_compare((CompareSpec) {
        .source = "fn add(a: i64, b: i64) -> i64 { return a + b }\n"
                  "var r = add(3, 4)\nprint(r)",
        .label = "function declaration and call",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_func_recursive) {
    run_compare((CompareSpec) {
        .source = "fn fib(n: i64) -> i64 {\n"
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
    run_compare((CompareSpec) {
        .source = "var sum = 0\n"
                  "var i = 0\nwhile (i < 3) {\n"
                  "  var j = 0\n  while (j < 3) {\n"
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
    run_compare((CompareSpec) {
        .source = "var a = \"hello\"\nvar b = \" world\"\n"
                  "var c = a + b\nprint(c)",
        .label = "string concatenation",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_string_concat_chain) {
    /* Multi-operand chain: "a" + "b" + "c" + "d" flattened to single STRBUF */
    run_compare((CompareSpec) {
        .source = "var a = \"hello\"\n"
                  "var b = \" \"\n"
                  "var c = \"world\"\n"
                  "var d = \"!\"\n"
                  "var result = a + b + c + d\n"
                  "print(result)",
        .label = "string concat chain: 4-way STRBUF flatten",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Mixed Types --- */

TEST(cmp_mixed_arith) {
    run_compare((CompareSpec) {
        .source = "var a = 10\nvar b = 3\n"
                  "print(a / b)\nprint(a % b)",
        .label = "integer division and modulo",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

/* --- Nested Function Calls --- */

TEST(cmp_nested_call) {
    run_compare((CompareSpec) {
        .source = "fn add(a: i64, b: i64) -> i64 { return a + b }\n"
                  "print(add(1, add(2, 3)))",
        .label = "nested function calls",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_func_early_return) {
    run_compare((CompareSpec) {
        .source = "fn abs(n: i64) -> i64 {\n"
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
    run_compare((CompareSpec) {
        .source = "fn fact(n: i64) -> i64 {\n"
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
    run_compare((CompareSpec) {
        .source = "var x = 3.14\nvar y = 2.0\n"
                  "print(x + y)\nprint(x * y)",
        .label = "f64 arithmetic",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Scope Shadowing --- */

TEST(cmp_block_scope) {
    run_compare((CompareSpec) {
        .source = "var x = 10\n"
                  "if (true) {\n"
                  "  var y = x + 5\n"
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
    run_compare((CompareSpec) {
        .source = "var a = 2\nvar b = 3\nvar c = 4\n"
                  "var r = (a + b) * c - a\nprint(r)",
        .label = "complex arithmetic expression",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_for_accumulate) {
    run_compare((CompareSpec) {
        .source = "var sum = 0\n"
                  "for (var i = 1; i <= 10; i = i + 1) {\n"
                  "  sum = sum + i\n"
                  "}\nprint(sum)",
        .label = "for-loop sum 1..10",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_chained_comparison) {
    run_compare((CompareSpec) {
        .source = "var a = 5\nvar b = 10\nvar c = 3\n"
                  "if (a > c && b > a) { print(1) } else { print(0) }",
        .label = "chained comparison with logical and",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_while_countdown) {
    run_compare((CompareSpec) {
        .source = "var n = 5\nvar result = 1\n"
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
    run_compare((CompareSpec) {
        .source = "var t = true\nvar f = false\n"
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
    run_compare((CompareSpec) {
        .source = "fn double(x: i64) -> i64 { return x * 2 }\n"
                  "fn inc(x: i64) -> i64 { return x + 1 }\n"
                  "print(inc(double(3)))",
        .label = "multiple function declarations and chained calls",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Bitwise Operations --- */

TEST(cmp_bitwise_and_or) {
    run_compare((CompareSpec) {
        .source = "var a = 12\nvar b = 10\n"
                  "print(a & b)\nprint(a | b)",
        .label = "bitwise AND and OR",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_bitwise_xor_shift) {
    run_compare((CompareSpec) {
        .source = "var a = 5\n"
                  "print(a ^ 3)\nprint(a << 2)\nprint(a >> 1)",
        .label = "bitwise XOR and shifts",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_bitwise_not) {
    run_compare((CompareSpec) {
        .source = "var a = 0\nprint(~a)\n"
                  "var b = 255\nprint(~b)",
        .label = "bitwise NOT",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Increment / Decrement --- */

TEST(cmp_increment) {
    run_compare((CompareSpec) {
        .source = "var x = 5\nx++\nprint(x)\n"
                  "x--\nx--\nprint(x)",
        .label = "increment and decrement operators",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Array Literal and Indexing --- */

TEST(cmp_array_literal) {
    run_compare((CompareSpec) {
        .source = "var arr = [10, 20, 30]\n"
                  "print(arr[0])\nprint(arr[1])\nprint(arr[2])",
        .label = "array literal and index access",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_array_assign) {
    run_compare((CompareSpec) {
        .source = "var arr = [1, 2, 3]\n"
                  "arr[1] = 99\nprint(arr[1])",
        .label = "array index assignment",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Nested Ternary --- */

TEST(cmp_nested_ternary) {
    run_compare((CompareSpec) {
        .source = "var x = 5\n"
                  "var r = x > 10 ? 1 : (x > 3 ? 2 : 3)\n"
                  "print(r)",
        .label = "nested ternary expression",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Multi-branch If-Else --- */

TEST(cmp_if_else_chain) {
    run_compare((CompareSpec) {
        .source = "var x = 15\n"
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
    run_compare((CompareSpec) {
        .source = "var a = 2\nvar b = 3\nvar c = 4\nvar d = 5\n"
                  "var r = ((a + b) * (c - d) + a * b) * c\n"
                  "print(r)",
        .label = "deeply nested arithmetic expression",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- While with Multiple Conditions --- */

TEST(cmp_while_multi_cond) {
    run_compare((CompareSpec) {
        .source = "var i = 0\nvar sum = 0\n"
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

/* A Map literal needs the '#' prefix (LANGUAGE_SPEC.md §2.4.7); a bare
 * '{...}' is an exact object shape, which supports only static-key indexing and
 * panics with E0402 at runtime.  This snippet used the bare form and so
 * tested an object shape, not a Map -- invisible for as long as the execution
 * comparison never ran. */
TEST(cmp_map_literal) {
    run_compare((CompareSpec) {
        .source = "var m = #{\"a\": 1, \"b\": 2}\n"
                  "print(m[\"a\"])\nprint(m[\"b\"])",
        .label = "map literal and key access",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Template String --- */

TEST(cmp_template_string) {
    run_compare((CompareSpec) {
        .source = "var name = \"world\"\n"
                  "var msg = \"hello ${name}\"\n"
                  "print(msg)",
        .label = "template string interpolation",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- For-in Loop --- */

TEST(cmp_for_in_array) {
    run_compare((CompareSpec) {
        .source = "var arr = [10, 20, 30]\n"
                  "var sum = 0\n"
                  "for (item in arr) { sum = sum + item }\n"
                  "print(sum)",
        .label = "for-in loop over array",
        .expect_xi_success = true,
        .min_similarity = 0.3,
        .check_exec = true,
    });
}

TEST(cmp_for_in_range) {
    run_compare((CompareSpec) {
        .source = "var sum = 0\n"
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
    run_compare((CompareSpec) {
        .source = "fn make_adder(x: i64) -> fn(i64) -> i64 {\n"
                  "  fn adder(y: i64) -> i64 { return x + y }\n"
                  "  return adder\n"
                  "}\n"
                  "var add5 = make_adder(5)\n"
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
    run_compare((CompareSpec) {
        .source = "var x: JSON.Value = 42\n"
                  "var y = x as i64\n"
                  "print(y)",
        .label = "as cast: JSON.Value(i64) as i64 succeeds",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

TEST(cmp_as_safe_match) {
    /* as? (safe cast) -> type matches -> value passes through */
    run_compare((CompareSpec) {
        .source = "var x: JSON.Value = \"hello\"\n"
                  "var y = x as string?\n"
                  "print(y)",
        .label = "as? safe cast: JSON.Value(string) as string? succeeds",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
        .expected_stdout = "hello\n",
    });
}

TEST(cmp_as_safe_mismatch) {
    /* as? (safe cast) -> type mismatch -> null */
    run_compare((CompareSpec) {
        .source = "var x: JSON.Value = \"hello\"\n"
                  "var y = x as i64?\n"
                  "print(y)",
        .label = "as? safe cast: JSON.Value(string) as i64? -> null",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
        .expected_stdout = "null\n",
    });
}

TEST(cmp_as_unsafe_mismatch) {
    /* as (unsafe cast) -> type mismatch -> throw on both compilation routes */
    run_compare((CompareSpec) {
        .source = "var x: JSON.Value = \"hello\"\n"
                  "var y = x as i64\n"
                  "print(y)",
        .label = "as unsafe cast: JSON.Value(string) as i64 -> throw",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
        .expected_stdout = "",
        .expect_runtime_failure = true,
    });
}

TEST(cmp_dynamic_as_checked_match) {
    run_compare((CompareSpec) {
        .source = "var x: JSON.Value = 42\nvar y = x as i64\nprint(y)",
        .label = "dynamic checked integer assertion preserves its value",
        .expect_xi_success = true, .min_similarity = 0.2,
        .check_exec = true, .expected_stdout = "42\n",
    });
}
TEST(cmp_numeric_as_witness_conversion) {
    run_compare((CompareSpec) {
        .source = "var x: f64 = 3.75\nvar y = x as i64\nprint(y)",
        .label = "numeric conversion uses its typed witness",
        .expect_xi_success = true, .min_similarity = 0.2,
        .check_exec = true, .expected_stdout = "3\n",
    });
}
TEST(cmp_numeric_as_witness_truncation) {
    run_compare((CompareSpec) {
        .source = "var x: u64 = 257\nvar y = x as u8\nprint(y)",
        .label = "typed integer truncation remains separate from assertion",
        .expect_xi_success = true, .min_similarity = 0.2,
        .check_exec = true, .expected_stdout = "1\n",
    });
}

/* --- Nullish Coalesce --- */

TEST(cmp_nullish_coalesce) {
    run_compare((CompareSpec) {
        .source = "var a: i64? = null\n"
                  "var b = a ?? 42\n"
                  "print(b)\n"
                  "var c: i64? = 10\n"
                  "var d = c ?? 99\n"
                  "print(d)",
        .label = "nullish coalesce operator",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Match Expression --- */

TEST(cmp_match_expr) {
    run_compare((CompareSpec) {
        .source = "var x = 3\n"
                  "var r = match (x) {\n"
                  "  1 -> 10\n"
                  "  2 -> 20\n"
                  "  3 -> 30\n"
                  "  _ -> 0\n"
                  "}\nprint(r)",
        .label = "match expression with literal patterns",
        .expect_xi_success = true,
        .min_similarity = 0.2,
        .check_exec = true,
    });
}

/* --- Try-Catch --- */

TEST(cmp_try_catch) {
    run_compare((CompareSpec) {
        .source = "var result = 0\n"
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
    run_compare((CompareSpec) {
        .source = "fn printSlice() {\n"
                  "  var arr = [1, 2, 3, 4, 5]\n"
                  "  var s: Slice<i64> = arr[1:3]\n"
                  "  print(s)\n"
                  "}\n"
                  "printSlice()",
        .label = "array slice expression",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Scope with Nested Functions --- */

TEST(cmp_nested_func_scope) {
    run_compare((CompareSpec) {
        .source = "fn outer() -> i64 {\n"
                  "  var x = 10\n"
                  "  fn inner() -> i64 { return x * 2 }\n"
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
    run_compare((CompareSpec) {
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
    run_compare((CompareSpec) {
        .source = "var x: i64? = null\n"
                  "var v = x ?? -1\n"
                  "print(v)",
        .label = "nullable with nullish coalesce fallback",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Method Calls on Builtins --- */

TEST(cmp_array_push) {
    run_compare((CompareSpec) {
        .source = "var arr = [10, 20]\n"
                  "arr.push(30)\n"
                  "print(len(arr))\nprint(arr[2])",
        .label = "array push and length",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_string_method) {
    run_compare((CompareSpec) {
        .source = "import text\n"
                  "var s = \"hello\"\n"
                  "print(len(s))\n"
                  "print(text.upper(s))",
        .label = "string length and text.upper",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Higher-Order Functions --- */

TEST(cmp_higher_order) {
    run_compare((CompareSpec) {
        .source = "fn makeAdder(x: i64) -> fn(i64) -> i64 {\n"
                  "    return fn(y: i64) -> i64 { return x + y }\n"
                  "}\n"
                  "var add5 = makeAdder(5)\n"
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
    run_compare((CompareSpec) {
        .source = "var total = 0\n"
                  "var matrix = [[1, 2], [3, 4]]\n"
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
    run_compare((CompareSpec) {
        .source = "var count = 0\n"
                  "for (c in \"hello\".runes()) {\n"
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
    run_compare((CompareSpec) {
        .source = "fn sum(arr: Array<i64>) -> i64 {\n"
                  "    var total = 0\n"
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



typedef struct CallableTargetExpectation {
    XaAnalyzer *analyzer;
    const char *callee;
    const char *const *target_names;
    uint32_t target_node_ids[8];
    uint32_t target_count;
    uint32_t calls_checked;
    bool incomplete;
} CallableTargetExpectation;

static void collect_expected_callable_declaration(AstNode *node, void *userdata) {
    CallableTargetExpectation *expect = (CallableTargetExpectation *) userdata;
    if (!node || node->type != AST_FUNCTION_DECL || !node->as.function_decl.name)
        return;
    for (uint32_t i = 0; i < expect->target_count; ++i)
        if (strcmp(node->as.function_decl.name, expect->target_names[i]) == 0)
            expect->target_node_ids[i] = node->node_id;
}

static void check_final_callable_targets(AstNode *node, void *userdata) {
    CallableTargetExpectation *expect = (CallableTargetExpectation *) userdata;
    if (!node || node->type != AST_CALL_EXPR || !node->as.call_expr.callee ||
        node->as.call_expr.callee->type != AST_VARIABLE ||
        strcmp(node->as.call_expr.callee->as.variable.name, expect->callee) != 0)
        return;
    ++expect->calls_checked;
    XaCallableTargetSetFact fact = {0};
    CHECK(xa_analyzer_get_callable_target_set(expect->analyzer, node, &fact),
          "canonical final call fact must be published, including unknown facts");
    if (expect->incomplete) {
        CHECK(!fact.complete && fact.target_count == 0 && fact.structural_signature_key == 0,
              "unknown capture must not retain earlier exact authority");
        return;
    }
    CHECK(fact.complete && fact.structural_signature_key != 0 &&
          fact.target_count == expect->target_count,
          "closed capture must contain exactly all observed declaration targets");
    for (uint32_t i = 0; i < expect->target_count; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < fact.target_count; ++j)
            if (fact.targets[j].function_node_id == expect->target_node_ids[i] &&
                fact.targets[j].symbol_id != 0 &&
                fact.targets[j].structural_signature_key == fact.structural_signature_key)
                found = true;
        CHECK(expect->target_node_ids[i] != 0 && found,
              "capture target must join the independently named Source declaration");
    }
}

static void check_source_callable_authority(const char *source, CallableTargetExpectation *expect,
                                           bool expect_live_loan_error) {
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    XrCompilerSessionOperationScope operation;
    REQUIRE(compare_operation_begin(session, &operation), "Source authority operation failed");
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    AstNode *program = analyzer ? xr_parse_with_source(session, source, "compare.xr") : NULL;
    if (!analyzer || !program) {
        xa_analyzer_free(analyzer);
        xr_program_destroy(program);
        (void) xr_compiler_session_operation_fail(&operation, XR_COMPILER_SESSION_OPERATION_FATAL);
        REQUIRE(false, "Source authority fixture must parse with an analyzer");
    }
    xa_analyzer_analyze(analyzer, "compare.xr", program);
    int diagnostic_count = 0;
    if (expect_live_loan_error) {
        bool found = false;
        for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count); diag; diag = diag->next)
            if (diag->severity == XR_DIAG_SEV_ERROR &&
                diag->code == XR_ERR_ANALYZE_BORROW_CONFLICT &&
                strstr(diag->message, "OWN-E-LIVE-LOAN"))
                found = true;
        CHECK(found, "callable capture rebinding must be refused by actual Source loan authority");
    } else {
        for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count); diag; diag = diag->next)
            CHECK(diag->severity != XR_DIAG_SEV_ERROR,
                  "callable target proof fixture must be source-valid");
        expect->analyzer = analyzer;
        xa_ast_walk(program, collect_expected_callable_declaration, NULL, expect);
        xa_ast_walk(program, check_final_callable_targets, NULL, expect);
        CHECK(expect->calls_checked > 0, "independent Source call expectation found no matching call");
    }
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
    CHECK(xr_compiler_session_operation_succeed(&operation), "Source authority operation finish failed");
}

TEST(cmp_compose_multiple_incoming) {
    run_compare((CompareSpec) {
        .source = "fn compose(f: fn(i64) -> i64, g: fn(i64) -> i64) -> fn(i64) -> i64 {\n"
                  " return fn(x: i64) -> i64 { return f(g(x)) }\n}\n"
                  "fn add1(x: i64) -> i64 { return x + 1 }\n"
                  "fn add10(x: i64) -> i64 { return x + 10 }\n"
                  "fn mul2(x: i64) -> i64 { return x * 2 }\n"
                  "fn mul3(x: i64) -> i64 { return x * 3 }\n"
                  "var h1 = compose(add1, mul2)\nvar h2 = compose(add10, mul3)\n"
                  "print(h1(5))\nprint(h2(5))",
        .label = "all incoming capture targets survive distinct composition calls",
        .expect_xi_success = true, .min_similarity = 0.1,
        .check_exec = true, .expected_stdout = "11\n25\n",
    });
}
TEST(cmp_callable_capture_rebind) {
    const char *source = "fn add1(x: i64) -> i64 { return x + 1 }\n"
                         "fn add10(x: i64) -> i64 { return x + 10 }\n"
                         "fn make() -> fn(i64) -> i64 {\n"
                         " var f = add1\nvar h = fn(x: i64) -> i64 { return f(x) }\n"
                         " f = add10\nreturn h\n}\nvar h = make()\nprint(h(5))";
    check_source_callable_authority(source, NULL, true);
    XrProto *wrapper = compile_wrapper(source);
    XrProto *direct = compile_xi(source);
    CHECK(wrapper == NULL && direct == NULL, "both consumers must reject a live callable-owner loan mutation");
    xr_instruction_unit_free(wrapper);
    xr_instruction_unit_free(direct);
}
TEST(cmp_callable_nested_return) {
    run_compare((CompareSpec) {
        .source = "fn add1(x: i64) -> i64 { return x + 1 }\n"
                  "fn wrap(f: fn(i64) -> i64) -> fn(i64) -> i64 {\n"
                  " var inner = fn(x: i64) -> i64 { return f(x) }\n"
                  " return fn(x: i64) -> i64 { return inner(x) + 10 }\n}\n"
                  "var h = wrap(add1)\nprint(h(5))",
        .label = "returned outer lambda inherits exact inner capture closure",
        .expect_xi_success = true, .min_similarity = 0.1,
        .check_exec = true, .expected_stdout = "16\n",
    });
}


TEST(cmp_callable_closed_target_union) {
    const char *source = "fn compose(f: fn(i64) -> i64, g: fn(i64) -> i64) -> fn(i64) -> i64 {\n"
        " return fn(x: i64) -> i64 { return f(g(x)) }\n}\n"
        "fn add1(x: i64) -> i64 { return x + 1 }\nfn add10(x: i64) -> i64 { return x + 10 }\n"
        "fn mul2(x: i64) -> i64 { return x * 2 }\nfn mul3(x: i64) -> i64 { return x * 3 }\n"
        "var h1 = compose(add1, mul2)\nvar h2 = compose(add10, mul3)\nprint(h1(5))\nprint(h2(5))";
    const char *f_names[] = {"add1", "add10"};
    const char *g_names[] = {"mul2", "mul3"};
    CallableTargetExpectation f_expect = {.callee = "f", .target_names = f_names, .target_count = 2};
    CallableTargetExpectation g_expect = {.callee = "g", .target_names = g_names, .target_count = 2};
    check_source_callable_authority(source, &f_expect, false);
    check_source_callable_authority(source, &g_expect, false);
}
TEST(cmp_callable_unknown_invalidates_exact) {
    const char *source = "fn compose(f: fn(i64) -> i64, g: fn(i64) -> i64) -> fn(i64) -> i64 {\n"
        " return fn(x: i64) -> i64 { return f(g(x)) }\n}\n"
        "fn add1(x: i64) -> i64 { return x + 1 }\nfn mul2(x: i64) -> i64 { return x * 2 }\n"
        "fn unknown(f: fn(i64) -> i64) -> fn(i64) -> i64 { return compose(f, mul2) }\n"
        "var h = compose(add1, mul2)\nprint(h(5))";
    CallableTargetExpectation expect = {.callee = "f", .incomplete = true};
    check_source_callable_authority(source, &expect, false);
    XrProto *wrapper = compile_wrapper(source);
    XrProto *direct = compile_xi(source);
    CHECK(wrapper == NULL && direct == NULL, "unknown incoming target must not obtain a closed executable graph");
    xr_instruction_unit_free(wrapper);
    xr_instruction_unit_free(direct);
}

/* --- Multiple Closures --- */

TEST(cmp_multi_closure) {
    run_compare((CompareSpec) {
        .source = "var x = 10\n"
                  "var add = fn(a: i64) -> i64 { return a + x }\n"
                  "var mul = fn(a: i64) -> i64 { return a * x }\n"
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
    run_compare((CompareSpec) {
        .source = "fn fib(n: i64) -> i64 {\n"
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
    run_compare((CompareSpec) {
        .source = "fn outer() -> i64 {\n"
                  "    var x = 10\n"
                  "    fn middle() -> i64 {\n"
                  "        fn inner() -> i64 { return x + 1 }\n"
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
    run_compare((CompareSpec) {
        .source = "fn counter() -> fn() -> i64 {\n"
                  "    var n = 0\n"
                  "    return fn() -> i64 { n += 1; return n }\n"
                  "}\n"
                  "var c = counter()\n"
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
    run_compare((CompareSpec) {
        .source = "fn compose(f: fn(i64) -> i64, g: fn(i64) -> i64) -> fn(i64) -> i64 {\n"
                  "    return fn(x: i64) -> i64 { return f(g(x)) }\n"
                  "}\n"
                  "fn add1(x: i64) -> i64 { return x + 1 }\n"
                  "fn mul2(x: i64) -> i64 { return x * 2 }\n"
                  "var h = compose(add1, mul2)\n"
                  "print(h(5))",
        .label = "function composition capturing two params",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
        .expected_stdout = "11\n",
    });
}

/* --- Callback / Higher-Order Apply --- */

TEST(cmp_apply_fn) {
    run_compare((CompareSpec) {
        .source = "fn apply(f: fn(i64) -> i64, x: i64) -> i64 {\n"
                  "    return f(x)\n"
                  "}\n"
                  "fn double(x: i64) -> i64 { return x * 2 }\n"
                  "fn square(x: i64) -> i64 { return x * x }\n"
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
    run_compare((CompareSpec) {
        .source = "var nums = [5, 3, 8, 1, 9, 2]\n"
                  "var max = nums[0]\n"
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
    run_compare((CompareSpec) {
        .source = "fn is_even(n: i64) -> bool {\n"
                  "    if (n == 0) { return true }\n"
                  "    return is_odd(n - 1)\n"
                  "}\n"
                  "fn is_odd(n: i64) -> bool {\n"
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
    run_compare((CompareSpec) {
        .source = "fn power(base: i64, exp: i64) -> i64 {\n"
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
    run_compare((CompareSpec) {
        .source = "fn gcd(a: i64, b: i64) -> i64 {\n"
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
    run_compare((CompareSpec) {
        .source = "class Point {\n"
                  "    x: i64\n"
                  "    y: i64\n"
                  "    constructor(x: i64, y: i64) {\n"
                  "        this.x = x\n"
                  "        this.y = y\n"
                  "    }\n"
                  "}\n"
                  "var p = Point(3, 4)\n"
                  "print(p.x)\n"
                  "print(p.y)",
        .label = "class basic: constructor + field access",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
        .expected_stdout = "3\n4\n",
    });
}

/* --- Class with method --- */

TEST(cmp_class_method) {
    run_compare((CompareSpec) {
        .source = "class Box {\n"
                  "    value: i64\n"
                  "    constructor(v) {\n"
                  "        this.value = v\n"
                  "    }\n"
                  "}\n"
                  "var b = Box(42)\n"
                  "print(b.value)\n"
                  "b.value = 99\n"
                  "print(b.value)",
        .label = "class field: read + write",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
        .expected_stdout = "42\n99\n",
    });
}

/* --- Struct literal --- */

/* A bare object literal is an exact object shape. Dot and static-string
 * bracket access resolve to the same field ordinal. */
TEST(cmp_struct_literal) {
    run_compare((CompareSpec) {
        .source = "var obj = { name: \"Alice\", age: 30 }\n"
                  "print(obj.name)\n"
                  "print(obj.age)",
        .label = "object literal with fields",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* --- Class inheritance --- */

TEST(cmp_class_inherit) {
    run_compare((CompareSpec) {
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
                  "var d = Dog(\"Rex\", \"Labrador\")\n"
                  "print(d.name)\n"
                  "print(d.breed)",
        .label = "class inheritance + method override",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
        .expected_stdout = "Rex\nLabrador\n",
    });
}

TEST(cmp_super_multiple_levels) {
    run_compare((CompareSpec) {
        .source = "class Root {\n root: i64\n constructor(x: i64) { this.root = x }\n}\n"
                  "class Middle extends Root {\n middle: i64\n constructor(x: i64, y: i64) { super(x); this.middle = y }\n}\n"
                  "class Leaf extends Middle {\n leaf: i64\n constructor(x: i64, y: i64, z: i64) { super(x, y); this.leaf = z }\n}\n"
                  "var l = Leaf(7, 9, 11)\nprint(l.root)\nprint(l.middle)\nprint(l.leaf)",
        .label = "three constructor frames preserve independent receiver owners",
        .expect_xi_success = true, .min_similarity = 0.1,
        .check_exec = true, .expected_stdout = "7\n9\n11\n",
    });
}
TEST(cmp_super_ordinary_method) {
    run_compare((CompareSpec) {
        .source = "class Parent {\n constructor() {}\n read() -> i64 { return 7 }\n}\n"
                  "class Child extends Parent {\n constructor() { super() }\n override read() -> i64 { return super.read() + 1 }\n}\n"
                  "var c = Child()\nprint(c.read())\nprint(c.read())",
        .label = "ordinary parent method retains its borrowed receiver contract",
        .expect_xi_success = true, .min_similarity = 0.1,
        .check_exec = true, .expected_stdout = "8\n8\n",
    });
}
TEST(cmp_super_constructor_error) {
    run_compare((CompareSpec) {
        .source = "enum ParentError { Failed }\nclass Parent {\n constructor() { throw ParentError.Failed }\n}\n"
                  "class Child extends Parent {\n constructor() { super() }\n}\n"
                  "try { var c = Child(); print(\"unreachable\") } catch (e) { print(\"caught\") }",
        .label = "parent constructor failure unwinds its acquired owner",
        .expect_xi_success = true, .min_similarity = 0.1,
        .check_exec = true, .expected_stdout = "caught\n",
    });
}


static XrMethod *owner_method(XrClass *cls, bool constructor) {
    for (uint16_t i = 0; i < cls->method_count; ++i) {
        XrMethod *m = &cls->methods[i];
        if (m->type == XMETHOD_CLOSURE && m->as.closure &&
            (constructor ? xr_method_is_constructor(m) : strcmp(m->name, "read") == 0))
            return m;
    }
    return NULL;
}

TEST(cmp_super_owner_physical) {
    const char *source =
        "class OwnerParent {\n constructor() {}\n read() -> i64 { return 7 }\n}\n"
        "class OwnerChild extends OwnerParent {\n constructor() { super() }\n"
        " override read() -> i64 { return super.read() + 1 }\n}\n";
    XrProto *p = compile_wrapper(source);
    REQUIRE(p != NULL, "Source receiver-owner fixture compile failed");
    int rc = -1;
    char *out = execute_and_capture(p, &rc);
    defer_proto_free(p);
    CHECK(rc == 0 && out && strcmp(out, "") == 0, "receiver fixture publication failed");
    xr_free(out);
    XrClass *child = xr_class_lookup_by_name(g_iso, "OwnerChild");
    REQUIRE(child != NULL && child->super != NULL, "published Source child class missing");
    XrCoroutine *coro = xr_test_init_coro(g_iso);
    REQUIRE(coro != NULL, "physical coroutine creation failed");
    XrVMContext *ctx = xr_vm_current_ctx(g_iso);
    REQUIRE(ctx != NULL && ctx->frame_count == 0, "physical context not quiescent");
    REQUIRE(ctx->frame_capacity >= 2 && ctx->stack_capacity >= 64,
            "physical context has insufficient frame and scratch capacity");
    XrVMContext saved = *ctx;
    int saved_depth = g_iso->vm.ctor_call_depth;
    CHECK(saved_depth == 0, "fixture left constructor frames active");
    for (int kind = 0; kind < 3; ++kind) {
        XrMethod *caller = owner_method(child, kind != 1);
        REQUIRE(caller != NULL, "Source-generated caller method missing");
        XrProto *cp = caller->as.closure->proto;
        XrInstruction *op = NULL;
        for (size_t i = 0; i < PROTO_CODE_COUNT(cp); ++i) {
            if (GET_OPCODE(PROTO_CODE(cp, i)) == OP_SUPERINVOKE) {
                op = &PROTO_CODE_BASE(cp)[i];
                break;
            }
        }
        REQUIRE(op != NULL && GETARG_C(*op) == 0, "exact generated SUPERINVOKE missing");
        int a = GETARG_A(*op);
        CHECK(a > 0, "Source super producer must use a fresh scratch window");
        XrValue method_name = PROTO_CONSTANT(cp, GETARG_B(*op));
        XrMethod *target = owner_method(child->super, kind != 1);
        REQUIRE(target != NULL && XR_IS_STRING(method_name) &&
                strcmp(target->name, xr_value_str_data(&method_name)) == 0,
                "exact source target and call constant differ");
        XrObjectInstance *inst = xr_instance_new(g_iso, child);
        REQUIRE(inst != NULL, "owned receiver allocation failed");
        CHECK(atomic_load_explicit(&inst->hdr.refcount, memory_order_relaxed) == 0,
              "fresh receiver must have one owner in zero-based RC");
        memset(ctx->frames, 0, sizeof(XrBcCallFrame) * 2);
        for (int i = 0; i < saved.stack_capacity; ++i)
            ctx->stack[i] = XR_NULL_VAL;
        ctx->frame_count = 1;
        ctx->handler_count = 0;
        ctx->pending_error = XR_NULL_VAL;
        ctx->current_exception = XR_NULL_VAL;
        ctx->stack_top = ctx->stack + cp->maxstacksize;
        ctx->frames[0].closure = caller->as.closure;
        ctx->frames[0].pc = op + 1;
        ctx->frames[0].base_offset = 0;
        ctx->stack[a + 1] = xr_value_from_instance(inst);
        g_iso->vm.ctor_call_depth = 0;
        if (kind == 2) {
            g_iso->vm.ctor_call_depth = XR_CTOR_CALL_STACK_MAX;
            for (int i = 0; i < XR_CTOR_CALL_STACK_MAX; ++i) {
                g_iso->vm.ctor_call_stack[i].class_ptr = child;
                g_iso->vm.ctor_call_stack[i].frame_count = 1;
            }
        }
        XrDispatchAction action = vm_superinvoke(g_iso, ctx, *op, ctx->stack,
                                               &ctx->frames[0], op + 1);
        if (kind < 2) {
            CHECK(action == XR_DISP_RESTART && ctx->frame_count == 2,
                  "successful exact target must publish one frame, kind=%d", kind);
            CHECK(ctx->frames[1].closure == target->as.closure &&
                  ctx->frames[1].base_offset == a + 1,
                  "super receiver must use the unified scratch frame");
            CHECK(g_iso->vm.ctor_call_depth == 1 &&
                  g_iso->vm.ctor_call_stack[0].class_ptr == child->super &&
                  g_iso->vm.ctor_call_stack[0].frame_count == 1,
                  "exact parent identity and return boundary missing");
            CHECK(atomic_load_explicit(&inst->hdr.refcount, memory_order_relaxed) ==
                      (kind == 0 ? 1 : 0),
                  "only successful constructor acquires a receiver owner");
            if (kind == 0) {
                xr_rc_release_value(xr_current_coro_heap(), xr_value_from_instance(inst));
                CHECK(atomic_load_explicit(&inst->hdr.refcount, memory_order_relaxed) == 0 &&
                      inst->klass == child && !(inst->hdr.extra & XR_OBJ_DEAD),
                      "parent result drop must preserve enclosing constructor receiver");
            }
        } else {
            CHECK(action == XR_DISP_RAISE && !XR_IS_NULL(ctx->current_exception) &&
                  xr_panic_info_get_code(g_iso, ctx->current_exception) == XR_ERR_STACK_OVERFLOW,
                  "failed frame must raise a real stack panic, kind=%d", kind);
            CHECK(atomic_load_explicit(&inst->hdr.refcount, memory_order_relaxed) == 0 &&
                  inst->klass == child && !(inst->hdr.extra & XR_OBJ_DEAD),
                  "failed frame must not acquire or drop receiver owner, kind=%d", kind);
            CHECK(ctx->frame_count < 2, "failure must not publish a parent frame");
        }
        if (!XR_IS_NULL(ctx->current_exception))
            xr_rc_release_value(xr_current_coro_heap(), ctx->current_exception);
        ctx->pending_error = XR_NULL_VAL;
        ctx->current_exception = XR_NULL_VAL;
        ctx->stack[a + 1] = XR_NULL_VAL;
        xr_rc_release_value(xr_current_coro_heap(), xr_value_from_instance(inst));
        *ctx = saved;
        g_iso->vm.ctor_call_depth = saved_depth;
    }
}

/* ========== Enum Tests ========== */

TEST(cmp_enum_basic) {
    run_compare((CompareSpec) {
        .source = "enum Color {\n"
                  "    Red,\n"
                  "    Green,\n"
                  "    Blue\n"
                  "}\n"
                  "var c = Color.Red\n"
                  "print(c.name)\n"
                  "print(c.ordinal)",
        .label = "enum basic: access name and ordinal",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* ========== For-in Map Tests ========== */

TEST(cmp_for_in_map) {
    run_compare((CompareSpec) {
        .source = "var m = #{ \"a\": 1, \"b\": 2, \"c\": 3 }\n"
                  "var sum = 0\n"
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
    run_compare((CompareSpec) {
        .source = "fn make_adder(n: i64) -> fn(i64) -> i64 {\n"
                  "    return fn(x: i64) -> i64 { return x + n }\n"
                  "}\n"
                  "var add5 = make_adder(5)\n"
                  "var add10 = make_adder(10)\n"
                  "print(add5(3))\n"
                  "print(add10(3))",
        .label = "closure factory: make_adder",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_closure_accumulator) {
    run_compare((CompareSpec) {
        .source = "fn make_acc() -> fn(i64) -> i64 {\n"
                  "    var total = 0\n"
                  "    return fn(n: i64) -> i64 { total += n; return total }\n"
                  "}\n"
                  "var acc = make_acc()\n"
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
    run_compare((CompareSpec) {
        .source = "fn outer() -> fn() -> fn() -> i64 {\n"
                  "    var val = 42\n"
                  "    return fn() -> fn() -> i64 {\n"
                  "        return fn() -> i64 { return val }\n"
                  "    }\n"
                  "}\n"
                  "print(outer()()())",
        .label = "triple-nested closure transitive capture",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* ========== Destructuring / Multi-assign / Collection Tests ========== */

TEST(cmp_destructure_array) {
    run_compare((CompareSpec) {
        .source = "var arr = [10, 20, 30]\n"
                  "var [a, b, c] = arr\n"
                  "print(a)\n"
                  "print(b)\n"
                  "print(c)",
        .label = "destructure array declaration",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
        .expected_stdout = "10\n20\n30\n",
    });
}

TEST(cmp_destructure_object) {
    run_compare((CompareSpec) {
        .source = "var { name, age } = { name: \"alice\", age: 30 }\n"
                  "print(name)\n"
                  "print(age)",
        .label = "destructure object declaration",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_multi_var_decl) {
    run_compare((CompareSpec) {
        .source = "fn pair() -> (i64, i64) { return (10, 20) }\n"
                  "var (x, y) = pair()\n"
                  "print(x)\n"
                  "print(y)",
        .label = "tuple-return var declaration",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_multi_assign) {
    run_compare((CompareSpec) {
        .source = "var x = 1\n"
                  "var y = 2\n"
                  "(x, y) = (y, x)\n"
                  "print(x)\n"
                  "print(y)",
        .label = "tuple destructure swap assignment",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_set_literal) {
    run_compare((CompareSpec) {
        .source = "var s = #[1, 2, 3, 2, 1]\n"
                  "print(len(s))",
        .label = "set literal with duplicates",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* Static bracket access is the exact-object spelling of the same field
 * ordinal selected by dot access. */
TEST(cmp_object_literal) {
    run_compare((CompareSpec) {
        .source = "var obj = { name: \"alice\", age: 30 }\n"
                  "print(obj[\"name\"])\n"
                  "print(obj[\"age\"])",
        .label = "object literal bracket access",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

/* ========== Coroutine Tests ========== */

TEST(cmp_defer_simple) {
    run_compare((CompareSpec) {
        .source = "fn cleanup() { print(\"deferred\") }\n"
                  "fn f() {\n"
                  "  defer { cleanup() }\n"
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
    run_compare((CompareSpec) {
        .source = "fn first() { print(\"first\") }\n"
                  "fn second() { print(\"second\") }\n"
                  "fn f() {\n"
                  "  defer { first() }\n"
                  "  defer { second() }\n"
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
    /* Coro.yield() in main should be a no-op (no other coroutines). */
    run_compare((CompareSpec) {
        .source = "print(\"before\")\nCoro.yield()\nprint(\"after\")",
        .label = "Coro.yield(): no-op without other coroutines",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
        .expected_stdout = "before\nafter\n",
    });
}

TEST(cmp_chan_new_unbuf) {
    /* Channel() creates an unbuffered channel; just type-check */
    run_compare((CompareSpec) {
        .source = "const ch: Channel<i64> = Channel()\nprint(typeName(ch))",
        .label = "Channel() -> unbuffered channel construction",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_chan_new_buffered) {
    run_compare((CompareSpec) {
        .source = "const ch: Channel<i64> = Channel(4)\nprint(typeName(ch))",
        .label = "Channel(N) -> buffered channel construction",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_chan_send_recv_buffered) {
    /* Buffered channel: send then recv on same coro works without scheduling */
    run_compare((CompareSpec) {
        .source = "const ch: Channel<i64> = Channel(2)\n"
                  "ch.send(10)\n"
                  "ch.send(20)\n"
                  "print(ch.recv())\n"
                  "print(ch.recv())",
        .label = "Channel(2) -> single-coro send+recv",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_chan_recv_match_uses_raw_opcode) {
    const char *src = "const ch: Channel<i64?> = Channel(2)\n"
                      "ch.send(0)\n"
                      "ch.send(null)\n"
                      "var zero = match (ch.recv()) {\n"
                      "  Recv.Value { value: v } -> v\n"
                      "  _ -> -1\n"
                      "}\n"
                      "var nil = match (ch.recv()) {\n"
                      "  Recv.Value { value: v } -> v\n"
                      "  _ -> 7\n"
                      "}\n"
                      "print(zero)\n"
                      "print(nil == null)\n";

    XrProto *p_xi = compile_xi(src);
    REQUIRE(p_xi != NULL, "Xi pipeline returned NULL for raw recv match");

    int raw_recv_count = proto_opcode_count(p_xi, OP_CHAN_RECV);
    int invoke_count = proto_opcode_count(p_xi, OP_INVOKE);
    fprintf(stderr, "  raw recv opcodes=%d invoke opcodes=%d\n", raw_recv_count, invoke_count);
    CHECK(raw_recv_count == 2, "recv+match should lower to 2 raw OP_CHAN_RECV, got %d",
          raw_recv_count);
    CHECK(invoke_count == 0, "recv+match must not fall back to method dispatch, got %d OP_INVOKE",
          invoke_count);

    xr_instruction_unit_free(p_xi);
}

TEST(cmp_go_simple) {
    /* go fn() — basic coroutine spawn + await.
     * Output ordering may differ; only verify compiles and runs. */
    run_compare((CompareSpec) {
        .source = "fn worker() { print(\"worker\") }\n"
                  "var task = go worker()\n"
                  "await task\n"
                  "print(\"done\")",
        .label = "go fn() -> basic spawn + await",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false, /* scheduling non-deterministic */
    });
}

TEST(cmp_go_with_chan) {
    /* go + channel: producer/consumer pattern */
    run_compare((CompareSpec) {
        .source = "fn producer(ch: Channel<i64>) { ch.send(42) }\n"
                  "const ch: Channel<i64> = Channel(1)\n"
                  "var task = go producer(ch)\n"
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
    run_compare((CompareSpec) {
        .source = "print(cancelled())",
        .label = "cancelled() -> false in main",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = true,
    });
}

TEST(cmp_scope_basic) {
    /* scope { body } — basic structured concurrency block */
    run_compare((CompareSpec) {
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
    run_compare((CompareSpec) {
        .source = "fn producer(ch: Channel<i64>) {\n"
                  "  ch.send(42)\n"
                  "}\n"
                  "const ch: Channel<i64> = Channel(1)\n"
                  "go producer(ch)\n"
                  "select {\n"
                  "  msg from ch -> {\n"
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
    run_compare((CompareSpec) {
        .source = "fn double(x: i64) -> i64 {\n"
                  "  return x * 2\n"
                  "}\n"
                  "var t1 = go double(10)\n"
                  "var t2 = go double(20)\n"
                  "var r = await [t1, t2]\n"
                  "print(r)",
        .label = "await all: wait for multiple tasks",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false, /* scheduling differs */
    });
}

TEST(cmp_await_any) {
    /* await any [t1, t2] — wait for first */
    run_compare((CompareSpec) {
        .source = "fn double(x: i64) -> i64 {\n"
                  "  return x * 2\n"
                  "}\n"
                  "var t1 = go double(10)\n"
                  "var t2 = go double(20)\n"
                  "var r = await any [t1, t2]\n"
                  "print(r)",
        .label = "await any: wait for first task",
        .expect_xi_success = true,
        .min_similarity = 0.1,
        .check_exec = false, /* scheduling differs */
    });
}

/* ========== Instruction Fusion Tests ========== */

TEST(fusion_addi) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return x + 1 }\nprint(f(10))",
        .label = "ADDI: x + small_const",
        .expect_op = OP_ADDI,
        .check_exec = true,
    });
}

TEST(fusion_addi_commutative) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return 3 + x }\nprint(f(10))",
        .label = "ADDI commutative: small_const + x",
        .expect_op = OP_ADDI,
        .check_exec = true,
    });
}

TEST(fusion_subi) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return x - 3 }\nprint(f(10))",
        .label = "SUBI: x - small_const",
        .expect_op = OP_SUBI,
        .check_exec = true,
    });
}

TEST(fusion_muli) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return x * 7 }\nprint(f(5))",
        .label = "MULI: x * small_const",
        .expect_op = OP_MULI,
        .check_exec = true,
    });
}

TEST(fusion_muli_commutative) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return 7 * x }\nprint(f(5))",
        .label = "MULI commutative: small_const * x",
        .expect_op = OP_MULI,
        .check_exec = true,
    });
}

TEST(fusion_addk) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return x + 40000 }\nprint(f(10))",
        .label = "ADDK: x + large_const",
        .expect_op = OP_ADDK,
        .check_exec = true,
    });
}

TEST(fusion_addk_commutative) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return 40000 + x }\nprint(f(10))",
        .label = "ADDK commutative: large_const + x",
        .expect_op = OP_ADDK,
        .check_exec = true,
    });
}

TEST(fusion_subk) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return x - 40000 }\nprint(f(50000))",
        .label = "SUBK: x - large_const",
        .expect_op = OP_SUBK,
        .check_exec = true,
    });
}

TEST(fusion_mulk) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return x * 40000 }\nprint(f(3))",
        .label = "MULK: x * large_const",
        .expect_op = OP_MULK,
        .check_exec = true,
    });
}

TEST(fusion_mulk_commutative) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return 40000 * x }\nprint(f(3))",
        .label = "MULK commutative: large_const * x",
        .expect_op = OP_MULK,
        .check_exec = true,
    });
}

TEST(fusion_divk) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return x / 40000 }\nprint(f(80000))",
        .label = "DIVK: x / large_const",
        .expect_op = OP_DIVK,
        .check_exec = true,
    });
}

TEST(fusion_modk) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) -> i64 { return x % 40000 }\nprint(f(123456))",
        .label = "MODK: x % large_const",
        .expect_op = OP_MODK,
        .check_exec = true,
    });
}

TEST(fusion_lti_branch) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) { if (x < 10) { print(1) } else { print(0) } }\nf(5)",
        .label = "LTI: branch x < small_const",
        .expect_op = OP_LTI,
        .check_exec = true,
    });
}

TEST(fusion_eqi_branch) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) { if (x == 5) { print(1) } else { print(0) } }\nf(5)",
        .label = "EQI: branch x == small_const",
        .expect_op = OP_EQI,
        .check_exec = true,
    });
}

TEST(fusion_lei_branch) {
    run_fusion((FusionSpec) {
        .source = "fn f(x: i64) { if (x <= 10) { print(1) } else { print(0) } }\nf(10)",
        .label = "LEI: branch x <= small_const",
        .expect_op = OP_LEI,
        .check_exec = true,
    });
}

/* ========== Summary Report ========== */

static void print_summary(void) {
    printf("\n=== %d/%d Xi Compare tests passed", tests_passed, tests_passed + tests_failed);
    printf(" ===\n");
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Compare: Compiler Wrapper vs Direct Pipeline ===\n\n");

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
    run_cmp_dynamic_as_checked_match();
    run_cmp_numeric_as_witness_conversion();
    run_cmp_numeric_as_witness_truncation();

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
    run_cmp_compose_multiple_incoming();
    run_cmp_callable_capture_rebind();
    run_cmp_callable_nested_return();
    run_cmp_callable_closed_target_union();
    run_cmp_callable_unknown_invalidates_exact();

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
    run_cmp_super_multiple_levels();
    run_cmp_super_ordinary_method();
    run_cmp_super_constructor_error();
    run_cmp_super_owner_physical();

    /* Enum */
    run_cmp_enum_basic();

    /* For-in map */
    run_cmp_for_in_map();

    /* Additional closures */
    run_cmp_closure_adder();
    run_cmp_closure_accumulator();

    /* Misc patterns */
    run_cmp_nested_closure();

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
    run_cmp_chan_recv_match_uses_raw_opcode();
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
