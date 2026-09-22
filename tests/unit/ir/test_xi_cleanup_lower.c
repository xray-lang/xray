/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_cleanup_lower.c - Source-backed lexical cleanup identity tests
 */

#include "../test_framework.h"
#include "analysis/xglobal_summary.h"
#include "frontend/analyzer/xa_typed_program.h"
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/canonical/xcanon.h"
#include "frontend/parser/xast_nodes.h"
#include "frontend/parser/xast_types.h"
#include "frontend/parser/xparse.h"
#include "ir/xi_cleanup.h"
#include "ir/xi_edit.h"
#include "ir/xi_lower.h"
#include "ir/xi_verify.h"
#include "toolchain/xcompiler_session.h"
#include "xray_vm.h"

/* Destroy frontend storage before inspecting Xi. Records and their members
 * must be owned by the receiving function, not the AST or cleanup-site array. */
static XiFunc *lower_cleanup_source(XrVMRuntime *runtime, const char *source) {
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(runtime);
    const XrCompileUnitIdentity identity = {
        .kind = XR_COMPILE_UNIT_MEMORY,
        .module_identity = "memory-module-v1:id=19:cleanup-frontier-v1",
    };
    if (!session || !xr_compiler_session_set_compile_unit_identity(session, &identity))
        return NULL;
    AstNode *program = xr_parse(session, source);
    XaAnalyzer *analyzer = program ? xa_analyzer_new(session) : NULL;
    XiFunc *result = NULL;
    if (analyzer) {
        xa_analyzer_analyze(analyzer, "cleanup.xr", program);
        XrCompilerSessionScope canon_scope;
        bool has_scope = program->type == AST_PROGRAM && program->as.program.arena &&
                         xr_compiler_session_push_arena(session, program->as.program.arena,
                                                        "cleanup.xr", &canon_scope);
        xr_canon_program(program, analyzer, session);
        if (has_scope)
            xr_compiler_session_pop_arena(&canon_scope);
        XaTypedProgramPublishResult typed = xa_typed_program_publish(analyzer, program, NULL, 0);
        analyzer->current_file = "cleanup.xr";
        if (typed.program)
            result = xi_lower_program(typed.program, runtime, false, NULL);
        else
            fprintf(stderr, "cleanup typed source rejected: %s\n",
                    typed.detail ? typed.detail : "");
        xa_typed_program_free(typed.program);
    }
    xa_analyzer_free(analyzer);
    if (program)
        xr_program_destroy(program);
    (void) xr_compiler_session_set_compile_unit_identity(session, NULL);
    return result;
}

typedef struct CleanupSourceCounts {
    uint32_t heads;
    uint32_t largest_frontier;
    uint32_t cross_block_pairs;
    uint32_t fatal;
    bool lifo_lines;
} CleanupSourceCounts;

static bool inspect_cleanup_tree(const XiFunc *function, CleanupSourceCounts *counts) {
    char error[192];
    if (!function || !xi_cleanup_verify(function, error, sizeof(error)) ||
        !xi_verify_stage(function, function->stage, error, sizeof(error))) {
        fprintf(stderr, "cleanup source identity rejected: %s\n", function ? error : "no Xi");
        if (function)
            xi_func_dump(function, stderr);
        return false;
    }
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        for (uint32_t index = 0u; index < block->nvalues; ++index) {
            const XiValue *value = block->values[index];
            if (!value || value->op != XI_CLEANUP_ENTER)
                continue;
            const XiCleanupBoundary *boundary = value->cleanup_boundary;
            if (boundary->frontier == value) {
                ++counts->heads;
                if (boundary->rank > counts->largest_frontier)
                    counts->largest_frontier = boundary->rank;
            }
            counts->cross_block_pairs += boundary->leave && boundary->leave->block != value->block;
            counts->fatal += boundary->kind == XI_CLEANUP_BOUNDARY_FATAL;
            if (boundary->remaining && boundary->remaining->line >= value->line)
                counts->lifo_lines = false;
        }
    }
    for (uint16_t child = 0u; child < function->nchildren; ++child) {
        if (!inspect_cleanup_tree(function->children[child], counts))
            return false;
    }
    return true;
}

static bool inspect_source(const char *source, CleanupSourceCounts *counts) {
    XrVMConfig config = {0};
    XrVMRuntime *runtime = xray_vm_new_full(&config);
    if (!runtime)
        return false;
    XiFunc *function = lower_cleanup_source(runtime, source);
    *counts = (CleanupSourceCounts) {.lifo_lines = true};
    bool valid = inspect_cleanup_tree(function, counts);
    xi_func_free(function);
    xray_vm_delete(runtime);
    return valid;
}

