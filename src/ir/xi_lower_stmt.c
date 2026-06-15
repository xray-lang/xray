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
#include "xi_effect.h"
#include "xi_lower_expr_helpers.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_names.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/class/xclass_info.h"
#include "../base/xchecks.h"
#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"

#include <string.h>
#include <stdio.h>

/* Forward declaration */
static void lower_stmts(XiLower *l, AstNode **stmts, int count);

static uint16_t stmt_narrow_op_for_type(struct XrType *type) {
    if (!type || type->kind != XR_KIND_INT || type->native_width == 0)
        return 0;
    switch (type->native_width) {
        case XR_NATIVE_I8:
            return XI_NARROW_I8;
        case XR_NATIVE_U8:
            return XI_NARROW_U8;
        case XR_NATIVE_I16:
            return XI_NARROW_I16;
        case XR_NATIVE_U16:
            return XI_NARROW_U16;
        case XR_NATIVE_I32:
            return XI_NARROW_I32;
        case XR_NATIVE_U32:
            return XI_NARROW_U32;
        default:
            return 0;
    }
}

static XiValue *stmt_narrow_for_target_type(XiLower *l, AstNode *node, XiValue *val,
                                            struct XrType *target_type) {
    if (!val || !val->type || !XR_TYPE_IS_INT(val->type))
        return val;
    uint16_t narrow_op = stmt_narrow_op_for_type(target_type);
    if (!narrow_op)
        return val;
    XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, target_type, 1);
    if (!n)
        return val;
    n->args[0] = val;
    n->line = (uint32_t) node->line;
    return n;
}

static XaSymbol *stmt_lookup_class_symbol(XiLower *l, const char *name) {
    if (!l || !l->analyzer || !name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(l->analyzer, name);
    if (sym && sym->kind == XA_SYM_CLASS)
        return sym;
    sym = xa_analyzer_lookup_in_scope(l->analyzer, name, l->analyzer->global_scope);
    if (sym && sym->kind == XA_SYM_CLASS)
        return sym;
    sym = xa_analyzer_lookup_deep(l->analyzer, name);
    return (sym && sym->kind == XA_SYM_CLASS) ? sym : NULL;
}

static XrStructLayout *stmt_lookup_struct_layout(XiLower *l, const char *name) {
    XaSymbol *sym = stmt_lookup_class_symbol(l, name);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return (links && links->class_info) ? links->class_info->struct_layout : NULL;
}

static XrClassInfo *stmt_class_info_for_type(XiLower *l, struct XrType *type) {
    if (!l || !type)
        return NULL;
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_ref) {
        return type->instance.class_ref;
    }
    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return NULL;
    XaSymbol *sym = stmt_lookup_class_symbol(l, type->instance.class_name);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return links ? links->class_info : NULL;
}

static XiValue *stmt_load_class_value(XiLower *l, const char *class_name) {
    if (!l || !class_name)
        return NULL;

    int class_var = xi_lower_var_find(l, 0, class_name);
    if (class_var >= 0) {
        if (l->is_program && l->shared_map[class_var] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[class_var];
            b.name = l->vars[class_var].name;
            b.type = l->vars[class_var].type;
            return xi_lower_emit_top_load(l, b, l->type_any);
        }
        return xi_lower_braun_read(l, class_var, l->cur_block);
    }

    XiTopBinding tb = xi_lower_find_top_binding(l, 0, class_name);
    if (xi_top_binding_valid(tb))
        return xi_lower_emit_top_load(l, tb, l->type_any);

    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, 0, class_name, &upval_type);
    if (upval_idx >= 0) {
        XiValue *cls = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, l->type_any, 0);
        if (cls)
            cls->aux_int = upval_idx;
        return cls;
    }
    return NULL;
}

static XiValue *stmt_default_struct_value_depth(XiLower *l, struct XrType *type, int line,
                                                int depth) {
    if (!l || !type || type->is_nullable)
        return NULL;
    if (depth > 16)
        return NULL;
    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return NULL;

    XrStructLayout *layout = xi_lower_struct_layout_of(type);
    const char *class_name = type->instance.class_name;
    if (!layout && class_name)
        layout = stmt_lookup_struct_layout(l, class_name);
    if (!layout || !class_name)
        return NULL;

    XiValue *cls = stmt_load_class_value(l, class_name);
    if (!cls)
        return NULL;

    XiValue *inst = xi_value_new(l->func, l->cur_block, XI_STRUCT_NEW, type, 1);
    if (!inst)
        return NULL;
    inst->args[0] = cls;
    inst->aux = (void *) layout;
    inst->flags |= XI_FLAG_SIDE_EFFECT;
    inst->line = (uint32_t) line;

    XrClassInfo *info = stmt_class_info_for_type(l, type);
    if (info) {
        int field_count =
            info->field_count < layout->field_count ? info->field_count : layout->field_count;
        for (int i = 0; i < field_count; i++) {
            if (layout->fields[i].native_type != XR_NATIVE_STRUCT)
                continue;
            XaSymbol *field = info->fields[i];
            XaSymbolLinks *links = field ? xa_analyzer_get_links(l->analyzer, field) : NULL;
            struct XrType *field_type = links ? links->type : NULL;
            XiValue *nested = stmt_default_struct_value_depth(l, field_type, line, depth + 1);
            if (!nested)
                continue;
            XiValue *set = xi_value_new(l->func, l->cur_block, XI_STRUCT_SET, l->type_unit, 2);
            if (!set)
                continue;
            set->args[0] = inst;
            set->args[1] = nested;
            set->aux = (void *) layout;
            set->aux_int = i;
            set->flags |= XI_FLAG_SIDE_EFFECT;
            set->line = (uint32_t) line;
        }
    }
    return inst;
}

static XiValue *stmt_default_struct_value(XiLower *l, struct XrType *type, int line) {
    return stmt_default_struct_value_depth(l, type, line, 0);
}

/* ========== Select Statement ========== */

static XiValue *lower_select_time_after(XiLower *l, SelectCaseNode *sc, int line) {
    XiValue *timeout = xi_lower_expr(l, sc->value);
    if (!timeout || !l->cur_block)
        return NULL;

    struct XrType *timer_type = xr_type_new_channel(l->isolate, l->type_int);
    if (!timer_type)
        timer_type = l->type_any;

    XiValue *timer = xi_value_new(l->func, l->cur_block, XI_TIME_AFTER, timer_type, 1);
    if (!timer)
        return NULL;
    timer->args[0] = timeout;
    timer->line = (uint32_t) line;
    return timer;
}

static struct XrType *stmt_channel_element_type(struct XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_CHANNEL)
        return type->container.element_type;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            struct XrType *elem = stmt_channel_element_type(type->union_type.members[i]);
            if (elem)
                return elem;
        }
    }
    return NULL;
}

static bool stmt_type_is_channel(struct XrType *type) {
    return type && (type->kind == XR_KIND_CHANNEL || stmt_channel_element_type(type) != NULL);
}

static XiValue *lower_chan_recv_status(XiLower *l, XiValue *recv) {
    if (!recv)
        return NULL;
    XiValue *status = xi_value_new(l->func, l->cur_block, XI_CHAN_RECV_STATUS, l->type_bool, 1);
    if (!status)
        return NULL;
    status->args[0] = recv;
    status->line = recv->line;
    return status;
}

static void lower_select_recv_ready_branch(XiLower *l, XiValue *recv_status, XiValue *chan,
                                           XiBlock *body_blk, XiBlock *next_blk) {
    XiValue *is_closed = xi_value_new(l->func, l->cur_block, XI_CHAN_IS_CLOSED, l->type_bool, 1);
    if (is_closed)
        is_closed->args[0] = chan;
    if (!recv_status || !is_closed)
        return;

    XiValue *ready = xi_binary(l->func, l->cur_block, XI_BOR, l->type_bool, recv_status, is_closed);
    if (ready)
        xi_block_set_if(l->cur_block, ready, body_blk, next_blk);
}

/* Emit the explicit dispose of the `after` timer channel at the select merge.
 * The timer value dominates merge; this performs the drop the compiler omits
 * across the select.block suspend. See design/885. */
static void lower_select_emit_timer_dispose(XiLower *l, XiValue *timer_chan_val, int line) {
    if (!timer_chan_val || !l->cur_block)
        return;
    XiValue *dispose = xi_value_new(l->func, l->cur_block, XI_CHAN_TIMER_DISPOSE, l->type_unit, 1);
    if (dispose) {
        dispose->args[0] = timer_chan_val;
        dispose->flags |= XI_FLAG_SIDE_EFFECT;
        dispose->line = (uint32_t) line;
    }
}

#define XI_LOWER_SELECT_STACK_CAP 32
#define XI_LOWER_MAX_SELECT_CASES ((int) UINT16_MAX)

typedef struct LowerSelectLists {
    XiValue **case_channels;
    XiValue **case_send_values;
    XiValue **block_channels;
    int block_channel_count;
    int cap;
} LowerSelectLists;

static bool lower_select_validate_case_count(XiLower *l, int case_count, int line) {
    if (case_count < 0 || case_count > XI_LOWER_MAX_SELECT_CASES) {
        fprintf(stderr, "[LOWER] select case count exceeds %d at line %d\n",
                XI_LOWER_MAX_SELECT_CASES, line);
        l->had_error = true;
        return false;
    }
    return true;
}

static bool lower_select_lists_init(XiLower *l, LowerSelectLists *lists, int case_count,
                                    XiValue **stack_case_channels, XiValue **stack_case_send_values,
                                    XiValue **stack_block_channels) {
    int cap = case_count > 0 ? case_count : 1;
    lists->block_channel_count = 0;
    lists->cap = cap;
    if (cap <= XI_LOWER_SELECT_STACK_CAP) {
        lists->case_channels = stack_case_channels;
        lists->case_send_values = stack_case_send_values;
        lists->block_channels = stack_block_channels;
        return true;
    }

    lists->case_channels =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiValue *)));
    lists->case_send_values =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiValue *)));
    lists->block_channels =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiValue *)));
    if (!lists->case_channels || !lists->case_send_values || !lists->block_channels) {
        l->had_error = true;
        return false;
    }
    memset(lists->case_channels, 0, (size_t) cap * sizeof(XiValue *));
    memset(lists->case_send_values, 0, (size_t) cap * sizeof(XiValue *));
    return true;
}

static bool lower_select_block_channel_add(XiLower *l, LowerSelectLists *lists, XiValue *chan,
                                           int line) {
    if (lists->block_channel_count >= lists->cap) {
        fprintf(stderr, "[LOWER] select block channel count exceeds %d at line %d\n", lists->cap,
                line);
        l->had_error = true;
        return false;
    }
    lists->block_channels[lists->block_channel_count++] = chan;
    return true;
}

/* After the case loop: park the select (SELECT_BLOCK over the collected
 * channels, or YIELD for a send-only select with no timeout) and wire the
 * back-edge to try_head, or fall through to merge for a default select. */
