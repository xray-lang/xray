/*
 * test_xi_lower.c - Unit tests for AST to Xi IR lowering
 *
 * Uses a minimal isolate + analyzer to test the full lowering pipeline.
 * Each test parses a small xray source snippet, runs the analyzer,
 * lowers to Xi IR, and verifies the dump output.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_lower.h"
#include "../../../src/ir/xi_own.h"
#include "../../../src/analysis/xglobal_producer.h"
#include "../../../src/analysis/xglobal_summary.h"
#include "../../../src/frontend/canonical/xcanon.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/value/xstruct_layout.h"
#include "../../../src/frontend/parser/xast_nodes.h"
#include "../../../src/frontend/parser/xast_types.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/frontend/analyzer/xa_typed_program.h"
#include "../../../src/frontend/analyzer/xa_intrinsic_registry.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/module/xmodule_graph.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../include/xray_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

/* Parse source, run analyzer, lower to Xi, dump and return XiFunc.
 * Caller must call xi_func_free() on the result. */
static XiFunc *lower_source(const char *source) {
    assert(g_iso != NULL);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);

    /* Parse */
    AstNode *program = xr_parse(session, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %s\n", source);
        return NULL;
    }

    /* Analyze */
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer) {
        fprintf(stderr, "  ANALYZER ALLOC FAILED\n");
        xr_program_destroy(program);
        return NULL;
    }
    xa_analyzer_analyze(analyzer, "test.xr", program);

    /* Re-install parse arena for canonicalizer allocations */
    XrCompilerSessionScope canon_scope;
    bool has_canon_scope =
        program->type == AST_PROGRAM && program->as.program.arena &&
        xr_compiler_session_push_arena(session, program->as.program.arena, "test.xr", &canon_scope);

    /* Canonicalize + Lower */
    xr_canon_program(program, analyzer, session);
    if (has_canon_scope)
        xr_compiler_session_pop_arena(&canon_scope);
    XaTypedProgramPublishResult typed = xa_typed_program_publish(analyzer, program, NULL, 0);
    XiFunc *func = typed.program ? xi_lower_program(typed.program, g_iso, false) : NULL;
    if (!typed.program) {
        fprintf(stderr, "  TYPED PROGRAM REJECTED (%s): %s\n",
                xa_typed_program_reason_name(typed.reason), typed.detail ? typed.detail : "");
    }
    xa_typed_program_free(typed.program);
    if (!func) {
        fprintf(stderr, "  LOWER FAILED for: %s\n", source);
        xa_analyzer_free(analyzer);
        xr_program_destroy(program);
        return NULL;
    }

    /* Dump to stdout for visual verification */
    xi_func_dump(func, stdout);

    /* Cleanup AST and analyzer (Xi IR is independent) */
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    return func;
}

typedef void (*GlobalEvidenceTestMutator)(XgGlobalEvidence *evidence);

static XiFunc *lower_source_with_global_evidence_ex(const char *source, XgGlobalEvidence *out_ev,
                                                    GlobalEvidenceTestMutator mutate_evidence) {
    assert(g_iso != NULL);
    assert(out_ev != NULL);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);

    AstNode *program = xr_parse(session, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %s\n", source);
        return NULL;
    }

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.source_path = "test.xr";
    spec.ast = program;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;
    if (!xg_global_evidence_build_from_module_graph(out_ev, &graph, XG_BUILD_NATIVE_RELEASE, 0)) {
        fprintf(stderr, "  GLOBAL EVIDENCE FAILED for: %s\n", source);
        xr_program_destroy(program);
        return NULL;
    }
    if (mutate_evidence)
        mutate_evidence(out_ev);

    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer) {
        fprintf(stderr, "  ANALYZER ALLOC FAILED\n");
        xg_global_evidence_free(out_ev);
        xr_program_destroy(program);
        return NULL;
    }
    xa_analyzer_analyze(analyzer, "test.xr", program);

    XrCompilerSessionScope canon_scope;
    bool has_canon_scope =
        program->type == AST_PROGRAM && program->as.program.arena &&
        xr_compiler_session_push_arena(session, program->as.program.arena, "test.xr", &canon_scope);
    xr_canon_program(program, analyzer, session);
    if (has_canon_scope)
        xr_compiler_session_pop_arena(&canon_scope);

    XaTypedProgramPublishResult typed = xa_typed_program_publish(analyzer, program, out_ev, 1);
    XiFunc *func = typed.program ? xi_lower_program(typed.program, g_iso, false) : NULL;
    if (!typed.program) {
        fprintf(stderr, "  TYPED PROGRAM REJECTED (%s): %s\n",
                xa_typed_program_reason_name(typed.reason), typed.detail ? typed.detail : "");
    }
    xa_typed_program_free(typed.program);
    if (!func) {
        fprintf(stderr, "  LOWER FAILED for: %s\n", source);
        xa_analyzer_free(analyzer);
        xg_global_evidence_free(out_ev);
        xr_program_destroy(program);
        return NULL;
    }

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
    return func;
}

static XiFunc *lower_source_with_global_evidence(const char *source, XgGlobalEvidence *out_ev) {
    return lower_source_with_global_evidence_ex(source, out_ev, NULL);
}

static XiFunc *func_tree_find_func_name(XiFunc *f, const char *name) {
    if (!f || !name)
        return NULL;
    if (f->name && strcmp(f->name, name) == 0)
        return f;
    for (uint16_t i = 0; i < f->nchildren; i++) {
        XiFunc *child = func_tree_find_func_name(f->children[i], name);
        if (child)
            return child;
    }
    return NULL;
}

static int func_count_trivial_phis(const XiFunc *f) {
    int count = 0;
    if (!f)
        return 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            const XiValue *unique = NULL;
            bool trivial = true;
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                const XiValue *arg = phi->value.args[a];
                if (!arg || arg == &phi->value || arg == unique)
                    continue;
                if (unique) {
                    trivial = false;
                    break;
                }
                unique = arg;
            }
            if (trivial && unique)
                count++;
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++)
        count += func_count_trivial_phis(f->children[i]);
    return count;
}

static XiFunc *func_tree_find_xg_body(XiFunc *f, XgFuncId body_func_id) {
    if (!f || body_func_id == XG_NO_ID)
        return NULL;
    if (f->xg_body_func_id == body_func_id)
        return f;
    for (uint16_t i = 0; i < f->nchildren; i++) {
        XiFunc *child = func_tree_find_xg_body(f->children[i], body_func_id);
        if (child)
            return child;
    }
    return NULL;
}

static int func_collect_method_calls(XiFunc *f, XiValue **out, int capacity) {
    int count = 0;
    if (!f)
        return 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *value = blk->values[i];
            if (!value || (value->op != XI_CALL_METHOD && value->op != XI_CALL_METHOD_DIRECT))
                continue;
            if (out && count < capacity)
                out[count] = value;
            count++;
        }
    }
    return count;
}

static uint32_t global_evidence_body_declared_name_id(const XgGlobalEvidence *ev,
                                                      const XgBodySummary *body) {
    if (!ev || !body)
        return 0;
    if (body->kind == XG_BODY_METHOD) {
        for (uint32_t i = 0; i < ev->nmethods; i++) {
            if (ev->methods[i].method_id == body->owner_method_id)
                return ev->methods[i].name_id;
        }
        return 0;
    }
    for (uint32_t i = 0; i < ev->ndecls; i++) {
        if (ev->decls[i].decl_id == body->owner_decl_id)
            return ev->decls[i].name_id;
    }
    return 0;
}

static const XgClassSummary *global_evidence_find_class_by_name(const XgGlobalEvidence *ev,
                                                                const char *name) {
    const XgClassSummary *match = NULL;
    uint32_t name_id = name ? xg_name_id(name) : 0;
    if (!ev || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        const XgClassSummary *cls = &ev->classes[i];
        if (cls->name_id != name_id)
            continue;
        if (match)
            return NULL;
        match = cls;
    }
    return match;
}

static const XgClassFieldSummary *
global_evidence_find_class_field_by_name(const XgGlobalEvidence *ev, XgClassId owner_class_id,
                                         const char *name) {
    const XgClassFieldSummary *match = NULL;
    uint32_t name_id = name ? xg_name_id(name) : 0;
    if (!ev || owner_class_id == XG_NO_ID || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < ev->nclass_fields; i++) {
        const XgClassFieldSummary *field = &ev->class_fields[i];
        if (field->owner_class_id != owner_class_id || field->name_id != name_id)
            continue;
        if (match)
            return NULL;
        match = field;
    }
    return match;
}

static void scramble_legacy_xi_identity_fields(XgGlobalEvidence *ev) {
    uint32_t stale_name_id = xg_name_id("<stale-xi-identity>");
    if (!ev)
        return;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        if (ev->bodies[i].kind == XG_BODY_MODULE_INIT)
            continue;
        ev->bodies[i].name_id = stale_name_id;
        ev->bodies[i].source_span_id = UINT32_MAX;
    }
    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        ev->callsites[i].source_span_id = UINT32_MAX;
        ev->callsites[i].body_ordinal = UINT32_MAX;
        ev->callsites[i].method_name_id = stale_name_id;
        ev->callsites[i].arg_count = UINT16_MAX;
    }
}

static int func_tree_has_op(XiFunc *f, uint16_t op) {
    if (!f)
        return 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i] && blk->values[i]->op == op)
                return 1;
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (func_tree_has_op(f->children[i], op))
            return 1;
    }
    return 0;
}

static XiValue *func_tree_find_op(XiFunc *f, uint16_t op) {
    if (!f)
        return NULL;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i] && blk->values[i]->op == op)
                return blk->values[i];
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        XiValue *v = func_tree_find_op(f->children[i], op);
        if (v)
            return v;
    }
    return NULL;
}

static XiValue *func_tree_find_method(XiFunc *f, const char *name) {
    if (!f || !name)
        return NULL;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v && v->op == XI_CALL_METHOD && v->aux && strcmp((const char *) v->aux, name) == 0)
                return v;
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        XiValue *v = func_tree_find_method(f->children[i], name);
        if (v)
            return v;
    }
    return NULL;
}

static int func_tree_has_builtin_name(XiFunc *f, const char *name) {
    if (!f || !name)
        return 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v && v->op == XI_CALL_BUILTIN && v->aux && strcmp((const char *) v->aux, name) == 0)
                return 1;
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (func_tree_has_builtin_name(f->children[i], name))
            return 1;
    }
    return 0;
}

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- %s ---\n", #name);                                                             \
        test_##name();                                                                             \
        printf("  PASS\n\n");                                                                      \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* ========== Tests ========== */

TEST(simple_arithmetic) {
    XiFunc *f = lower_source("var x = 1 + 2\nvar y = x * 3\nprint(y)");
    assert(f != NULL);
    assert(f->nblocks >= 1);
    /* Entry block should have: const 1, const 2, add, const 3, mul, print */
    assert(f->entry->nvalues >= 5);
    xi_func_free(f);
}

TEST(variable_assignment) {
    XiFunc *f = lower_source("var x = 10\nx = x + 5\nprint(x)");
    assert(f != NULL);
    assert(f->nblocks >= 1);
    xi_func_free(f);
}

