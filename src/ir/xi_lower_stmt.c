/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower_stmt.c - Compound statement lowering (extracted from xi_lower.c)
 *
 * Contains: select, scope_block, for-in loops, try-catch, match expressions.
 * These are the larger, self-contained statement/expression lowering functions.
 */

#include "xi_lower_internal.h"
#include "xi.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"

#include <string.h>
#include <stdio.h>

/* Forward declaration */
static void lower_stmts(XiLower *l, AstNode **stmts, int count);

/* ========== Select Statement ========== */

XR_FUNC void xi_lower_select(XiLower *l, AstNode *node) {
    SelectStmtNode *sel = &node->as.select_stmt;
    int n = sel->case_count;

    XiBlock *merge = xi_block_new(l->func);
    int max_cases = n > 32 ? 32 : n;

    for (int i = 0; i < max_cases; i++) {
        AstNode *case_node = sel->cases[i];
        SelectCaseNode *sc = &case_node->as.select_case;

        if (sc->is_default || sc->is_timeout) {
            /* Default and after-timeout cases execute when no channel case
             * fired in the non-blocking try-recv/try-send chain above. */
            xi_lower_stmt(l, sc->body);
            if (l->cur_block)
                xi_block_set_jump(l->cur_block, merge);
        } else {
            XiBlock *body_blk = xi_block_new(l->func);
            XiBlock *next_blk = xi_block_new(l->func);

            if (sc->is_send) {
                XiValue *chan = xi_lower_expr(l, sc->channel);
                XiValue *val = xi_lower_expr(l, sc->value);
                if (chan && val) {
                    XiValue *send = xi_value_new(l->func, l->cur_block,
                                                  XI_CHAN_TRY_SEND, l->type_bool, 2);
                    if (send) {
                        send->args[0] = chan;
                        send->args[1] = val;
                        send->flags |= XI_FLAG_SIDE_EFFECT;
                        xi_block_set_if(l->cur_block, send, body_blk, next_blk);
                    }
                }
            } else {
                XiValue *chan = xi_lower_expr(l, sc->channel);
                if (chan) {
                    struct XrType *val_type = l->type_any;
                    XiValue *recv = xi_value_new(l->func, l->cur_block,
                                                  XI_CHAN_TRY_RECV, val_type, 1);
                    if (recv) {
                        recv->args[0] = chan;
                        recv->flags |= XI_FLAG_SIDE_EFFECT;
                    }
                    /* Try-recv returns null on empty channel.
                     * Branch: null→next_blk (try next case),
                     *         non-null→body_blk (handle this case). */
                    XiValue *is_null = xi_value_new(l->func, l->cur_block,
                                                     XI_ISNULL, l->type_bool, 1);
                    if (is_null && recv) {
                        is_null->args[0] = recv;
                        xi_block_set_if(l->cur_block, is_null, next_blk, body_blk);
                    }
                    if (sc->var_name && recv) {
                        int var_id = xi_lower_var_create(l, sc->var_symbol_id,
                                                          sc->var_name, val_type);
                        xi_lower_braun_write(l, var_id, body_blk, recv);
                    }
                }
            }

            xi_lower_braun_seal(l, body_blk);
            xi_lower_braun_seal(l, next_blk);

            l->cur_block = body_blk;
            xi_lower_stmt(l, sc->body);
            if (l->cur_block)
                xi_block_set_jump(l->cur_block, merge);

            l->cur_block = next_blk;
        }
    }

    if (l->cur_block && l->cur_block != merge)
        xi_block_set_jump(l->cur_block, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
}

/* ========== Scope Block ========== */

XR_FUNC XiValue *xi_lower_scope_block(XiLower *l, AstNode *node) {
    ScopeBlockNode *sb = &node->as.scope_block;

    XiValue *enter = xi_value_new(l->func, l->cur_block,
                                  XI_SCOPE_ENTER, l->type_void, 0);
    if (enter) {
        enter->aux_int = sb->scope_mode;
        enter->flags |= XI_FLAG_SIDE_EFFECT;
        enter->line = (uint32_t) node->line;
    }

    xi_lower_stmt(l, sb->body);

    struct XrType *res_type = (sb->scope_mode == 2) ? l->type_any : l->type_void;
    XiValue *exit_v = xi_value_new(l->func, l->cur_block,
                                   XI_SCOPE_EXIT, res_type, 0);
    if (exit_v) {
        exit_v->aux_int = sb->scope_mode;
        exit_v->flags |= XI_FLAG_SIDE_EFFECT;
        exit_v->line = (uint32_t) node->line;
    }
    return exit_v;
}

/* ========== Pattern Test ========== */

XR_FUNC XiValue *xi_lower_pattern_test(XiLower *l, XiValue *subject, AstNode *pattern) {
    if (!pattern || !subject) return NULL;

    switch (pattern->type) {
        case AST_PATTERN_WILDCARD:
            return xi_const_bool(l->func, l->cur_block, true, l->type_bool);

        case AST_PATTERN_LITERAL: {
            XiValue *lit = xi_lower_expr(l, pattern->as.pattern_literal.value);
            if (!lit) return NULL;
            return xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, subject, lit);
        }

        case AST_PATTERN_RANGE: {
            /* Half-open interval [start, end), consistent with for-in range. */
            XiValue *start = xi_lower_expr(l, pattern->as.pattern_range.start);
            XiValue *end = xi_lower_expr(l, pattern->as.pattern_range.end);
            if (!start || !end) return NULL;
            XiValue *ge = xi_binary(l->func, l->cur_block, XI_GE,
                                     l->type_bool, subject, start);
            XiValue *lt = xi_binary(l->func, l->cur_block, XI_LT,
                                     l->type_bool, subject, end);
            return xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, ge, lt);
        }

        case AST_PATTERN_MULTI: {
            PatternMultiNode *mp = &pattern->as.pattern_multi;
            XiValue *result = NULL;
            for (int i = 0; i < mp->count; i++) {
                XiValue *test = xi_lower_pattern_test(l, subject, mp->patterns[i]);
                if (!test) continue;
                if (!result)
                    result = test;
                else
                    result = xi_binary(l->func, l->cur_block, XI_BOR,
                                        l->type_bool, result, test);
            }
            return result ? result : xi_const_bool(l->func, l->cur_block,
                                                     false, l->type_bool);
        }

        default:
            return xi_const_bool(l->func, l->cur_block, false, l->type_bool);
    }
}

/* ========== Match Expression ========== */

