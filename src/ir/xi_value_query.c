/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_value_query.c - Backend-neutral IR value/type classification predicates
 */

#include "xi_value_query.h"
#include "xi_analysis.h"
#include "xi_range.h"
#include "../runtime/value/xtype.h"
#include <string.h>

XR_FUNC bool xi_type_is_channel(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_CHANNEL)
        return true;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (xi_type_is_channel(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

XR_FUNC bool xi_type_is_named_instance(const XrType *type, const char *name) {
    if (!type || !name)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return type->instance.class_name && strcmp(type->instance.class_name, name) == 0;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (xi_type_is_named_instance(type->union_type.members[i], name))
                return true;
        }
    }
    return false;
}

XR_FUNC bool xi_type_is_task(const XrType *type) {
    return xi_type_is_named_instance(type, "Task");
}

XR_FUNC bool xi_type_is_thread(const XrType *type) {
    return xi_type_is_named_instance(type, "Thread");
}

/* Strip BOX/UNBOX/COPY identity wrappers so the test sees the carried type. */
static const XiValue *xi_value_unwrap_identity(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v)) &&
           v->nargs >= 1)
        v = v->args[0];
    return v;
}

XR_FUNC bool xi_value_type_is_channel(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_channel(v->type);
}

XR_FUNC bool xi_value_type_is_task(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_task(v->type);
}

XR_FUNC bool xi_value_type_is_thread(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_thread(v->type);
}

XR_FUNC bool xi_value_type_is_atomic(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_named_instance(v->type, "Atomic");
}

XR_FUNC bool xi_value_type_is_work_queue(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_named_instance(v->type, "WorkQueue");
}

XR_FUNC bool xi_value_type_is_result_group(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_named_instance(v->type, "ResultGroup");
}

XR_FUNC bool xi_value_type_is_countdown_latch(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_named_instance(v->type, "CountdownLatch");
}

XR_FUNC bool xi_value_type_is_semaphore(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_named_instance(v->type, "Semaphore");
}

XR_FUNC bool xi_value_type_is_event_count(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_named_instance(v->type, "EventCount");
}

XR_FUNC bool xi_value_type_is_unknown(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return !v || !v->type || v->type->kind == XR_KIND_UNKNOWN;
}

static bool xi_value_const_int(const XiValue *value, int64_t *out) {
    const XiValue *v = xi_value_unwrap_identity(value);
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_INT || !out)
        return false;
    *out = v->aux_int;
    return true;
}

static uint16_t xi_negated_cmp_op(uint16_t op) {
    switch ((XiOp) op) {
        case XI_EQ:
            return XI_NE;
        case XI_NE:
            return XI_EQ;
        case XI_LT:
            return XI_GE;
        case XI_LE:
            return XI_GT;
        case XI_GT:
            return XI_LE;
        case XI_GE:
            return XI_LT;
        default:
            return XI_OP_COUNT;
    }
}

static bool xi_same_int_value(const XiFunc *f, const XiValue *a, const XiValue *b) {
    a = xi_value_unwrap_identity(a);
    b = xi_value_unwrap_identity(b);
    if (!a || !b || !a->type || !b->type || a->type->kind != XR_KIND_INT ||
        b->type->kind != XR_KIND_INT)
        return false;
    if (a == b)
        return true;
    if (f && a->op == XI_LOAD_UPVAL && b->op == XI_LOAD_UPVAL && a->aux_int == b->aux_int &&
        a->aux_int >= 0 && a->aux_int < f->ncaptures) {
        const XiCapture *cap = &f->captures[a->aux_int];
        return cap && !cap->needs_cell && cap->capture_kind != XI_CAPTURE_BY_MUT_CELL &&
               cap->capture_kind != XI_CAPTURE_CORO_SHARED;
    }
    return false;
}

static bool xi_cmp_implies_value_positive(const XiFunc *f, const XiValue *cond,
                                          const XiValue *value, bool truth) {
    cond = xi_value_unwrap_identity(cond);
    value = xi_value_unwrap_identity(value);
    if (!cond || !value || !value->type || value->type->kind != XR_KIND_INT)
        return false;
    if (cond->type && cond->type->kind == XR_KIND_BOOL) {
        if (cond->op == XI_NOT && cond->nargs >= 1)
            return xi_cmp_implies_value_positive(f, cond->args[0], value, !truth);
        if (((truth && cond->op == XI_BAND) || (!truth && cond->op == XI_BOR)) &&
            cond->nargs >= 2) {
            return xi_cmp_implies_value_positive(f, cond->args[0], value, truth) ||
                   xi_cmp_implies_value_positive(f, cond->args[1], value, truth);
        }
    }
    if (cond->nargs < 2)
        return false;

    uint16_t op = truth ? cond->op : xi_negated_cmp_op(cond->op);
    if (op == XI_OP_COUNT)
        return false;

    int64_t c = 0;
    if (xi_same_int_value(f, cond->args[0], value) && xi_value_const_int(cond->args[1], &c)) {
        switch ((XiOp) op) {
            case XI_GT:
                return c >= 0;
            case XI_GE:
            case XI_EQ:
                return c >= 1;
            default:
                return false;
        }
    }
    if (xi_value_const_int(cond->args[0], &c) && xi_same_int_value(f, cond->args[1], value)) {
        switch ((XiOp) op) {
            case XI_LT:
                return c >= 0;
            case XI_LE:
            case XI_EQ:
                return c >= 1;
            default:
                return false;
        }
    }
    return false;
}