TEST(three_source_defers_keep_distinct_lifo_frontiers) {
    CleanupSourceCounts counts;
    bool valid = inspect_source("fn run() {\n"
                                "  var value = 0\n"
                                "  defer { value = value + 1 }\n"
                                "  defer { value = value + 2 }\n"
                                "  defer { value = value + 3 }\n"
                                "  value = 42\n"
                                "}\n",
                                &counts);
    ASSERT(valid);
    ASSERT(counts.heads >= 2u);
    ASSERT_EQ_INT(counts.largest_frontier, 3u);
    ASSERT(counts.lifo_lines);
    ASSERT_EQ_INT(counts.fatal, 0u);
}

TEST(branching_cleanup_pairs_are_not_confused_with_block_layout) {
    CleanupSourceCounts counts;
    bool valid = inspect_source("fn run(flag: bool) {\n"
                                "  var value = 0\n"
                                "  defer { value = value + 1 }\n"
                                "  defer { if (flag) { value = 2 } else { value = 3 } }\n"
                                "  value = 42\n"
                                "}\n",
                                &counts);
    ASSERT(valid);
    ASSERT_EQ_INT(counts.largest_frontier, 2u);
    ASSERT(counts.cross_block_pairs > 0u);
    ASSERT(counts.lifo_lines);
}

TEST(nested_cleanup_bodies_keep_independent_frontier_occurrences) {
    CleanupSourceCounts counts;
    bool valid = inspect_source("fn run() {\n"
                                "  var value = 0\n"
                                "  defer { value = value + 1 }\n"
                                "  defer {\n"
                                "    defer { value = value + 2 }\n"
                                "    value = value + 3\n"
                                "  }\n"
                                "  value = 42\n"
                                "}\n",
                                &counts);
    ASSERT(valid);
    ASSERT_EQ_INT(counts.largest_frontier, 2u);
    ASSERT(counts.heads >= 4u);
    ASSERT(counts.lifo_lines);
}

TEST(nested_cleanup_can_read_outer_cleanup_local) {
    CleanupSourceCounts counts;
    bool valid = inspect_source("fn run(flag: bool) {\n"
                                "  var value = 0\n"
                                "  defer {\n"
                                "    var inner = 2\n"
                                "    defer {\n"
                                "      if (flag) { value = inner } else { value = inner + 1 }\n"
                                "    }\n"
                                "    value = value + 3\n"
                                "  }\n"
                                "  value = 42\n"
                                "}\n",
                                &counts);
    ASSERT(valid);
    ASSERT(counts.heads >= 3u);
    ASSERT(counts.lifo_lines);
}

TEST(possible_cleanup_panic_does_not_erase_the_normal_boundary) {
    CleanupSourceCounts counts;
    bool valid = inspect_source("fn run(condition: bool) {\n"
                                "  defer { assert(condition) }\n"
                                "  print(42)\n"
                                "}\n",
                                &counts);
    ASSERT(valid);
    ASSERT_EQ_INT(counts.fatal, 0u);
    ASSERT(counts.heads > 0u);
    ASSERT_EQ_INT(counts.largest_frontier, 1u);
}