XR_FUNC XiValue *xi_lower_match(XiLower *l, AstNode *node) {
    MatchExprNode *m = &node->as.match_expr;
    XiValue *subject = xi_lower_expr(l, m->expr);
    if (!subject) return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiBlock *merge = xi_block_new(l->func);
    int arm_count = m->arm_count;

    XiBlock *body_exits[32];
    XiValue *body_vals[32];
    int exit_count = 0;
    int max_arms = arm_count > 32 ? 32 : arm_count;

    for (int i = 0; i < max_arms; i++) {
        AstNode *arm_node = m->arms[i];
        MatchArmNode *arm = &arm_node->as.match_arm;

        /* Detect binding pattern: AST_PATTERN_LITERAL wrapping AST_VARIABLE.
         * Bind the match subject to the named variable so guard expressions
         * (and the arm body) can reference it. The pattern test is always
         * true — selection is determined entirely by the guard. */
        bool is_binding = false;
        if (arm->pattern && arm->pattern->type == AST_PATTERN_LITERAL) {
            AstNode *pval = arm->pattern->as.pattern_literal.value;
            if (pval && pval->type == AST_VARIABLE) {
                is_binding = true;
                const char *bname = pval->as.variable.name;
                uint32_t bsid = pval->as.variable.symbol_id;
                int var_id = xi_lower_var_create(l, bsid, bname,
                                                  subject->type ? subject->type : l->type_any);
                xi_lower_braun_write(l, var_id, l->cur_block, subject);
            }
        }

        XiValue *test;
        if (is_binding) {
            /* Binding always matches; guard narrows. */
            test = arm->guard ? xi_lower_expr(l, arm->guard) : NULL;
        } else {
            test = xi_lower_pattern_test(l, subject, arm->pattern);
            if (arm->guard && test) {
                XiValue *guard = xi_lower_expr(l, arm->guard);
                if (guard)
                    test = xi_binary(l->func, l->cur_block, XI_BAND,
                                      l->type_bool, test, guard);
            }
        }

        bool is_last = (i == max_arms - 1);

        if (is_last || !test) {
            XiValue *val = xi_lower_expr(l, arm->body);
            if (l->cur_block) {
                if (exit_count < 32) {
                    body_exits[exit_count] = l->cur_block;
                    body_vals[exit_count] = val;
                    exit_count++;
                }
                xi_block_set_jump(l->cur_block, merge);
            }
        } else {
            XiBlock *body_blk = xi_block_new(l->func);
            XiBlock *next_blk = xi_block_new(l->func);
            xi_block_set_if(l->cur_block, test, body_blk, next_blk);
            xi_lower_braun_seal(l, body_blk);
            xi_lower_braun_seal(l, next_blk);

            l->cur_block = body_blk;
            XiValue *val = xi_lower_expr(l, arm->body);
            if (l->cur_block) {
                if (exit_count < 32) {
                    body_exits[exit_count] = l->cur_block;
                    body_vals[exit_count] = val;
                    exit_count++;
                }
                xi_block_set_jump(l->cur_block, merge);
            }

            l->cur_block = next_blk;
        }
    }

    if (l->cur_block && l->cur_block != merge) {
        if (exit_count < 32) {
            body_exits[exit_count] = l->cur_block;
            body_vals[exit_count] = xi_const_null(l->func, l->cur_block, l->type_null);
            exit_count++;
        }
        xi_block_set_jump(l->cur_block, merge);
    }

    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
    if (!l->cur_block) return NULL;

    if (merge->npreds == 1) {
        return (exit_count > 0) ? body_vals[0] : NULL;
    }

    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (!phi) return NULL;
    for (uint16_t p = 0; p < merge->npreds; p++) {
        phi->value.args[p] = xi_const_null(l->func, merge, l->type_null);
        for (int j = 0; j < exit_count; j++) {
            if (merge->preds[p] == body_exits[j]) {
                phi->value.args[p] = body_vals[j] ? body_vals[j]
                    : xi_const_null(l->func, merge, l->type_null);
                break;
            }
        }
    }
    return &phi->value;
}

/* ========== For-In Loop (index-based) ========== */