static void lower_select_park(XiLower *l, bool has_default, bool has_send, bool has_timeout,
                              XiValue **block_channels, int block_channel_count, XiBlock *try_head,
                              XiBlock *merge, int line) {
    if (!l->cur_block || l->cur_block == merge)
        return;
    if (has_default) {
        xi_block_set_jump(l->cur_block, merge);
        return;
    }
    if ((!has_send || has_timeout) && block_channel_count > 0) {
        if (block_channel_count > XI_LOWER_MAX_SELECT_CASES) {
            fprintf(stderr, "[LOWER] select block channel count exceeds %d at line %d\n",
                    XI_LOWER_MAX_SELECT_CASES, line);
            l->had_error = true;
            return;
        }
        XiValue *block = xi_value_new(l->func, l->cur_block, XI_SELECT_BLOCK, l->type_unit,
                                      (uint16_t) block_channel_count);
        if (block) {
            block->aux_int = block_channel_count;
            block->line = (uint32_t) line;
            for (int i = 0; i < block_channel_count; i++)
                block->args[i] = block_channels[i];
        }
    } else {
        XiValue *yield = xi_value_new(l->func, l->cur_block, XI_YIELD, l->type_unit, 0);
        if (yield) {
            yield->flags |= XI_FLAG_SIDE_EFFECT;
            yield->line = (uint32_t) line;
        }
    }
    xi_block_set_jump(l->cur_block, try_head ? try_head : merge);
}

XR_FUNC void xi_lower_select(XiLower *l, AstNode *node) {
    SelectStmtNode *sel = &node->as.select_stmt;
    int n = sel->case_count;
    if (!lower_select_validate_case_count(l, n, node->line))
        return;

    XiBlock *merge = xi_block_new(l->func);
    bool has_default_case = false;
    bool has_timeout_case = false;
    bool has_send_case = false;
    bool blocking_select = false;
    XiBlock *try_head = NULL;
    XiValue *stack_case_channels[XI_LOWER_SELECT_STACK_CAP] = {0};
    XiValue *stack_case_send_values[XI_LOWER_SELECT_STACK_CAP] = {0};
    XiValue *stack_block_channels[XI_LOWER_SELECT_STACK_CAP];
    LowerSelectLists lists;
    XiValue *timer_chan_val = NULL;  // `after` timer channel; disposed at merge (design/885)
    if (!lower_select_lists_init(l, &lists, n, stack_case_channels, stack_case_send_values,
                                 stack_block_channels))
        return;
    for (int i = 0; i < n; i++) {
        SelectCaseNode *sc = &sel->cases[i]->as.select_case;
        if (sc->is_default)
            has_default_case = true;
        if (sc->is_timeout)
            has_timeout_case = true;
        if (sc->is_send)
            has_send_case = true;
    }

    blocking_select = !has_default_case;
    if (blocking_select) {
        for (int i = 0; i < n; i++) {
            SelectCaseNode *sc = &sel->cases[i]->as.select_case;
            if (sc->is_default)
                continue;
            if (sc->is_timeout) {
                lists.case_channels[i] = lower_select_time_after(l, sc, sel->cases[i]->line);
                timer_chan_val = lists.case_channels[i];
                continue;
            }
            lists.case_channels[i] = xi_lower_expr(l, sc->channel);
            if (sc->is_send)
                lists.case_send_values[i] = xi_lower_expr(l, sc->value);
            if (!l->cur_block)
                return;
        }

        try_head = xi_block_new(l->func);
        if (!try_head)
            return;
        xi_block_set_jump(l->cur_block, try_head);
        l->cur_block = try_head;
    }

    // Create the `after` timer up front (non-blocking selects skip the pre-pass)
    // so every case body can dispose it before any non-local exit. See design/885.
    if (!timer_chan_val) {
        for (int i = 0; i < n; i++) {
            SelectCaseNode *sc = &sel->cases[i]->as.select_case;
            if (sc->is_timeout) {
                lists.case_channels[i] = lower_select_time_after(l, sc, sel->cases[i]->line);
                timer_chan_val = lists.case_channels[i];
                break;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        AstNode *case_node = sel->cases[i];
        SelectCaseNode *sc = &case_node->as.select_case;

        if (sc->is_default) {
            lower_select_emit_timer_dispose(l, timer_chan_val, node->line);
            xi_lower_stmt(l, sc->body);
            if (l->cur_block)
                xi_block_set_jump(l->cur_block, merge);
        } else {
            XiBlock *body_blk = xi_block_new(l->func);
            XiBlock *next_blk = xi_block_new(l->func);

            if (sc->is_send) {
                XiValue *chan =
                    lists.case_channels[i] ? lists.case_channels[i] : xi_lower_expr(l, sc->channel);
                XiValue *val = lists.case_send_values[i] ? lists.case_send_values[i]
                                                         : xi_lower_expr(l, sc->value);
                if (chan && val) {
                    XiValue *send =
                        xi_value_new(l->func, l->cur_block, XI_CHAN_TRY_SEND, l->type_bool, 2);
                    if (send) {
                        send->args[0] = chan;
                        send->args[1] = val;
                        send->flags |= XI_FLAG_SIDE_EFFECT;
                        xi_block_set_if(l->cur_block, send, body_blk, next_blk);
                    }
                }
            } else {
                XiValue *chan = NULL;
                if (sc->is_timeout)
                    chan = lists.case_channels[i] ? lists.case_channels[i]
                                                  : lower_select_time_after(l, sc, case_node->line);
                else
                    chan = lists.case_channels[i] ? lists.case_channels[i]
                                                  : xi_lower_expr(l, sc->channel);
                if (chan) {
                    if (!lower_select_block_channel_add(l, &lists, chan, case_node->line))
                        return;
                    struct XrType *val_type = l->type_any;
                    XiValue *recv =
                        xi_value_new(l->func, l->cur_block, XI_CHAN_TRY_RECV, val_type, 1);
                    if (recv) {
                        recv->args[0] = chan;
                        recv->flags |= XI_FLAG_SIDE_EFFECT;
                    }
                    XiValue *recv_status = lower_chan_recv_status(l, recv);
                    lower_select_recv_ready_branch(l, recv_status, chan, body_blk, next_blk);
                    if (sc->var_name && recv) {
                        int var_id =
                            xi_lower_var_create(l, sc->var_symbol_id, sc->var_name, val_type);
                        xi_lower_braun_write(l, var_id, body_blk, recv);
                    }
                }
            }

            xi_lower_braun_seal(l, body_blk);
            xi_lower_braun_seal(l, next_blk);

            l->cur_block = body_blk;
            lower_select_emit_timer_dispose(l, timer_chan_val, node->line);
            xi_lower_stmt(l, sc->body);
            if (l->cur_block)
                xi_block_set_jump(l->cur_block, merge);

            l->cur_block = next_blk;
        }
    }

    lower_select_park(l, has_default_case, has_send_case, has_timeout_case, lists.block_channels,
                      lists.block_channel_count, try_head, merge, node->line);

    if (try_head)
        xi_lower_braun_seal(l, try_head);
    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
}

/* ========== Scope Block ========== */

XR_FUNC XiValue *xi_lower_scope_block(XiLower *l, AstNode *node) {
    ScopeBlockNode *sb = &node->as.scope_block;

    XiValue *enter = xi_value_new(l->func, l->cur_block, XI_SCOPE_ENTER, l->type_unit, 0);
    if (enter) {
        enter->aux_int = sb->scope_mode;
        enter->flags |= XI_FLAG_SIDE_EFFECT;
        enter->line = (uint32_t) node->line;
    }

    xi_lower_stmt(l, sb->body);

    struct XrType *res_type = (sb->scope_mode == 2) ? l->type_any : l->type_unit;
    XiValue *exit_v = xi_value_new(l->func, l->cur_block, XI_SCOPE_EXIT, res_type, 0);
    if (exit_v) {
        exit_v->aux_int = sb->scope_mode;
        exit_v->flags |= XI_FLAG_SIDE_EFFECT;
        exit_v->line = (uint32_t) node->line;
    }

    /* A linked scope re-raises the first child failure.  Value-channel
     * (enum) failures land in pending_error at XI_SCOPE_EXIT; route them to
     * the enclosing catch (inside try) or propagate them (fallible fn), the
     * same way a fallible call does.  Panic-channel child failures unwind
     * inside OP_SCOPE_EXIT and never reach here. */
    if (sb->scope_mode == 1 /* XR_SCOPE_LINKED */)
        xi_lower_insert_err_check(l, node);

    return exit_v;
}

/* ========== Pattern Test ========== */

/* True iff the pattern can be reached as part of a tuple slot and acts
 * purely as a binding/wildcard — no equality test, just a name capture
 * that always matches. */
static bool pattern_is_irrefutable_binding(AstNode *pattern) {
    if (!pattern)
        return true;
    if (pattern->type == AST_PATTERN_WILDCARD)
        return true;
    if (pattern->type == AST_PATTERN_LITERAL) {
        AstNode *pval = pattern->as.pattern_literal.value;
        if (pval && pval->type == AST_VARIABLE)
            return true;
    }
    return false;
}

/* Resolve the static element type for tuple slot `idx`. Falls back to
 * `type_any` when the analyzer hasn't proven a tuple type for the
 * subject (e.g. the source uses Json or untyped values). */
static struct XrType *tuple_elem_type(XiLower *l, struct XrType *subject_type, int idx) {
    if (subject_type) {
        struct XrType *et = xr_type_tuple_get(subject_type, idx);
        if (et)
            return et;
    }
    return l->type_any;
}

XR_FUNC XiValue *xi_lower_pattern_test(XiLower *l, XiValue *subject, AstNode *pattern) {
    if (!pattern || !subject)
        return NULL;

    switch (pattern->type) {
        case AST_PATTERN_WILDCARD:
            return xi_const_bool(l->func, l->cur_block, true, l->type_bool);

        case AST_PATTERN_LITERAL: {
            /* A bare AST_VARIABLE literal at this depth is a nested
             * binding (e.g. inside `(0, x)`); its match test is
             * unconditional — the actual capture is performed by
             * lower_pattern_bindings before the test runs. */
            AstNode *pval = pattern->as.pattern_literal.value;
            if (pval && pval->type == AST_VARIABLE)
                return xi_const_bool(l->func, l->cur_block, true, l->type_bool);

            XiValue *lit = xi_lower_expr(l, pattern->as.pattern_literal.value);
            if (!lit)
                return NULL;
            return xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, subject, lit);
        }

        case AST_PATTERN_RANGE: {
            /* Half-open interval [start, end), consistent with for-in range. */
            XiValue *start = xi_lower_expr(l, pattern->as.pattern_range.start);
            XiValue *end = xi_lower_expr(l, pattern->as.pattern_range.end);
            if (!start || !end)
                return NULL;
            XiValue *ge = xi_binary(l->func, l->cur_block, XI_GE, l->type_bool, subject, start);
            XiValue *lt = xi_binary(l->func, l->cur_block, XI_LT, l->type_bool, subject, end);
            return xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, ge, lt);
        }

        case AST_PATTERN_MULTI: {
            PatternMultiNode *mp = &pattern->as.pattern_multi;
            XiValue *result = NULL;
            for (int i = 0; i < mp->count; i++) {
                XiValue *test = xi_lower_pattern_test(l, subject, mp->patterns[i]);
                if (!test)
                    continue;
                if (!result)
                    result = test;
                else
                    result = xi_binary(l->func, l->cur_block, XI_BOR, l->type_bool, result, test);
            }
            return result ? result : xi_const_bool(l->func, l->cur_block, false, l->type_bool);
        }

        case AST_PATTERN_TUPLE: {
            /* Per-slot conjunction: TUPLE_GET each refutable slot and
             * AND its sub-test. Irrefutable slots (wildcard / binding)
             * contribute nothing to the test — they are always true and
             * folding them in would just bloat the IR. */
            PatternTupleNode *tp = &pattern->as.pattern_tuple;
            XiValue *result = NULL;
            for (int i = 0; i < tp->count; i++) {
                AstNode *sub = tp->patterns[i];
                if (pattern_is_irrefutable_binding(sub))
                    continue;
                struct XrType *et = tuple_elem_type(l, subject->type, i);
                XiValue *get = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et, 1);
                if (!get)
                    return NULL;
                get->args[0] = subject;
                get->aux_int = i;
                XiValue *test = xi_lower_pattern_test(l, get, sub);
                if (!test)
                    continue;
                if (!result)
                    result = test;
                else
                    result = xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, result, test);
            }
            /* All-irrefutable tuple pattern (e.g. `(_, _)`) matches anything
             * the analyzer let through — emit a constant true. */
            return result ? result : xi_const_bool(l->func, l->cur_block, true, l->type_bool);
        }

        case AST_PATTERN_ADT: {
            /* ADT variant destructure: compare tag field against variant.
             * subject.fields[0] is XrEnumValue* stored at construction.
             * Lower the variant expression (e.g. Shape.Circle) and check
             * equality with the tag. */
            PatternAdtNode *ap = &pattern->as.pattern_adt;
            XiValue *tag = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, l->type_any, 1);
            if (!tag)
                return NULL;
            tag->args[0] = subject;
            tag->aux_int = 0; /* field[0] = variant tag */

            XiValue *variant_val = xi_lower_expr(l, ap->variant);
            if (!variant_val)
                return NULL;
            return xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, tag, variant_val);
        }

        case AST_PATTERN_TYPE: {
            /* `is T [name]`: runtime type test against T. The binding (if
             * present) is captured in lower_pattern_bindings once the
             * test succeeds. */
            PatternTypeNode *tp = &pattern->as.pattern_type;
            return xi_lower_is_test(l, subject, tp->type, pattern->line);
        }

        default:
            return xi_const_bool(l->func, l->cur_block, false, l->type_bool);
    }
}