TEST(primitive_panic_exits_have_independent_nested_cleanup_frontiers) {
    XrVMConfig config = {0};
    XrVMRuntime *runtime = xray_vm_new_full(&config);
    ASSERT(runtime != NULL);
    XiFunc *root = lower_cleanup_source(runtime,
        "fn run(divisor: i64) -> i64 {\n"
        "  var count = 0\n"
        "  defer { count = count + 1 }\n"
        "  {\n"
        "    defer { if (divisor > 0) { count = count + 2 } else { count = count + 3 } }\n"
        "    const quotient = 84 / divisor\n"
        "    const remainder = 85 % divisor\n"
        "    return quotient + remainder\n"
        "  }\n"
        "}\n");
    XgGlobalEvidence evidence = {0};
    char error[256] = {0};
    bool valid = root && xi_normalize_panic_exits(root, &evidence, runtime, error, sizeof(error));
    XiFunc *function = root && root->nchildren == 1u ? root->children[0] : NULL;
    uint32_t receivers = 0u;
    uint32_t ended_regions = 0u;
    if (valid && function) {
        for (uint32_t b = 0u; b < function->nblocks; ++b) {
            XiBlock *block = function->blocks[b];
            if (block->exit_reason != XI_EXIT_REASON_PANIC)
                continue;
            for (uint32_t i = 0u; i < block->nvalues; ++i) {
                XiValue *value = block->values[i];
                ended_regions += value->op == XI_END_TRY;
                if (value->op != XI_CATCH || value->aux_int != XI_CATCH_AUX_POINT_PANIC)
                    continue;
                XiValue *point = value->aux;
                valid = valid && point && point->block && point->block->nvalues > 0u &&
                        point->block->values[0] == point && block->npreds == 1u &&
                        block->preds[0] == point->block &&
                        (point->op == XI_DIV || point->op == XI_MOD);
                ++receivers;
            }
        }
        valid = valid && xi_cleanup_verify(function, error, sizeof(error));
        XiEditFingerprint before = xi_edit_fingerprint(function);
        valid = valid && xi_normalize_panic_exits(root, &evidence, runtime, error, sizeof(error));
        XiEditFingerprint after = xi_edit_fingerprint(function);
        valid = valid && before.cfg == after.cfg && before.values == after.values;
    } else {
        valid = false;
    }
    if (!valid)
        fprintf(stderr, "normalized cleanup rejected: %s\n", error);
    xi_func_free(root);
    xray_vm_delete(runtime);
    ASSERT(valid);
    ASSERT_EQ_INT(receivers, 2u);
    ASSERT_EQ_INT(ended_regions, 4u);
}

TEST(panic_normalization_preserves_constructive_error_consumer) {
    XrVMConfig config = {0};
    XrVMRuntime *runtime = xray_vm_new_full(&config);
    ASSERT(runtime);
    XiFunc *root = lower_cleanup_source(
        runtime, "fn part(value:i64)->i64 { return value / 1000 }\n"
                 "class Box {\n"
                 "  value:i64\n"
                 "  constructor(value:i64) { this.value=part(value) }\n"
                 "}\nconst box=Box(1000)\n");
    XiValue *check = NULL;
    for (uint32_t b = 0u; root && b < root->nblocks; ++b)
        for (uint32_t i = 0u; i < root->blocks[b]->nvalues; ++i) {
            XiValue *value = root->blocks[b]->values[i];
            if (value->op == XI_ERR_CHECK && value->error_producer)
                check = value;
        }
    bool valid = root && check && check->error_producer->block == check->block;
    char error[256] = {0};
    if (valid) {
        XiValue *producer = check->error_producer;
        root->xg_body_func_id = 1u;
        producer->xg_callsite_id = 1u;
        XgCallsiteSummary site = {
            .callsite_id = 1u, .owner_func_id = 1u,
            .flags = XG_CALL_ERROR_EFFECT_VERIFIED | XG_CALL_MAY_PANIC,
        };
        XgGlobalEvidence evidence = {.callsites = &site, .ncallsites = 1u};
        valid = xi_normalize_panic_exits(root, &evidence, runtime, error, sizeof(error)) &&
                xi_err_check_producer(root, check) == producer &&
                check->block == producer->block &&
                check->block->control == check &&
                check->block->kind == XI_BLOCK_IF &&
                xi_value_has_panic_continuation(root, producer) &&
                xi_verify_stage(root, root->stage, error, sizeof(error));
        XiEditFingerprint before = xi_edit_fingerprint(root);
        valid = valid && xi_normalize_panic_exits(root, &evidence, runtime, error, sizeof(error));
        XiEditFingerprint after = xi_edit_fingerprint(root);
        valid = valid && before.cfg == after.cfg && before.values == after.values;
    }
    if (!valid)
        fprintf(stderr, "constructive error normalization rejected: %s\n", error);
    xi_func_free(root);
    xray_vm_delete(runtime);
    ASSERT(valid);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Xi cleanup source identities");
RUN_TEST(three_source_defers_keep_distinct_lifo_frontiers);
RUN_TEST(branching_cleanup_pairs_are_not_confused_with_block_layout);
RUN_TEST(nested_cleanup_bodies_keep_independent_frontier_occurrences);
RUN_TEST(nested_cleanup_can_read_outer_cleanup_local);
RUN_TEST(possible_cleanup_panic_does_not_erase_the_normal_boundary);
RUN_TEST(primitive_panic_exits_have_independent_nested_cleanup_frontiers);
RUN_TEST(panic_normalization_preserves_constructive_error_consumer);
TEST_MAIN_END()