TEST(if_else) {
    XiFunc *f = lower_source("var x = 10\n"
                             "if (x > 5) {\n"
                             "    print(1)\n"
                             "} else {\n"
                             "    print(0)\n"
                             "}\n");
    assert(f != NULL);
    /* Should have: entry, then, else, merge blocks */
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(while_loop) {
    XiFunc *f = lower_source("var i = 0\n"
                             "while (i < 10) {\n"
                             "    i = i + 1\n"
                             "}\n"
                             "print(i)\n");
    assert(f != NULL);
    /* Should have: entry, cond, body, exit blocks */
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(loop_invariant_rc_trivial_phi_is_rewritten) {
    XiFunc *f = lower_source("fn invariant(xs: Array<int>) -> int {\n"
                             "    var i = 0\n"
                             "    while (i < 2) {\n"
                             "        print(len(xs))\n"
                             "        i = i + 1\n"
                             "    }\n"
                             "    return len(xs)\n"
                             "}\n"
                             "print(invariant([1, 2]))\n");
    assert(f != NULL);
    XiFunc *invariant = func_tree_find_func_name(f, "invariant");
    assert(invariant != NULL);
    assert(func_count_trivial_phis(invariant) == 0 &&
           "sealed loop headers must rewrite all users of trivial RC phis");
    xi_func_free(f);
}

TEST(for_loop) {
    XiFunc *f = lower_source("var sum = 0\n"
                             "for (var i = 0; i < 5; i = i + 1) {\n"
                             "    sum = sum + i\n"
                             "}\n"
                             "print(sum)\n");
    assert(f != NULL);
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(nested_if) {
    XiFunc *f = lower_source("var x = 10\n"
                             "if (x > 5) {\n"
                             "    if (x > 8) {\n"
                             "        print(2)\n"
                             "    } else {\n"
                             "        print(1)\n"
                             "    }\n"
                             "} else {\n"
                             "    print(0)\n"
                             "}\n");
    assert(f != NULL);
    assert(f->nblocks >= 5);
    xi_func_free(f);
}

TEST(bool_literals) {
    XiFunc *f = lower_source("var a = true\n"
                             "var b = false\n"
                             "var c = !a\n"
                             "print(c)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(float_arithmetic) {
    XiFunc *f = lower_source("var x = 3.14\n"
                             "var y = x * 2.0\n"
                             "print(y)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(string_const) {
    XiFunc *f = lower_source("var msg = \"hello\"\n"
                             "print(msg)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(comparison_ops) {
    XiFunc *f = lower_source("var a = 1\n"
                             "var b = 2\n"
                             "var eq = a == b\n"
                             "var ne = a != b\n"
                             "var lt = a < b\n"
                             "print(eq)\n"
                             "print(ne)\n"
                             "print(lt)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(compound_assignment) {
    XiFunc *f = lower_source("var x = 10\n"
                             "x += 5\n"
                             "x -= 2\n"
                             "print(x)\n");
    assert(f != NULL);
    assert(f->nblocks >= 1);
    /* const 10, const 5, add, const 2, sub, print */
    assert(f->entry->nvalues >= 5);
    xi_func_free(f);
}

TEST(inc_dec) {
    XiFunc *f = lower_source("var x = 0\n"
                             "x++\n"
                             "x++\n"
                             "x--\n"
                             "print(x)\n");
    assert(f != NULL);
    /* const 0, [const 1, add] * 2, [const 1, sub], print */
    assert(f->entry->nvalues >= 6);
    xi_func_free(f);
}

TEST(ternary_expr) {
    XiFunc *f = lower_source("var x = 10\n"
                             "var y = (x > 5) ? 1 : 0\n"
                             "print(y)\n");
    assert(f != NULL);
    /* ternary produces: entry, then, else, merge blocks */
    assert(f->nblocks >= 4);
    xi_func_free(f);
}

TEST(break_continue) {
    XiFunc *f = lower_source("var i = 0\n"
                             "while (i < 100) {\n"
                             "    i = i + 1\n"
                             "    if (i == 5) {\n"
                             "        break\n"
                             "    }\n"
                             "    if (i == 3) {\n"
                             "        continue\n"
                             "    }\n"
                             "}\n"
                             "print(i)\n");
    assert(f != NULL);
    assert(f->nblocks >= 4);
    xi_func_free(f);
}

TEST(nested_while) {
    XiFunc *f = lower_source("var sum = 0\n"
                             "var i = 0\n"
                             "while (i < 3) {\n"
                             "    var j = 0\n"
                             "    while (j < 3) {\n"
                             "        sum = sum + 1\n"
                             "        j = j + 1\n"
                             "    }\n"
                             "    i = i + 1\n"
                             "}\n"
                             "print(sum)\n");
    assert(f != NULL);
    /* Two nested loops: each needs cond + body + exit blocks */
    assert(f->nblocks >= 6);
    xi_func_free(f);
}

TEST(type_propagation) {
    XiFunc *f = lower_source("var a = 1\n"
                             "var b = 2.0\n"
                             "var c = a + a\n"
                             "var d = b + b\n"
                             "var e = a > 0\n"
                             "print(c)\n"
                             "print(d)\n"
                             "print(e)\n");
    assert(f != NULL);
    /* Verify types: walk entry block values */
    XiBlock *b0 = f->entry;
    assert(b0 != NULL);
    /* Find add operations and check their types */
    int found_int_add = 0, found_float_add = 0, found_bool_gt = 0;
    for (uint32_t i = 0; i < b0->nvalues; i++) {
        XiValue *v = b0->values[i];
        if (v->op == XI_ADD && v->type && v->type->kind == XR_KIND_INT)
            found_int_add = 1;
        if (v->op == XI_ADD && v->type && v->type->kind == XR_KIND_FLOAT)
            found_float_add = 1;
        if (v->op == XI_GT && v->type && v->type->kind == XR_KIND_BOOL)
            found_bool_gt = 1;
    }
    assert(found_int_add && "int + int should produce int");
    assert(found_float_add && "float + float should produce float");
    assert(found_bool_gt && "a > 0 should produce bool");
    xi_func_free(f);
}

TEST(array_literal) {
    XiFunc *f = lower_source("var arr = [1, 2, 3]\n"
                             "print(arr)\n");
    assert(f != NULL);
    /* Should have: CONST*3 elements, ARRAY_NEW, INDEX_SET*3, PRINT */
    int found_array_new = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_ARRAY_NEW)
            found_array_new = 1;
    }
    assert(found_array_new && "should have ARRAY_NEW op");
    xi_func_free(f);
}

TEST(index_access) {
    XiFunc *f = lower_source("var arr = [10, 20, 30]\n"
                             "var x = arr[1]\n"
                             "print(x)\n");
    assert(f != NULL);
    int found_index_get = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_INDEX_GET)
            found_index_get = 1;
    }
    assert(found_index_get && "should have INDEX_GET op");
    xi_func_free(f);
}

TEST(member_access) {
    XiFunc *f = lower_source("var arr = [1, 2, 3]\n"
                             "var n = len(arr)\n"
                             "print(n)\n");
    assert(f != NULL);
    int found_len = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        XiValue *v = f->entry->values[i];
        if (v->op == XI_LEN)
            found_len = 1;
    }
    assert(found_len && "should lower len() as a compiler-known builtin");
    xi_func_free(f);
}

TEST(member_access_field_symbols_are_distinct) {
    XiFunc *f = lower_source("fn worker() -> int {\n"
                             "    Coro.yield()\n"
                             "    return 1\n"
                             "}\n"
                             "var task = go worker()\n"
                             "print(task.done)\n"
                             "print(task.status)\n");
    assert(f != NULL);

    int64_t done_symbol = 0;
    int64_t status_symbol = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; blk && i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_LOAD_FIELD || !v->aux)
                continue;
            const char *name = (const char *) v->aux;
            if (strcmp(name, "done") == 0)
                done_symbol = v->aux_int;
            if (strcmp(name, "status") == 0)
                status_symbol = v->aux_int;
        }
    }

    assert(done_symbol > 0 && "task.done should carry a field symbol");
    assert(status_symbol > 0 && "task.status should carry a field symbol");
    assert(done_symbol != status_symbol && "different task fields need distinct symbols");
    xi_func_free(f);
}

TEST(bytes_new_low_level_methods_lower_to_semantic_ops) {
    XiFunc *f = lower_source("fn exerciseBytes() {\n"
                             "  var src = Array<byte>(8)\n"
                             "  var view: Slice<byte> = src[:]\n"
                             "  var dst = Array<byte>(0)\n"
                             "  var h = view.load<u16>(0, Endian.LE)\n"
                             "  var a = view.load<u32>(0, Endian.LE)\n"
                             "  var b = view.load<u64>(0, Endian.LE)\n"
                             "  view.store<u16>(6, h, Endian.LE)\n"
                             "  dst.appendFrom(view[0:2])\n"
                             "  dst.repeatFrom(2, 4)\n"
                             "  print(h)\n"
                             "  print(a)\n"
                             "  print(b)\n"
                             "}\n"
                             "exerciseBytes()\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_BYTE_SLICE_LOAD_U16) &&
           "load<u16> should lower to Array<byte> op");
    assert(func_tree_has_op(f, XI_BYTE_SLICE_LOAD_U32) &&
           "load<u32> should lower to Array<byte> op");
    assert(func_tree_has_op(f, XI_BYTE_SLICE_LOAD_U64) &&
           "load<u64> should lower to Array<byte> op");
    assert(func_tree_has_op(f, XI_BYTE_SLICE_STORE_U16) &&
           "store<u16> should lower to Array<byte> op");
    XiValue *load_u16 = func_tree_find_op(f, XI_BYTE_SLICE_LOAD_U16);
    XiValue *load_u32 = func_tree_find_op(f, XI_BYTE_SLICE_LOAD_U32);
    XiValue *load_u64 = func_tree_find_op(f, XI_BYTE_SLICE_LOAD_U64);
    XiValue *store_u16 = func_tree_find_op(f, XI_BYTE_SLICE_STORE_U16);
    assert(load_u16 && load_u16->xa_intrinsic_id == XA_INTRINSIC_BYTE_SLICE_LOAD &&
           "typed load must retain the canonical analyzer intrinsic id");
    assert(load_u32 && load_u32->xa_intrinsic_id == XA_INTRINSIC_BYTE_SLICE_LOAD);
    assert(load_u64 && load_u64->xa_intrinsic_id == XA_INTRINSIC_BYTE_SLICE_LOAD);
    assert(store_u16 && store_u16->xa_intrinsic_id == XA_INTRINSIC_BYTE_SLICE_STORE &&
           "typed store must retain the canonical analyzer intrinsic id");
    assert(func_tree_has_op(f, XI_BYTE_ARRAY_APPEND_FROM) &&
           "appendFrom should lower to stable Array<byte> append op");
    assert(func_tree_has_op(f, XI_BYTE_ARRAY_REPEAT_FROM) &&
           "repeatFrom should lower to stable Array<byte> repeat op");
    assert(!func_tree_find_method(f, "appendFrom") &&
           "appendFrom should not remain an explicit method call");
    assert(!func_tree_find_method(f, "repeatFrom") &&
           "repeatFrom should not remain an explicit method call");
    assert(!func_tree_find_method(f, "load") && !func_tree_find_method(f, "store") &&
           "canonical byte slice memory operations must not leak as ordinary calls");
    assert(!func_tree_has_builtin_name(f, "bytes_load_u16_le") &&
           !func_tree_has_builtin_name(f, "bytes_load_u32_le") &&
           "load should not lower through string builtin");
    xi_func_free(f);
}

TEST(atomic_methods_lower_to_nothrow_canonical_ops) {
    XiFunc *f = lower_source("fn update(counter: Atomic<int>) -> int {\n"
                             "  var before = counter.load(Ordering.Relaxed)\n"
                             "  counter.store(before + 1, Ordering.Release)\n"
                             "  var old = counter.fetchAdd(2, Ordering.AcquireRelease)\n"
                             "  var swapped = counter.swap(old, Ordering.SeqCst)\n"
                             "  var (seen, ok) = counter.compareExchange(swapped, 9)\n"
                             "  return seen\n"
                             "}\n");
    assert(f != NULL);

    XiValue *load = func_tree_find_op(f, XI_ATOMIC_LOAD);
    XiValue *store = func_tree_find_op(f, XI_ATOMIC_STORE);
    XiValue *rmw = func_tree_find_op(f, XI_ATOMIC_RMW);
    assert(load && load->xa_intrinsic_id == XA_INTRINSIC_ATOMIC_LOAD);
    assert(store && store->xa_intrinsic_id == XA_INTRINSIC_ATOMIC_STORE);
    assert(rmw && (rmw->xa_intrinsic_id == XA_INTRINSIC_ATOMIC_FETCH_ADD ||
                   rmw->xa_intrinsic_id == XA_INTRINSIC_ATOMIC_SWAP ||
                   rmw->xa_intrinsic_id == XA_INTRINSIC_ATOMIC_COMPARE_EXCHANGE));
    assert((load->flags & XI_FLAG_MAY_THROW) == 0 && (store->flags & XI_FLAG_MAY_THROW) == 0 &&
           (rmw->flags & XI_FLAG_MAY_THROW) == 0 &&
           "typed Atomic operations are intrinsically nothrow");
    assert(!func_tree_find_op(f, XI_ERR_CHECK) &&
           "nothrow Atomic operations must not create synthetic error edges");
    assert(!func_tree_find_method(f, "load") && !func_tree_find_method(f, "store") &&
           !func_tree_find_method(f, "fetchAdd") &&
           "canonical Atomic operations must not leak as ordinary method calls");
    xi_func_free(f);
}

TEST(user_method_named_fetch_add_remains_ordinary_call) {
    XiFunc *f = lower_source("class Counter {\n"
                             "  fetchAdd(delta: int) -> int { return delta }\n"
                             "}\n"
                             "fn use(counter: Counter) -> int {\n"
                             "  return counter.fetchAdd(2)\n"
                             "}\n");
    assert(f != NULL);
    assert(func_tree_find_method(f, "fetchAdd") != NULL &&
           "a user declaration with the same spelling must remain an ordinary method call");
    assert(!func_tree_has_op(f, XI_ATOMIC_RMW) &&
           "Atomic semantics must come from resolved receiver identity, never method spelling");
    xi_func_free(f);
}