/* Walk the pattern tree and bind every AST_VARIABLE leaf to the
 * corresponding subject value. Tuple sub-patterns reach their slot via
 * a fresh XI_TUPLE_GET; subsequent loads of the same slot get folded
 * by the const_fold tuple-projection peephole when paired with a
 * TUPLE_NEW source. */
static void lower_pattern_bindings(XiLower *l, XiValue *subject, AstNode *pattern) {
    if (!pattern || !subject)
        return;

    if (pattern->type == AST_PATTERN_LITERAL) {
        AstNode *pval = pattern->as.pattern_literal.value;
        if (pval && pval->type == AST_VARIABLE) {
            const char *bname = pval->as.variable.name;
            uint32_t bsid = pval->as.variable.symbol_id;
            int var_id =
                xi_lower_var_create(l, bsid, bname, subject->type ? subject->type : l->type_any);
            xi_lower_braun_write(l, var_id, l->cur_block, subject);
        }
        return;
    }

    if (pattern->type == AST_PATTERN_TUPLE) {
        PatternTupleNode *tp = &pattern->as.pattern_tuple;
        for (int i = 0; i < tp->count; i++) {
            AstNode *sub = tp->patterns[i];
            if (!sub || sub->type == AST_PATTERN_WILDCARD)
                continue;
            struct XrType *et = tuple_elem_type(l, subject->type, i);
            XiValue *get = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et, 1);
            if (!get)
                continue;
            get->args[0] = subject;
            get->aux_int = i;
            lower_pattern_bindings(l, get, sub);
        }
    }

    /* ADT variant destructure: bind payload fields.
     * Payload slots are at fields[1], fields[2], ... */
    if (pattern->type == AST_PATTERN_ADT) {
        PatternAdtNode *ap = &pattern->as.pattern_adt;
        for (int i = 0; i < ap->count; i++) {
            AstNode *sub = ap->patterns[i];
            if (!sub || sub->type == AST_PATTERN_WILDCARD)
                continue;
            struct XrType *payload_type =
                xa_analyzer_resolve_adt_payload_type(l->analyzer, subject->type, ap->variant, i);
            XiValue *field = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD,
                                          payload_type ? payload_type : l->type_any, 1);
            if (!field)
                continue;
            field->args[0] = subject;
            field->aux_int = 1 + i; /* payload starts at field[1] */
            lower_pattern_bindings(l, field, sub);
        }
    }

    /* Type pattern: bind the narrowed name (if any) to the subject. The
     * subject's static type is the union; the binding sees only the
     * matching arm and is typed as T by the analyzer. */
    if (pattern->type == AST_PATTERN_TYPE) {
        PatternTypeNode *tp = &pattern->as.pattern_type;
        if (tp->binding_name) {
            int var_id = xi_lower_var_create(l, tp->symbol_id, tp->binding_name,
                                             subject->type ? subject->type : l->type_any);
            xi_lower_braun_write(l, var_id, l->cur_block, subject);
        }
    }
}

/* ========== Match Expression ========== */

static void lower_match_no_match_throw(XiLower *l, int line) {
    if (!l || !l->cur_block)
        return;

    struct XrType *exception_type = xr_type_new_class(NULL, "Exception");
    XiValue *cls = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, exception_type, 0);
    if (!cls)
        return;
    cls->aux_int = XR_GLOBAL_VAR_EXCEPTION;
    cls->aux = (void *) "Exception";

    XiValue *msg =
        xi_const_str(l->func, l->cur_block, "E0442: non-exhaustive match", l->type_string);
    XiValue *exc = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, exception_type, 2);
    if (!exc)
        return;
    exc->args[0] = cls;
    exc->args[1] = msg;
    exc->aux = (void *) "constructor";
    exc->aux_int = (int64_t) xi_lower_method_symbol(l, "constructor") << 1;
    exc->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    exc->line = (uint32_t) line;

    XiValue *thr = xi_value_new(l->func, l->cur_block, XI_THROW, l->type_unit, 1);
    if (!thr)
        return;
    thr->args[0] = exc;
    thr->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    thr->line = (uint32_t) line;
    l->cur_block->kind = XI_BLOCK_UNREACHABLE;
    l->cur_block->control = exc;
    l->cur_block = NULL;
}

static bool match_recv_value_pattern(AstNode *pattern, AstNode **payload_out) {
    if (payload_out)
        *payload_out = NULL;
    if (!pattern || pattern->type != AST_PATTERN_ADT)
        return false;
    PatternAdtNode *ap = &pattern->as.pattern_adt;
    if (ap->count != 1 || !ap->variant || ap->variant->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &ap->variant->as.member_access;
    if (!ma->name || strcmp(ma->name, "Value") != 0)
        return false;
    if (!ma->object || ma->object->type != AST_VARIABLE)
        return false;
    if (strcmp(ma->object->as.variable.name, "Recv") != 0)
        return false;
    if (payload_out)
        *payload_out = ap->patterns ? ap->patterns[0] : NULL;
    return true;
}

static bool match_pattern_is_wildcard(AstNode *pattern) {
    return pattern && pattern->type == AST_PATTERN_WILDCARD;
}

static bool match_channel_recv_subject(XiLower *l, AstNode *expr, AstNode **chan_expr_out,
                                       bool *try_recv_out) {
    if (chan_expr_out)
        *chan_expr_out = NULL;
    if (try_recv_out)
        *try_recv_out = false;
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;

    CallExprNode *call = &expr->as.call_expr;
    if (call->arg_count != 0 || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->object || !ma->name)
        return false;

    bool is_recv = strcmp(ma->name, "recv") == 0;
    bool is_try_recv = strcmp(ma->name, "tryRecv") == 0;
    if (!is_recv && !is_try_recv)
        return false;

    struct XrType *recv_type = xa_analyzer_get_node_type(l->analyzer, ma->object);
    if (!stmt_type_is_channel(recv_type))
        return false;

    if (chan_expr_out)
        *chan_expr_out = ma->object;
    if (try_recv_out)
        *try_recv_out = is_try_recv;
    return true;
}

static bool match_channel_recv_fast_supported(MatchExprNode *m) {
    bool saw_value = false;
    for (int i = 0; i < m->arm_count; i++) {
        AstNode *arm_node = m->arms[i];
        if (!arm_node || arm_node->type != AST_MATCH_ARM)
            return false;
        MatchArmNode *arm = &arm_node->as.match_arm;
        AstNode *payload = NULL;
        if (match_recv_value_pattern(arm->pattern, &payload)) {
            if (!pattern_is_irrefutable_binding(payload))
                return false;
            saw_value = true;
            continue;
        }
        if (!match_pattern_is_wildcard(arm->pattern))
            return false;
    }
    return saw_value;
}

#define XI_LOWER_MATCH_EXIT_STACK_CAP 32
#define XI_LOWER_MAX_MATCH_ARMS ((int) UINT16_MAX - 1)

typedef struct LowerMatchExitList {
    XiBlock **blocks;
    XiValue **values;
    int count;
    int cap;
} LowerMatchExitList;

static bool lower_match_validate_arm_count(XiLower *l, int arm_count, int line) {
    if (arm_count < 0 || arm_count > XI_LOWER_MAX_MATCH_ARMS) {
        fprintf(stderr, "[LOWER] match arm count exceeds %d at line %d\n", XI_LOWER_MAX_MATCH_ARMS,
                line);
        l->had_error = true;
        return false;
    }
    return true;
}

static bool lower_match_exit_list_init(XiLower *l, LowerMatchExitList *list, int arm_count,
                                       XiBlock **stack_blocks, XiValue **stack_values) {
    int cap = arm_count > 0 ? arm_count : 1;
    list->count = 0;
    list->cap = cap;
    if (cap <= XI_LOWER_MATCH_EXIT_STACK_CAP) {
        list->blocks = stack_blocks;
        list->values = stack_values;
        return true;
    }

    list->blocks =
        (XiBlock **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiBlock *)));
    list->values =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) cap * sizeof(XiValue *)));
    if (!list->blocks || !list->values) {
        l->had_error = true;
        return false;
    }
    return true;
}

static bool lower_match_exit_list_add(XiLower *l, LowerMatchExitList *list, XiBlock *block,
                                      XiValue *value, int line) {
    if (list->count >= list->cap) {
        fprintf(stderr, "[LOWER] match exit count exceeds %d at line %d\n", list->cap, line);
        l->had_error = true;
        return false;
    }
    list->blocks[list->count] = block;
    list->values[list->count] = value;
    list->count++;
    return true;
}

static XiValue *lower_channel_recv_match_phi(XiLower *l, XiBlock *merge, struct XrType *result_type,
                                             const LowerMatchExitList *exits) {
    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
    if (!l->cur_block)
        return NULL;

    if (merge->npreds == 1)
        return (exits->count > 0) ? exits->values[0] : NULL;

    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (!phi)
        return NULL;
    for (uint16_t p = 0; p < merge->npreds; p++) {
        phi->value.args[p] = xi_const_null(l->func, merge, l->type_null);
        for (int j = 0; j < exits->count; j++) {
            if (merge->preds[p] == exits->blocks[j]) {
                phi->value.args[p] = exits->values[j] ? exits->values[j]
                                                      : xi_const_null(l->func, merge, l->type_null);
                break;
            }
        }
    }
    return &phi->value;
}