static void lower_for_in_loop(XiLower *l, AstNode *node,
                               XiValue *init_val, XiValue *limit,
                               XiValue *get_item_coll) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    struct XrType *item_type = s->item_type ? s->item_type : l->type_any;

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__for_idx_%d", sid);
    char *idx_name = (char *)xi_func_arena_alloc(l->func, (uint32_t)(strlen(buf) + 1));
    XR_DCHECK(idx_name != NULL, "arena alloc failed for idx_name");
    memcpy(idx_name, buf, strlen(buf) + 1);

    snprintf(buf, sizeof(buf), "__for_lim_%d", sid);
    char *lim_name = (char *)xi_func_arena_alloc(l->func, (uint32_t)(strlen(buf) + 1));
    XR_DCHECK(lim_name != NULL, "arena alloc failed for lim_name");
    memcpy(lim_name, buf, strlen(buf) + 1);

    snprintf(buf, sizeof(buf), "__for_col_%d", sid);
    char *col_name = (char *)xi_func_arena_alloc(l->func, (uint32_t)(strlen(buf) + 1));
    XR_DCHECK(col_name != NULL, "arena alloc failed for col_name");
    memcpy(col_name, buf, strlen(buf) + 1);

    int idx_var = xi_lower_var_create(l, 0, idx_name, l->type_int);
    int lim_var = xi_lower_var_create(l, 0, lim_name, l->type_int);
    xi_lower_braun_write(l, idx_var, l->cur_block, init_val);
    xi_lower_braun_write(l, lim_var, l->cur_block, limit);

    int col_var = -1;
    if (get_item_coll) {
        col_var = xi_lower_var_create(l, 0, col_name, l->type_any);
        xi_lower_braun_write(l, col_var, l->cur_block, get_item_coll);
    }

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *incr_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    xi_block_set_jump(l->cur_block, cond_blk);

    l->cur_block = cond_blk;
    XiValue *cur_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
    XiValue *cur_lim = xi_lower_braun_read(l, lim_var, l->cur_block);
    XR_DCHECK(cur_idx != NULL, "braun_read idx must not be NULL");
    XiValue *cond = xi_binary(l->func, l->cur_block, XI_LT,
                              l->type_bool, cur_idx, cur_lim);
    if (cond)
        xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);

    xi_lower_braun_seal(l, body_blk);

    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = incr_blk;

    l->cur_block = body_blk;
    XiValue *body_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
    XiValue *item;

    if (get_item_coll) {
        XiValue *body_col = xi_lower_braun_read(l, col_var, l->cur_block);
        item = xi_value_new(l->func, l->cur_block, XI_INDEX_GET,
                            item_type, 2);
        if (item) {
            item->args[0] = body_col;
            item->args[1] = body_idx;
            item->line = (uint32_t)node->line;
        }
    } else {
        item = body_idx;
    }

    int item_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    if (item) xi_lower_braun_write(l, item_var, l->cur_block, item);

    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, incr_blk);

    xi_lower_braun_seal(l, incr_blk);

    l->cur_block = incr_blk;
    if (incr_blk->npreds > 0) {
        XiValue *inc_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
        XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
        XiValue *new_idx = xi_binary(l->func, l->cur_block, XI_ADD,
                                     l->type_int, inc_idx, one);
        if (new_idx) xi_lower_braun_write(l, idx_var, l->cur_block, new_idx);
    }
    if (l->cur_block && incr_blk->npreds > 0)
        xi_block_set_jump(l->cur_block, cond_blk);

    xi_lower_braun_seal(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* ========== For-In Key-Value (iterator protocol) ========== */

static void lower_for_in_keyvalue(XiLower *l, AstNode *node) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    uint32_t line = (uint32_t)node->line;

    XiValue *coll = xi_lower_expr(l, s->collection);
    if (!coll || !l->cur_block) return;

    XiValue *iter = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                 l->type_any, 1);
    if (!iter) return;
    iter->args[0] = coll;
    iter->aux = (void *)"entriesIterator";
    iter->aux_int = (int64_t)xi_lower_method_symbol(l, "entriesIterator") << 1;
    iter->flags |= XI_FLAG_SIDE_EFFECT;
    iter->line = line;

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__kv_iter_%d", sid);
    char *iter_name = (char *)xi_func_arena_alloc(l->func, (uint32_t)(strlen(buf) + 1));
    XR_DCHECK(iter_name != NULL, "arena alloc failed");
    memcpy(iter_name, buf, strlen(buf) + 1);
    int iter_var = xi_lower_var_create(l, 0, iter_name, l->type_any);
    xi_lower_braun_write(l, iter_var, l->cur_block, iter);

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    xi_block_set_jump(l->cur_block, cond_blk);

    l->cur_block = cond_blk;
    XiValue *iter_cond = xi_lower_braun_read(l, iter_var, l->cur_block);
    XiValue *has_next = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                     l->type_bool, 1);
    if (!has_next) return;
    has_next->args[0] = iter_cond;
    has_next->aux = (void *)"hasNext";
    has_next->aux_int = (int64_t)xi_lower_method_symbol(l, "hasNext") << 1;
    has_next->flags |= XI_FLAG_SIDE_EFFECT;
    has_next->line = line;
    xi_block_set_if(l->cur_block, has_next, body_blk, exit_blk);

    xi_lower_braun_seal(l, body_blk);

    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = cond_blk;

    l->cur_block = body_blk;
    XiValue *iter_body = xi_lower_braun_read(l, iter_var, l->cur_block);
    XiValue *entry = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                  l->type_any, 1);
    if (!entry) return;
    entry->args[0] = iter_body;
    entry->aux = (void *)"next";
    entry->aux_int = (int64_t)xi_lower_method_symbol(l, "next") << 1;
    entry->flags |= XI_FLAG_SIDE_EFFECT;
    entry->line = line;

    struct XrType *item_type = s->item_type ? s->item_type : l->type_any;

    XiValue *idx0 = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    XiValue *key_val = xi_value_new(l->func, l->cur_block, XI_INDEX_GET,
                                    item_type, 2);
    if (key_val) {
        key_val->args[0] = entry;
        key_val->args[1] = idx0;
        key_val->line = line;
    }
    int key_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    if (key_val) xi_lower_braun_write(l, key_var, l->cur_block, key_val);

    if (s->value_name) {
        XiValue *idx1 = xi_const_int(l->func, l->cur_block, 1, l->type_int);
        XiValue *val_val = xi_value_new(l->func, l->cur_block, XI_INDEX_GET,
                                        l->type_any, 2);
        if (val_val) {
            val_val->args[0] = entry;
            val_val->args[1] = idx1;
            val_val->line = line;
        }
        int val_var = xi_lower_var_create(l, s->value_symbol_id, s->value_name, l->type_any);
        if (val_val) xi_lower_braun_write(l, val_var, l->cur_block, val_val);
    }

    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, cond_blk);

    xi_lower_braun_seal(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* ========== For-In: Custom Iterator Protocol ========== */

/* Lower `for (item in obj)` where obj has an iterator() method returning
 * an object with hasNext(): bool and next(): T.
 *
 * Desugars to:
 *   let __iter = obj.iterator()
 *   while (__iter.hasNext()) {
 *       let item = __iter.next()
 *       <body>
 *   }
 */
static void lower_for_in_custom_iterator(XiLower *l, AstNode *node,
                                          XiValue *coll) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    uint32_t line = (uint32_t)node->line;

    /* Call iterator() on the collection */
    XiValue *iter = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                 l->type_any, 1);
    if (!iter) return;
    iter->args[0] = coll;
    iter->aux = (void *)"iterator";
    iter->aux_int = (int64_t)xi_lower_method_symbol(l, "iterator") << 1;
    iter->flags |= XI_FLAG_SIDE_EFFECT;
    iter->line = line;

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__ci_iter_%d", sid);
    char *iter_name = (char *)xi_func_arena_alloc(l->func, (uint32_t)(strlen(buf) + 1));
    XR_DCHECK(iter_name != NULL, "arena alloc failed");
    memcpy(iter_name, buf, strlen(buf) + 1);
    int iter_var = xi_lower_var_create(l, 0, iter_name, l->type_any);
    xi_lower_braun_write(l, iter_var, l->cur_block, iter);

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    xi_block_set_jump(l->cur_block, cond_blk);

    /* Condition: __iter.hasNext() */
    l->cur_block = cond_blk;
    XiValue *iter_cond = xi_lower_braun_read(l, iter_var, l->cur_block);
    XiValue *has_next = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                     l->type_bool, 1);
    if (!has_next) return;
    has_next->args[0] = iter_cond;
    has_next->aux = (void *)"hasNext";
    has_next->aux_int = (int64_t)xi_lower_method_symbol(l, "hasNext") << 1;
    has_next->flags |= XI_FLAG_SIDE_EFFECT;
    has_next->line = line;
    xi_block_set_if(l->cur_block, has_next, body_blk, exit_blk);

    xi_lower_braun_seal(l, body_blk);

    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = cond_blk;

    /* Body: let item = __iter.next(); <body> */
    l->cur_block = body_blk;
    XiValue *iter_body = xi_lower_braun_read(l, iter_var, l->cur_block);
    XiValue *next_val = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                      l->type_any, 1);
    if (!next_val) return;
    next_val->args[0] = iter_body;
    next_val->aux = (void *)"next";
    next_val->aux_int = (int64_t)xi_lower_method_symbol(l, "next") << 1;
    next_val->flags |= XI_FLAG_SIDE_EFFECT;
    next_val->line = line;

    struct XrType *item_type = s->item_type ? s->item_type : l->type_any;
    int item_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    xi_lower_braun_write(l, item_var, l->cur_block, next_val);

    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, cond_blk);

    xi_lower_braun_seal(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* ========== For-In Dispatcher ========== */

/* Check if collection type uses builtin length+index iteration (array,
 * string, map, set, bytes) or needs the custom iterator protocol. */
static bool is_builtin_iterable_collection(XiLower *l, AstNode *coll_node) {
    struct XrType *t = xi_lower_node_type(l, coll_node);
    if (!t || t->kind == XR_KIND_UNKNOWN)
        return true;  /* unknown: assume builtin for backward compat */
    return xr_kind_is_builtin_iterable(t->kind);
}

XR_FUNC void xi_lower_for_in(XiLower *l, AstNode *node) {
    ForInStmtNode *s = &node->as.for_in_stmt;

    if (s->is_keyvalue) {
        lower_for_in_keyvalue(l, node);
        return;
    }

    if (s->collection->type == AST_RANGE) {
        RangeNode *rn = &s->collection->as.range;
        XiValue *start = xi_lower_expr(l, rn->start);
        if (!start || !l->cur_block) return;
        XiValue *end = xi_lower_expr(l, rn->end);
        if (!end || !l->cur_block) return;
        lower_for_in_loop(l, node, start, end, NULL);
        return;
    }

    XiValue *coll = xi_lower_expr(l, s->collection);
    if (!coll || !l->cur_block) return;

    /* Class instances with iterator() use the custom protocol */
    if (!is_builtin_iterable_collection(l, s->collection)) {
        lower_for_in_custom_iterator(l, node, coll);
        return;
    }

    XiValue *len = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD,
                                l->type_int, 1);
    if (!len) return;
    len->args[0] = coll;
    len->aux = (void *)"length";
    len->line = (uint32_t)node->line;

    XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    lower_for_in_loop(l, node, zero, len, coll);
}

/* ========== Try-Catch ========== */

/* try-finally (no catch): inline finally code on both normal and exception
 * paths.  A separate finally block cannot resolve SSA variables because the
 * exception path bypasses the normal CFG, leaving the block with zero
 * predecessors for Braun read.
 *
 * Normal path:    try body → [finally inline] → merge → OP_END_TRY
 * Exception path: OP_CATCH → [finally inline] → OP_THROW (re-throw) */