TEST(exact_integer_bit_methods_lower_to_typed_semantic_ops) {
    XiFunc *f = lower_source("var octet: u8 = 129\n"
                             "var rotated = octet.rotateLeft(-1)\n"
                             "var restored = rotated.rotateRight(15)\n"
                             "var half: i16 = -2\n"
                             "var swapped = half.byteswap()\n"
                             "print(restored)\n"
                             "print(swapped)\n"
                             "print(octet.popcount())\n"
                             "print(octet.leadingZeros())\n"
                             "var wide: int = 256\n"
                             "print(wide.trailingZeros())\n");
    assert(f != NULL);

    XiValue *rotl = func_tree_find_op(f, XI_BIT_ROTL);
    XiValue *rotr = func_tree_find_op(f, XI_BIT_ROTR);
    XiValue *bswap = func_tree_find_op(f, XI_BIT_BSWAP);
    XiValue *popcount = func_tree_find_op(f, XI_BIT_POPCOUNT);
    XiValue *clz = func_tree_find_op(f, XI_BIT_CLZ);
    XiValue *ctz = func_tree_find_op(f, XI_BIT_CTZ);
    assert(rotl && rotr && bswap && popcount && clz && ctz &&
           "all exact-width bit methods should lower to stable Xi ops");
    assert(rotl->aux_int == XR_NATIVE_U8 && rotr->aux_int == XR_NATIVE_U8 &&
           "rotate ops should retain the exact receiver width in aux_int");
    assert(rotl->type && rotl->type->scalar_rep == XR_NATIVE_U8 && rotr->type &&
           rotr->type->scalar_rep == XR_NATIVE_U8 &&
           "rotate results should preserve the exact receiver type");
    assert(bswap->aux_int == XR_NATIVE_I16 && bswap->type &&
           bswap->type->scalar_rep == XR_NATIVE_I16 &&
           "byteswap should preserve signed exact-width receiver type");
    assert(popcount->aux_int == XR_NATIVE_U8 && popcount->type &&
           popcount->type->kind == XR_KIND_INT && popcount->type->scalar_rep == 0 &&
           "bit queries should return canonical int while retaining receiver width metadata");
    assert(ctz->aux_int == 0 && ctz->type && ctz->type->scalar_rep == 0 &&
           "canonical int should retain its zero native-width tag and 64-bit bit semantics");
    assert(!func_tree_find_method(f, "rotateLeft") && !func_tree_find_method(f, "byteswap") &&
           !func_tree_find_method(f, "popcount") &&
           "exact-width bit methods should not survive as string-dispatched method calls");
    assert(rotl->xa_intrinsic_id == XA_INTRINSIC_BITS_ROTATE_LEFT &&
           rotr->xa_intrinsic_id == XA_INTRINSIC_BITS_ROTATE_RIGHT &&
           bswap->xa_intrinsic_id == XA_INTRINSIC_BITS_BYTESWAP &&
           popcount->xa_intrinsic_id == XA_INTRINSIC_BITS_POPCOUNT &&
           clz->xa_intrinsic_id == XA_INTRINSIC_BITS_LEADING_ZEROS &&
           ctz->xa_intrinsic_id == XA_INTRINSIC_BITS_TRAILING_ZEROS &&
           "bit Xi ops must carry analyzer-owned canonical semantic identities");
    xi_func_free(f);
}

TEST(throw_stmt) {
    XiFunc *f = lower_source("enum LowerErr { Error }\n"
                             "var x = 1\n"
                             "if (x == 0) {\n"
                             "    throw LowerErr.Error\n"
                             "}\n"
                             "print(x)\n");
    assert(f != NULL);
    /* Should have: entry + then(throw) + else + merge */
    assert(f->nblocks >= 3);
    /* Find the throw block — should use XI_ERR_RETURN (value-return
     * error channel) and terminate with RETURN kind. */
    int found_err_return = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_ERR_RETURN) {
                found_err_return = 1;
                assert(blk->kind == XI_BLOCK_RETURN);
            }
        }
    }
    assert(found_err_return && "should have ERR_RETURN op");
    xi_func_free(f);
}

TEST(for_in_loop) {
    XiFunc *f = lower_source("var arr = [10, 20, 30]\n"
                             "for (x in arr) {\n"
                             "    print(x)\n"
                             "}\n");
    assert(f != NULL);
    /* Should have: entry, cond, body, incr, exit blocks (loop structure) */
    assert(f->nblocks >= 4);
    /* Array for-in is desugared to an index-based loop with canonical len(). */
    int found_len = 0, found_index_get = 0, found_lt = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            uint16_t op = blk->values[i]->op;
            if (op == XI_LEN)
                found_len = 1;
            if (op == XI_INDEX_GET)
                found_index_get = 1;
            if (op == XI_LT)
                found_lt = 1;
        }
    }
    assert(found_len && "should use canonical len builtin");
    assert(found_index_get && "should have INDEX_GET for coll[idx]");
    assert(found_lt && "should have LT for idx < len");
    xi_func_free(f);
}

TEST(nullish_coalesce) {
    XiFunc *f = lower_source("var x: int? = null\n"
                             "var y = x ?? 42\n"
                             "print(y)\n");
    assert(f != NULL);
    /* Canonicalized to: x == null ? 42 : x → ternary with EQ null check.
     * Produces: entry, then_branch, else_branch, merge blocks. */
    assert(f->nblocks >= 3);
    /* Verify EQ op exists (null-check from canonicalized ternary) */
    int found_eq = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_EQ)
                found_eq = 1;
        }
    }
    assert(found_eq && "should have EQ op for null check");
    xi_func_free(f);
}

TEST(map_literal) {
    XiFunc *f = lower_source("var m = #{\"a\": 1, \"b\": 2}\n"
                             "print(m)\n");
    assert(f != NULL);
    int found_map_new = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_MAP_NEW)
            found_map_new = 1;
    }
    assert(found_map_new && "should have MAP_NEW op");
    xi_func_free(f);
}

TEST(match_expr) {
    XiFunc *f = lower_source("var x = 2\n"
                             "var y = match (x) {\n"
                             "    1 -> 10,\n"
                             "    2 -> 20,\n"
                             "    _ -> 0\n"
                             "}\n"
                             "print(y)\n");
    assert(f != NULL);
    /* Match generates: chain of test blocks + body blocks + merge */
    assert(f->nblocks >= 3);
    /* Verify EQ comparisons for pattern matching */
    int found_eq = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_EQ)
                found_eq++;
        }
    }
    assert(found_eq >= 2 && "should have >= 2 EQ ops for pattern tests");
    xi_func_free(f);
}

TEST(try_catch) {
    XiFunc *f = lower_source("var result = 0\n"
                             "try {\n"
                             "    result = 42\n"
                             "} catch (e) {\n"
                             "    result = -1\n"
                             "}\n"
                             "print(result)\n");
    assert(f != NULL);
    /* try-catch generates: entry, try_blk, catch_blk, merge */
    assert(f->nblocks >= 3);
    /* New error model: catch block uses XI_ERR_CATCH to read the
     * error channel.  No XI_TRY/XI_CATCH (those are for panic). */
    int found_err_catch = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v->op == XI_ERR_CATCH)
                found_err_catch = 1;
        }
    }
    assert(found_err_catch && "should have XI_ERR_CATCH op");
    xi_func_free(f);
}

TEST(try_catch_defer) {
    XiFunc *f = lower_source("var x = 0\n"
                             "defer { x = 9 }\n"
                             "try {\n"
                             "    x = 1\n"
                             "} catch (e) {\n"
                             "    x = 2\n"
                             "}\n");
    assert(f != NULL);
    /* try + catch + merge = at least 3 blocks */
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(object_literal) {
    XiFunc *f = lower_source("var obj = {a: 1, b: 2}\n"
                             "print(obj)\n");
    assert(f != NULL);
    int found_alloc = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_JSON_NEW)
            found_alloc = 1;
    }
    assert(found_alloc && "should have JSON_NEW for object literal");
    xi_func_free(f);
}

TEST(class_field_access_lowers_with_global_evidence_id) {
#define REQUIRE_CLASS_FIELD_EVIDENCE(cond, msg)                                                    \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "class_field_access_lowers_with_global_evidence_id: %s\n", msg);       \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("class Base {\n"
                                          "    wide: int\n"
                                          "    constructor(wide: int) {\n"
                                          "        this.wide = wide\n"
                                          "    }\n"
                                          "}\n"
                                          "\n"
                                          "class Child extends Base {\n"
                                          "    flag: bool\n"
                                          "    constructor(wide: int, flag: bool) {\n"
                                          "        super(wide)\n"
                                          "        this.flag = flag\n"
                                          "    }\n"
                                          "}\n"
                                          "\n"
                                          "fn touch(c: ref Child) -> int {\n"
                                          "    c.flag = false\n"
                                          "    return c.wide\n"
                                          "}\n"
                                          "\n"
                                          "var child = Child(7, true)\n"
                                          "print(touch(ref child))\n",
                                          &ev);
    REQUIRE_CLASS_FIELD_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_CLASS_FIELD_EVIDENCE(ev.nclass_fields == 2, "producer should record class fields");

    const XgClassSummary *base = global_evidence_find_class_by_name(&ev, "Base");
    const XgClassSummary *child = global_evidence_find_class_by_name(&ev, "Child");
    REQUIRE_CLASS_FIELD_EVIDENCE(base != NULL, "Base evidence should be present");
    REQUIRE_CLASS_FIELD_EVIDENCE(child != NULL, "Child evidence should be present");
    REQUIRE_CLASS_FIELD_EVIDENCE(child->parent_class_id == base->class_id,
                                 "Child should retain parent class evidence");

    const XgClassFieldSummary *wide =
        global_evidence_find_class_field_by_name(&ev, base->class_id, "wide");
    const XgClassFieldSummary *flag =
        global_evidence_find_class_field_by_name(&ev, child->class_id, "flag");
    REQUIRE_CLASS_FIELD_EVIDENCE(wide != NULL, "Base.wide field evidence should be present");
    REQUIRE_CLASS_FIELD_EVIDENCE(flag != NULL, "Child.flag field evidence should be present");

    XiFunc *touch = func_tree_find_func_name(main_func, "touch");
    REQUIRE_CLASS_FIELD_EVIDENCE(touch != NULL, "target function should be present");
    uint32_t wide_load_id = 0;
    uint32_t flag_store_id = 0;
    for (uint32_t b = 0; b < touch->nblocks; b++) {
        XiBlock *blk = touch->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || !v->aux)
                continue;
            if (v->op == XI_LOAD_FIELD && strcmp((const char *) v->aux, "wide") == 0)
                wide_load_id = v->xg_class_field_id;
            if (v->op == XI_STORE_FIELD && strcmp((const char *) v->aux, "flag") == 0)
                flag_store_id = v->xg_class_field_id;
        }
    }

    REQUIRE_CLASS_FIELD_EVIDENCE(wide_load_id == wide->field_id,
                                 "Child receiver should bind inherited Base.wide field id");
    REQUIRE_CLASS_FIELD_EVIDENCE(flag_store_id == flag->field_id,
                                 "Child.flag store should bind its own field id");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_CLASS_FIELD_EVIDENCE
}

TEST(class_field_default_initializer_store_lowers_with_global_evidence_id) {
#define REQUIRE_CLASS_FIELD_INIT_EVIDENCE(cond, msg)                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr,                                                                        \
                    "class_field_default_initializer_store_lowers_with_global_evidence_id: %s\n",  \
                    msg);                                                                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func = lower_source_with_global_evidence("class Holder {\n"
                                                          "    values: Array<int> = []\n"
                                                          "}\n"
                                                          "\n"
                                                          "var h = Holder()\n"
                                                          "print(len(h.values))\n",
                                                          &ev);
    REQUIRE_CLASS_FIELD_INIT_EVIDENCE(main_func != NULL, "source should lower");

    const XgClassSummary *holder = global_evidence_find_class_by_name(&ev, "Holder");
    REQUIRE_CLASS_FIELD_INIT_EVIDENCE(holder != NULL, "Holder evidence should be present");
    const XgClassFieldSummary *values =
        global_evidence_find_class_field_by_name(&ev, holder->class_id, "values");
    REQUIRE_CLASS_FIELD_INIT_EVIDENCE(values != NULL,
                                      "Holder.values field evidence should be present");

    XiFunc *ctor = func_tree_find_func_name(main_func, "constructor");
    REQUIRE_CLASS_FIELD_INIT_EVIDENCE(ctor != NULL, "synthetic constructor should be present");
    uint32_t values_store_id = 0;
    for (uint32_t b = 0; b < ctor->nblocks; b++) {
        XiBlock *blk = ctor->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v && v->op == XI_STORE_FIELD && v->aux &&
                strcmp((const char *) v->aux, "values") == 0)
                values_store_id = v->xg_class_field_id;
        }
    }

    REQUIRE_CLASS_FIELD_INIT_EVIDENCE(
        values_store_id == values->field_id,
        "synthetic constructor field default store should bind Holder.values field id");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_CLASS_FIELD_INIT_EVIDENCE
}

TEST(json_access_lowers_with_global_evidence_id) {
#define REQUIRE_JSON_EVIDENCE(cond, msg)                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "json_access_lowers_with_global_evidence_id: %s\n", msg);              \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn updateAge() -> int {\n"
                                          "    var j: Json = { name: \"ada\", age: 1 }\n"
                                          "    j.age = 2\n"
                                          "    return j.age\n"
                                          "}\n"
                                          "print(updateAge())\n",
                                          &ev);
    REQUIRE_JSON_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_JSON_EVIDENCE(ev.njson_accesses == 2, "producer should record Json get and set");
    XiFunc *update = func_tree_find_func_name(main_func, "updateAge");
    REQUIRE_JSON_EVIDENCE(update != NULL, "target function should be present");

    uint32_t get_id = 0;
    uint32_t set_id = 0;
    for (uint32_t b = 0; b < update->nblocks; b++) {
        XiBlock *blk = update->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->op == XI_JSON_GET_F) {
                REQUIRE_JSON_EVIDENCE(v->xg_json_access_id != 0,
                                      "Json field get should bind global access evidence");
                get_id = v->xg_json_access_id;
            } else if (v->op == XI_JSON_SET_F) {
                REQUIRE_JSON_EVIDENCE(v->xg_json_access_id != 0,
                                      "Json field set should bind global access evidence");
                set_id = v->xg_json_access_id;
            }
        }
    }
    REQUIRE_JSON_EVIDENCE(get_id != 0, "Json field get should be found");
    REQUIRE_JSON_EVIDENCE(set_id != 0, "Json field set should be found");
    REQUIRE_JSON_EVIDENCE(get_id != set_id, "Json get/set should use distinct access rows");

    int matched_get = 0;
    int matched_set = 0;
    for (uint32_t i = 0; i < ev.njson_accesses; i++) {
        const XgJsonAccessSummary *row = &ev.json_accesses[i];
        if (row->json_access_id == get_id) {
            REQUIRE_JSON_EVIDENCE(row->access_kind == XG_JSON_ACCESS_FIELD_GET,
                                  "bound get id should point at field_get row");
            REQUIRE_JSON_EVIDENCE(row->field_ordinal == 1,
                                  "bound get id should preserve field ordinal");
            matched_get = 1;
        }
        if (row->json_access_id == set_id) {
            REQUIRE_JSON_EVIDENCE(row->access_kind == XG_JSON_ACCESS_FIELD_SET,
                                  "bound set id should point at field_set row");
            REQUIRE_JSON_EVIDENCE(row->field_ordinal == 1,
                                  "bound set id should preserve field ordinal");
            matched_set = 1;
        }
    }
    REQUIRE_JSON_EVIDENCE(matched_get && matched_set, "bound ids should re-derive from evidence");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_JSON_EVIDENCE
}