static bool lower_channel_recv_match(XiLower *l, AstNode *node, XiValue **out_value) {
    if (out_value)
        *out_value = NULL;
    MatchExprNode *m = &node->as.match_expr;
    AstNode *chan_expr = NULL;
    bool is_try_recv = false;
    if (!match_channel_recv_subject(l, m->expr, &chan_expr, &is_try_recv))
        return false;
    if (!match_channel_recv_fast_supported(m))
        return false;
    if (!lower_match_validate_arm_count(l, m->arm_count, node->line))
        return true;

    XiValue *chan = xi_lower_expr(l, chan_expr);
    if (!chan || !l->cur_block)
        return true;

    struct XrType *payload_type = stmt_channel_element_type(chan->type);
    if (!payload_type)
        payload_type = l->type_any;

    XiValue *recv = xi_value_new(l->func, l->cur_block,
                                 is_try_recv ? XI_CHAN_TRY_RECV : XI_CHAN_RECV, payload_type, 1);
    if (!recv)
        return true;
    recv->args[0] = chan;
    recv->flags |= XI_FLAG_SIDE_EFFECT;
    if (!is_try_recv)
        recv->flags |= XI_FLAG_MAY_SUSPEND;
    recv->line = (uint32_t) node->line;

    XiValue *recv_status = lower_chan_recv_status(l, recv);
    if (!recv_status)
        return true;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *stack_body_exits[XI_LOWER_MATCH_EXIT_STACK_CAP];
    XiValue *stack_body_vals[XI_LOWER_MATCH_EXIT_STACK_CAP];
    LowerMatchExitList exits;
    if (!lower_match_exit_list_init(l, &exits, m->arm_count, stack_body_exits, stack_body_vals))
        return true;

    for (int i = 0; i < m->arm_count; i++) {
        AstNode *arm_node = m->arms[i];
        MatchArmNode *arm = &arm_node->as.match_arm;
        AstNode *payload = NULL;
        bool is_value_arm = match_recv_value_pattern(arm->pattern, &payload);

        XiValue *test = NULL;
        if (is_value_arm) {
            lower_pattern_bindings(l, recv, payload);
            test = recv_status;
            if (arm->guard) {
                XiValue *guard = xi_lower_expr(l, arm->guard);
                if (guard)
                    test = xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, test, guard);
            }
        } else if (arm->guard) {
            test = xi_lower_expr(l, arm->guard);
        }

        if (!test) {
            XiValue *val = NULL;
            if (arm->body && arm->body->type == AST_BLOCK) {
                xi_lower_stmt(l, arm->body);
                if (l->cur_block)
                    val = xi_const_null(l->func, l->cur_block, l->type_null);
            } else {
                val = xi_lower_expr(l, arm->body);
            }
            if (l->cur_block) {
                if (!lower_match_exit_list_add(l, &exits, l->cur_block, val, node->line))
                    return true;
                xi_block_set_jump(l->cur_block, merge);
            }
            l->cur_block = NULL;
            break;
        }

        XiBlock *body_blk = xi_block_new(l->func);
        XiBlock *next_blk = xi_block_new(l->func);
        xi_block_set_if(l->cur_block, test, body_blk, next_blk);
        xi_lower_braun_seal(l, body_blk);
        xi_lower_braun_seal(l, next_blk);

        l->cur_block = body_blk;
        XiValue *val = NULL;
        if (arm->body && arm->body->type == AST_BLOCK) {
            xi_lower_stmt(l, arm->body);
            if (l->cur_block)
                val = xi_const_null(l->func, l->cur_block, l->type_null);
        } else {
            val = xi_lower_expr(l, arm->body);
        }
        if (l->cur_block) {
            if (!lower_match_exit_list_add(l, &exits, l->cur_block, val, node->line))
                return true;
            xi_block_set_jump(l->cur_block, merge);
        }

        l->cur_block = next_blk;
    }

    if (l->cur_block && l->cur_block != merge)
        lower_match_no_match_throw(l, node->line);

    if (out_value)
        *out_value = lower_channel_recv_match_phi(l, merge, result_type, &exits);
    return true;
}

XR_FUNC XiValue *xi_lower_match(XiLower *l, AstNode *node) {
    MatchExprNode *m = &node->as.match_expr;
    XiValue *fast_value = NULL;
    if (lower_channel_recv_match(l, node, &fast_value))
        return fast_value;

    XiValue *subject = xi_lower_expr(l, m->expr);
    if (!subject)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiBlock *merge = xi_block_new(l->func);
    int arm_count = m->arm_count;
    if (!lower_match_validate_arm_count(l, arm_count, node->line))
        return NULL;
    XiBlock *stack_body_exits[XI_LOWER_MATCH_EXIT_STACK_CAP];
    XiValue *stack_body_vals[XI_LOWER_MATCH_EXIT_STACK_CAP];
    LowerMatchExitList exits;
    if (!lower_match_exit_list_init(l, &exits, arm_count, stack_body_exits, stack_body_vals))
        return NULL;

    for (int i = 0; i < arm_count; i++) {
        AstNode *arm_node = m->arms[i];
        MatchArmNode *arm = &arm_node->as.match_arm;

        /* Bind every named slot in the pattern (top-level bare name or
         * AST_VARIABLEs nested inside a tuple pattern) before lowering
         * the test or guard, so both can reference the captures.
         *
         * is_top_binding is the legacy "bare-name pattern" case where
         * the match test reduces to TRUE and selection is decided
         * entirely by the optional guard. Tuple patterns don't get
         * that shortcut: their refutable slots still need TUPLE_GET-
         * based equality testing. */
        lower_pattern_bindings(l, subject, arm->pattern);

        bool is_top_irrefutable = !arm->guard && pattern_is_irrefutable_binding(arm->pattern);
        bool is_top_binding = false;
        if (arm->pattern && arm->pattern->type == AST_PATTERN_LITERAL) {
            AstNode *pval = arm->pattern->as.pattern_literal.value;
            if (pval && pval->type == AST_VARIABLE)
                is_top_binding = true;
        }

        XiValue *test;
        if (is_top_irrefutable) {
            /* Top-level wildcard / bare binding without a guard always matches. */
            test = NULL;
        } else if (is_top_binding) {
            /* Guarded bare-name pattern narrows with the guard expression. */
            test = arm->guard ? xi_lower_expr(l, arm->guard) : NULL;
        } else {
            test = xi_lower_pattern_test(l, subject, arm->pattern);
            if (arm->guard && test) {
                XiValue *guard = xi_lower_expr(l, arm->guard);
                if (guard)
                    test = xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, test, guard);
            }
        }

        if (!test) {
            XiValue *val = NULL;
            if (arm->body && arm->body->type == AST_BLOCK) {
                xi_lower_stmt(l, arm->body);
                if (l->cur_block)
                    val = xi_const_null(l->func, l->cur_block, l->type_null);
            } else {
                val = xi_lower_expr(l, arm->body);
            }
            if (l->cur_block) {
                if (!lower_match_exit_list_add(l, &exits, l->cur_block, val, node->line))
                    return NULL;
                xi_block_set_jump(l->cur_block, merge);
            }
            l->cur_block = NULL;
            break;
        } else {
            XiBlock *body_blk = xi_block_new(l->func);
            XiBlock *next_blk = xi_block_new(l->func);
            xi_block_set_if(l->cur_block, test, body_blk, next_blk);
            xi_lower_braun_seal(l, body_blk);
            xi_lower_braun_seal(l, next_blk);

            l->cur_block = body_blk;
            XiValue *val = NULL;
            if (arm->body && arm->body->type == AST_BLOCK) {
                xi_lower_stmt(l, arm->body);
                if (l->cur_block)
                    val = xi_const_null(l->func, l->cur_block, l->type_null);
            } else {
                val = xi_lower_expr(l, arm->body);
            }
            if (l->cur_block) {
                if (!lower_match_exit_list_add(l, &exits, l->cur_block, val, node->line))
                    return NULL;
                xi_block_set_jump(l->cur_block, merge);
            }

            l->cur_block = next_blk;
        }
    }

    if (l->cur_block && l->cur_block != merge) {
        lower_match_no_match_throw(l, node->line);
    }

    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
    if (!l->cur_block)
        return NULL;

    if (merge->npreds == 1) {
        return (exits.count > 0) ? exits.values[0] : NULL;
    }

    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (!phi)
        return NULL;
    for (uint16_t p = 0; p < merge->npreds; p++) {
        phi->value.args[p] = xi_const_null(l->func, merge, l->type_null);
        for (int j = 0; j < exits.count; j++) {
            if (merge->preds[p] == exits.blocks[j]) {
                phi->value.args[p] =
                    exits.values[j] ? exits.values[j] : xi_const_null(l->func, merge, l->type_null);
                break;
            }
        }
    }
    return &phi->value;
}

/* ========== For-In Loop (index-based) ========== */