static void lower_try_finally(XiLower *l, TryCatchNode *tc, AstNode *node) {
    XiBlock *try_blk = xi_block_new(l->func);
    XiBlock *exc_blk = xi_block_new(l->func);
    XiBlock *merge   = xi_block_new(l->func);

    XiValue *try_op = xi_value_new(l->func, l->cur_block, XI_TRY,
                                    l->type_void, 0);
    if (try_op) {
        try_op->aux = (void *)exc_blk;
        try_op->aux_int = -1;  /* no separate finally block */
        try_op->flags |= XI_FLAG_SIDE_EFFECT;
        try_op->line = (uint32_t)node->line;
    }

    xi_block_set_jump(l->cur_block, try_blk);
    xi_lower_braun_seal(l, try_blk);

    /* Normal path: try body → inline finally → merge */
    l->cur_block = try_blk;
    l->try_depth++;
    xi_lower_stmt(l, tc->try_body);
    l->try_depth--;
    if (l->cur_block) {
        xi_lower_stmt(l, tc->finally_body);
        if (l->cur_block)
            xi_block_set_jump(l->cur_block, merge);
    }

    /* Exception path: catch → inline finally → re-throw */
    XiBlock *pretry_blk = try_blk->preds[0];
    XR_DCHECK(pretry_blk != NULL, "try block must have a predecessor");
    xi_block_add_pred(exc_blk, pretry_blk);
    xi_lower_braun_seal(l, exc_blk);
    l->cur_block = exc_blk;

    XiValue *catch_op = xi_value_new(l->func, l->cur_block, XI_CATCH,
                                      l->type_any, 0);
    if (catch_op) {
        catch_op->flags |= XI_FLAG_SIDE_EFFECT;
        catch_op->line = (uint32_t)node->line;
    }

    xi_lower_stmt(l, tc->finally_body);

    if (l->cur_block && catch_op) {
        XiValue *rethrow = xi_value_new(l->func, l->cur_block, XI_THROW,
                                         l->type_void, 1);
        if (rethrow) {
            rethrow->args[0] = catch_op;
            rethrow->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
            rethrow->line = (uint32_t)node->line;
        }
        l->cur_block->kind = XI_BLOCK_UNREACHABLE;
        l->cur_block->control = catch_op;
        l->cur_block = NULL;
    }

    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;

    if (l->cur_block) {
        XiValue *end_op = xi_value_new(l->func, l->cur_block, XI_END_TRY,
                                        l->type_void, 0);
        if (end_op) {
            end_op->flags |= XI_FLAG_SIDE_EFFECT;
            end_op->line = (uint32_t)node->line;
        }
    }
}

/* try-catch or try-catch-finally: the catch block has proper CFG
 * predecessors, so Braun SSA variable resolution works normally.
 * For try-catch-finally, a separate finally block is used (reachable from
 * both try-body and catch-body normal exits). */
static void lower_try_catch_impl(XiLower *l, TryCatchNode *tc,
                                  AstNode *node) {
    XiBlock *try_blk = xi_block_new(l->func);
    XiBlock *catch_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *finally_blk = tc->finally_body ? xi_block_new(l->func) : NULL;
    XiBlock *normal_target = finally_blk ? finally_blk : merge;

    XiValue *try_op = xi_value_new(l->func, l->cur_block, XI_TRY,
                                    l->type_void, 0);
    if (try_op) {
        try_op->aux = (void *)catch_blk;
        try_op->aux_int = finally_blk ? (int64_t)finally_blk->id : -1;
        try_op->flags |= XI_FLAG_SIDE_EFFECT;
        try_op->line = (uint32_t)node->line;
    }

    xi_block_set_jump(l->cur_block, try_blk);
    xi_lower_braun_seal(l, try_blk);

    l->cur_block = try_blk;
    l->try_depth++;
    xi_lower_stmt(l, tc->try_body);
    l->try_depth--;
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, normal_target);

    /* Catch block */
    XiBlock *pretry_blk = try_blk->preds[0];
    XR_DCHECK(pretry_blk != NULL, "try block must have a predecessor");
    xi_block_add_pred(catch_blk, pretry_blk);
    xi_lower_braun_seal(l, catch_blk);
    l->cur_block = catch_blk;

    XiValue *catch_op = xi_value_new(l->func, l->cur_block, XI_CATCH,
                                      l->type_any, 0);
    if (catch_op) {
        catch_op->flags |= XI_FLAG_SIDE_EFFECT;
        catch_op->line = tc->catch_var_line > 0
                         ? (uint32_t)tc->catch_var_line
                         : (uint32_t)node->line;
    }

    if (tc->catch_var && catch_op) {
        int var_id = xi_lower_var_create(l, tc->catch_symbol_id,
                                          tc->catch_var, l->type_any);
        xi_lower_braun_write(l, var_id, l->cur_block, catch_op);
    }

    xi_lower_stmt(l, tc->catch_body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, normal_target);

    /* Finally block (only for try-catch-finally) */
    if (finally_blk) {
        xi_lower_braun_seal(l, finally_blk);
        l->cur_block = finally_blk;

        XiValue *fin_op = xi_value_new(l->func, l->cur_block, XI_FINALLY,
                                        l->type_void, 0);
        if (fin_op) {
            fin_op->flags |= XI_FLAG_SIDE_EFFECT;
            fin_op->line = (uint32_t)node->line;
        }

        xi_lower_stmt(l, tc->finally_body);
    }

    if (finally_blk && l->cur_block)
        xi_block_set_jump(l->cur_block, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;

    if (l->cur_block) {
        XiValue *end_op = xi_value_new(l->func, l->cur_block, XI_END_TRY,
                                        l->type_void, 0);
        if (end_op) {
            end_op->flags |= XI_FLAG_SIDE_EFFECT;
            end_op->line = (uint32_t)node->line;
        }
    }
}

XR_FUNC void xi_lower_try_catch(XiLower *l, AstNode *node) {
    TryCatchNode *tc = &node->as.try_catch;
    bool has_catch = (tc->catch_body != NULL || tc->catch_var != NULL);

    if (!has_catch && tc->finally_body) {
        lower_try_finally(l, tc, node);
    } else {
        lower_try_catch_impl(l, tc, node);
    }
}

/* ========== Defer / Yield (from xi_lower_expr.c) ========== */

static void lower_defer(XiLower *l, AstNode *node) {
    DeferStmtNode *d = &node->as.defer_stmt;
    AstNode *expr = d->expr;
    if (!expr || !l->cur_block) return;

    /* OP_DEFER expects: args[0]=callee, args[1..n]=call arguments.
     * The parser stores either a call expression (defer fn(a, b))
     * or a closure (defer { block }).  Decompose accordingly. */
    if (expr->type == AST_CALL_EXPR) {
        CallExprNode *call = &expr->as.call_expr;
        XiValue *callee = xi_lower_expr(l, call->callee);
        if (!callee || !l->cur_block) return;

        int nargs = call->arg_count;
        XR_DCHECK(nargs <= 250, "lower_defer: too many arguments");

        XiValue *v = xi_value_new(l->func, l->cur_block,
                                  XI_DEFER, l->type_void,
                                  (uint16_t)(1 + nargs));
        if (!v) return;
        v->args[0] = callee;
        for (int i = 0; i < nargs; i++) {
            XiValue *arg = xi_lower_expr(l, call->arguments[i]);
            if (!arg) return;
            v->args[1 + i] = arg;
        }
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
    } else {
        /* defer { block } — parser wraps in anonymous function expr */
        XiValue *callee = xi_lower_expr(l, expr);
        if (!callee || !l->cur_block) return;

        XiValue *v = xi_value_new(l->func, l->cur_block,
                                  XI_DEFER, l->type_void, 1);
        if (!v) return;
        v->args[0] = callee;
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
    }
}


static void lower_yield_stmt(XiLower *l) {
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_YIELD, l->type_void, 0);
    if (v) v->flags |= XI_FLAG_SIDE_EFFECT;
}

/* ========== Destructuring (from xi_lower_expr.c) ========== */

/*
 * Bind destructure pattern elements to extracted values from 'src'.
 * Array patterns: INDEX_GET by position.
 * Object patterns: LOAD_FIELD by field name.
 * Identifier patterns: bind directly.
 */