TEST(json_alias_shape_access_lowers_with_global_evidence_id) {
#define REQUIRE_JSON_ALIAS_EVIDENCE(cond, msg)                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "json_alias_shape_access_lowers_with_global_evidence_id: %s\n", msg);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn readAlias() -> Json {\n"
                                          "    var a: Json = { name: \"ada\", age: 1 }\n"
                                          "    var b: Json = a\n"
                                          "    return b.age\n"
                                          "}\n"
                                          "print(readAlias())\n",
                                          &ev);
    REQUIRE_JSON_ALIAS_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_JSON_ALIAS_EVIDENCE(ev.njson_accesses == 1,
                                "producer should record one Json alias field get");
    XiFunc *read_alias = func_tree_find_func_name(main_func, "readAlias");
    REQUIRE_JSON_ALIAS_EVIDENCE(read_alias != NULL, "target function should be present");

    uint32_t access_id = 0;
    uint32_t direct_count = 0;
    for (uint32_t b = 0; b < read_alias->nblocks; b++) {
        XiBlock *blk = read_alias->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_JSON_GET_F)
                continue;
            direct_count++;
            REQUIRE_JSON_ALIAS_EVIDENCE(v->aux_int == 1,
                                        "alias Json direct get should use propagated ordinal");
            REQUIRE_JSON_ALIAS_EVIDENCE(v->xg_json_access_id != 0,
                                        "alias Json direct get should bind global access evidence");
            access_id = v->xg_json_access_id;
        }
    }
    REQUIRE_JSON_ALIAS_EVIDENCE(direct_count == 1,
                                "alias Json field get should lower to one direct indexed get");
    REQUIRE_JSON_ALIAS_EVIDENCE(access_id == ev.json_accesses[0].json_access_id,
                                "bound id should point at propagated alias access row");
    REQUIRE_JSON_ALIAS_EVIDENCE(ev.json_accesses[0].field_ordinal == 1,
                                "propagated alias access should keep field ordinal");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_JSON_ALIAS_EVIDENCE
}

TEST(json_computed_key_access_lowers_with_global_evidence_id) {
#define REQUIRE_JSON_INDEX_EVIDENCE(cond, msg)                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "json_computed_key_access_lowers_with_global_evidence_id: %s\n", msg); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn readKey(k: string) -> Json {\n"
                                          "    var j: Json = { name: \"ada\", age: 1 }\n"
                                          "    return j[k]\n"
                                          "}\n"
                                          "print(readKey(\"name\"))\n",
                                          &ev);
    REQUIRE_JSON_INDEX_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_JSON_INDEX_EVIDENCE(ev.njson_accesses == 1,
                                "producer should record computed Json index get");
    XiFunc *read_key = func_tree_find_func_name(main_func, "readKey");
    REQUIRE_JSON_INDEX_EVIDENCE(read_key != NULL, "target function should be present");

    uint32_t access_id = 0;
    for (uint32_t b = 0; b < read_key->nblocks; b++) {
        XiBlock *blk = read_key->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_INDEX_GET)
                continue;
            REQUIRE_JSON_INDEX_EVIDENCE(v->xg_json_access_id != 0,
                                        "Json computed index should bind global access evidence");
            access_id = v->xg_json_access_id;
        }
    }
    REQUIRE_JSON_INDEX_EVIDENCE(access_id == ev.json_accesses[0].json_access_id,
                                "bound id should point at computed-key access row");
    REQUIRE_JSON_INDEX_EVIDENCE(ev.json_accesses[0].access_kind == XG_JSON_ACCESS_INDEX_GET,
                                "row should describe index_get");
    REQUIRE_JSON_INDEX_EVIDENCE((ev.json_accesses[0].flags & XG_JSON_ACCESS_COMPUTED_KEY) != 0,
                                "row should keep computed-key evidence");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_JSON_INDEX_EVIDENCE
}

TEST(json_static_key_index_lowers_to_direct_field_with_global_evidence_id) {
#define REQUIRE_JSON_STATIC_INDEX_EVIDENCE(cond, msg)                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr,                                                                        \
                    "json_static_key_index_lowers_to_direct_field_with_global_evidence_id: %s\n",  \
                    msg);                                                                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn updateKey() -> Json {\n"
                                          "    var j: Json = { name: \"ada\", age: 1 }\n"
                                          "    j[\"age\"] = 2\n"
                                          "    return j[\"age\"]\n"
                                          "}\n"
                                          "print(updateKey())\n",
                                          &ev);
    REQUIRE_JSON_STATIC_INDEX_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_JSON_STATIC_INDEX_EVIDENCE(ev.njson_accesses == 2,
                                       "producer should record static-key get and set");
    XiFunc *update_key = func_tree_find_func_name(main_func, "updateKey");
    REQUIRE_JSON_STATIC_INDEX_EVIDENCE(update_key != NULL, "target function should be present");

    uint32_t get_id = 0;
    uint32_t set_id = 0;
    uint32_t generic_index_count = 0;
    for (uint32_t b = 0; b < update_key->nblocks; b++) {
        XiBlock *blk = update_key->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->op == XI_INDEX_GET || v->op == XI_INDEX_SET)
                generic_index_count++;
            if (v->op == XI_JSON_GET_F) {
                REQUIRE_JSON_STATIC_INDEX_EVIDENCE(v->aux_int == 1,
                                                   "static-key get should use field ordinal");
                REQUIRE_JSON_STATIC_INDEX_EVIDENCE(v->xg_json_access_id != 0,
                                                   "static-key get should bind evidence");
                get_id = v->xg_json_access_id;
            }
            if (v->op == XI_JSON_SET_F) {
                REQUIRE_JSON_STATIC_INDEX_EVIDENCE(v->aux_int == 1,
                                                   "static-key set should use field ordinal");
                REQUIRE_JSON_STATIC_INDEX_EVIDENCE(v->xg_json_access_id != 0,
                                                   "static-key set should bind evidence");
                set_id = v->xg_json_access_id;
            }
        }
    }
    REQUIRE_JSON_STATIC_INDEX_EVIDENCE(generic_index_count == 0,
                                       "static-key Json access should not use generic index ops");
    REQUIRE_JSON_STATIC_INDEX_EVIDENCE(get_id != 0 && set_id != 0,
                                       "both static-key direct ops should be present");
    REQUIRE_JSON_STATIC_INDEX_EVIDENCE(get_id != set_id,
                                       "static-key get and set should use distinct access rows");

    bool matched_get = false;
    bool matched_set = false;
    for (uint32_t i = 0; i < ev.njson_accesses; i++) {
        const XgJsonAccessSummary *row = &ev.json_accesses[i];
        if (row->json_access_id == get_id) {
            REQUIRE_JSON_STATIC_INDEX_EVIDENCE(row->access_kind == XG_JSON_ACCESS_INDEX_GET,
                                               "bound get id should point at index_get row");
            matched_get = true;
        }
        if (row->json_access_id == set_id) {
            REQUIRE_JSON_STATIC_INDEX_EVIDENCE(row->access_kind == XG_JSON_ACCESS_INDEX_SET,
                                               "bound set id should point at index_set row");
            matched_set = true;
        }
    }
    REQUIRE_JSON_STATIC_INDEX_EVIDENCE(matched_get && matched_set,
                                       "bound ids should re-derive from static-key evidence");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_JSON_STATIC_INDEX_EVIDENCE
}

TEST(json_open_shape_member_access_lowers_to_dynamic_lookup) {
#define REQUIRE_JSON_OPEN_EVIDENCE(cond, msg)                                                      \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "json_open_shape_member_access_lowers_to_dynamic_lookup: %s\n", msg);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn readName(k: string) -> Json {\n"
                                          "    var j: Json = { name: \"ada\", [k]: 1 }\n"
                                          "    return j.name\n"
                                          "}\n"
                                          "print(readName(\"age\"))\n",
                                          &ev);
    REQUIRE_JSON_OPEN_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_JSON_OPEN_EVIDENCE(ev.njson_shapes == 1, "producer should record one Json shape");
    REQUIRE_JSON_OPEN_EVIDENCE(ev.njson_accesses == 1, "producer should record Json field get");
    REQUIRE_JSON_OPEN_EVIDENCE(ev.json_shapes[0].shape_kind == XG_JSON_SHAPE_OPEN,
                               "shape should be open");

    XiFunc *read_name = func_tree_find_func_name(main_func, "readName");
    REQUIRE_JSON_OPEN_EVIDENCE(read_name != NULL, "target function should be present");

    uint32_t direct_count = 0;
    uint32_t dynamic_count = 0;
    for (uint32_t b = 0; b < read_name->nblocks; b++) {
        XiBlock *blk = read_name->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->op == XI_JSON_GET_F)
                direct_count++;
            if (v->op == XI_LOAD_FIELD && v->aux && strcmp((const char *) v->aux, "name") == 0)
                dynamic_count++;
        }
    }
    REQUIRE_JSON_OPEN_EVIDENCE(direct_count == 0,
                               "open Json shape must not lower to direct indexed get");
    REQUIRE_JSON_OPEN_EVIDENCE(dynamic_count == 1,
                               "open Json shape should keep dynamic field lookup");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_JSON_OPEN_EVIDENCE
}

TEST(json_open_shape_static_key_index_lowers_to_dynamic_lookup) {
#define REQUIRE_JSON_OPEN_INDEX_EVIDENCE(cond, msg)                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "json_open_shape_static_key_index_lowers_to_dynamic_lookup: %s\n",     \
                    msg);                                                                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn readName(k: string) -> Json {\n"
                                          "    var j: Json = { name: \"ada\", [k]: 1 }\n"
                                          "    return j[\"name\"]\n"
                                          "}\n"
                                          "print(readName(\"age\"))\n",
                                          &ev);
    REQUIRE_JSON_OPEN_INDEX_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_JSON_OPEN_INDEX_EVIDENCE(ev.njson_shapes == 1, "producer should record one Json shape");
    REQUIRE_JSON_OPEN_INDEX_EVIDENCE(ev.njson_accesses == 1,
                                     "producer should record Json static-key index get");
    REQUIRE_JSON_OPEN_INDEX_EVIDENCE(ev.json_shapes[0].shape_kind == XG_JSON_SHAPE_OPEN,
                                     "shape should be open");
    REQUIRE_JSON_OPEN_INDEX_EVIDENCE(ev.json_accesses[0].access_kind == XG_JSON_ACCESS_INDEX_GET,
                                     "access should be an index_get row");

    XiFunc *read_name = func_tree_find_func_name(main_func, "readName");
    REQUIRE_JSON_OPEN_INDEX_EVIDENCE(read_name != NULL, "target function should be present");

    uint32_t direct_count = 0;
    uint32_t dynamic_count = 0;
    uint32_t access_id = 0;
    for (uint32_t b = 0; b < read_name->nblocks; b++) {
        XiBlock *blk = read_name->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->op == XI_JSON_GET_F)
                direct_count++;
            if (v->op == XI_INDEX_GET && v->xg_json_access_id != 0) {
                dynamic_count++;
                access_id = v->xg_json_access_id;
            }
        }
    }
    REQUIRE_JSON_OPEN_INDEX_EVIDENCE(direct_count == 0,
                                     "open Json shape must not lower to direct indexed get");
    REQUIRE_JSON_OPEN_INDEX_EVIDENCE(dynamic_count == 1,
                                     "open Json shape should keep dynamic index lookup");
    REQUIRE_JSON_OPEN_INDEX_EVIDENCE(access_id == ev.json_accesses[0].json_access_id,
                                     "dynamic index lookup should retain Json evidence id");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_JSON_OPEN_INDEX_EVIDENCE
}