static bool xi_cmp_implies_value_nonnegative(const XiFunc *f, const XiValue *cond,
                                             const XiValue *value, bool truth) {
    cond = xi_value_unwrap_identity(cond);
    value = xi_value_unwrap_identity(value);
    if (!cond || !value || !value->type || value->type->kind != XR_KIND_INT)
        return false;
    if (cond->type && cond->type->kind == XR_KIND_BOOL) {
        if (cond->op == XI_NOT && cond->nargs >= 1)
            return xi_cmp_implies_value_nonnegative(f, cond->args[0], value, !truth);
        if (((truth && cond->op == XI_BAND) || (!truth && cond->op == XI_BOR)) &&
            cond->nargs >= 2) {
            return xi_cmp_implies_value_nonnegative(f, cond->args[0], value, truth) ||
                   xi_cmp_implies_value_nonnegative(f, cond->args[1], value, truth);
        }
    }
    if (cond->nargs < 2)
        return false;

    uint16_t op = truth ? cond->op : xi_negated_cmp_op(cond->op);
    if (op == XI_OP_COUNT)
        return false;

    int64_t c = 0;
    if (xi_same_int_value(f, cond->args[0], value) && xi_value_const_int(cond->args[1], &c)) {
        switch ((XiOp) op) {
            case XI_GT:
                return c >= -1;
            case XI_GE:
            case XI_EQ:
                return c >= 0;
            default:
                return false;
        }
    }
    if (xi_value_const_int(cond->args[0], &c) && xi_same_int_value(f, cond->args[1], value)) {
        switch ((XiOp) op) {
            case XI_LT:
                return c >= -1;
            case XI_LE:
            case XI_EQ:
                return c >= 0;
            default:
                return false;
        }
    }
    return false;
}

static bool xi_cmp_implies_value_ge(const XiFunc *f, const XiValue *cond, const XiValue *value,
                                    bool truth, int64_t lower_bound) {
    cond = xi_value_unwrap_identity(cond);
    value = xi_value_unwrap_identity(value);
    if (!cond || !value || !value->type || value->type->kind != XR_KIND_INT)
        return false;
    if (cond->type && cond->type->kind == XR_KIND_BOOL) {
        if (cond->op == XI_NOT && cond->nargs >= 1)
            return xi_cmp_implies_value_ge(f, cond->args[0], value, !truth, lower_bound);
        if (((truth && cond->op == XI_BAND) || (!truth && cond->op == XI_BOR)) &&
            cond->nargs >= 2) {
            return xi_cmp_implies_value_ge(f, cond->args[0], value, truth, lower_bound) ||
                   xi_cmp_implies_value_ge(f, cond->args[1], value, truth, lower_bound);
        }
    }
    if (cond->nargs < 2)
        return false;

    uint16_t op = truth ? cond->op : xi_negated_cmp_op(cond->op);
    if (op == XI_OP_COUNT)
        return false;

    int64_t c = 0;
    if (xi_same_int_value(f, cond->args[0], value) && xi_value_const_int(cond->args[1], &c)) {
        switch ((XiOp) op) {
            case XI_GT:
                return lower_bound == INT64_MIN || c >= lower_bound - 1;
            case XI_GE:
            case XI_EQ:
                return c >= lower_bound;
            default:
                return false;
        }
    }
    if (xi_value_const_int(cond->args[0], &c) && xi_same_int_value(f, cond->args[1], value)) {
        switch ((XiOp) op) {
            case XI_LT:
                return lower_bound == INT64_MIN || c >= lower_bound - 1;
            case XI_LE:
            case XI_EQ:
                return c >= lower_bound;
            default:
                return false;
        }
    }
    return false;
}

static int xi_if_truth_on_path_to_block(const XiBlock *guard, const XiBlock *site) {
    if (!guard || !site || guard == site || guard->kind != XI_BLOCK_IF || !guard->succs[0] ||
        !guard->succs[1])
        return -1;
    bool true_path = xi_dominates(guard->succs[0], site);
    bool false_path = xi_dominates(guard->succs[1], site);
    if (true_path == false_path)
        return -1;
    return true_path ? 1 : 0;
}