static void lower_destructure_bind(XiLower *l, XrDestructurePattern *pat,
                                    XiValue *src) {
    if (!pat || !src || !l->cur_block) return;

    switch (pat->type) {
        case PATTERN_ARRAY: {
            int n = pat->as.array.element_count;
            for (int i = 0; i < n; i++) {
                XrDestructurePattern *elem = pat->as.array.elements[i];
                if (!elem) continue;
                XiValue *idx = xi_const_int(l->func, l->cur_block, i, l->type_int);
                XiValue *val = xi_value_new(l->func, l->cur_block,
                                             XI_INDEX_GET, l->type_any, 2);
                if (val) { val->args[0] = src; val->args[1] = idx; }
                lower_destructure_bind(l, elem, val);
            }
            break;
        }
        case PATTERN_OBJECT: {
            int n = pat->as.object.field_count;
            for (int i = 0; i < n; i++) {
                char *fname = pat->as.object.field_names[i];
                XrDestructurePattern *sub = pat->as.object.patterns[i];
                if (!fname) continue;
                /* Use INDEX_GET with string key — works for both JSON objects
                 * and maps (Xi lowers object literals as NEWMAP). */
                XiValue *key = xi_const_str(l->func, l->cur_block,
                                             fname, l->type_string);
                XiValue *val = xi_value_new(l->func, l->cur_block,
                                             XI_INDEX_GET, l->type_any, 2);
                if (val) { val->args[0] = src; val->args[1] = key; }
                lower_destructure_bind(l, sub, val);
            }
            break;
        }
        case PATTERN_IDENTIFIER: {
            const char *name = pat->as.identifier.name;
            if (!name) break;
            uint32_t sid = pat->as.identifier.symbol_id;
            int var_id = xi_lower_var_create(l, sid, name, l->type_any);
            xi_lower_braun_write(l, var_id, l->cur_block, src);
            break;
        }
        default:
            break;
    }
}

/* Destructure declaration: let [a, b] = expr or let {x, y} = expr */
static void lower_destructure_decl(XiLower *l, AstNode *node) {
    DestructureDeclNode *dd = &node->as.destructure_decl;
    XiValue *init = xi_lower_expr(l, dd->initializer);
    if (!init || !dd->pattern) return;
    lower_destructure_bind(l, dd->pattern, init);
}

/* Destructure assignment: [a, b] = [b, a] */
static void lower_destructure_assign(XiLower *l, AstNode *node) {
    DestructureAssignNode *da = &node->as.destructure_assign;
    XiValue *rhs = xi_lower_expr(l, da->value);
    if (!rhs || !da->pattern) return;
    lower_destructure_bind(l, da->pattern, rhs);
}

/* Multi-value declaration: let a, b = foo()
 *
 * When a single call expression provides all values, the VM returns
 * multiple results to consecutive registers (OP_CALL C=nresults).
 * We encode nresults in XI_CALL's aux_int bits 8-15 and use
 * XI_EXTRACT to reference the i-th result register. */
static void lower_multi_var_decl(XiLower *l, AstNode *node) {
    MultiVarDeclNode *mv = &node->as.multi_var_decl;

    /* Single call returning multiple values: let x, y = pair() */
    if (mv->value_count == 1 && mv->name_count > 1 &&
        mv->values[0]->type == AST_CALL_EXPR) {
        XiValue *call_val = xi_lower_expr(l, mv->values[0]);
        if (!call_val) return;

        /* Encode return count in aux_int upper bits (preserving self_call flag) */
        int flags = (int)(call_val->aux_int & 0xFF);
        call_val->aux_int = flags | (mv->name_count << 8);

        /* First name binds to the call result directly */
        struct XrType *vtype = call_val->type ? call_val->type : l->type_any;
        int var0 = xi_lower_var_create(l, 0, mv->names[0], vtype);
        xi_lower_braun_write(l, var0, l->cur_block, call_val);

        /* Remaining names extracted from consecutive result registers */
        for (int i = 1; i < mv->name_count; i++) {
            XiValue *ext = xi_value_new(l->func, l->cur_block,
                                         XI_EXTRACT, l->type_any, 1);
            if (!ext) break;
            ext->args[0] = call_val;
            ext->aux_int = i;
            int var_i = xi_lower_var_create(l, 0, mv->names[i], l->type_any);
            xi_lower_braun_write(l, var_i, l->cur_block, ext);
        }
        return;
    }

    /* General case: evaluate each value expression */
    for (int i = 0; i < mv->name_count && i < mv->value_count; i++) {
        XiValue *val = xi_lower_expr(l, mv->values[i]);
        struct XrType *vtype = val ? val->type : l->type_any;
        int var_id = xi_lower_var_create(l, 0, mv->names[i], vtype);
        if (val) xi_lower_braun_write(l, var_id, l->cur_block, val);
    }
    /* Names without values get null */
    for (int i = mv->value_count; i < mv->name_count; i++) {
        int var_id = xi_lower_var_create(l, 0, mv->names[i], l->type_any);
        XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
        xi_lower_braun_write(l, var_id, l->cur_block, null_val);
    }
}

/* Multi-value assignment: a, b = b, a */
static void lower_multi_assign(XiLower *l, AstNode *node) {
    MultiAssignNode *ma = &node->as.multi_assign;
    /* Evaluate all RHS first to support swaps */
    XiValue *rhs_vals[32];
    int n = ma->value_count > 32 ? 32 : ma->value_count;
    for (int i = 0; i < n; i++)
        rhs_vals[i] = xi_lower_expr(l, ma->values[i]);

    /* Assign to each target */
    for (int i = 0; i < ma->target_count && i < n; i++) {
        AstNode *tgt = ma->targets[i];
        if (tgt && tgt->type == AST_VARIABLE) {
            const char *name = tgt->as.variable.name;
            struct XrType *vtype = rhs_vals[i] ? rhs_vals[i]->type : l->type_any;
            uint32_t sid = tgt->as.variable.symbol_id;
            int var_id = xi_lower_var_create(l, sid, name, vtype);
            if (rhs_vals[i]) xi_lower_braun_write(l, var_id, l->cur_block, rhs_vals[i]);
        }
    }
}


/* ========== Basic Statement Lowering (from xi_lower_expr.c) ========== */

/* ========== Statement Lowering ========== */

static void lower_var_decl(XiLower *l, AstNode *node) {
    const char *name = node->as.var_decl.name;
    uint32_t sid = node->as.var_decl.symbol_id;
    struct XrType *type = xi_lower_node_type(l, node);

    int var_id = xi_lower_var_create(l, sid, name, type);

    XiValue *init_val;
    if (node->as.var_decl.initializer) {
        init_val = xi_lower_expr(l, node->as.var_decl.initializer);
        if (!init_val) return;
    } else {
        init_val = xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* When the initializer comes from a different variable, insert an
     * explicit copy so the new variable gets its own SSA value.  Without
     * this, both variables map to the same physical register and
     * loop-carried updates to the source corrupt the snapshot. */
    if (init_val->var_id != 0xFF && init_val->var_id != (uint8_t)var_id) {
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY,
                                      init_val->type, 1);
        if (copy) {
            copy->args[0] = init_val;
            init_val = copy;
        }
    }
    xi_lower_braun_write(l, var_id, l->cur_block, init_val);

    /* For program-level shared variables, also store into shared array */
    if (l->is_program && l->shared_map[var_id] >= 0) {
        XiValue *store = xi_value_new(l->func, l->cur_block,
                                       XI_SET_SHARED, l->type_void, 1);
        if (store) {
            store->args[0] = init_val;
            store->aux_int = l->shared_map[var_id];
            store->flags |= XI_FLAG_SIDE_EFFECT;
        }
    }
}