TEST(record_access_lowers_with_global_evidence_id) {
#define REQUIRE_RECORD_EVIDENCE(cond, msg)                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "record_access_lowers_with_global_evidence_id: %s\n", msg);            \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn readAge() -> int {\n"
                                          "    var user = { name: \"ada\", age: 1 }\n"
                                          "    return user.age\n"
                                          "}\n"
                                          "print(readAge())\n",
                                          &ev);
    REQUIRE_RECORD_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_RECORD_EVIDENCE(ev.njson_accesses == 0,
                            "bare object literal should not produce Json access evidence");
    REQUIRE_RECORD_EVIDENCE(ev.nrecord_accesses == 1, "producer should record Record field get");
    XiFunc *read = func_tree_find_func_name(main_func, "readAge");
    REQUIRE_RECORD_EVIDENCE(read != NULL, "target function should be present");

    uint32_t get_id = 0;
    uint32_t json_bound_count = 0;
    for (uint32_t b = 0; b < read->nblocks; b++) {
        XiBlock *blk = read->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_JSON_GET_F)
                continue;
            if (v->xg_json_access_id != 0)
                json_bound_count++;
            if (v->xg_record_access_id != 0)
                get_id = v->xg_record_access_id;
        }
    }
    REQUIRE_RECORD_EVIDENCE(json_bound_count == 0, "Record access should not bind Json id");
    REQUIRE_RECORD_EVIDENCE(get_id != 0, "Record field get should bind global access evidence");
    REQUIRE_RECORD_EVIDENCE(ev.record_accesses[0].record_access_id == get_id,
                            "bound id should point at Record access row");
    REQUIRE_RECORD_EVIDENCE(ev.record_accesses[0].access_kind == XG_RECORD_ACCESS_FIELD_GET,
                            "bound id should point at field_get row");
    REQUIRE_RECORD_EVIDENCE(ev.record_accesses[0].field_ordinal == 1,
                            "bound id should preserve field ordinal");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_RECORD_EVIDENCE
}

static const char *json_codec_same_line_source(void) {
    return "type User = { name: string, age: int }\n"
           "fn codecs() -> string {\n"
           "    var parsed: Json = Json.parse(\"{\\\"name\\\":\\\"A\\\",\\\"age\\\":1}\"); var "
           "decoded: User = Json.decode<User>(parsed)!; var encoded: Json = Json.encode(decoded); "
           "return Json.stringify(encoded)\n"
           "}\n"
           "print(codecs())\n";
}

static uint8_t xi_json_codec_kind_for_test(const XiValue *value) {
    if (!value)
        return 0;
    if (value->op == XI_JSON_DECODE)
        return XG_JSON_CODEC_DECODE;
    if (value->op != XI_CALL_METHOD || !value->aux)
        return 0;
    const char *method = (const char *) value->aux;
    if (strcmp(method, "parse") == 0)
        return XG_JSON_CODEC_PARSE;
    if (strcmp(method, "encode") == 0)
        return XG_JSON_CODEC_ENCODE;
    if (strcmp(method, "stringify") == 0)
        return XG_JSON_CODEC_STRINGIFY;
    return 0;
}

static void invalidate_json_codec_source_nodes(XgGlobalEvidence *evidence) {
    if (!evidence)
        return;
    for (uint32_t i = 0; i < evidence->njson_codecs; i++)
        evidence->json_codecs[i].source_node_id += UINT32_C(1000000);
}

#define TEST_REQUIRE(cond, msg)                                                                    \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "json_codec_source_identity: %s\n", (msg));                            \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

TEST(json_codec_calls_bind_exact_source_node_evidence_ids) {
    XgGlobalEvidence ev = {0};
    XiFunc *root = lower_source_with_global_evidence(json_codec_same_line_source(), &ev);
    TEST_REQUIRE(root != NULL, "same-line Json codec source should lower");
    TEST_REQUIRE(ev.njson_codecs == 4, "producer should record all four Json codec calls");
    XiFunc *codecs = func_tree_find_func_name(root, "codecs");
    TEST_REQUIRE(codecs != NULL && codecs->xg_body_func_id != XG_NO_ID,
                 "codec body should bind its global body id");

    uint32_t codec_sites = 0;
    uint32_t bound_sites = 0;
    uint32_t shared_span = ev.json_codecs[0].source_span_id;
    for (uint32_t i = 0; i < ev.njson_codecs; i++) {
        TEST_REQUIRE(ev.json_codecs[i].source_node_id != 0,
                     "codec evidence should preserve AST node identity");
        TEST_REQUIRE(ev.json_codecs[i].source_span_id == shared_span,
                     "fixture should place all codec calls on one source line");
        for (uint32_t j = i + 1; j < ev.njson_codecs; j++)
            TEST_REQUIRE(ev.json_codecs[i].source_node_id != ev.json_codecs[j].source_node_id,
                         "same-line codec calls should have distinct AST node ids");
    }
    for (uint32_t b = 0; b < codecs->nblocks; b++) {
        XiBlock *block = codecs->blocks[b];
        if (!block)
            continue;
        for (uint32_t i = 0; i < block->nvalues; i++) {
            XiValue *value = block->values[i];
            uint8_t expected_kind = xi_json_codec_kind_for_test(value);
            if (expected_kind == 0)
                continue;
            codec_sites++;
            TEST_REQUIRE(value->xg_json_codec_id != XG_NO_ID,
                         "codec Xi value should carry a stable evidence id");
            const XgJsonCodecSummary *row =
                xg_global_evidence_find_json_codec(&ev, value->xg_json_codec_id);
            TEST_REQUIRE(row != NULL, "bound codec id should resolve to evidence");
            TEST_REQUIRE(row->owner_func_id == codecs->xg_body_func_id,
                         "codec id should bind within the current owner body");
            TEST_REQUIRE(row->codec_kind == expected_kind,
                         "codec id should bind the expected codec kind");
            bound_sites++;
        }
    }
    TEST_REQUIRE(codec_sites == 4 && bound_sites == 4,
                 "parse/decode/encode/stringify should all bind exactly once");

    xi_func_free(root);
    xg_global_evidence_free(&ev);
}

TEST(json_codec_binding_does_not_fallback_to_source_span) {
    XgGlobalEvidence ev = {0};
    XiFunc *root = lower_source_with_global_evidence_ex(json_codec_same_line_source(), &ev,
                                                        invalidate_json_codec_source_nodes);
    TEST_REQUIRE(root != NULL, "codec source with stale node ids should still lower semantically");
    XiFunc *codecs = func_tree_find_func_name(root, "codecs");
    TEST_REQUIRE(codecs != NULL, "codec body should be present");

    uint32_t codec_sites = 0;
    uint32_t bound_sites = 0;
    for (uint32_t b = 0; b < codecs->nblocks; b++) {
        XiBlock *block = codecs->blocks[b];
        if (!block)
            continue;
        for (uint32_t i = 0; i < block->nvalues; i++) {
            XiValue *value = block->values[i];
            if (xi_json_codec_kind_for_test(value) == 0)
                continue;
            codec_sites++;
            if (value->xg_json_codec_id != XG_NO_ID)
                bound_sites++;
        }
    }
    TEST_REQUIRE(codec_sites == 4, "fixture should still contain all codec sites");
    TEST_REQUIRE(bound_sites == 0, "stale AST node ids must not bind through matching line spans");

    xi_func_free(root);
    xg_global_evidence_free(&ev);
}

#undef TEST_REQUIRE

TEST(map_key_access_lowers_with_global_evidence_id) {
#define REQUIRE_KEY_EVIDENCE(cond, msg)                                                            \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "map_key_access_lowers_with_global_evidence_id: %s\n", msg);           \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn updateScore() -> int {\n"
                                          "    var scores = #{\"ada\": 7, \"lin\": 9}\n"
                                          "    scores[\"ada\"] = 8\n"
                                          "    return scores[\"ada\"]\n"
                                          "}\n"
                                          "print(updateScore())\n",
                                          &ev);
    REQUIRE_KEY_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_KEY_EVIDENCE(ev.nkey_accesses == 2, "producer should record Map set and get");
    XiFunc *update = func_tree_find_func_name(main_func, "updateScore");
    REQUIRE_KEY_EVIDENCE(update != NULL, "target function should be present");

    uint32_t get_id = 0;
    uint32_t set_id = 0;
    uint32_t tagged_index_ops = 0;
    for (uint32_t b = 0; b < update->nblocks; b++) {
        XiBlock *blk = update->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || (v->op != XI_INDEX_GET && v->op != XI_INDEX_SET))
                continue;
            tagged_index_ops++;
            if (v->xg_key_access_id == 0)
                continue;
            if (v->op == XI_INDEX_GET)
                get_id = v->xg_key_access_id;
            else
                set_id = v->xg_key_access_id;
        }
    }
    REQUIRE_KEY_EVIDENCE(tagged_index_ops >= 4,
                         "literal initialization plus source accesses should lower to index ops");
    REQUIRE_KEY_EVIDENCE(get_id != 0, "Map index get should bind key-access evidence");
    REQUIRE_KEY_EVIDENCE(set_id != 0, "Map index set should bind key-access evidence");
    REQUIRE_KEY_EVIDENCE(get_id != set_id, "Map get/set should use distinct access rows");

    int matched_get = 0;
    int matched_set = 0;
    for (uint32_t i = 0; i < ev.nkey_accesses; i++) {
        const XgKeyAccessSummary *row = &ev.key_accesses[i];
        if (row->access_id == get_id) {
            REQUIRE_KEY_EVIDENCE(row->op == XG_KEY_ACCESS_INDEX_GET,
                                 "bound get id should point at index_get row");
            REQUIRE_KEY_EVIDENCE((row->flags & XG_KEY_ACCESS_CONST_KEY) != 0,
                                 "bound get row should preserve const-key evidence");
            matched_get = 1;
        }
        if (row->access_id == set_id) {
            REQUIRE_KEY_EVIDENCE(row->op == XG_KEY_ACCESS_SET,
                                 "bound set id should point at set row");
            REQUIRE_KEY_EVIDENCE((row->flags & XG_KEY_ACCESS_MUTATING) != 0,
                                 "bound set row should preserve mutating evidence");
            matched_set = 1;
        }
    }
    REQUIRE_KEY_EVIDENCE(matched_get && matched_set, "bound ids should re-derive from evidence");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_KEY_EVIDENCE
}

TEST(map_key_access_alias_shape_lowers_with_global_evidence_id) {
#define REQUIRE_ALIAS_KEY_EVIDENCE(cond, msg)                                                      \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "map_key_access_alias_shape_lowers_with_global_evidence_id: %s\n",     \
                    msg);                                                                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn updateAlias() -> int {\n"
                                          "    var scores = #{\"ada\": 7, \"lin\": 9}\n"
                                          "    var alias = scores\n"
                                          "    alias[\"ada\"] = 8\n"
                                          "    var assigned: Map<string, int> = #{}\n"
                                          "    assigned = alias\n"
                                          "    return assigned[\"ada\"]\n"
                                          "}\n"
                                          "print(updateAlias())\n",
                                          &ev);
    REQUIRE_ALIAS_KEY_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_ALIAS_KEY_EVIDENCE(ev.nkey_accesses == 2,
                               "producer should record aliased Map set and get");
    REQUIRE_ALIAS_KEY_EVIDENCE(ev.key_accesses[0].receiver_shape_id == ev.map_shapes[0].shape_id,
                               "alias set should retain original map shape");
    REQUIRE_ALIAS_KEY_EVIDENCE(ev.key_accesses[1].receiver_shape_id == ev.map_shapes[0].shape_id,
                               "assigned get should retain propagated map shape");
    XiFunc *update = func_tree_find_func_name(main_func, "updateAlias");
    REQUIRE_ALIAS_KEY_EVIDENCE(update != NULL, "target function should be present");

    uint32_t get_id = 0;
    uint32_t set_id = 0;
    for (uint32_t b = 0; b < update->nblocks; b++) {
        XiBlock *blk = update->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || (v->op != XI_INDEX_GET && v->op != XI_INDEX_SET) || v->xg_key_access_id == 0)
                continue;
            if (v->op == XI_INDEX_GET)
                get_id = v->xg_key_access_id;
            else
                set_id = v->xg_key_access_id;
        }
    }
    REQUIRE_ALIAS_KEY_EVIDENCE(get_id == ev.key_accesses[1].access_id,
                               "assigned Map get should bind propagated evidence");
    REQUIRE_ALIAS_KEY_EVIDENCE(set_id == ev.key_accesses[0].access_id,
                               "alias Map set should bind propagated evidence");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_ALIAS_KEY_EVIDENCE
}