static void lower_for_in_loop(XiLower *l, AstNode *node, XiValue *init_val, XiValue *limit,
                              XiValue *get_item_coll) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    (void) s;
    struct XrType *item_type = xi_lower_node_type(l, node);

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__for_idx_%d", sid);
    char *idx_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(idx_name != NULL, "arena alloc failed for idx_name");
    memcpy(idx_name, buf, strlen(buf) + 1);

    snprintf(buf, sizeof(buf), "__for_lim_%d", sid);
    char *lim_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(lim_name != NULL, "arena alloc failed for lim_name");
    memcpy(lim_name, buf, strlen(buf) + 1);

    snprintf(buf, sizeof(buf), "__for_col_%d", sid);
    char *col_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
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
    XiValue *cond = xi_binary(l->func, l->cur_block, XI_LT, l->type_bool, cur_idx, cur_lim);
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
        item = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, item_type, 2);
        if (item) {
            item->args[0] = body_col;
            item->args[1] = body_idx;
            item->line = (uint32_t) node->line;
        }
    } else {
        item = body_idx;
    }

    int item_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    if (item)
        xi_lower_braun_write(l, item_var, l->cur_block, item);

    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, incr_blk);

    xi_lower_braun_seal(l, incr_blk);

    l->cur_block = incr_blk;
    if (incr_blk->npreds > 0) {
        XiValue *inc_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
        XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
        XiValue *new_idx = xi_binary(l->func, l->cur_block, XI_ADD, l->type_int, inc_idx, one);
        if (new_idx)
            xi_lower_braun_write(l, idx_var, l->cur_block, new_idx);
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
    uint32_t line = (uint32_t) node->line;

    XiValue *coll = xi_lower_expr(l, s->collection);
    if (!coll || !l->cur_block)
        return;

    XiValue *iter = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_any, 1);
    if (!iter)
        return;
    iter->args[0] = coll;
    iter->aux = (void *) "entriesIterator";
    iter->aux_int = (int64_t) xi_lower_method_symbol(l, "entriesIterator") << 1;
    iter->flags |= XI_FLAG_SIDE_EFFECT;
    iter->line = line;

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__kv_iter_%d", sid);
    char *iter_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
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
    XiValue *has_next = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_bool, 1);
    if (!has_next)
        return;
    has_next->args[0] = iter_cond;
    has_next->aux = (void *) "hasNext";
    has_next->aux_int = (int64_t) xi_lower_method_symbol(l, "hasNext") << 1;
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
    XiValue *entry = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_any, 1);
    if (!entry)
        return;
    entry->args[0] = iter_body;
    entry->aux = (void *) "next";
    entry->aux_int = (int64_t) xi_lower_method_symbol(l, "next") << 1;
    entry->flags |= XI_FLAG_SIDE_EFFECT;
    entry->line = line;

    struct XrType *item_type = xi_lower_node_type(l, node);

    /* The iterator yields a (key, value) tuple per step (see
     * xr_iterator_next: Map/Json/Array/String all build XrTuple pairs).
     * Read each slot with TUPLE_GET so the access matches the runtime
     * representation; downstream peephole can fold this against a
     * fresh TUPLE_NEW when the source is inlinable. */
    XiValue *key_val = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, item_type, 1);
    if (key_val) {
        key_val->args[0] = entry;
        key_val->aux_int = 0;
        key_val->line = line;
    }
    int key_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    if (key_val)
        xi_lower_braun_write(l, key_var, l->cur_block, key_val);

    if (s->value_name) {
        XiValue *val_val = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, l->type_any, 1);
        if (val_val) {
            val_val->args[0] = entry;
            val_val->aux_int = 1;
            val_val->line = line;
        }
        int val_var = xi_lower_var_create(l, s->value_symbol_id, s->value_name, l->type_any);
        if (val_val)
            xi_lower_braun_write(l, val_var, l->cur_block, val_val);
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
static void lower_for_in_custom_iterator(XiLower *l, AstNode *node, XiValue *coll) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    uint32_t line = (uint32_t) node->line;

    /* Call iterator() on the collection */
    XiValue *iter = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_any, 1);
    if (!iter)
        return;
    iter->args[0] = coll;
    iter->aux = (void *) "iterator";
    iter->aux_int = (int64_t) xi_lower_method_symbol(l, "iterator") << 1;
    iter->flags |= XI_FLAG_SIDE_EFFECT;
    iter->line = line;

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__ci_iter_%d", sid);
    char *iter_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
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
    XiValue *has_next = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_bool, 1);
    if (!has_next)
        return;
    has_next->args[0] = iter_cond;
    has_next->aux = (void *) "hasNext";
    has_next->aux_int = (int64_t) xi_lower_method_symbol(l, "hasNext") << 1;
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
    XiValue *next_val = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_any, 1);
    if (!next_val)
        return;
    next_val->args[0] = iter_body;
    next_val->aux = (void *) "next";
    next_val->aux_int = (int64_t) xi_lower_method_symbol(l, "next") << 1;
    next_val->flags |= XI_FLAG_SIDE_EFFECT;
    next_val->line = line;

    struct XrType *item_type = xi_lower_node_type(l, node);
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

/* ========== For-In Enum Loop (memberCount + getMember) ========== */

static void lower_for_in_enum_loop(XiLower *l, AstNode *node, XiValue *init_val, XiValue *limit,
                                   XiValue *enum_cls) {
    ForInStmtNode *s = &node->as.for_in_stmt;
    struct XrType *item_type = xi_lower_node_type(l, node);

    int sid = l->synthetic_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "__ei_%d", sid);
    char *idx_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(idx_name != NULL, "arena alloc failed");
    memcpy(idx_name, buf, strlen(buf) + 1);

    snprintf(buf, sizeof(buf), "__el_%d", sid);
    char *lim_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(lim_name != NULL, "arena alloc failed");
    memcpy(lim_name, buf, strlen(buf) + 1);

    snprintf(buf, sizeof(buf), "__ec_%d", sid);
    char *cls_name = (char *) xi_func_arena_alloc(l->func, (uint32_t) (strlen(buf) + 1));
    XR_DCHECK(cls_name != NULL, "arena alloc failed");
    memcpy(cls_name, buf, strlen(buf) + 1);

    int idx_var = xi_lower_var_create(l, 0, idx_name, l->type_int);
    int lim_var = xi_lower_var_create(l, 0, lim_name, l->type_int);
    int cls_var = xi_lower_var_create(l, 0, cls_name, l->type_any);
    xi_lower_braun_write(l, idx_var, l->cur_block, init_val);
    xi_lower_braun_write(l, lim_var, l->cur_block, limit);
    xi_lower_braun_write(l, cls_var, l->cur_block, enum_cls);

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *incr_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    xi_block_set_jump(l->cur_block, cond_blk);

    l->cur_block = cond_blk;
    XiValue *cur_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
    XiValue *cur_lim = xi_lower_braun_read(l, lim_var, l->cur_block);
    XR_DCHECK(cur_idx != NULL, "braun_read idx must not be NULL");
    XiValue *cond = xi_binary(l->func, l->cur_block, XI_LT, l->type_bool, cur_idx, cur_lim);
    if (cond)
        xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);

    xi_lower_braun_seal(l, body_blk);

    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = incr_blk;

    l->cur_block = body_blk;
    XiValue *body_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
    XiValue *body_cls = xi_lower_braun_read(l, cls_var, l->cur_block);

    /* Call enum_cls.getMember(idx) to get the enum member */
    XiValue *item = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, item_type, 2);
    if (item) {
        item->args[0] = body_cls;
        item->args[1] = body_idx;
        item->aux = (void *) "getMember";
        item->aux_int = (int64_t) xi_lower_method_symbol(l, "getMember") << 1;
        item->flags |= XI_FLAG_SIDE_EFFECT;
        item->line = (uint32_t) node->line;
    }

    int item_var = xi_lower_var_create(l, s->item_symbol_id, s->item_name, item_type);
    if (item)
        xi_lower_braun_write(l, item_var, l->cur_block, item);

    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, incr_blk);

    xi_lower_braun_seal(l, incr_blk);

    l->cur_block = incr_blk;
    if (incr_blk->npreds > 0) {
        XiValue *inc_idx = xi_lower_braun_read(l, idx_var, l->cur_block);
        XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
        XiValue *new_idx = xi_binary(l->func, l->cur_block, XI_ADD, l->type_int, inc_idx, one);
        if (new_idx)
            xi_lower_braun_write(l, idx_var, l->cur_block, new_idx);
    }
    if (l->cur_block && incr_blk->npreds > 0)
        xi_block_set_jump(l->cur_block, cond_blk);

    xi_lower_braun_seal(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* ========== For-In Dispatcher ========== */

/* Whether the collection's static type is iterable via the fast
 * length + INDEX_GET path. Only Array, Set and string qualify: those
 * have integer indexable layouts that produce the loop variable's
 * canonical type directly. Map / Json instead route through the
 * iterator() / hasNext() / next() protocol, which lets `for (k in m)`
 * yield real keys and `for (k in obj)` yield string keys, matching
 * the analyzer's item-type inference and Python / Go conventions. */
static bool is_index_iterable_collection(XiLower *l, AstNode *coll_node) {
    struct XrType *t = xi_lower_node_type(l, coll_node);
    if (!t || t->kind == XR_KIND_UNKNOWN)
        return true; /* unknown: assume builtin for backward compat */
    return t->kind == XR_KIND_ARRAY || t->kind == XR_KIND_SET || t->kind == XR_KIND_STRING;
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
        if (!start || !l->cur_block)
            return;
        XiValue *end = xi_lower_expr(l, rn->end);
        if (!end || !l->cur_block)
            return;
        lower_for_in_loop(l, node, start, end, NULL);
        return;
    }

    XiValue *coll = xi_lower_expr(l, s->collection);
    if (!coll || !l->cur_block)
        return;

    /* Enum types: iterate via memberCount + getMember(i) */
    struct XrType *coll_type = xi_lower_node_type(l, s->collection);
    if (coll_type && coll_type->kind == XR_KIND_ENUM) {
        XiValue *len = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, l->type_int, 1);
        if (!len)
            return;
        len->args[0] = coll;
        len->aux = (void *) "memberCount";
        len->aux_int = xi_lower_method_symbol(l, "memberCount");
        len->line = (uint32_t) node->line;

        XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
        /* Reuse index loop but with getMember call for item retrieval */
        lower_for_in_enum_loop(l, node, zero, len, coll);
        return;
    }

    /* Anything that isn't a fast index-iterable collection (Map, Json,
     * tuple, struct, custom class) goes through the iterator() protocol.
     * The analyzer is responsible for rejecting collection types that
     * have no iterator() method (tuple / struct without one). */
    if (!is_index_iterable_collection(l, s->collection)) {
        lower_for_in_custom_iterator(l, node, coll);
        return;
    }

    XiValue *len = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, l->type_int, 1);
    if (!len)
        return;
    len->args[0] = coll;
    len->aux = (void *) "length";
    len->aux_int = xi_lower_method_symbol(l, "length");
    len->line = (uint32_t) node->line;

    XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    lower_for_in_loop(l, node, zero, len, coll);
}

/* ========== Try-Catch ========== */

/* Re-propagate an already-materialized error value (e.g. read by
 * XI_ERR_CATCH) through the value channel, respecting the enclosing
 * try scope:
 *   - inside an outer error-catch (try_depth > 0): XI_ERR_SET + jump to
 *     that catch block, so the error stays local to this function.
 *   - otherwise: XI_ERR_RETURN, returning the error from the function.
 * Consumes l->cur_block (sets it to the jump/return target state). */
XR_FUNC void xi_lower_reprop_error(XiLower *l, XiValue *val, AstNode *node) {
    if (!l->cur_block || !val)
        return;
    if (l->try_depth > 0) {
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_ERR_SET, l->type_unit, 1);
        if (set) {
            set->args[0] = val;
            set->flags |= XI_FLAG_SIDE_EFFECT;
            set->line = (uint32_t) node->line;
        }
        XiBlock *catch_blk = l->catch_targets[l->try_depth - 1];
        xi_block_set_jump(l->cur_block, catch_blk);
        l->cur_block = NULL;
    } else {
        XiValue *reprop = xi_value_new(l->func, l->cur_block, XI_ERR_RETURN, l->type_unit, 1);
        if (reprop) {
            reprop->args[0] = val;
            reprop->flags |= XI_FLAG_SIDE_EFFECT;
            reprop->line = (uint32_t) node->line;
        }
        l->cur_block->kind = XI_BLOCK_RETURN;
        l->cur_block->control = reprop;
        l->cur_block = NULL;
    }
}

/* Upper bound on catch clauses we partition on the stack.  Multi-catch
 * with more than this is pathological; clauses beyond it are ignored. */
#define XR_TRY_MAX_CATCH 32

/* Lower the error catch block: XI_ERR_CATCH binds the pending error,
 * then the error catch clauses run (single or is-T chain).  All control
 * flow is the value-return error channel — no handler stack. */