static void lower_print(XiLower *l, AstNode *node) {
    PrintNode *p = &node->as.print_stmt;
    int nargs = (int) p->expr_count;

    /* Evaluate all arguments first so they appear before PRINT in the block */
    XiValue *arg_vals[16];
    int n = nargs > 16 ? 16 : nargs;
    for (int i = 0; i < n; i++) {
        arg_vals[i] = xi_lower_expr(l, p->exprs[i]);
    }

    /* Emit one XI_PRINT per argument with correct spacing/newline flags.
     * aux_int encoding: bit0 = add_space (maps to B field),
     *                   bit1+ = C field  (bit0 = newline after print) */
    for (int i = 0; i < n; i++) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PRINT,
                                   l->type_void, 1);
        if (!v) return;
        v->args[0] = arg_vals[i];

        int add_space = (i > 0) ? 1 : 0;
        int newline   = (i == n - 1) ? 1 : 0;
        v->aux_int = add_space | (newline << 1);

        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
    }
}

static void lower_throw(XiLower *l, AstNode *node) {
    ThrowStmtNode *t = &node->as.throw_stmt;
    XiValue *val = xi_lower_expr(l, t->expression);
    if (!val) return;

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_THROW, l->type_void, 1);
    if (!v) return;
    v->args[0] = val;
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    v->line = (uint32_t) node->line;

    if (l->try_depth > 0) {
        /* Inside try-catch: keep the block alive so the enclosing scope
         * can connect it to the merge.  OP_THROW transfers control at
         * runtime, making any subsequent JMP dead code.  But the CFG edge
         * is needed for SSA: variable definitions before the throw must
         * appear in the merge block's phi. */
    } else {
        /* Outside try-catch: no handler to catch this, terminate block. */
        l->cur_block->kind = XI_BLOCK_UNREACHABLE;
        l->cur_block->control = val;
        l->cur_block = NULL;
    }
}

static void lower_return(XiLower *l, AstNode *node) {
    ReturnStmtNode *ret = &node->as.return_stmt;
    XiValue *val = NULL;

    if (ret->value_count == 1 && ret->values[0]) {
        val = xi_lower_expr(l, ret->values[0]);
        /* Tail-call detection: mark calls in return position so the emitter
         * uses OP_TAILCALL / OP_INVOKE_TAIL (constant-space recursion).
         *
         * XI_CALL_METHOD → always safe (OP_INVOKE_TAIL handles all types).
         * XI_CALL with self_call flag → always safe (same closure).
         * XI_CALL with callee typed as function → safe.
         * Other XI_CALL (class constructors, etc.) → NOT safe; OP_TAILCALL
         * only handles closures and would fail on class objects. */
        if (val && val->op == XI_CALL_METHOD) {
            val->flags |= XI_FLAG_TAIL;
        } else if (val && val->op == XI_CALL) {
            bool is_self = (val->aux_int & 0xFF) == 1;
            bool callee_is_func = val->nargs >= 1 && val->args[0] &&
                                  val->args[0]->type &&
                                  val->args[0]->type->kind == XR_KIND_FUNCTION;
            if (is_self || callee_is_func) {
                val->flags |= XI_FLAG_TAIL;
            }
        }
    } else if (ret->value_count > 1) {
        /* Multi-value return: evaluate all expressions first, then package */
        int n = ret->value_count;
        XiValue *vals[16];
        XR_DCHECK(n <= 16, "multi-return exceeds local limit");
        for (int i = 0; i < n && i < 16; i++) {
            vals[i] = xi_lower_expr(l, ret->values[i]);
        }
        XiValue *mret = xi_value_new(l->func, l->cur_block,
                                      XI_MULTI_RET, l->type_any, (uint16_t)n);
        if (mret) {
            for (int i = 0; i < n; i++) {
                mret->args[i] = vals[i];
            }
            val = mret;
        }
    }

    xi_block_set_return(l->cur_block, val);
    l->cur_block = NULL;
}

static void lower_block(XiLower *l, AstNode *node) {
    /* No scope push/pop needed: the analyzer assigns unique symbol_ids
     * to variables in different scopes, so shadowed variables naturally
     * get distinct var_id slots in the Braun SSA. */
    lower_stmts(l, node->as.block.statements, node->as.block.count);
}

static void lower_if(XiLower *l, AstNode *node) {
    IfStmtNode *s = &node->as.if_stmt;

    XiValue *cond = xi_lower_expr(l, s->condition);
    if (!cond || !l->cur_block) return;

    XiBlock *then_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *else_blk = s->else_branch ? xi_block_new(l->func) : merge;

    xi_block_set_if(l->cur_block, cond, then_blk, else_blk);

    /* then_blk has 1 pred (cur_block) — seal immediately */
    xi_lower_braun_seal(l, then_blk);
    if (s->else_branch)
        xi_lower_braun_seal(l, else_blk);

    /* Then branch */
    l->cur_block = then_blk;
    xi_lower_stmt(l, s->then_branch);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, merge);

    /* Else branch */
    if (s->else_branch) {
        l->cur_block = else_blk;
        xi_lower_stmt(l, s->else_branch);
        if (l->cur_block)
            xi_block_set_jump(l->cur_block, merge);
    }

    /* merge preds now fully known — seal and continue */
    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
}