TEST(map_set_method_key_access_lowers_with_global_evidence_id) {
#define REQUIRE_METHOD_KEY_EVIDENCE(cond, msg)                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "map_set_method_key_access_lowers_with_global_evidence_id: %s\n",      \
                    msg);                                                                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func =
        lower_source_with_global_evidence("fn touch() -> int {\n"
                                          "    var scores = #{\"ada\": 7, \"lin\": 9}\n"
                                          "    var seen: Set<string> = #[\"ada\"]\n"
                                          "    scores.get(\"ada\")\n"
                                          "    scores.containsKey(\"lin\")\n"
                                          "    scores.delete(\"lin\")\n"
                                          "    scores.set(\"ada\", 8)\n"
                                          "    scores.clear()\n"
                                          "    seen.contains(\"ada\")\n"
                                          "    seen.add(\"lin\")\n"
                                          "    seen.delete(\"ada\")\n"
                                          "    seen.clear()\n"
                                          "    return 0\n"
                                          "}\n"
                                          "print(touch())\n",
                                          &ev);
    REQUIRE_METHOD_KEY_EVIDENCE(main_func != NULL, "source should lower");
    REQUIRE_METHOD_KEY_EVIDENCE(ev.nkey_accesses == 9,
                                "producer should record Map/Set method key accesses");
    XiFunc *touch = func_tree_find_func_name(main_func, "touch");
    REQUIRE_METHOD_KEY_EVIDENCE(touch != NULL, "target function should be present");

    uint32_t bound_ids[16];
    uint32_t bound_count = 0;
    uint32_t internal_adds_without_key_access = 0;
    for (uint32_t b = 0; b < touch->nblocks; b++) {
        XiBlock *blk = touch->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_CALL_METHOD)
                continue;
            const char *method = (const char *) v->aux;
            if (v->xg_key_access_id != 0) {
                REQUIRE_METHOD_KEY_EVIDENCE(bound_count < 16, "too many bound key accesses");
                bound_ids[bound_count++] = v->xg_key_access_id;
            } else if (method && strcmp(method, "add") == 0) {
                internal_adds_without_key_access++;
            }
        }
    }
    REQUIRE_METHOD_KEY_EVIDENCE(bound_count == 9,
                                "every source Map/Set method key access should bind evidence");
    REQUIRE_METHOD_KEY_EVIDENCE(internal_adds_without_key_access >= 1,
                                "Set literal population should not consume method evidence");

    uint32_t map_gets = 0;
    uint32_t map_sets = 0;
    uint32_t map_has = 0;
    uint32_t map_deletes = 0;
    uint32_t map_clears = 0;
    uint32_t set_adds = 0;
    uint32_t set_has = 0;
    uint32_t set_deletes = 0;
    uint32_t set_clears = 0;
    for (uint32_t i = 0; i < bound_count; i++) {
        const XgKeyAccessSummary *row = NULL;
        for (uint32_t j = 0; j < ev.nkey_accesses; j++) {
            if (ev.key_accesses[j].access_id == bound_ids[i]) {
                row = &ev.key_accesses[j];
                break;
            }
        }
        REQUIRE_METHOD_KEY_EVIDENCE(row != NULL, "bound id should point at evidence row");
        REQUIRE_METHOD_KEY_EVIDENCE(row->op != XG_KEY_ACCESS_INDEX_GET,
                                    "method call should not bind index-get evidence");
        if (row->container_kind == XG_MAP_CONTAINER_MAP) {
            if (row->op == XG_KEY_ACCESS_GET)
                map_gets++;
            else if (row->op == XG_KEY_ACCESS_SET)
                map_sets++;
            else if (row->op == XG_KEY_ACCESS_HAS)
                map_has++;
            else if (row->op == XG_KEY_ACCESS_DELETE)
                map_deletes++;
            else if (row->op == XG_KEY_ACCESS_CLEAR)
                map_clears++;
        } else if (row->container_kind == XG_MAP_CONTAINER_SET) {
            if (row->op == XG_KEY_ACCESS_ADD)
                set_adds++;
            else if (row->op == XG_KEY_ACCESS_HAS)
                set_has++;
            else if (row->op == XG_KEY_ACCESS_DELETE)
                set_deletes++;
            else if (row->op == XG_KEY_ACCESS_CLEAR)
                set_clears++;
        }
    }
    REQUIRE_METHOD_KEY_EVIDENCE(map_gets == 1, "Map.get evidence should bind once");
    REQUIRE_METHOD_KEY_EVIDENCE(map_sets == 1, "Map.set evidence should bind once");
    REQUIRE_METHOD_KEY_EVIDENCE(map_has == 1, "Map.has evidence should bind once");
    REQUIRE_METHOD_KEY_EVIDENCE(map_deletes == 1, "Map.delete evidence should bind once");
    REQUIRE_METHOD_KEY_EVIDENCE(map_clears == 1, "Map.clear evidence should bind once");
    REQUIRE_METHOD_KEY_EVIDENCE(set_adds == 1, "Set.add evidence should bind once");
    REQUIRE_METHOD_KEY_EVIDENCE(set_has == 1, "Set.has evidence should bind once");
    REQUIRE_METHOD_KEY_EVIDENCE(set_deletes == 1, "Set.delete evidence should bind once");
    REQUIRE_METHOD_KEY_EVIDENCE(set_clears == 1, "Set.clear evidence should bind once");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_METHOD_KEY_EVIDENCE
}

TEST(strong_source_node_identity_binds_same_line_calls_and_same_name_bodies) {
#define REQUIRE_STRONG_IDENTITY(cond, msg)                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "strong_source_node_identity: %s\n", msg);                             \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XgGlobalEvidence ev;
    memset(&ev, 0, sizeof(ev));
    XiFunc *main_func = lower_source_with_global_evidence_ex(
        "interface Valued { value() -> int }; "
        "class Left implements Valued { value() -> int { return 1 } }; "
        "class Right implements Valued { value() -> int { return 2 } }; "
        "fn value() -> int { return 3 }; "
        "fn exercise(left: Left, right: Right, named: Valued) -> int { "
        "return left.value() + right.value() + named.value() }",
        &ev, scramble_legacy_xi_identity_fields);
    REQUIRE_STRONG_IDENTITY(main_func != NULL, "source should lower with global evidence");

    uint32_t value_name_id = xg_name_id("value");
    uint32_t exercise_name_id = xg_name_id("exercise");
    const XgBodySummary *exercise_body = NULL;
    const XgBodySummary *value_bodies[3] = {0};
    uint32_t value_body_count = 0;
    for (uint32_t i = 0; i < ev.nbodies; i++) {
        const XgBodySummary *body = &ev.bodies[i];
        uint32_t declared_name_id = global_evidence_body_declared_name_id(&ev, body);
        if (body->kind == XG_BODY_FUNCTION && declared_name_id == exercise_name_id)
            exercise_body = body;
        if (declared_name_id == value_name_id &&
            (body->kind == XG_BODY_FUNCTION || body->kind == XG_BODY_METHOD)) {
            if (value_body_count < 3)
                value_bodies[value_body_count] = body;
            value_body_count++;
        }
    }
    REQUIRE_STRONG_IDENTITY(exercise_body != NULL, "exercise body evidence should exist");
    REQUIRE_STRONG_IDENTITY(value_body_count == 3,
                            "ordinary function and both same-name methods should have bodies");
    REQUIRE_STRONG_IDENTITY(value_bodies[0]->source_span_id == UINT32_MAX &&
                                value_bodies[1]->source_span_id == UINT32_MAX &&
                                value_bodies[2]->source_span_id == UINT32_MAX,
                            "scrambled diagnostic spans must not participate in body identity");
    for (uint32_t i = 0; i < value_body_count; i++) {
        REQUIRE_STRONG_IDENTITY(value_bodies[i]->name_id != value_name_id,
                                "scrambled body names must not participate in identity");
        REQUIRE_STRONG_IDENTITY(value_bodies[i]->source_node_id != 0,
                                "source-backed body must carry a node identity");
        REQUIRE_STRONG_IDENTITY(func_tree_find_xg_body(main_func, value_bodies[i]->func_id) != NULL,
                                "each same-name body must bind to its Xi function");
        for (uint32_t j = 0; j < i; j++)
            REQUIRE_STRONG_IDENTITY(value_bodies[i]->source_node_id !=
                                        value_bodies[j]->source_node_id,
                                    "same-name body identities must remain distinct");
    }

    XiFunc *exercise = func_tree_find_xg_body(main_func, exercise_body->func_id);
    REQUIRE_STRONG_IDENTITY(exercise != NULL, "exercise Xi body should bind by source node");
    XiValue *calls[3] = {0};
    REQUIRE_STRONG_IDENTITY(func_collect_method_calls(exercise, calls, 3) == 3,
                            "exercise should lower all three method calls");

    uint32_t method_calls = 0;
    uint32_t interface_calls = 0;
    for (uint32_t i = 0; i < 3; i++) {
        const XgCallsiteSummary *callsite =
            xg_global_evidence_find_callsite(&ev, calls[i]->xg_callsite_id);
        REQUIRE_STRONG_IDENTITY(callsite != NULL, "each Xi call should carry its callsite id");
        REQUIRE_STRONG_IDENTITY(callsite->owner_func_id == exercise_body->func_id,
                                "callsite owner must be the bound exercise body");
        REQUIRE_STRONG_IDENTITY(
            callsite->source_span_id == UINT32_MAX && callsite->body_ordinal == UINT32_MAX &&
                callsite->method_name_id != value_name_id && callsite->arg_count == UINT16_MAX,
            "legacy callsite match fields must not participate in identity");
        REQUIRE_STRONG_IDENTITY(callsite->source_node_id != 0,
                                "source-backed callsite must carry a node identity");
        REQUIRE_STRONG_IDENTITY(calls[i]->xg_method_id == callsite->method_id,
                                "Xi method identity must come from the matched callsite");
        for (uint32_t j = 0; j < i; j++) {
            const XgCallsiteSummary *previous =
                xg_global_evidence_find_callsite(&ev, calls[j]->xg_callsite_id);
            REQUIRE_STRONG_IDENTITY(calls[i]->xg_callsite_id != calls[j]->xg_callsite_id,
                                    "same-line Xi calls must bind distinct callsite ids");
            REQUIRE_STRONG_IDENTITY(previous &&
                                        previous->source_node_id != callsite->source_node_id,
                                    "same-line calls must bind distinct source nodes");
        }
        if (callsite->kind == XG_CALL_METHOD) {
            REQUIRE_STRONG_IDENTITY(calls[i]->xg_interface_dispatch_slot == UINT32_MAX,
                                    "class call must not carry an interface dispatch slot");
            method_calls++;
        } else if (callsite->kind == XG_CALL_INTERFACE) {
            uint32_t expected_slot = UINT32_MAX;
            REQUIRE_STRONG_IDENTITY(xg_global_evidence_interface_dispatch_slot(
                                        &ev, callsite->receiver_static_interface_id,
                                        callsite->method_id, &expected_slot),
                                    "interface call evidence must resolve a dispatch slot");
            REQUIRE_STRONG_IDENTITY(calls[i]->xg_interface_dispatch_slot == expected_slot,
                                    "Xi interface slot must come from the matched method row");
            interface_calls++;
        }
    }
    REQUIRE_STRONG_IDENTITY(method_calls == 2 && interface_calls == 1,
                            "callsite kind must distinguish class and interface dispatch");

    xi_func_free(main_func);
    xg_global_evidence_free(&ev);

#undef REQUIRE_STRONG_IDENTITY
}

TEST(nested_function) {
    XiFunc *f = lower_source("fn add(a: int, b: int) -> int {\n"
                             "    return a + b\n"
                             "}\n"
                             "var r = add(1, 2)\n"
                             "print(r)\n");
    assert(f != NULL);
    /* Parent should have a child function */
    assert(f->nchildren == 1);
    XiFunc *child = f->children[0];
    assert(child != NULL);
    assert(child->nparams == 2);
    /* Child should have at least an ADD and a return */
    assert(child->nblocks >= 1);
    assert(child->entry->nvalues >= 1);
    /* Parent should have CLOSURE_NEW and CALL */
    int found_closure = 0, found_call = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_CLOSURE_NEW)
            found_closure = 1;
        if (f->entry->values[i]->op == XI_CALL)
            found_call = 1;
    }
    assert(found_closure && "parent should have CLOSURE_NEW");
    assert(found_call && "parent should have CALL");
    xi_func_free(f);
}

TEST(function_expr) {
    XiFunc *f = lower_source("var double = fn(x: int) -> int { return x * 2 }\n"
                             "var r = double(5)\n"
                             "print(r)\n");
    assert(f != NULL);
    assert(f->nchildren == 1);
    XiFunc *child = f->children[0];
    assert(child != NULL);
    assert(child->nparams == 1);
    xi_func_free(f);
}

TEST(multiple_functions) {
    XiFunc *f = lower_source("fn foo() -> int { return 1 }\n"
                             "fn bar() -> int { return 2 }\n"
                             "print(foo() + bar())\n");
    assert(f != NULL);
    assert(f->nchildren == 2);
    xi_func_free(f);
}

TEST(template_string) {
    XiFunc *f = lower_source("var name = \"world\"\n"
                             "var msg = \"hello ${name}!\"\n"
                             "print(msg)\n");
    assert(f != NULL);
    int found_concat = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_STR_CONCAT)
            found_concat = 1;
    }
    assert(found_concat && "should have STR_CONCAT for template string");
    xi_func_free(f);
}

TEST(go_await) {
    XiFunc *f = lower_source("fn work() -> int { return 42 }\n"
                             "var t = go work()\n"
                             "var r = await t\n"
                             "print(r)\n");
    assert(f != NULL);
    int found_go = 0, found_await = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_GO)
            found_go = 1;
        if (f->entry->values[i]->op == XI_AWAIT) {
            found_await = 1;
            assert((f->entry->values[i]->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0 &&
                   "visible task await must not be one-shot");
        }
    }
    assert(found_go && "should have GO op");
    assert(found_await && "should have AWAIT op");
    xi_func_free(f);
}

TEST(direct_await_go_one_shot) {
    XiFunc *f = lower_source("fn work() -> int { return 42 }\n"
                             "var r = await go work()\n"
                             "print(r)\n");
    assert(f != NULL);
    int found_await = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_AWAIT) {
            found_await = 1;
            assert((f->entry->values[i]->aux_int & XI_AWAIT_AUX_CONSUME_TASK) != 0 &&
                   "direct await-go should be one-shot");
        }
    }
    assert(found_await && "should have AWAIT op");
    xi_func_free(f);
}

TEST(unique_result_task_await_consumes_handle) {
    XiFunc *f = lower_source("fn values() -> Array<int> { return [1, 2, 3] }\n"
                             "fn run() {\n"
                             "  var task = go values()\n"
                             "  var result = await task\n"
                             "  print(len(result))\n"
                             "}\n"
                             "run()\n");
    assert(f != NULL);
    XiValue *await = func_tree_find_op(f, XI_AWAIT);
    assert(await != NULL);
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) != 0 &&
           "unique-result Task await must consume its handle");
    XiFunc *values = func_tree_find_func_name(f, "values");
    XiValue *array = values ? func_tree_find_op(values, XI_ARRAY_NEW) : NULL;
    assert(array != NULL);
    assert(xi_value_allocation_storage_mode(array) == XR_OBJ_STORAGE_TRANSFER &&
           "a unique local returned by owner-forward must be allocated transferable");
    xi_func_free(f);
}