static void lower_error_catch_clauses(XiLower *l, XrCatchClause **errc, int errn, AstNode *node,
                                      XiBlock *normal_target) {
    XiValue *catch_op = xi_value_new(l->func, l->cur_block, XI_ERR_CATCH, l->type_any, 0);
    if (catch_op) {
        catch_op->flags |= XI_FLAG_SIDE_EFFECT;
        catch_op->line = (errn > 0 && errc[0]->var_line > 0) ? (uint32_t) errc[0]->var_line
                                                             : (uint32_t) node->line;
    }

    if (errn == 1) {
        XrCatchClause *cc = errc[0];
        if (cc->var_name && catch_op) {
            int var_id = xi_lower_var_create(l, cc->symbol_id, cc->var_name, l->type_any);
            xi_lower_braun_write(l, var_id, l->cur_block, catch_op);
        }
        xi_lower_stmt(l, cc->body);
        return;
    }

    /* Multi-catch: if-else chain with is-T checks. */
    for (int ci = 0; ci < errn; ci++) {
        XrCatchClause *cc = errc[ci];
        bool is_last = (ci == errn - 1);
        bool has_type = (cc->type != NULL);

        if (has_type && !is_last) {
            XiValue *is_val = xi_lower_is_test(l, catch_op, cc->type, cc->var_line);
            XiBlock *match_blk = xi_block_new(l->func);
            XiBlock *next_blk = xi_block_new(l->func);

            xi_block_set_if(l->cur_block, is_val, match_blk, next_blk);

            xi_lower_braun_seal(l, match_blk);
            l->cur_block = match_blk;
            if (cc->var_name && catch_op) {
                int var_id = xi_lower_var_create(l, cc->symbol_id, cc->var_name, l->type_any);
                xi_lower_braun_write(l, var_id, l->cur_block, catch_op);
            }
            xi_lower_stmt(l, cc->body);
            if (l->cur_block)
                xi_block_set_jump(l->cur_block, normal_target);

            xi_lower_braun_seal(l, next_blk);
            l->cur_block = next_blk;
        } else if (has_type) {
            XiValue *is_val = xi_lower_is_test(l, catch_op, cc->type, cc->var_line);
            XiBlock *match_blk = xi_block_new(l->func);
            XiBlock *reprop_blk = xi_block_new(l->func);

            xi_block_set_if(l->cur_block, is_val, match_blk, reprop_blk);

            xi_lower_braun_seal(l, match_blk);
            l->cur_block = match_blk;
            if (cc->var_name && catch_op) {
                int var_id = xi_lower_var_create(l, cc->symbol_id, cc->var_name, l->type_any);
                xi_lower_braun_write(l, var_id, l->cur_block, catch_op);
            }
            xi_lower_stmt(l, cc->body);
            if (l->cur_block)
                xi_block_set_jump(l->cur_block, normal_target);

            /* Unmatched: re-propagate to enclosing scope. */
            xi_lower_braun_seal(l, reprop_blk);
            l->cur_block = reprop_blk;
            xi_lower_reprop_error(l, catch_op, node);
        } else {
            if (cc->var_name && catch_op) {
                int var_id = xi_lower_var_create(l, cc->symbol_id, cc->var_name, l->type_any);
                xi_lower_braun_write(l, var_id, l->cur_block, catch_op);
            }
            xi_lower_stmt(l, cc->body);
        }
    }
}

/* try-catch (with optional finally).  error and panic are two strictly
 * separate channels:
 *
 *   - `catch (e)` / `catch (e: T)`  → ERROR channel (user `throw <enum>`).
 *     Pure value-return: pending_error + CFG branches.  No handler stack.
 *
 *   - `catch panic (p)`             → PANIC channel (div-zero, OOB, expr!,
 *     assert, …).  Uses XI_TRY/OP_TRY handler stack + unwind.  Only this
 *     clause observes runtime faults.
 *
 * XI_TRY is emitted iff a `catch panic` clause is present. */
static void lower_try_catch_impl(XiLower *l, TryCatchNode *tc, AstNode *node) {
    /* Partition catch clauses: error clauses vs. the (optional) panic clause. */
    XrCatchClause *errc[XR_TRY_MAX_CATCH];
    int errn = 0;
    XrCatchClause *panic_clause = NULL;
    for (int i = 0; i < tc->catch_count; i++) {
        XrCatchClause *cc = tc->catch_clauses[i];
        if (!cc)
            continue;
        if (cc->is_panic)
            panic_clause = cc;
        else if (errn < XR_TRY_MAX_CATCH)
            errc[errn++] = cc;
    }
    bool has_err = errn > 0;
    bool has_panic = panic_clause != NULL;

    XiBlock *try_blk = xi_block_new(l->func);
    XiBlock *catch_blk = has_err ? xi_block_new(l->func) : NULL;
    XiBlock *panic_blk = has_panic ? xi_block_new(l->func) : NULL;
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *normal_target = merge;

    /* Panic handler: register OP_TRY pointing at panic_blk.  This is the
     * VM's mechanism for synchronous runtime faults (the only thing that
     * uses the handler stack now). */
    if (has_panic) {
        XiValue *try_op = xi_value_new(l->func, l->cur_block, XI_TRY, l->type_unit, 0);
        if (try_op) {
            try_op->aux = (void *) panic_blk;
            try_op->aux_int = -1;
            try_op->flags |= XI_FLAG_SIDE_EFFECT;
            try_op->line = (uint32_t) node->line;
        }
    }

    xi_block_set_jump(l->cur_block, try_blk);
    xi_lower_braun_seal(l, try_blk);

    /* Error catch scope: a `throw <enum>` or fallible call inside the body
     * branches to catch_blk via the value channel (see lower_throw /
     * xi_lower_insert_err_check, which consult try_depth/catch_targets). */
    if (has_err) {
        l->catch_targets[l->try_depth] = catch_blk;
        l->try_depth++;
    }
    l->cur_block = try_blk;
    l->dead_after_throw = false;
    xi_lower_stmt(l, tc->try_body);
    if (has_err)
        l->try_depth--;

    XiBlock *try_exit_blk = l->cur_block;

    /* Normal path: pop panic handler (if any) and go to merge. */
    if (l->cur_block) {
        if (has_panic) {
            XiValue *end_op = xi_value_new(l->func, l->cur_block, XI_END_TRY, l->type_unit, 0);
            if (end_op) {
                end_op->flags |= XI_FLAG_SIDE_EFFECT;
                end_op->line = (uint32_t) node->line;
            }
        }
        xi_block_set_jump(l->cur_block, normal_target);
    }

    /* ---- Error catch block (value channel) ---- */
    if (has_err) {
        /* The body reaches catch_blk via CFG edges (ERR_SET+jump or
         * ERR_HAS+IF).  If none exist, add try exit as predecessor so
         * Braun SSA still sees the try body's definitions. */
        XiBlock *catch_pred = try_exit_blk ? try_exit_blk : try_blk;
        if (catch_blk->npreds == 0)
            xi_block_add_pred(catch_blk, catch_pred);
        xi_lower_braun_seal(l, catch_blk);
        l->cur_block = catch_blk;
        l->dead_after_throw = false;

        /* Leaving the try scope on the error path: pop the panic handler. */
        if (has_panic) {
            XiValue *end_op = xi_value_new(l->func, l->cur_block, XI_END_TRY, l->type_unit, 0);
            if (end_op) {
                end_op->flags |= XI_FLAG_SIDE_EFFECT;
                end_op->line = (uint32_t) node->line;
            }
        }

        lower_error_catch_clauses(l, errc, errn, node, normal_target);

        if (l->cur_block)
            xi_block_set_jump(l->cur_block, merge);
    }

    /* ---- Panic catch block (unwind channel) ---- */
    if (has_panic) {
        /* panic_blk is reached only via the implicit unwind edge (OP_TRY
         * handler), invisible to the SSA builder.  Add try entry as pred. */
        if (panic_blk->npreds == 0)
            xi_block_add_pred(panic_blk, try_blk);
        xi_lower_braun_seal(l, panic_blk);
        l->cur_block = panic_blk;
        l->dead_after_throw = false;

        XiValue *catch_op = xi_value_new(l->func, l->cur_block, XI_CATCH, l->type_any, 0);
        if (catch_op) {
            catch_op->flags |= XI_FLAG_SIDE_EFFECT;
            catch_op->line = (uint32_t) panic_clause->var_line;
        }
        if (panic_clause->var_name && catch_op) {
            int var_id = xi_lower_var_create(l, panic_clause->symbol_id, panic_clause->var_name,
                                             l->type_any);
            xi_lower_braun_write(l, var_id, l->cur_block, catch_op);
        }
        xi_lower_stmt(l, panic_clause->body);

        /* Pop the handler now that the panic is handled. */
        if (l->cur_block) {
            XiValue *end_op = xi_value_new(l->func, l->cur_block, XI_END_TRY, l->type_unit, 0);
            if (end_op) {
                end_op->flags |= XI_FLAG_SIDE_EFFECT;
                end_op->line = (uint32_t) node->line;
            }
            xi_block_set_jump(l->cur_block, merge);
        }
    }

    xi_lower_braun_seal(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
    l->dead_after_throw = false;
}

XR_FUNC void xi_lower_try_catch(XiLower *l, AstNode *node) {
    lower_try_catch_impl(l, &node->as.try_catch, node);
}

/* ========== Defer / Yield (from xi_lower_expr.c) ========== */

static void lower_defer(XiLower *l, AstNode *node) {
    DeferStmtNode *d = &node->as.defer_stmt;
    AstNode *expr = d->expr;
    if (!expr || !l->cur_block)
        return;

    /* OP_DEFER expects: args[0]=callee, args[1..n]=call arguments.
     * The parser stores either a call expression (defer fn(a, b))
     * or a closure (defer { block }).  Decompose accordingly. */
    if (expr->type == AST_CALL_EXPR) {
        CallExprNode *call = &expr->as.call_expr;
        XiValue *callee = xi_lower_expr(l, call->callee);
        if (!callee || !l->cur_block)
            return;

        int nargs = call->arg_count;
        XR_DCHECK(nargs <= 250, "lower_defer: too many arguments");

        XiValue *v =
            xi_value_new(l->func, l->cur_block, XI_DEFER, l->type_unit, (uint16_t) (1 + nargs));
        if (!v)
            return;
        v->args[0] = callee;
        for (int i = 0; i < nargs; i++) {
            XiValue *arg = xi_lower_expr(l, call->arguments[i]);
            if (!arg)
                return;
            v->args[1 + i] = arg;
        }
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
    } else {
        /* defer { block } — parser wraps in anonymous function expr */
        XiValue *callee = xi_lower_expr(l, expr);
        if (!callee || !l->cur_block)
            return;

        XiValue *v = xi_value_new(l->func, l->cur_block, XI_DEFER, l->type_unit, 1);
        if (!v)
            return;
        v->args[0] = callee;
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
    }
}

static void lower_yield_stmt(XiLower *l) {
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_YIELD, l->type_unit, 0);
    if (v)
        v->flags |= XI_FLAG_SIDE_EFFECT;
}

/* ========== Destructuring (from xi_lower_expr.c) ========== */

/*
 * Bind destructure pattern elements to extracted values from 'src'.
 * Array patterns: INDEX_GET by position.
 * Object patterns: LOAD_FIELD by field name.
 * Identifier patterns: bind directly.
 */
