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

/* ========== Select Statement ========== */

XR_FUNC void xi_lower_select(XiLower *l, AstNode *node) {
    SelectStmtNode *sel = &node->as.select_stmt;
    int n = sel->case_count;

    XiBlock *merge = xi_block_new(l->func);
    int max_cases = n > 32 ? 32 : n;

    for (int i = 0; i < max_cases; i++) {
        AstNode *case_node = sel->cases[i];
        SelectCaseNode *sc = &case_node->as.select_case;

        if (sc->is_default) {
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
                                                  XI_CHAN_SEND, l->type_bool, 2);
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
                                                  XI_CHAN_RECV, val_type, 1);
                    if (recv) {
                        recv->args[0] = chan;
                        recv->flags |= XI_FLAG_SIDE_EFFECT;
                    }
                    if (sc->var_name && recv) {
                        int var_id = xi_lower_var_create(l, sc->var_name, val_type);
                        xi_lower_braun_write(l, var_id, l->cur_block, recv);
                    }
                    xi_block_set_jump(l->cur_block, body_blk);
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

XR_FUNC void xi_lower_scope_block(XiLower *l, AstNode *node) {
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
            XiValue *start = xi_lower_expr(l, pattern->as.pattern_range.start);
            XiValue *end = xi_lower_expr(l, pattern->as.pattern_range.end);
            if (!start || !end) return NULL;
            XiValue *ge = xi_binary(l->func, l->cur_block, XI_GE,
                                     l->type_bool, subject, start);
            XiValue *le = xi_binary(l->func, l->cur_block, XI_LE,
                                     l->type_bool, subject, end);
            return xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, ge, le);
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

        XiValue *test = xi_lower_pattern_test(l, subject, arm->pattern);

        if (arm->guard && test) {
            XiValue *guard = xi_lower_expr(l, arm->guard);
            if (guard)
                test = xi_binary(l->func, l->cur_block, XI_BAND,
                                  l->type_bool, test, guard);
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

    int idx_var = xi_lower_var_create(l, idx_name, l->type_int);
    int lim_var = xi_lower_var_create(l, lim_name, l->type_int);
    xi_lower_braun_write(l, idx_var, l->cur_block, init_val);
    xi_lower_braun_write(l, lim_var, l->cur_block, limit);

    int col_var = -1;
    if (get_item_coll) {
        col_var = xi_lower_var_create(l, col_name, l->type_any);
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

    int item_var = xi_lower_var_create(l, s->item_name, item_type);
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
    iter->flags |= XI_FLAG_SIDE_EFFECT;
    iter->line = line;

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__kv_iter_%d", sid);
    char *iter_name = (char *)xi_func_arena_alloc(l->func, (uint32_t)(strlen(buf) + 1));
    XR_DCHECK(iter_name != NULL, "arena alloc failed");
    memcpy(iter_name, buf, strlen(buf) + 1);
    int iter_var = xi_lower_var_create(l, iter_name, l->type_any);
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
    int key_var = xi_lower_var_create(l, s->item_name, item_type);
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
        int val_var = xi_lower_var_create(l, s->value_name, l->type_any);
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

/* ========== For-In Dispatcher ========== */

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

XR_FUNC void xi_lower_try_catch(XiLower *l, AstNode *node) {
    TryCatchNode *tc = &node->as.try_catch;

    XiBlock *try_blk = xi_block_new(l->func);
    XiBlock *catch_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *finally_blk = tc->finally_body ? xi_block_new(l->func) : NULL;
    XiBlock *normal_target = finally_blk ? finally_blk : merge;

    XiValue *try_op = xi_value_new(l->func, l->cur_block, XI_TRY,
                                    l->type_void, 0);
    if (try_op) {
        try_op->aux = (void *)catch_blk;
        try_op->aux_int = finally_blk ? 1 : 0;
        try_op->flags |= XI_FLAG_SIDE_EFFECT;
        try_op->line = (uint32_t)node->line;
    }

    xi_block_set_jump(l->cur_block, try_blk);
    xi_lower_braun_seal(l, try_blk);

    l->cur_block = try_blk;
    xi_lower_stmt(l, tc->try_body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, normal_target);

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
                         ? (uint32_t)tc->catch_var_line : (uint32_t)node->line;
    }

    if (tc->catch_var && catch_op) {
        int var_id = xi_lower_var_create(l, tc->catch_var, l->type_any);
        xi_lower_braun_write(l, var_id, l->cur_block, catch_op);
    }

    xi_lower_stmt(l, tc->catch_body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, normal_target);

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

    if (finally_blk && l->cur_block) {
        xi_block_set_jump(l->cur_block, merge);
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