#define TUPLE_TASK_REQUIRE(cond, msg)                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "tuple_result_task_await_consumes_handle: %s\n", (msg));               \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

TEST(tuple_result_task_await_consumes_handle) {
    XiFunc *f = lower_source("fn pair() -> (int, int) { return (10, 20) }\n"
                             "var task = go pair()\n"
                             "var result = await task\n"
                             "print(result)\n");
    TUPLE_TASK_REQUIRE(f != NULL, "tuple Task source should lower");
    XiValue *await = func_tree_find_op(f, XI_AWAIT);
    TUPLE_TASK_REQUIRE(await != NULL, "tuple Task source should contain await");
    TUPLE_TASK_REQUIRE((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) != 0,
                       "tuple-result Task await must consume its handle");
    XiFunc *pair = func_tree_find_func_name(f, "pair");
    XiValue *tuple = pair ? func_tree_find_op(pair, XI_TUPLE_NEW) : NULL;
    TUPLE_TASK_REQUIRE(tuple != NULL, "pair should lower a tuple allocation");
    TUPLE_TASK_REQUIRE(xi_tuple_storage_mode(tuple) == XR_OBJ_STORAGE_TRANSFER,
                       "fresh tuple Task result must be allocated transferable");
    xi_func_free(f);
}

#undef TUPLE_TASK_REQUIRE

TEST(copy_struct_task_result_plans_shared_publication) {
    XiFunc *f = lower_source("struct Pair { a: int; b: int }\n"
                             "fn pair() -> Pair { return Pair{a: 10, b: 20} }\n"
                             "var task = go pair()\n"
                             "var result = await task\n"
                             "print(result.a + result.b)\n");
    assert(f != NULL);
    XiValue *go = func_tree_find_op(f, XI_GO);
    XiValue *await = func_tree_find_op(f, XI_AWAIT);
    assert(go != NULL && await != NULL);
    assert((go->aux_int & XI_GO_AUX_RESULT_COPY_SHARED) != 0 &&
           "pointer-backed Copy result must carry a compiler publication plan");
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0 &&
           "Copy result Task remains multi-observer");
    xi_func_free(f);

    XiFunc *record_f = lower_source("type PairRecord = { a: int, b: int }\n"
                                    "fn recordPair() -> PairRecord { return {a: 10, b: 20} }\n"
                                    "var recordTask = go recordPair()\n"
                                    "var recordResult = await recordTask\n"
                                    "print(recordResult.a + recordResult.b)\n");
    assert(record_f != NULL);
    XiValue *record_go = func_tree_find_op(record_f, XI_GO);
    XiValue *record_await = func_tree_find_op(record_f, XI_AWAIT);
    assert(record_go != NULL && record_await != NULL);
    assert((record_go->aux_int & XI_GO_AUX_RESULT_COPY_SHARED) != 0 &&
           "pointer-backed record result must carry a compiler publication plan");
    assert((record_await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0 &&
           "record result Task remains multi-observer");
    xi_func_free(record_f);
}

TEST(go_arg_transfer_modes) {
    XiFunc *copy_ir = lower_source("fn worker(xs: Array<int>) -> int { return len(xs) }\n"
                                   "var xs = [1, 2]\n"
                                   "var task = go worker(copy(xs))\n"
                                   "print(await task)\n");
    assert(copy_ir != NULL);
    XiValue *copy_go = func_tree_find_op(copy_ir, XI_GO);
    assert(copy_go != NULL && "copy case should lower a GO op");
    assert(copy_go->nargs == 2 && "go worker(copy(xs)) should keep callee plus one arg");
    assert(xi_go_arg_transfer_mode(copy_go, 0) == XR_TRANSFER_COPY &&
           "copy(...) at a go boundary must be encoded as COPY transfer");
    assert(!func_tree_has_op(copy_ir, XI_COPY) &&
           "go boundary copy(...) must not lower to a separate copy op before GO");
    xi_func_free(copy_ir);

    XiFunc *move_ir = lower_source("fn take(xs: move Array<int>) -> int { return len(xs) }\n"
                                   "fn moveCase() {\n"
                                   "  var xs: Array<int> = [1, 2]\n"
                                   "  var task = go take(move xs)\n"
                                   "  print(await task)\n"
                                   "}\n"
                                   "moveCase()\n");
    assert(move_ir != NULL);
    XiValue *move_go = func_tree_find_op(move_ir, XI_GO);
    assert(move_go != NULL && "move case should lower a GO op");
    assert(xi_go_arg_transfer_mode(move_go, 0) == XR_TRANSFER_MOVE &&
           "move at a go boundary must be encoded as MOVE transfer");
    assert(func_tree_has_op(move_ir, XI_SOURCE_MOVE) &&
           "move transfer should still consume source ownership");
    xi_func_free(move_ir);

    XiFunc *share_ir = lower_source("fn observe(ch: Channel<int>) -> int { return 1 }\n"
                                    "var ch = Channel<int>(1)\n"
                                    "var task = go observe(ch)\n"
                                    "print(await task)\n");
    assert(share_ir != NULL);
    XiValue *share_go = func_tree_find_op(share_ir, XI_GO);
    assert(share_go != NULL && "shared case should lower a GO op");
    assert(xi_go_arg_transfer_mode(share_go, 0) == XR_TRANSFER_SHARE &&
           "sync-handle go arguments should be encoded as zero-copy SHARE transfer");
    xi_func_free(share_ir);
}

TEST(hoisted_sync_handle_capture_is_shared) {
    XiFunc *f = lower_source("fn launch() {\n"
                             "  const gate = Atomic(0)\n"
                             "  fn worker() -> int { return gate.load() }\n"
                             "  var task = go worker()\n"
                             "  print(await task)\n"
                             "}\n"
                             "launch()\n");
    assert(f != NULL);
    XiFunc *worker = func_tree_find_func_name(f, "worker");
    assert(worker != NULL);
    assert(worker->ncaptures == 1);
    assert(worker->captures[0].needs_cell && "hoisting must preserve the forward-initializer cell");
    assert(!worker->captures[0].is_mutable && !worker->captures[0].is_reassigned &&
           "initialization ordering is not source-level mutation");
    assert(worker->captures[0].capture_kind == XI_CAPTURE_SHARED &&
           "Atomic has stable synchronized identity even when hoisting needs a cell");
    assert(xi_capture_cross_execution_action(&worker->captures[0]) == XR_TRANSFER_SYNC_SHARE &&
           "a synchronized handle must share identity rather than move ownership");
    xi_func_free(f);
}

TEST(channel_send_transfer_modes) {
    XiFunc *copy_ir = lower_source("const ch: Channel<Array<int>> = Channel(1)\n"
                                   "var xs = [1, 2]\n"
                                   "ch.send(copy(xs))\n");
    assert(copy_ir != NULL);
    XiValue *copy_send = func_tree_find_op(copy_ir, XI_CHAN_SEND);
    assert(copy_send != NULL && "copy case should lower to CHAN_SEND");
    assert(xi_chan_send_transfer_mode(copy_send) == XR_TRANSFER_COPY &&
           "copy(...) at a channel send boundary must be encoded as COPY transfer");
    assert(!func_tree_has_op(copy_ir, XI_COPY) &&
           "channel boundary copy(...) must not lower to a separate copy op before send");
    xi_func_free(copy_ir);

    XiFunc *move_ir = lower_source("const ch: Channel<Array<int>> = Channel(1)\n"
                                   "fn sendMove() {\n"
                                   "  var xs: Array<int> = [1, 2]\n"
                                   "  ch.send(move xs)\n"
                                   "}\n"
                                   "sendMove()\n");
    assert(move_ir != NULL);
    XiValue *move_send = func_tree_find_op(move_ir, XI_CHAN_SEND);
    assert(move_send != NULL && "move case should lower to CHAN_SEND");
    assert(xi_chan_send_transfer_mode(move_send) == XR_TRANSFER_MOVE &&
           "move at a channel send boundary must be encoded as MOVE transfer");
    assert(func_tree_has_op(move_ir, XI_SOURCE_MOVE) &&
           "move transfer should still consume source ownership");
    xi_func_free(move_ir);

    XiFunc *try_ir = lower_source("const ch: Channel<Array<int>> = Channel(1)\n"
                                  "var xs = [1, 2]\n"
                                  "print(ch.trySend(copy(xs)))\n");
    assert(try_ir != NULL);
    XiValue *try_send = func_tree_find_method(try_ir, "trySend");
    assert(try_send != NULL && "copy trySend should lower to CALL_METHOD");
    assert(xi_chan_send_transfer_mode(try_send) == XR_TRANSFER_COPY &&
           "copy(...) at trySend boundary must be encoded as COPY transfer");
    xi_func_free(try_ir);

    XiFunc *timeout_ir = lower_source("const ch: Channel<Array<int>> = Channel(1)\n"
                                      "var xs = [1, 2]\n"
                                      "print(ch.sendTimeout(copy(xs), 0))\n");
    assert(timeout_ir != NULL);
    XiValue *timeout_send = func_tree_find_method(timeout_ir, "sendTimeout");
    assert(timeout_send != NULL && "copy sendTimeout should lower to CALL_METHOD");
    assert(xi_chan_send_transfer_mode(timeout_send) == XR_TRANSFER_COPY &&
           "copy(...) at sendTimeout boundary must be encoded as COPY transfer");
    xi_func_free(timeout_ir);
}

TEST(defer_stmt) {
    XiFunc *f = lower_source("fn cleanup() { print(0) }\n"
                             "defer cleanup()\n"
                             "print(1)\n");
    assert(f != NULL);
    int found_defer = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_DEFER)
            found_defer = 1;
    }
    assert(found_defer && "should have DEFER op");
    xi_func_free(f);
}

TEST(defer_args_lower_before_defer) {
    XiFunc *f = lower_source("fn cleanup(msg: string, value: int) { print(msg); print(value) }\n"
                             "defer cleanup(\"result\", 42)\n"
                             "print(\"body\")\n");
    assert(f != NULL);
    XiBlock *blk = f->entry;
    XiValue *defer = NULL;
    uint32_t defer_index = 0;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i]->op == XI_DEFER) {
            defer = blk->values[i];
            defer_index = i;
            break;
        }
    }
    assert(defer && "should have DEFER op");
    assert(defer->nargs == 1 && "defer should store the deferred closure only");
    XiValue *closure = defer->args[0];
    assert(closure && closure->op == XI_CLOSURE_NEW &&
           "defer call args should be snapshotted into the deferred closure");
    assert(closure->nargs == 2 && "deferred closure should capture both eager arguments");
    for (uint16_t a = 0; a < closure->nargs; a++) {
        int found_before_defer = 0;
        for (uint32_t i = 0; i < defer_index; i++) {
            if (blk->values[i] == closure->args[a]) {
                found_before_defer = 1;
                break;
            }
        }
        assert(found_before_defer && "defer argument capture must be lowered before XI_DEFER");
    }
    xi_func_free(f);
}

TEST(set_literal) {
    XiFunc *f = lower_source("var s = #[1, 2, 3]\n"
                             "print(s)\n");
    assert(f != NULL);
    int found_set_new = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_SET_NEW)
            found_set_new = 1;
    }
    assert(found_set_new && "should have SET_NEW op");
    xi_func_free(f);
}

TEST(is_expr) {
    XiFunc *f = lower_source("var x = 42\n"
                             "var ok = x is int\n"
                             "print(ok)\n");
    assert(f != NULL);
    int found_is = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_IS)
            found_is = 1;
    }
    assert(found_is && "should have IS op");
    xi_func_free(f);
}

TEST(slice_expr) {
    XiFunc *f = lower_source("fn sliceLocal() {\n"
                             "  var arr = [1, 2, 3, 4]\n"
                             "  var sub: Slice<int> = arr[1:3]\n"
                             "  print(sub)\n"
                             "}\n"
                             "sliceLocal()\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_SLICE) && "should have SLICE op");
    xi_func_free(f);
}

TEST(range_expr) {
    XiFunc *f = lower_source("var r = 1..10\n"
                             "print(r)\n");
    assert(f != NULL);
    XiValue *range = func_tree_find_op(f, XI_RANGE);
    assert(range && "should have RANGE op");
    assert(range->aux_int == 0 && "half-open range should clear inclusive flag");
    xi_func_free(f);
}

TEST(range_inclusive_expr) {
    XiFunc *f = lower_source("var r = 1..=10\n"
                             "print(r)\n");
    assert(f != NULL);
    XiValue *range = func_tree_find_op(f, XI_RANGE);
    assert(range && "should have RANGE op");
    assert(range->aux_int == 1 && "inclusive range should set inclusive flag");
    xi_func_free(f);
}

TEST(optional_chain) {
    XiFunc *f = lower_source("var obj = {name: \"alice\"}\n"
                             "var n = obj?.name\n"
                             "print(n)\n");
    assert(f != NULL);
    /* Optional chain generates ISNULL + branch + merge */
    int found_isnull = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_ISNULL)
                found_isnull = 1;
        }
    }
    assert(found_isnull && "should have ISNULL for optional chain");
    assert(f->nblocks >= 3 && "should have branch blocks for optional chain");
    xi_func_free(f);
}