static void lower_destructure_bind(XiLower *l, XrDestructurePattern *pat, XiValue *src) {
    if (!pat || !src || !l->cur_block)
        return;

    switch (pat->type) {
        case PATTERN_ARRAY: {
            int n = pat->as.array.element_count;
            for (int i = 0; i < n; i++) {
                XrDestructurePattern *elem = pat->as.array.elements[i];
                if (!elem)
                    continue;
                XiValue *idx = xi_const_int(l->func, l->cur_block, i, l->type_int);
                XiValue *val = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, l->type_any, 2);
                if (val) {
                    val->args[0] = src;
                    val->args[1] = idx;
                }
                lower_destructure_bind(l, elem, val);
            }
            break;
        }
        case PATTERN_TUPLE: {
            /* Tuples are heterogeneous and immutable: each element comes
             * from a fixed compile-time position, read via XI_TUPLE_GET.
             * The analyzer has bounds-checked arity at the decl site, so
             * we trust pat->as.array.element_count here. */
            int n = pat->as.array.element_count;
            for (int i = 0; i < n; i++) {
                XrDestructurePattern *elem = pat->as.array.elements[i];
                if (!elem)
                    continue;
                XiValue *val = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, l->type_any, 1);
                if (val) {
                    val->args[0] = src;
                    val->aux_int = i;
                }
                lower_destructure_bind(l, elem, val);
            }
            break;
        }
        case PATTERN_OBJECT: {
            int n = pat->as.object.field_count;
            for (int i = 0; i < n; i++) {
                char *fname = pat->as.object.field_names[i];
                XrDestructurePattern *sub = pat->as.object.patterns[i];
                if (!fname)
                    continue;
                /* Use INDEX_GET with string key — works for both JSON objects
                 * and maps (Xi lowers object literals as NEWMAP). */
                XiValue *key = xi_const_str(l->func, l->cur_block, fname, l->type_string);
                XiValue *val = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, l->type_any, 2);
                if (val) {
                    val->args[0] = src;
                    val->args[1] = key;
                }
                lower_destructure_bind(l, sub, val);
            }
            break;
        }
        case PATTERN_IDENTIFIER: {
            const char *name = pat->as.identifier.name;
            if (!name)
                break;
            uint32_t sid = pat->as.identifier.symbol_id;
            /* Resolution order mirrors lower_assignment: local var
             * (with shared-slot follow-up if program-level), then
             * shared from an enclosing scope, then upvalue. The
             * destructure-decl form is handled by the create-write
             * fast path below; only the assign form needs the wider
             * search because the identifier may resolve outward. */
            int var_id = xi_lower_var_find(l, sid, name);
            if (var_id >= 0) {
                xi_lower_braun_write(l, var_id, l->cur_block, src);
                if (l->is_program && l->shared_map[var_id] >= 0) {
                    XiTopBinding b;
                    b.slot = l->shared_map[var_id];
                    b.name = l->vars[var_id].name;
                    b.type = l->vars[var_id].type;
                    xi_lower_emit_top_store(l, b, src);
                }
                break;
            }
            XiTopBinding tb = xi_lower_find_top_binding(l, sid, name);
            if (xi_top_binding_valid(tb)) {
                xi_lower_emit_top_store(l, tb, src);
                break;
            }
            int upval_idx = xi_lower_resolve_upvalue(l, sid, name, NULL);
            if (upval_idx >= 0) {
                XiValue *store =
                    xi_value_new(l->func, l->cur_block, XI_STORE_UPVAL, l->type_unit, 1);
                if (store) {
                    store->args[0] = src;
                    store->aux_int = upval_idx;
                    store->flags |= XI_FLAG_SIDE_EFFECT;
                }
                break;
            }
            /* Fall through: declaration-style binding (create fresh
             * local). Reached for destructure-decl PATTERN_IDENTIFIER
             * because the analyzer has not pre-bound the symbol. */
            int new_var = xi_lower_var_create(l, sid, name, l->type_any);
            xi_lower_braun_write(l, new_var, l->cur_block, src);
            break;
        }
        case PATTERN_SKIP:
            break;
        default:
            XR_CHECK(false, "xi_lower: invalid destructure pattern");
    }
}

/* Destructure declaration: let [a, b] = expr or let {x, y} = expr */
static void lower_destructure_decl(XiLower *l, AstNode *node) {
    DestructureDeclNode *dd = &node->as.destructure_decl;
    XiValue *init = xi_lower_expr(l, dd->initializer);
    if (!init || !dd->pattern)
        return;
    lower_destructure_bind(l, dd->pattern, init);
}

/* Destructure assignment: [a, b] = [b, a] or (a, b) = (b, a) */
static void lower_destructure_assign(XiLower *l, AstNode *node) {
    DestructureAssignNode *da = &node->as.destructure_assign;
    XiValue *rhs = xi_lower_expr(l, da->value);
    if (!rhs || !da->pattern)
        return;
    lower_destructure_bind(l, da->pattern, rhs);
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
        if (!init_val)
            return;
        /* Implicit int→float promotion: when the variable is declared as
         * float but the initializer evaluates to int, insert XI_CONVERT. */
        if (type && XR_TYPE_IS_FLOAT(type) && init_val->type && XR_TYPE_IS_INT(init_val->type)) {
            XiValue *conv = xi_value_new(l->func, l->cur_block, XI_CONVERT, l->type_float, 1);
            if (conv) {
                conv->args[0] = init_val;
                conv->line = (uint32_t) node->line;
                init_val = conv;
            }
        }
        init_val = stmt_narrow_for_target_type(l, node, init_val, type);
    } else {
        /* Zero-value initialization for typed variables without initializer.
         * Nullable types (T?) default to null. Non-nullable primitives:
         * int→0, float→0.0, bool→false; default-initializable structs
         * allocate a zero/default-filled struct value. */
        init_val = stmt_default_struct_value(l, type, node->line);
        if (init_val) {
            /* Already built. */
        } else if (type && !type->is_nullable && type->kind == XR_KIND_INT)
            init_val = xi_const_int(l->func, l->cur_block, 0, l->type_int);
        else if (type && !type->is_nullable && type->kind == XR_KIND_FLOAT)
            init_val = xi_const_float(l->func, l->cur_block, 0.0, l->type_float);
        else if (type && !type->is_nullable && type->kind == XR_KIND_BOOL)
            init_val = xi_const_bool(l->func, l->cur_block, false, l->type_bool);
        else
            init_val = xi_const_null(l->func, l->cur_block, l->type_null);
    }

    /* Propagate reified generic elem_tid when there is an explicit type
     * annotation on a container literal (e.g. let a: Array<int> = [1,2]).
     * Only the annotation distinguishes typed from untyped containers. */
    if (node->as.var_decl.type_annotation && type) {
        if (init_val->op == XI_ARRAY_NEW && XR_TYPE_IS_ARRAY(type) &&
            type->container.element_type) {
            uint8_t tid = xr_type_to_tid(type->container.element_type);
            init_val->aux_int = (int64_t) ((tid << 2) | ((uint8_t) init_val->aux_int & 0x03));
        } else if (init_val->op == XI_SET_NEW && type->kind == XR_KIND_SET &&
                   type->container.element_type) {
            uint8_t tid = xr_type_to_tid(type->container.element_type);
            init_val->aux_int =
                (int64_t) (((tid & 0x1F) << 2) | ((uint8_t) init_val->aux_int & 0x03));
        } else if (init_val->op == XI_CHAN_NEW && type->kind == XR_KIND_CHANNEL &&
                   type->container.element_type) {
            init_val->aux_int = xr_type_to_tid(type->container.element_type);
        } else if (init_val->op == XI_MAP_NEW && XR_TYPE_IS_MAP(type)) {
            uint8_t flags = (uint8_t) (init_val->aux_int & 0x03);
            uint8_t value_tid = 0, key_kind = 0;
            if (type->map.value_type)
                value_tid = xr_type_to_tid(type->map.value_type);
            if (type->map.key_type) {
                uint8_t ktid = xr_type_to_tid(type->map.key_type);
                if (ktid == XR_TID_STRING)
                    key_kind = 1;
                else if (ktid == XR_TID_INT)
                    key_kind = 2;
            }
            init_val->aux_int = (int64_t) ((key_kind << 7) | ((value_tid & 0x1F) << 2) | flags);
        }
    }
    /* When the initializer comes from a different variable, insert an
     * explicit copy so the new variable gets its own SSA value.  Without
     * this, both variables map to the same physical register and
     * loop-carried updates to the source corrupt the snapshot. */
    bool needs_copy =
        (xi_var_id_is_valid(init_val->var_id) && init_val->var_id != (XiVarId) var_id);
    /* Value types (structs) always need deep copy on assignment regardless
     * of var_id — the source could be a shared variable, upvalue, or
     * function return whose identity must not leak into the new binding. */
    if (!needs_copy && type && type->is_value_type) {
        needs_copy = true;
    }
    if (needs_copy) {
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, init_val->type, 1);
        if (copy) {
            copy->args[0] = init_val;
            init_val = copy;
        }
    }
    xi_lower_braun_write(l, var_id, l->cur_block, init_val);

    /* For program-level variables, also store into backing store */
    if (l->is_program && l->shared_map[var_id] >= 0) {
        XiTopBinding b;
        b.slot = l->shared_map[var_id];
        b.name = l->vars[var_id].name;
        b.type = l->vars[var_id].type;
        xi_lower_emit_top_store(l, b, init_val);
    }
}

static void lower_print(XiLower *l, AstNode *node) {
    PrintNode *p = &node->as.print_stmt;
    int nargs = (int) p->expr_count;
    if (nargs < 0 || nargs > (int) UINT16_MAX) {
        fprintf(stderr, "[LOWER] print argument count exceeds %u at line %d\n",
                (unsigned) UINT16_MAX, (int) node->line);
        l->had_error = true;
        return;
    }

    XiValue *stack_args[32];
    XiValue **arg_vals = stack_args;
    if (nargs > 32) {
        arg_vals = (XiValue **) xi_func_arena_alloc(
            l->func, (uint32_t) ((size_t) nargs * sizeof(XiValue *)));
        if (!arg_vals)
            return;
    }
    for (int i = 0; i < nargs; i++) {
        arg_vals[i] = xi_lower_expr(l, p->exprs[i]);
        if (!arg_vals[i])
            return;
    }

    /* Emit one XI_PRINT per argument with correct spacing/newline flags.
     * aux_int encoding:
     *   bit0 = add_space   → OP_PRINT B field
     *   bit1 = newline     → OP_PRINT C bit0
     *   bits 2..3 = slot type hint → OP_PRINT C bits 1..2 (unused here)
     *   bit4 = skip_null   → OP_PRINT C bit3 (REPL auto-echo only) */
    int skip_null = p->skip_null ? 1 : 0;
    for (int i = 0; i < nargs; i++) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PRINT, l->type_unit, 1);
        if (!v)
            return;
        v->args[0] = arg_vals[i];

        int add_space = (i > 0) ? 1 : 0;
        int newline = (i == nargs - 1) ? 1 : 0;
        v->aux_int = add_space | (newline << 1) | (skip_null << 4);

        v->flags = xi_op_default_effects(XI_PRINT);
        v->line = (uint32_t) node->line;
    }
}

static void lower_throw(XiLower *l, AstNode *node) {
    ThrowStmtNode *t = &node->as.throw_stmt;
    XiValue *val = xi_lower_expr(l, t->expression);
    if (!val)
        return;

    /* `throw <enum>` is the value-return error channel: write the error
     * and either branch to the enclosing error-catch (try_depth > 0) or
     * return it from the current function. */
    xi_lower_reprop_error(l, val, node);
}