static bool xi_value_has_positive_dominating_guard(const XiFunc *f, const XiValue *value,
                                                   const XiBlock *site) {
    if (!f || !value || !site)
        return false;
    xi_ensure_dominators((XiFunc *) f);
    for (const XiBlock *guard = site->idom; guard; guard = guard->idom) {
        int truth = xi_if_truth_on_path_to_block(guard, site);
        if (truth >= 0 && xi_cmp_implies_value_positive(f, guard->control, value, truth != 0))
            return true;
    }
    return false;
}

static bool xi_value_has_nonnegative_dominating_guard(const XiFunc *f, const XiValue *value,
                                                      const XiBlock *site) {
    if (!f || !value || !site)
        return false;
    xi_ensure_dominators((XiFunc *) f);
    for (const XiBlock *guard = site->idom; guard; guard = guard->idom) {
        int truth = xi_if_truth_on_path_to_block(guard, site);
        if (truth >= 0 && xi_cmp_implies_value_nonnegative(f, guard->control, value, truth != 0))
            return true;
    }
    return false;
}

static bool xi_value_has_ge_dominating_guard(const XiFunc *f, const XiValue *value,
                                             const XiBlock *site, int64_t lower_bound) {
    if (!f || !value || !site)
        return false;
    xi_ensure_dominators((XiFunc *) f);
    for (const XiBlock *guard = site->idom; guard; guard = guard->idom) {
        int truth = xi_if_truth_on_path_to_block(guard, site);
        if (truth >= 0 &&
            xi_cmp_implies_value_ge(f, guard->control, value, truth != 0, lower_bound))
            return true;
    }
    return false;
}

XR_FUNC bool xi_value_known_positive_at(const XiFunc *f, const XiValue *value,
                                        const XiBlock *site) {
    const XiValue *v = xi_value_unwrap_identity(value);
    if (!v || !v->type || v->type->kind != XR_KIND_INT)
        return false;

    int64_t c = 0;
    if (xi_value_const_int(v, &c))
        return c > 0;
    if (xi_range_known_positive(xi_range_of(v)))
        return true;
    return xi_value_has_positive_dominating_guard(f, v, site);
}

static bool xi_value_is_unsigned_i64_safe_width(const XiValue *v) {
    if (!v || !v->type || v->type->kind != XR_KIND_INT)
        return false;

    if (v->type->native_width == XR_NATIVE_U8 || v->type->native_width == XR_NATIVE_U16 ||
        v->type->native_width == XR_NATIVE_U32)
        return true;

    switch ((XiOp) v->op) {
        case XI_NARROW_U8:
        case XI_NARROW_U16:
        case XI_NARROW_U32:
        case XI_WIDEN_U8:
        case XI_WIDEN_U16:
        case XI_WIDEN_U32:
            return true;
        default:
            return false;
    }
}

XR_FUNC bool xi_value_known_nonnegative_at(const XiFunc *f, const XiValue *value,
                                           const XiBlock *site) {
    const XiValue *v = xi_value_unwrap_identity(value);
    if (!v || !v->type || v->type->kind != XR_KIND_INT)
        return false;

    int64_t c = 0;
    if (xi_value_const_int(v, &c))
        return c >= 0;
    if (xi_value_is_unsigned_i64_safe_width(v))
        return true;
    if (v->op == XI_CONVERT && v->nargs >= 1 && v->args[0] &&
        xi_value_known_nonnegative_at(f, v->args[0], site))
        return true;
    if (xi_range_known_nonneg(xi_range_of(v)))
        return true;
    return xi_value_has_nonnegative_dominating_guard(f, v, site);
}

XR_FUNC bool xi_value_known_ge_at(const XiFunc *f, const XiValue *value, const XiBlock *site,
                                  int64_t lower_bound) {
    const XiValue *v = xi_value_unwrap_identity(value);
    if (!v || !v->type || v->type->kind != XR_KIND_INT)
        return false;

    int64_t c = 0;
    if (xi_value_const_int(v, &c))
        return c >= lower_bound;
    if (lower_bound <= 0 && xi_value_is_unsigned_i64_safe_width(v))
        return true;
    if (v->op == XI_CONVERT && v->nargs >= 1 && v->args[0] &&
        xi_value_known_ge_at(f, v->args[0], site, lower_bound))
        return true;
    if (xi_range_known_ge(xi_range_of(v), lower_bound))
        return true;
    return xi_value_has_ge_dominating_guard(f, v, site, lower_bound);
}