TEST(optional_call) {
    XiFunc *f = lower_source("type IntFn = (int) -> int\n"
                             "fn bump(x: int) -> int { return x + 1 }\n"
                             "var fnv: IntFn? = bump\n"
                             "var n = fnv?.(41)\n"
                             "print(n)\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_ISNULL) && "optional call should null-check callee");
    assert(func_tree_has_op(f, XI_CALL) && "optional call should call only on non-null path");
    xi_func_free(f);
}

TEST(struct_literal) {
    XiFunc *f = lower_source("struct Point {\n"
                             "    x: float\n"
                             "    y: float\n"
                             "}\n"
                             "var p = Point{x: 1.0, y: 2.0}\n"
                             "print(p)\n");
    assert(f != NULL);
    int found_new = 0, found_set = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_AGG_NEW)
                found_new = 1;
            if (blk->values[i]->op == XI_AGG_SET)
                found_set = 1;
        }
    }
    assert(found_new && "struct literal should emit AGG_NEW");
    assert(found_set && "struct literal should set fields via AGG_SET");
    xi_func_free(f);
}

TEST(struct_literal_inside_function) {
    XiFunc *f = lower_source("struct Pair {\n"
                             "    a: int\n"
                             "    b: int\n"
                             "}\n"
                             "fn run() -> int {\n"
                             "    var p = Pair{a: 1, b: 2}\n"
                             "    return p.a + p.b\n"
                             "}\n"
                             "print(run())\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_AGG_NEW) && "function-local struct literal should emit AGG_NEW");
    assert(func_tree_has_op(f, XI_AGG_SET) &&
           "function-local struct literal should set fields via AGG_SET");
    assert(func_tree_has_op(f, XI_AGG_GET) &&
           "function-local struct field access should emit AGG_GET");
    xi_func_free(f);
}

TEST(unresolved_struct_literal_does_not_lower_to_json) {
    XiFunc *f = lower_source("var p = Missing{x: 1}\n"
                             "print(p)\n");
    assert(f == NULL && "unresolved struct literal must not fall back to Json object");
}

TEST(struct_field_store_narrows_scalar_rep) {
    XiFunc *f = lower_source("struct Sample {\n"
                             "    octet: byte\n"
                             "}\n"
                             "fn run() -> int {\n"
                             "    var p = Sample{octet: 200}\n"
                             "    p.octet = p.octet + 1\n"
                             "    return p.octet\n"
                             "}\n"
                             "print(run())\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_NARROW_U8) &&
           "u8 struct field writes should narrow before storage");
    XiValue *narrow = func_tree_find_op(f, XI_NARROW_U8);
    assert(narrow && narrow->type && narrow->type->kind == XR_KIND_INT &&
           narrow->type->scalar_rep == XR_NATIVE_U8 &&
           "NARROW_U8 result type should carry the target native width");
    assert(func_tree_has_op(f, XI_AGG_SET) && "struct field writes should use AGG_SET");
    xi_func_free(f);
}

TEST(struct_method_receivers_use_call_bound_places) {
    XiFunc *f = lower_source("struct Counter {\n"
                             "    value: int\n"
                             "    bump(delta: int) {\n"
                             "        this.value = this.value + delta\n"
                             "    }\n"
                             "    read() -> int {\n"
                             "        return this.value\n"
                             "    }\n"
                             "}\n"
                             "fn exercise() -> int {\n"
                             "    var counter = Counter{value: 1}\n"
                             "    counter.bump(2)\n"
                             "    return counter.read()\n"
                             "}\n"
                             "print(exercise())\n");
    assert(f != NULL);

    XiValue *bump = func_tree_find_method(f, "bump");
    XiValue *read = func_tree_find_method(f, "read");
    assert(bump != NULL && read != NULL && "struct calls should remain method calls in Xi");
    assert(bump->call_plan && bump->call_plan->verified && bump->call_plan->has_receiver &&
           "mutating struct receiver should carry a verified place plan");
    assert(bump->call_plan->receiver.param_mode == XR_PARAM_REF &&
           bump->call_plan->receiver.place == bump->args[0] && bump->args[0]->op == XI_LOCAL_ADDR &&
           "mutating struct receiver should use a ref call-bound local place");
    assert(read->call_plan && read->call_plan->verified && read->call_plan->has_receiver &&
           read->call_plan->receiver.param_mode == XR_PARAM_READ &&
           read->call_plan->receiver.place == read->args[0] && read->args[0]->op == XI_LOCAL_ADDR &&
           "readonly struct receiver should use a read call-bound local place");

    xi_func_free(f);
}

TEST(read_value_struct_param_uses_internal_call_place) {
    XiFunc *f = lower_source("struct Pair { a: int; b: int }\n"
                             "fn sum(p: Pair) -> int {\n"
                             "    return p.a + p.b\n"
                             "}\n"
                             "fn exercise() -> int {\n"
                             "    var pair = Pair{a: 1, b: 2}\n"
                             "    return sum(pair)\n"
                             "}\n"
                             "print(exercise())\n");
    assert(f != NULL);

    XiFunc *sum = func_tree_find_func_name(f, "sum");
    XiFunc *exercise = func_tree_find_func_name(f, "exercise");
    XiValue *call = func_tree_find_op(exercise, XI_CALL);
    assert(sum && sum->nparams == 1 && xi_value_is_read_place_param(sum->params[0]) &&
           "semantic read value-struct parameters should publish internal place ABI evidence");
    assert(call && call->call_plan && call->call_plan->verified && call->call_plan->nargs == 1 &&
           call->call_plan->args[0].param_mode == XR_PARAM_READ &&
           call->call_plan->args[0].place == call->args[1] && call->args[1]->op == XI_LOCAL_ADDR &&
           "ordinary calls should infer a nonescaping read place without source syntax");

    xi_func_free(f);
}

TEST(as_to_scalar_rep_int_lowers_to_narrow) {
    XiFunc *f = lower_source("fn run(i: int) -> int {\n"
                             "    var v = i as u16\n"
                             "    return int(v)\n"
                             "}\n"
                             "print(run(65537))\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_NARROW_U16) &&
           "int as u16 should lower to native-width narrowing");
    assert(!func_tree_has_op(f, XI_AS) && "numeric width cast should not use tagged XI_AS");
    XiValue *narrow = func_tree_find_op(f, XI_NARROW_U16);
    assert(narrow && narrow->type && narrow->type->kind == XR_KIND_INT &&
           narrow->type->scalar_rep == XR_NATIVE_U16 &&
           "NARROW_U16 result type should carry the cast target width");
    xi_func_free(f);
}

TEST(force_unwrap) {
    XiFunc *f = lower_source("var x: int? = 42\n"
                             "var y = x!\n"
                             "print(y)\n");
    assert(f != NULL);
    /* Force unwrap generates ISNULL + branch */
    int found_isnull = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_ISNULL)
                found_isnull = 1;
        }
    }
    assert(found_isnull && "should have ISNULL for force unwrap");
    assert(f->nblocks >= 3 && "force unwrap should create throw/ok branches");
    xi_func_free(f);
}

TEST(destructure_decl) {
    XiFunc *f = lower_source("var arr = [1, 2, 3]\n"
                             "var [a, b, c] = arr\n"
                             "print(a + b + c)\n");
    assert(f != NULL);
    /* Destructure should create INDEX_GET ops */
    int index_count = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_INDEX_GET)
            index_count++;
    }
    assert(index_count >= 3 && "should have 3 INDEX_GET for destructure");
    xi_func_free(f);
}

TEST(multi_assign) {
    XiFunc *f = lower_source("var a = 1\n"
                             "var b = 2\n"
                             "(a, b) = (b, a)\n"
                             "print(a)\n"
                             "print(b)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(enum_access) {
    XiFunc *f = lower_source("enum Color {\n"
                             "    Red,\n"
                             "    Green,\n"
                             "    Blue\n"
                             "}\n"
                             "var c = Color.Red\n"
                             "print(c)\n");
    assert(f != NULL);
    int found_load = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_LOAD_FIELD)
            found_load = 1;
    }
    assert(found_load && "should have LOAD_FIELD for enum access");
    xi_func_free(f);
}

TEST(import_export_skip) {
    XiFunc *f = lower_source("import \"math\" as math\n"
                             "var x = 42\n"
                             "print(x)\n");
    assert(f != NULL);
    /* Import is compile-time, should not generate any special ops */
    assert(f->entry->nvalues >= 2);
    xi_func_free(f);
}

TEST(class_decl_skip) {
    XiFunc *f = lower_source("class Dog {\n"
                             "    name: string = \"\"\n"
                             "}\n"
                             "print(1)\n");
    assert(f != NULL);
    /* Class decl is compile-time, lowered body goes nowhere.
     * Main should still have print. */
    int found_print = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_PRINT)
            found_print = 1;
    }
    assert(found_print && "should still have print after class decl");
    xi_func_free(f);
}

TEST(yield_stmt) {
    XiFunc *f = lower_source("Coro.yield()\n"
                             "print(1)\n");
    assert(f != NULL);
    int found_yield = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_YIELD)
            found_yield = 1;
    }
    assert(found_yield && "should have YIELD op");
    xi_func_free(f);
}

TEST(canonical_effect_sidecars_reach_xi) {
    XiFunc *f = lower_source("fn grow(data: ref Array<int>) { data.push(1) }\n");
    assert(f != NULL);
    XiFunc *grow = func_tree_find_func_name(f, "grow");
    assert(grow != NULL);
    assert(grow->analyzer_effect_id != 0);
    assert(grow->analyzer_memory_effect_id != 0);
    assert(grow->analyzer_effect_fingerprint != 0);
    assert(grow->analyzer_memory_effect_fingerprint != 0);
    assert((grow->semantic_effects & XA_SEM_EFFECT_ALLOC) != 0);
    assert(grow->analyzer_memory_effect_complete);
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Lower Unit Tests ===\n\n");

    setup();

    run_simple_arithmetic();
    run_variable_assignment();
    run_if_else();
    run_while_loop();
    run_loop_invariant_rc_trivial_phi_is_rewritten();
    run_for_loop();
    run_nested_if();
    run_bool_literals();
    run_float_arithmetic();
    run_string_const();
    run_comparison_ops();
    run_compound_assignment();
    run_inc_dec();
    run_ternary_expr();
    run_break_continue();
    run_nested_while();
    run_type_propagation();
    run_array_literal();
    run_index_access();
    run_member_access();
    run_member_access_field_symbols_are_distinct();
    run_bytes_new_low_level_methods_lower_to_semantic_ops();
    run_atomic_methods_lower_to_nothrow_canonical_ops();
    run_user_method_named_fetch_add_remains_ordinary_call();
    run_exact_integer_bit_methods_lower_to_typed_semantic_ops();
    run_throw_stmt();
    run_for_in_loop();
    run_nullish_coalesce();
    run_map_literal();
    run_match_expr();
    run_try_catch();
    run_try_catch_defer();
    run_object_literal();
    run_class_field_access_lowers_with_global_evidence_id();
    run_class_field_default_initializer_store_lowers_with_global_evidence_id();
    run_json_access_lowers_with_global_evidence_id();
    run_json_alias_shape_access_lowers_with_global_evidence_id();
    run_json_computed_key_access_lowers_with_global_evidence_id();
    run_json_static_key_index_lowers_to_direct_field_with_global_evidence_id();
    run_json_open_shape_member_access_lowers_to_dynamic_lookup();
    run_json_open_shape_static_key_index_lowers_to_dynamic_lookup();
    run_record_access_lowers_with_global_evidence_id();
    run_json_codec_calls_bind_exact_source_node_evidence_ids();
    run_json_codec_binding_does_not_fallback_to_source_span();
    run_map_key_access_lowers_with_global_evidence_id();
    run_map_key_access_alias_shape_lowers_with_global_evidence_id();
    run_map_set_method_key_access_lowers_with_global_evidence_id();
    run_strong_source_node_identity_binds_same_line_calls_and_same_name_bodies();
    run_nested_function();
    run_function_expr();
    run_multiple_functions();
    run_template_string();
    run_go_await();
    run_direct_await_go_one_shot();
    run_unique_result_task_await_consumes_handle();
    run_tuple_result_task_await_consumes_handle();
    run_copy_struct_task_result_plans_shared_publication();
    run_go_arg_transfer_modes();
    run_hoisted_sync_handle_capture_is_shared();
    run_channel_send_transfer_modes();
    run_defer_stmt();
    run_defer_args_lower_before_defer();
    run_set_literal();
    run_is_expr();
    run_slice_expr();
    run_range_expr();
    run_range_inclusive_expr();
    run_optional_chain();
    run_optional_call();
    run_struct_literal();
    run_struct_literal_inside_function();
    run_unresolved_struct_literal_does_not_lower_to_json();
    run_struct_field_store_narrows_scalar_rep();
    run_struct_method_receivers_use_call_bound_places();
    run_read_value_struct_param_uses_internal_call_place();
    run_as_to_scalar_rep_int_lowers_to_narrow();
    run_force_unwrap();
    run_destructure_decl();
    run_multi_assign();
    run_enum_access();
    run_import_export_skip();
    run_class_decl_skip();
    run_yield_stmt();
    run_canonical_effect_sidecars_reach_xi();

    teardown();

    printf("=== %d/%d Xi Lower tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
