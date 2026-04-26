/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_match.c - Xray Match expression compiler
 *
 * KEY CONCEPT:
 *   Implementation strategy:
 *   - Use if-else chain to simulate match
 *   - Each branch compiles to: test + conditional_jump + body + jump_to_end
 */

#include "xexpr.h"
#include "../../base/xchecks.h"
#include "xexpr_desc.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "xemit.h"
#include "xregalloc.h"
#include "../parser/xast.h"
#include <stdlib.h>
#include "../../base/xmalloc.h"

// ========== Match Expression Main Compilation Function ==========

/*
 * Compile match expression
 *
 * Strategy: Convert match to if-else chain
 *
 * match x {
 *     1 => "one",
 *     2 => "two",
 *     _ => "other"
 * }
 *
 * Converts to:
 *
 * if (x == 1) {
 *     result = "one"
 * } else if (x == 2) {
 *     result = "two"
 * } else {
 *     result = "other"
 * }
 */
int compile_match_expr(XrCompilerContext *ctx, XrCompiler *c, MatchExprNode *node) {
    XR_DCHECK(ctx != NULL, "compile_match_expr: NULL ctx");
    XR_DCHECK(c != NULL, "compile_match_expr: NULL compiler");
    XR_DCHECK(node != NULL, "compile_match_expr: NULL node");
    // 1. Compile match expression
    XrExprDesc match_expr = xr_compile_expr(ctx, c, node->expr);
    int match_reg = xexpr_to_anyreg(ctx, c, &match_expr);

    // 2. Allocate result register
    int result_reg = reg_alloc(ctx, c);

    // 3. Save all jump-to-end PC list
    int *end_jumps = (int *) xr_malloc(sizeof(int) * node->arm_count);
    int end_jump_count = 0;

    // 4. Compile each branch
    for (int i = 0; i < node->arm_count; i++) {
        AstNode *arm = node->arms[i];
        MatchArmNode *arm_node = &arm->as.match_arm;
        AstNode *pattern = arm_node->pattern;
        AstNode *guard = arm_node->guard;
        AstNode *body = arm_node->body;

        // 4.1 Handle wildcard pattern
        if (pattern->type == AST_PATTERN_WILDCARD) {
            if (guard) {
                // Has guard condition: test guard, if failed jump to next branch
                XrExprDesc guard_e = xr_compile_expr(ctx, c, guard);
                int guard_reg = xexpr_to_anyreg(ctx, c, &guard_e);
                xemit_test(c->emitter, guard_reg, 0);
                int skip_guard = emit_jump(c->emitter, OP_JMP);

                // guard success: compile branch body and jump to match end
                XrExprDesc body_e = xr_compile_expr(ctx, c, body);
                int body_reg = xexpr_to_anyreg(ctx, c, &body_e);
                emit_move(c->emitter, result_reg, body_reg);
                end_jumps[end_jump_count++] = emit_jump(c->emitter, OP_JMP);

                // guard failed: patch jump point, continue to next branch
                patch_jump(c->emitter, skip_guard, -1);
            } else {
                // Wildcard without guard: unconditional match, as final branch
                XrExprDesc body_e = xr_compile_expr(ctx, c, body);
                int body_reg = xexpr_to_anyreg(ctx, c, &body_e);
                emit_move(c->emitter, result_reg, body_reg);
                // Unconditional wildcard is final branch, exit directly
                break;
            }
            continue;  // Wildcard with guard, continue processing next branch
        }

        // 4.2 Handle literal pattern (including binding variables)
        else if (pattern->type == AST_PATTERN_LITERAL) {
            PatternLiteralNode *lit = &pattern->as.pattern_literal;
            AstNode *value = lit->value;

            /*
             * Check if binding variable pattern (identifier + guard condition)
             * Example: n if (n < 0) => "negative"
             * In this case, n is binding variable, need to assign match_reg value to it
             */
            if (value->type == AST_VARIABLE && guard) {
                const char *bind_name = value->as.variable.name;

                // Add binding variable in current scope
                XrString *name_str = xr_compile_time_intern(ctx->X, bind_name, strlen(bind_name));
                int bind_reg = reg_alloc(ctx, c);
                scope_define_local_reg(ctx, c, name_str, bind_reg);

                // Copy match value to binding variable
                emit_move(c->emitter, bind_reg, match_reg);

                // Compile guard condition
                XrExprDesc guard_e = xr_compile_expr(ctx, c, guard);
                int guard_reg = xexpr_to_anyreg(ctx, c, &guard_e);
                xemit_test(c->emitter, guard_reg, 0);
                int skip_guard = emit_jump(c->emitter, OP_JMP);

                // Guard success: compile branch body
                XrExprDesc body_e = xr_compile_expr(ctx, c, body);
                int body_reg = xexpr_to_anyreg(ctx, c, &body_e);
                emit_move(c->emitter, result_reg, body_reg);
                end_jumps[end_jump_count++] = emit_jump(c->emitter, OP_JMP);

                /*
                 * Guard failed: patch jump, continue to next branch
                 * Note: binding variable doesn't need explicit removal, it's just a local variable
                 * definition Subsequent branches will override or scope naturally expires after
                 * branch ends
                 */
                patch_jump(c->emitter, skip_guard, c->emitter->pc);
                continue;
            }

            // Normal literal pattern: compile test value
            XrExprDesc test_e = xr_compile_expr(ctx, c, value);
            int test_reg = xexpr_to_anyreg(ctx, c, &test_e);

            // Generate comparison: EQ match_reg, test_reg, 0 (don't skip when equal)
            xemit_eq(c->emitter, match_reg, test_reg, 0);

            // Skip this branch body when not matching
            int skip_body = emit_jump(c->emitter, OP_JMP);

            // If has guard condition, need extra test
            if (guard) {
                XrExprDesc guard_e = xr_compile_expr(ctx, c, guard);
                int guard_reg = xexpr_to_anyreg(ctx, c, &guard_e);
                xemit_test(c->emitter, guard_reg, 0);  // Test guard condition
                int skip_guard =
                    emit_jump(c->emitter, OP_JMP);  // Guard failed, jump to next branch

                // Compile branch body
                XrExprDesc body_e = xr_compile_expr(ctx, c, body);
                int body_reg = xexpr_to_anyreg(ctx, c, &body_e);
                emit_move(c->emitter, result_reg, body_reg);

                // Jump to match end
                end_jumps[end_jump_count++] = emit_jump(c->emitter, OP_JMP);

                // Patch guard failed jump
                patch_jump(c->emitter, skip_guard, c->emitter->pc);

                // Patch pattern not matching jump
                patch_jump(c->emitter, skip_body, c->emitter->pc);
            } else {
                // No guard condition
                // Compile branch body
                XrExprDesc body_e = xr_compile_expr(ctx, c, body);
                int body_reg = xexpr_to_anyreg(ctx, c, &body_e);
                emit_move(c->emitter, result_reg, body_reg);

                // Jump to match end
                end_jumps[end_jump_count++] = emit_jump(c->emitter, OP_JMP);

                // Patch skip branch body jump
                patch_jump(c->emitter, skip_body, c->emitter->pc);
            }
        }

        // 4.3 Handle range pattern
        else if (pattern->type == AST_PATTERN_RANGE) {
            PatternRangeNode *range = &pattern->as.pattern_range;

            // Compile range start and end
            XrExprDesc start_e = xr_compile_expr(ctx, c, range->start);
            int start_reg = xexpr_to_anyreg(ctx, c, &start_e);
            XrExprDesc end_e = xr_compile_expr(ctx, c, range->end);
            int end_reg = xexpr_to_anyreg(ctx, c, &end_e);

            /*
             * Generate range test (half-open interval [start, end)):
             * if (match_reg >= start_reg && match_reg < end_reg) {
             *     // in range
             * } else {
             *     // jump to next branch
             * }
             */

            // Test match >= start (i.e. !(match < start))
            xemit_lt(c->emitter, match_reg, start_reg,
                     1);  // match < start ? if yes, don't skip next
            int fail1 =
                emit_jump(c->emitter, OP_JMP);  // executed when match < start, jump to next branch

            // Test match < end (i.e. match < end is false => skip next)
            xemit_lt(c->emitter, match_reg, end_reg, 0);  // match < end ? if no, don't skip next
            int fail2 =
                emit_jump(c->emitter, OP_JMP);  // executed when match >= end, jump to next branch

            // In range, if has guard condition, need to test
            if (guard) {
                XrExprDesc guard_e = xr_compile_expr(ctx, c, guard);
                int guard_reg = xexpr_to_anyreg(ctx, c, &guard_e);
                xemit_test(c->emitter, guard_reg, 0);
                int skip_guard = emit_jump(c->emitter, OP_JMP);

                // Compile branch body
                XrExprDesc body_e = xr_compile_expr(ctx, c, body);
                int body_reg = xexpr_to_anyreg(ctx, c, &body_e);
                emit_move(c->emitter, result_reg, body_reg);

                // Jump to match end
                end_jumps[end_jump_count++] = emit_jump(c->emitter, OP_JMP);

                // Patch guard failed jump
                patch_jump(c->emitter, skip_guard, c->emitter->pc);
            } else {
                // No guard condition, compile branch body
                XrExprDesc body_e = xr_compile_expr(ctx, c, body);
                int body_reg = xexpr_to_anyreg(ctx, c, &body_e);
                emit_move(c->emitter, result_reg, body_reg);

                // Jump to match end
                end_jumps[end_jump_count++] = emit_jump(c->emitter, OP_JMP);
            }

            // Patch range test failed jumps
            patch_jump(c->emitter, fail1, c->emitter->pc);
            patch_jump(c->emitter, fail2, c->emitter->pc);
        }

        // 4.4 Handle multi-value pattern
        else if (pattern->type == AST_PATTERN_MULTI) {
            PatternMultiNode *multi = &pattern->as.pattern_multi;

            // Generate test for each value
            int *value_tests = (int *) xr_malloc(sizeof(int) * multi->count);

            for (int j = 0; j < multi->count; j++) {
                AstNode *value_pattern = multi->patterns[j];
                if (value_pattern->type == AST_PATTERN_LITERAL) {
                    AstNode *value = value_pattern->as.pattern_literal.value;
                    XrExprDesc test_e = xr_compile_expr(ctx, c, value);
                    int test_reg = xexpr_to_anyreg(ctx, c, &test_e);

                    // Test equality
                    xemit_eq(c->emitter, match_reg, test_reg, 1);  // skip next when equal
                    value_tests[j] =
                        emit_jump(c->emitter, OP_JMP);  // jump to branch body when equal
                }
            }

            // All tests failed, jump to next branch
            int skip_body = emit_jump(c->emitter, OP_JMP);

            // Patch all success jumps to here
            for (int j = 0; j < multi->count; j++) {
                patch_jump(c->emitter, value_tests[j], c->emitter->pc);
            }

            // If has guard condition, need to test
            if (guard) {
                XrExprDesc guard_e = xr_compile_expr(ctx, c, guard);
                int guard_reg = xexpr_to_anyreg(ctx, c, &guard_e);
                xemit_test(c->emitter, guard_reg, 0);
                int skip_guard = emit_jump(c->emitter, OP_JMP);

                // Compile branch body
                XrExprDesc body_e = xr_compile_expr(ctx, c, body);
                int body_reg = xexpr_to_anyreg(ctx, c, &body_e);
                emit_move(c->emitter, result_reg, body_reg);

                // Jump to match end
                end_jumps[end_jump_count++] = emit_jump(c->emitter, OP_JMP);

                // Patch guard failed jump
                patch_jump(c->emitter, skip_guard, c->emitter->pc);
            } else {
                // No guard condition, compile branch body
                XrExprDesc body_e = xr_compile_expr(ctx, c, body);
                int body_reg = xexpr_to_anyreg(ctx, c, &body_e);
                emit_move(c->emitter, result_reg, body_reg);

                // Jump to match end
                end_jumps[end_jump_count++] = emit_jump(c->emitter, OP_JMP);
            }

            // Patch skip branch body
            patch_jump(c->emitter, skip_body, c->emitter->pc);

            xr_free(value_tests);
        }

        else {
            xr_compiler_error(ctx, c, "unsupported pattern type: %d", pattern->type);
        }
    }

    // 5. Patch all end jumps
    for (int i = 0; i < end_jump_count; i++) {
        patch_jump(c->emitter, end_jumps[i], c->emitter->pc);
    }

    // 6. Cleanup
    xr_free(end_jumps);

    // 7. Return result register
    return result_reg;
}