static void lower_while(XiLower *l, AstNode *node) {
    WhileStmtNode *s = &node->as.while_stmt;

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    /* Jump to condition — cond_blk is a loop header (unsealed) */
    xi_block_set_jump(l->cur_block, cond_blk);

    /* Condition: cond_blk NOT sealed yet (back edge pending) */
    l->cur_block = cond_blk;
    XiValue *cond = xi_lower_expr(l, s->condition);
    if (cond)
        xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);

    /* body_blk has 1 pred (cond_blk) — seal immediately */
    xi_lower_braun_seal(l, body_blk);

    /* Body */
    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = cond_blk;

    l->cur_block = body_blk;
    xi_lower_stmt(l, s->body);
    if (l->cur_block)  /* back edge */
        xi_block_set_jump(l->cur_block, cond_blk);

    /* All preds of cond_blk now known (entry + back edge) — seal */
    xi_lower_braun_seal(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

static void lower_for(XiLower *l, AstNode *node) {
    ForStmtNode *s = &node->as.for_stmt;

    /* Initializer in current block */
    if (s->initializer)
        xi_lower_stmt(l, s->initializer);
    if (!l->cur_block) return;

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *incr_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    /* cond_blk is a loop header — do NOT seal yet */
    xi_block_set_jump(l->cur_block, cond_blk);

    /* Condition */
    l->cur_block = cond_blk;
    if (s->condition) {
        XiValue *cond = xi_lower_expr(l, s->condition);
        if (cond)
            xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);
    } else {
        xi_block_set_jump(l->cur_block, body_blk);
    }

    xi_lower_braun_seal(l, body_blk);

    /* Body */
    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = incr_blk;

    l->cur_block = body_blk;
    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, incr_blk);

    xi_lower_braun_seal(l, incr_blk);

    /* Increment */
    l->cur_block = incr_blk;
    if (s->increment) {
        if (incr_blk->npreds > 0)
            xi_lower_expr(l, s->increment);
    }
    if (l->cur_block && incr_blk->npreds > 0)
        xi_block_set_jump(l->cur_block, cond_blk);

    /* cond_blk back edge now added — seal */
    xi_lower_braun_seal(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* lower_for_in_loop, lower_for_in_keyvalue, lower_for_in → xi_lower_stmt.c */

/* (function bodies removed — see xi_lower_stmt.c)
 * Remaining: lower_break, lower_continue kept here as they are tiny. */

static void lower_break(XiLower *l) {
    if (l->break_target && l->cur_block) {
        xi_block_set_jump(l->cur_block, l->break_target);
        l->cur_block = NULL;
    }
}

static void lower_continue(XiLower *l) {
    if (l->continue_target && l->cur_block) {
        xi_block_set_jump(l->cur_block, l->continue_target);
        l->cur_block = NULL;
    }
}


/* Re-export: "export { a, b as c } from './file'" or "export * from './file'".
 * Records XiReexportEntry on XiFunc; emit_reexports() generates bytecodes. */
static void lower_reexport_stmt(XiLower *l, AstNode *node) {
    XR_DCHECK(l != NULL, "lower_reexport_stmt: NULL lowerer");
    XR_DCHECK(node != NULL, "lower_reexport_stmt: NULL node");
    ExportStmtNode *exp = &node->as.export_stmt;
    if (!exp->from_path) return;

    XiFunc *f = l->func;
    if (exp->is_reexport_all) {
        /* export * from "./file" — single entry with name=NULL */
        XiReexportEntry *e = (XiReexportEntry *)xi_func_arena_alloc(
            f, (uint32_t)sizeof(XiReexportEntry));
        if (!e) return;
        uint32_t pl = (uint32_t)strlen(exp->from_path);
        char *pc = (char *)xi_func_arena_alloc(f, pl + 1);
        if (pc) memcpy(pc, exp->from_path, pl + 1);
        e->from_path = pc;
        e->name = NULL;
        e->alias = NULL;

        /* Append to reexports array (grow by doubling) */
        uint16_t idx = f->reexport_count;
        if (idx == 0 || !f->reexports) {
            uint16_t cap = 4;
            f->reexports = (XiReexportEntry *)xi_func_arena_alloc(
                f, (uint32_t)(cap * sizeof(XiReexportEntry)));
            if (!f->reexports) return;
        }
        f->reexports[idx] = *e;
        f->reexport_count = idx + 1;
        return;
    }

    /* Selective re-export: export { a, b as c } from "./file" */
    for (int i = 0; i < exp->reexport_count; i++) {
        ReexportMember *m = &exp->reexport_members[i];
        if (!m->name) continue;

        uint16_t idx = f->reexport_count;
        /* Ensure array capacity (initial alloc or grow) */
        if (idx == 0 || !f->reexports) {
            uint16_t cap = (uint16_t)(exp->reexport_count > 4 ? exp->reexport_count : 4);
            f->reexports = (XiReexportEntry *)xi_func_arena_alloc(
                f, (uint32_t)(cap * sizeof(XiReexportEntry)));
            if (!f->reexports) return;
        }

        XiReexportEntry *e = &f->reexports[idx];
        /* Arena-copy strings */
        uint32_t pl = (uint32_t)strlen(exp->from_path);
        char *pc = (char *)xi_func_arena_alloc(f, pl + 1);
        if (pc) memcpy(pc, exp->from_path, pl + 1);
        e->from_path = pc;

        uint32_t nl = (uint32_t)strlen(m->name);
        char *nc = (char *)xi_func_arena_alloc(f, nl + 1);
        if (nc) memcpy(nc, m->name, nl + 1);
        e->name = nc;

        if (m->alias) {
            uint32_t al = (uint32_t)strlen(m->alias);
            char *ac = (char *)xi_func_arena_alloc(f, al + 1);
            if (ac) memcpy(ac, m->alias, al + 1);
            e->alias = ac;
        } else {
            e->alias = NULL;
        }
        f->reexport_count = idx + 1;
    }
}

/* Selective import: import { square, cube } from "./math_lib"
 * Creates XI_IMPORT_REF values for each member and binds them as local
 * variables.  The AOT driver resolves module_path + member_name to the
 * target module's shared slot after all modules are lowered. */
static void lower_import_stmt(XiLower *l, AstNode *node) {
    XR_DCHECK(l != NULL, "lower_import_stmt: NULL lowerer");
    XR_DCHECK(node != NULL, "lower_import_stmt: NULL node");
    ImportStmtNode *imp = &node->as.import_stmt;

    /* Whole-module import: import math / import math as m.
     * Emit XI_IMPORT_REF with member_name=NULL so xi_emit generates
     * OP_IMPORT without OP_GETPROP, binding the module object itself. */
    if (imp->member_count == 0) {
        const char *local_name = imp->alias ? imp->alias : imp->module_name;
        if (!local_name) return;
        struct XrType *type = xr_type_new_unknown(NULL);
        XiImportRef *ref = (XiImportRef *)xi_func_arena_alloc(
                                l->func, (uint32_t)sizeof(XiImportRef));
        XR_DCHECK(ref != NULL, "lower_import_stmt: arena alloc failed");
        ref->member_name = NULL;
        ref->resolved_mod_index = -1;
        ref->resolved_shared_slot = -1;
        ref->module_path = NULL;
        if (imp->module_name) {
            uint32_t ml = (uint32_t)strlen(imp->module_name);
            char *mc = (char *)xi_func_arena_alloc(l->func, ml + 1);
            if (mc) { memcpy(mc, imp->module_name, ml + 1); ref->module_path = mc; }
        }

        XiValue *v = xi_value_new(l->func, l->cur_block, XI_IMPORT_REF, type, 0);
        if (!v) return;
        v->aux = (void *)ref;
        v->aux_int = -1;
        v->line = (uint32_t)node->line;

        int var_id = xi_lower_var_create(l, imp->symbol_id, local_name, type);
        xi_lower_braun_write(l, var_id, l->cur_block, v);

        /* Store into shared array so nested functions can access via XI_GET_SHARED */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiValue *store = xi_value_new(l->func, l->cur_block,
                                           XI_SET_SHARED, l->type_void, 1);
            if (store) {
                store->args[0] = v;
                store->aux_int = l->shared_map[var_id];
                store->flags |= XI_FLAG_SIDE_EFFECT;
            }
        }
        return;
    }

    for (int i = 0; i < imp->member_count; i++) {
        ImportMember *m = &imp->members[i];
        const char *local_name = m->alias ? m->alias : m->name;

        /* Create XI_IMPORT_REF carrying module path and member name */
        struct XrType *type = xr_type_new_unknown(NULL);
        XiImportRef *ref = (XiImportRef *)xi_func_arena_alloc(
                                l->func, (uint32_t)sizeof(XiImportRef));
        XR_DCHECK(ref != NULL, "lower_import_stmt: arena alloc failed");
        /* Copy strings into arena so they survive AST destruction */
        ref->module_path = NULL;
        ref->member_name = NULL;
        if (imp->module_name) {
            uint32_t ml = (uint32_t)strlen(imp->module_name);
            char *mc = (char *)xi_func_arena_alloc(l->func, ml + 1);
            if (mc) { memcpy(mc, imp->module_name, ml + 1); ref->module_path = mc; }
        }
        if (m->name) {
            uint32_t nl = (uint32_t)strlen(m->name);
            char *nc = (char *)xi_func_arena_alloc(l->func, nl + 1);
            if (nc) { memcpy(nc, m->name, nl + 1); ref->member_name = nc; }
        }
        ref->resolved_mod_index = -1;
        ref->resolved_shared_slot = -1;

        XiValue *v = xi_value_new(l->func, l->cur_block, XI_IMPORT_REF, type, 0);
        if (!v) return;
        v->aux = (void *)ref;
        v->aux_int = -1;
        v->line = (uint32_t)node->line;

        /* Bind as a local variable so subsequent references resolve */
        int var_id = xi_lower_var_create(l, m->symbol_id, local_name, type);
        xi_lower_braun_write(l, var_id, l->cur_block, v);

        /* Store into shared array so nested functions can access via XI_GET_SHARED */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiValue *store = xi_value_new(l->func, l->cur_block,
                                           XI_SET_SHARED, l->type_void, 1);
            if (store) {
                store->args[0] = v;
                store->aux_int = l->shared_map[var_id];
                store->flags |= XI_FLAG_SIDE_EFFECT;
            }
        }
    }
}