static void lower_return(XiLower *l, AstNode *node) {
    ReturnStmtNode *ret = &node->as.return_stmt;
    XiValue *val = NULL;

    if (ret->value_count == 1 && ret->values[0]) {
        val = xi_lower_expr(l, ret->values[0]);
        /* Tail-call detection: mark calls in return position so the emitter
         * uses OP_TAILCALL / OP_INVOKE_TAIL (constant-space recursion).
         *
         * IMPORTANT: only apply when the AST return expression is directly
         * a call (AST_CALL_EXPR). If the return expression is a variable
         * that was assigned from a call, SSA propagation makes val->op
         * appear as XI_CALL_METHOD/XI_CALL, but intervening statements
         * (print, assignments) between the call and return make tail-call
         * optimization incorrect — it would discard those side effects.
         *
         * XI_CALL_METHOD → always safe (OP_INVOKE_TAIL handles all types).
         * XI_CALL with self_call flag → always safe (same closure).
         * XI_CALL with callee typed as function → safe.
         * Other XI_CALL (class constructors, etc.) → NOT safe; OP_TAILCALL
         * only handles closures and would fail on class objects. */
        bool is_direct_call = (ret->values[0]->type == AST_CALL_EXPR);
        if (is_direct_call && val && val->op == XI_CALL_METHOD) {
            val->flags |= XI_FLAG_TAIL;
        } else if (is_direct_call && val && val->op == XI_CALL) {
            bool is_self = (val->aux_int & 0xFF) == 1;
            bool callee_is_func = val->nargs >= 1 && val->args[0] && val->args[0]->type &&
                                  val->args[0]->type->kind == XR_KIND_FUNCTION;
            if (is_self || callee_is_func) {
                val->flags |= XI_FLAG_TAIL;
            }
        }
    } else if (ret->value_count > 1) {
        XR_CHECK(false, "obsolete multi-value return reached Xi lowering");
        return;
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
    if (!cond || !l->cur_block)
        return;

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
    if (l->cur_block) /* back edge */
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
    if (!l->cur_block)
        return;

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
    if (!exp->from_path)
        return;

    XiFunc *f = l->func;
    if (exp->is_reexport_all) {
        /* export * from "./file" — single entry with name=NULL */
        XiReexportEntry *e =
            (XiReexportEntry *) xi_func_arena_alloc(f, (uint32_t) sizeof(XiReexportEntry));
        if (!e)
            return;
        uint32_t pl = (uint32_t) strlen(exp->from_path);
        char *pc = (char *) xi_func_arena_alloc(f, pl + 1);
        if (pc)
            memcpy(pc, exp->from_path, pl + 1);
        e->from_path = pc;
        e->name = NULL;
        e->alias = NULL;

        /* Append to reexports array (grow by doubling) */
        uint16_t idx = f->reexport_count;
        if (idx == 0 || !f->reexports) {
            uint16_t cap = 4;
            f->reexports = (XiReexportEntry *) xi_func_arena_alloc(
                f, (uint32_t) (cap * sizeof(XiReexportEntry)));
            if (!f->reexports)
                return;
        }
        f->reexports[idx] = *e;
        f->reexport_count = idx + 1;
        return;
    }

    /* Selective re-export: export { a, b as c } from "./file" */
    for (int i = 0; i < exp->reexport_count; i++) {
        ReexportMember *m = &exp->reexport_members[i];
        if (!m->name)
            continue;

        uint16_t idx = f->reexport_count;
        /* Ensure array capacity (initial alloc or grow) */
        if (idx == 0 || !f->reexports) {
            uint16_t cap = (uint16_t) (exp->reexport_count > 4 ? exp->reexport_count : 4);
            f->reexports = (XiReexportEntry *) xi_func_arena_alloc(
                f, (uint32_t) (cap * sizeof(XiReexportEntry)));
            if (!f->reexports)
                return;
        }

        XiReexportEntry *e = &f->reexports[idx];
        /* Arena-copy strings */
        uint32_t pl = (uint32_t) strlen(exp->from_path);
        char *pc = (char *) xi_func_arena_alloc(f, pl + 1);
        if (pc)
            memcpy(pc, exp->from_path, pl + 1);
        e->from_path = pc;

        uint32_t nl = (uint32_t) strlen(m->name);
        char *nc = (char *) xi_func_arena_alloc(f, nl + 1);
        if (nc)
            memcpy(nc, m->name, nl + 1);
        e->name = nc;

        if (m->alias) {
            uint32_t al = (uint32_t) strlen(m->alias);
            char *ac = (char *) xi_func_arena_alloc(f, al + 1);
            if (ac)
                memcpy(ac, m->alias, al + 1);
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
     * OP_LOAD_MODULE, binding the module object itself. */
    if (imp->member_count == 0) {
        const char *local_name = imp->alias ? imp->alias : imp->module_name;
        if (!local_name)
            return;
        struct XrType *type = xr_type_new_unknown(NULL);
        XiImportRef *ref =
            (XiImportRef *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiImportRef));
        XR_DCHECK(ref != NULL, "lower_import_stmt: arena alloc failed");
        ref->member_name = NULL;
        ref->resolved_mod_index = -1;
        ref->resolved_shared_slot = -1;
        ref->module_path = NULL;
        if (imp->module_name) {
            uint32_t ml = (uint32_t) strlen(imp->module_name);
            char *mc = (char *) xi_func_arena_alloc(l->func, ml + 1);
            if (mc) {
                memcpy(mc, imp->module_name, ml + 1);
                ref->module_path = mc;
            }
        }

        XiValue *v = xi_value_new(l->func, l->cur_block, XI_IMPORT_REF, type, 0);
        if (!v)
            return;
        v->aux = (void *) ref;
        v->aux_int = -1;
        v->line = (uint32_t) node->line;

        int var_id = xi_lower_var_create(l, imp->symbol_id, local_name, type);
        xi_lower_braun_write(l, var_id, l->cur_block, v);

        /* Store into backing store so nested functions can access */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            xi_lower_emit_top_store(l, b, v);
        }
        return;
    }

    for (int i = 0; i < imp->member_count; i++) {
        ImportMember *m = &imp->members[i];
        const char *local_name = m->alias ? m->alias : m->name;

        /* Create XI_IMPORT_REF carrying module path and member name */
        struct XrType *type = xr_type_new_unknown(NULL);
        XiImportRef *ref =
            (XiImportRef *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiImportRef));
        XR_DCHECK(ref != NULL, "lower_import_stmt: arena alloc failed");
        /* Copy strings into arena so they survive AST destruction */
        ref->module_path = NULL;
        ref->member_name = NULL;
        if (imp->module_name) {
            uint32_t ml = (uint32_t) strlen(imp->module_name);
            char *mc = (char *) xi_func_arena_alloc(l->func, ml + 1);
            if (mc) {
                memcpy(mc, imp->module_name, ml + 1);
                ref->module_path = mc;
            }
        }
        if (m->name) {
            uint32_t nl = (uint32_t) strlen(m->name);
            char *nc = (char *) xi_func_arena_alloc(l->func, nl + 1);
            if (nc) {
                memcpy(nc, m->name, nl + 1);
                ref->member_name = nc;
            }
        }
        ref->resolved_mod_index = -1;
        ref->resolved_shared_slot = -1;

        XiValue *v = xi_value_new(l->func, l->cur_block, XI_IMPORT_REF, type, 0);
        if (!v)
            return;
        v->aux = (void *) ref;
        v->aux_int = -1;
        v->line = (uint32_t) node->line;

        /* Bind as a local variable so subsequent references resolve */
        int var_id = xi_lower_var_create(l, m->symbol_id, local_name, type);
        xi_lower_braun_write(l, var_id, l->cur_block, v);

        /* Store into backing store so nested functions can access.
         * Without this mirror, nested scopes see null for the imported
         * member because the read path emits the matching load via the
         * top-binding helper. */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            xi_lower_emit_top_store(l, b, v);
        }
    }
}

/* Main statement dispatcher */
XR_FUNC void xi_lower_stmt(XiLower *l, AstNode *node) {
    if (!node)
        return;
    if (!l->cur_block)
        return; /* dead code */

    switch (node->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            lower_var_decl(l, node);
            break;

        case AST_EXPR_STMT: {
            XiValue *expr = xi_lower_expr(l, node->as.expr_stmt);
            if (expr && expr->op == XI_GO)
                expr->flags |= XI_FLAG_FIRE_AND_FORGET;
        } break;

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
        case AST_CALL_EXPR:
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
            XR_DCHECK_FMT(false, "unsupported stmt AST kind %d in lowering", (int) node->type);
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
        if (!s)
            continue;
        const char *name = NULL;
        uint32_t sid = 0;
        struct XrType *type = NULL;
        bool is_func = false;
        switch (s->type) {
            case AST_FUNCTION_DECL:
                name = s->as.function_decl.name;
                sid = s->as.function_decl.symbol_id;
                type = xi_lower_node_type(l, s);
                is_func = true;
                break;
            case AST_VAR_DECL:
            case AST_CONST_DECL:
                name = s->as.var_decl.name;
                sid = s->as.var_decl.symbol_id;
                type = xi_lower_node_type(l, s);
                break;
            default:
                continue;
        }
        if (!name)
            continue;
        int var_id = xi_lower_var_find(l, sid, name);
        if (var_id < 0) {
            var_id = xi_lower_var_create(l, sid, name, type);
            if (is_func) {
                XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
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
            if (!l->cur_block)
                break;
            AstNode *s = stmts[i];
            if (s && s->type == AST_FUNCTION_DECL && s->as.function_decl.name != NULL)
                xi_lower_stmt(l, s);
        }
        /* After hoisting, mark parent variables that are captured by any
         * hoisted child. Hoisting reorders closures before variable
         * initializers, so the initializer has no IR uses (the capture
         * already bound to the braun-read null placeholder). Marking
         * keeps the initializer alive through DCE. */
        for (uint16_t ci = 0; ci < l->func->nchildren; ci++) {
            XiFunc *child = l->func->children[ci];
            if (!child)
                continue;
            for (uint16_t cj = 0; cj < child->ncaptures; cj++) {
                XiCapture *cap = &child->captures[cj];
                if (cap->source != XI_CAPTURE_SRC_REG)
                    continue;
                /* Resolve capture name back to parent var_id */
                int vid = -1;
                if (cap->value && xi_var_id_is_valid(cap->value->var_id))
                    vid = (int) cap->value->var_id;
                else if (cap->name)
                    vid = xi_lower_var_find(l, 0, cap->name);
                if (vid >= 0 && vid < l->var_count)
                    l->vars[vid].captured_by_child = true;
            }
        }
        for (int i = 0; i < count; i++) {
            if (!l->cur_block)
                break; /* dead code after return/break */
            AstNode *s = stmts[i];
            if (s && s->type == AST_FUNCTION_DECL && s->as.function_decl.name != NULL)
                continue; /* already hoisted */
            xi_lower_stmt(l, s);
        }
    } else {
        for (int i = 0; i < count; i++) {
            if (!l->cur_block)
                break;
            xi_lower_stmt(l, stmts[i]);
        }
    }
}