/* Main statement dispatcher */
XR_FUNC void xi_lower_stmt(XiLower *l, AstNode *node) {
    if (!node) return;
    if (!l->cur_block) return;  /* dead code */

    switch (node->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            lower_var_decl(l, node);
            break;

        case AST_EXPR_STMT:
            xi_lower_expr(l, node->as.expr_stmt);
            break;

        case AST_PRINT_STMT:
            lower_print(l, node);
            break;

        case AST_RETURN_STMT:
            lower_return(l, node);
            break;

        case AST_BLOCK:
            lower_block(l, node);
            break;

        case AST_IF_STMT:
            lower_if(l, node);
            break;

        case AST_WHILE_STMT:
            lower_while(l, node);
            break;

        case AST_FOR_STMT:
            lower_for(l, node);
            break;

        case AST_FOR_IN_STMT:
            xi_lower_for_in(l, node);
            break;

        case AST_BREAK_STMT:
            lower_break(l);
            break;

        case AST_CONTINUE_STMT:
            lower_continue(l);
            break;

        case AST_THROW_STMT:
            lower_throw(l, node);
            break;

        case AST_TRY_CATCH:
            xi_lower_try_catch(l, node);
            break;

        /* Function declaration as statement */
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            xi_lower_function_decl(l, node);
            break;

        case AST_DEFER_STMT:
            lower_defer(l, node);
            break;

        /* Select statement (channel multiplexing) */
        case AST_SELECT_STMT:
            xi_lower_select(l, node);
            break;

        /* Scope block (structured concurrency) */
        case AST_SCOPE_BLOCK:
            xi_lower_scope_block(l, node);
            break;

        /* Yield execution */
        case AST_YIELD_STMT:
            lower_yield_stmt(l);
            break;

        /* Destructuring */
        case AST_DESTRUCTURE_DECL:
            lower_destructure_decl(l, node);
            break;
        case AST_DESTRUCTURE_ASSIGN:
            lower_destructure_assign(l, node);
            break;

        /* Multi-value declarations and assignments */
        case AST_MULTI_VAR_DECL:
            lower_multi_var_decl(l, node);
            break;
        case AST_MULTI_ASSIGN:
            lower_multi_assign(l, node);
            break;

        /* Module system: import creates XI_IMPORT_REF for selective imports.
         * Export unwraps to lower the inner declaration. */
        case AST_IMPORT_STMT:
            lower_import_stmt(l, node);
            break;
        case AST_EXPORT_STMT:
            if (node->as.export_stmt.declaration) {
                xi_lower_stmt(l, node->as.export_stmt.declaration);
            } else if (node->as.export_stmt.from_path) {
                lower_reexport_stmt(l, node);
            }
            /* export-list form (export a, b) is handled purely via
             * prescan_shared_vars export_flags → emit_module_exports. */
            break;

        case AST_CLASS_DECL:
            xi_lower_class_decl(l, node);
            break;
        case AST_STRUCT_DECL:
            xi_lower_class_decl(l, node);
            break;
        case AST_INTERFACE_DECL:
        case AST_TYPE_ALIAS:
            break;
        case AST_ENUM_DECL:
            xi_lower_enum_decl(l, node);
            break;

        /* Match expression used as statement */
        case AST_MATCH_EXPR:
            xi_lower_expr(l, node);
            break;

        /* Expressions that appear as statements (assignment, call, etc.) */
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
        case AST_CALL_EXPR:
        case AST_INC:
        case AST_DEC:
        case AST_MEMBER_SET:
        case AST_INDEX_SET:
        case AST_GO_EXPR:
        case AST_AWAIT_EXPR:
        case AST_NEW_EXPR:
        case AST_MOVE_EXPR:
            xi_lower_expr(l, node);
            break;

        default:
            /* Every analyzer-accepted AST node must be lowerable.
             * Reaching here indicates a compiler bug, not a user error. */
            XR_DCHECK_FMT(false, "unsupported stmt AST kind %d in lowering",
                          (int)node->type);
            l->had_error = true;
            break;
    }
}

static void prescan_block_decls(XiLower *l, AstNode **stmts, int count) {
    /* Pre-register declarations as Braun SSA variables so hoisted function
     * bodies can resolve forward references.
     *
     * Functions: get a null placeholder value (needed for register allocation
     * and cell-based upvalue capture) marked with SIDE_EFFECT to survive DCE.
     *
     * Variables (let/const): only create the variable slot without writing a
     * null placeholder — the actual let/const initializer assigns the register.
     * This avoids cell-wrapping conflicts where the null occupies the register
     * before the real initialization overwrites it. */
    for (int i = 0; i < count; i++) {
        AstNode *s = stmts[i];
        if (!s) continue;
        const char *name = NULL;
        uint32_t sid = 0;
        struct XrType *type = NULL;
        bool is_func = false;
        switch (s->type) {
            case AST_FUNCTION_DECL:
                name = s->as.function_decl.name;
                sid  = s->as.function_decl.symbol_id;
                type = xi_lower_node_type(l, s);
                is_func = true;
                break;
            case AST_VAR_DECL:
            case AST_CONST_DECL:
                name = s->as.var_decl.name;
                sid  = s->as.var_decl.symbol_id;
                type = xi_lower_node_type(l, s);
                break;
            default:
                break;
        }
        if (!name) continue;
        int var_id = xi_lower_var_find(l, sid, name);
        if (var_id < 0) {
            var_id = xi_lower_var_create(l, sid, name, type);
            if (is_func) {
                XiValue *null_val = xi_const_null(l->func, l->cur_block,
                                                    l->type_null);
                if (null_val)
                    null_val->flags |= XI_FLAG_SIDE_EFFECT;
                xi_lower_braun_write(l, var_id, l->cur_block, null_val);
            }
        }
        if (is_func)
            l->vars[var_id].hoisted = true;
    }
}

static void lower_stmts(XiLower *l, AstNode **stmts, int count) {
    /* Pre-register declarations and hoist function bodies.
     * Function bodies are lowered first so same-scope forward calls
     * (e.g. calling greetBlock before its declaration) resolve to an
     * actual closure rather than the null placeholder. */
    /* At module level, shared variables already handle forward references
     * for program-level functions.  Hoisting only applies inside function
     * bodies where nested functions capture sibling function variables. */
    bool in_loop = (l->break_target != NULL);
    if (l->cur_block && !l->is_program && !in_loop) {
        prescan_block_decls(l, stmts, count);
        for (int i = 0; i < count; i++) {
            if (!l->cur_block) break;
            AstNode *s = stmts[i];
            if (s && s->type == AST_FUNCTION_DECL &&
                s->as.function_decl.name != NULL)
                xi_lower_stmt(l, s);
        }
        /* After hoisting, mark parent variables that are captured by any
         * hoisted child. Hoisting reorders closures before variable
         * initializers, so the initializer has no IR uses (the capture
         * already bound to the braun-read null placeholder). Marking
         * keeps the initializer alive through DCE. */
        for (uint16_t ci = 0; ci < l->func->nchildren; ci++) {
            XiFunc *child = l->func->children[ci];
            if (!child) continue;
            for (uint16_t cj = 0; cj < child->ncaptures; cj++) {
                XiCapture *cap = &child->captures[cj];
                if (cap->source != XI_CAPTURE_SRC_REG) continue;
                /* Resolve capture name back to parent var_id */
                int vid = -1;
                if (cap->value && cap->value->var_id != 0xFF)
                    vid = (int)cap->value->var_id;
                else if (cap->name)
                    vid = xi_lower_var_find(l, 0, cap->name);
                if (vid >= 0 && vid < l->var_count)
                    l->vars[vid].captured_by_child = true;
            }
        }
        for (int i = 0; i < count; i++) {
            if (!l->cur_block) break;  /* dead code after return/break */
            AstNode *s = stmts[i];
            if (s && s->type == AST_FUNCTION_DECL &&
                s->as.function_decl.name != NULL)
                continue;  /* already hoisted */
            xi_lower_stmt(l, s);
        }
    } else {
        for (int i = 0; i < count; i++) {
            if (!l->cur_block) break;
            xi_lower_stmt(l, stmts[i]);
        }
    }
}
