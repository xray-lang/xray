/*
 * test_xi_opt.c - Unit tests for Xi IR optimization passes
 *
 * Tests constant folding, copy propagation, dead code elimination,
 * phi simplification, and the combined pass runner.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/ir/xi_core_api.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_opt_block_simplify.h"
#include "../../../src/ir/xi_opt_jump_thread.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"
#include "../../../src/plan/semantic/xr_semantic_verify.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* Minimal XrType stubs */
static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_float = {.kind = XR_KIND_FLOAT, .id = 2, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};
static XrType stub_null = {.kind = XR_KIND_NULL, .id = 4, .frozen = true};
static XrType stub_str = {.kind = XR_KIND_STRING, .id = 5, .frozen = true};
static XrType stub_void = {.kind = XR_KIND_UNIT, .id = 6, .frozen = true};
static XrType stub_func = {.kind = XR_KIND_FUNCTION, .id = 7, .frozen = true};
static XrType stub_task = {
    .kind = XR_KIND_INSTANCE, .id = 10, .frozen = true, .instance = {.class_name = "Task"}};
static XrType stub_result_group = {
    .kind = XR_KIND_INSTANCE, .id = 13, .frozen = true, .instance = {.class_name = "ResultGroup"}};
static XrType stub_task_array = {
    .kind = XR_KIND_ARRAY,
    .id = 12,
    .frozen = true,
    .container = {.element_type = &stub_task},
};
static XrType stub_u8 = {.kind = XR_KIND_INT, .id = 8, .frozen = true, .scalar_rep = XR_NATIVE_U8};
static XrType stub_uint64 = {
    .kind = XR_KIND_INT, .id = 11, .frozen = true, .scalar_rep = XR_NATIVE_U64};
static XrType stub_u8_array = {
    .kind = XR_KIND_ARRAY,
    .id = 9,
    .frozen = true,
    .container = {.element_type = &stub_u8},
};
static XrType stub_hof_int = {
    .kind = XR_KIND_INT,
    .id = 1,
    .frozen = true,
    .scalar_rep = XR_NATIVE_I64,
};
static XrFunctionParam stub_hof_unary_int_params[] = {
    {.type = &stub_hof_int, .mode = XR_PARAM_READ},
};
static XrFunctionParam stub_hof_reduce_params[] = {
    {.type = &stub_hof_int, .mode = XR_PARAM_READ},
    {.type = &stub_hof_int, .mode = XR_PARAM_READ},
};
static XrType stub_hof_bool = {
    .kind = XR_KIND_BOOL,
    .id = 2,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_hof_map_callback = {
    .kind = XR_KIND_FUNCTION,
    .id = 123,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .params = stub_hof_unary_int_params,
        .param_count = 1,
        .min_params = 1,
        .return_type = &stub_hof_int,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};
static XrType stub_hof_filter_callback = {
    .kind = XR_KIND_FUNCTION,
    .id = 125,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .params = stub_hof_unary_int_params,
        .param_count = 1,
        .min_params = 1,
        .return_type = &stub_hof_bool,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};
static XrType stub_hof_reduce_callback = {
    .kind = XR_KIND_FUNCTION,
    .id = 126,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {
        .params = stub_hof_reduce_params,
        .param_count = 2,
        .min_params = 2,
        .return_type = &stub_hof_int,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};
static XrType stub_hof_int_array = {
    .kind = XR_KIND_ARRAY,
    .id = 124,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_hof_int},
};

static int tests_passed = 0;
static int tests_failed = 0;

/* Create function with sealed entry block. */
static XiFunc *make_func(const char *name, XrType *ret_type) {
    XiFunc *f = xi_func_new(name, ret_type);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

static int pred_index(const XiBlock *blk, const XiBlock *pred) {
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] == pred)
            return (int) i;
    }
    return -1;
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

/* assert() is compiled out under NDEBUG, which is how the release lane builds
 * this binary. A case that has to hold in both lanes states it with this. */
#define REQUIRE(cond)                                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                                     \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* ========== Constant Folding Tests ========== */

TEST(const_fold_int_add) {
    /* 3 + 4 -> 7 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c3, c4);
    xi_block_set_return(blk, add);

    xi_opt_const_fold(f);

    assert(add->op == XI_CONST && "add should be folded to CONST");
    assert(add->aux_int == 7 && "3 + 4 should be 7");
    assert(add->nargs == 0 && "folded value should have 0 args");
    xi_func_free(f);
}

TEST(const_fold_int_sub) {
    /* 10 - 3 -> 7 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c10 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *sub = xi_binary(f, blk, XI_SUB, &stub_int, c10, c3);

    xi_opt_const_fold(f);

    assert(sub->op == XI_CONST && sub->aux_int == 7);
    xi_func_free(f);
}

TEST(const_fold_int_mul) {
    /* 5 * 6 -> 30 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c5 = xi_const_int(f, blk, 5, &stub_int);
    XiValue *c6 = xi_const_int(f, blk, 6, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, c5, c6);

    xi_opt_const_fold(f);

    assert(mul->op == XI_CONST && mul->aux_int == 30);
    xi_func_free(f);
}

TEST(const_fold_int_div) {
    /* 20 / 4 -> 5 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c20 = xi_const_int(f, blk, 20, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *div = xi_binary(f, blk, XI_DIV, &stub_int, c20, c4);

    xi_opt_const_fold(f);

    assert(div->op == XI_CONST && div->aux_int == 5);
    xi_func_free(f);
}

TEST(const_fold_div_by_zero) {
    /* 10 / 0 -> NOT folded (undefined behavior) */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c10 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *div = xi_binary(f, blk, XI_DIV, &stub_int, c10, c0);

    xi_opt_const_fold(f);

    assert(div->op == XI_DIV && "div by zero should NOT be folded");
    xi_func_free(f);
}

TEST(const_fold_int_compare) {
    /* 3 < 5 -> true (1) */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c5 = xi_const_int(f, blk, 5, &stub_int);
    XiValue *lt = xi_binary(f, blk, XI_LT, &stub_bool, c3, c5);

    xi_opt_const_fold(f);

    assert(lt->op == XI_CONST && lt->aux_int == 1 && "3 < 5 should be true");
    xi_func_free(f);
}

TEST(const_fold_uint64_compare) {
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *high = xi_const_int(f, blk, INT64_MIN, &stub_uint64);
    XiValue *zero = xi_const_int(f, blk, 0, &stub_int);
    XiValue *gt = xi_binary(f, blk, XI_GT, &stub_bool, high, zero);

    xi_opt_const_fold(f);

    assert(gt->op == XI_CONST && gt->aux_int == 1 &&
           "u64 high-bit value should fold as greater than zero");
    xi_func_free(f);
}

TEST(const_fold_uint64_type_view_copy) {
    XiFunc *f = make_func("test", &stub_uint64);
    XiBlock *blk = f->entry;

    XiValue *base = xi_const_int(f, blk, INT64_MAX, &stub_int);
    XiValue *view = xi_value_new(f, blk, XI_COPY, &stub_uint64, 1);
    assert(view != NULL);
    view->args[0] = base;
    XiValue *one = xi_const_int(f, blk, 1, &stub_int);
    XiValue *sum = xi_binary(f, blk, XI_ADD, &stub_uint64, view, one);

    xi_opt_const_fold(f);

    assert(sum->op == XI_CONST && sum->aux_int == INT64_MIN &&
           "u64 type-view copy should not block constant folding");
    assert(sum->type == &stub_uint64 && "folded value should keep u64 static type");
    xi_func_free(f);
}

TEST(const_fold_neg) {
    /* -(42) -> -42 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c42 = xi_const_int(f, blk, 42, &stub_int);
    XiValue *neg = xi_unary(f, blk, XI_NEG, &stub_int, c42);

    xi_opt_const_fold(f);

    assert(neg->op == XI_CONST && neg->aux_int == -42);
    xi_func_free(f);
}

TEST(const_fold_not) {
    /* !true -> false */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *ctrue = xi_const_bool(f, blk, true, &stub_bool);
    XiValue *n = xi_unary(f, blk, XI_NOT, &stub_bool, ctrue);

    xi_opt_const_fold(f);

    assert(n->op == XI_CONST && n->aux_int == 0 && "!true should be false");
    xi_func_free(f);
}

TEST(const_fold_float_add) {
    /* 1.5 + 2.5 -> 4.0 */
    XiFunc *f = make_func("test", &stub_float);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_float(f, blk, 1.5, &stub_float);
    XiValue *c2 = xi_const_float(f, blk, 2.5, &stub_float);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_float, c1, c2);

    xi_opt_const_fold(f);

    assert(add->op == XI_CONST && "float add should be folded");
    double result;
    memcpy(&result, &add->aux_int, sizeof(double));
    assert(result == 4.0 && "1.5 + 2.5 should be 4.0");
    xi_func_free(f);
}

TEST(const_fold_chain) {
    /* (2 + 3) * 4 -> 5 * 4 -> 20 after two passes */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c2, c3);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *mul = xi_binary(f, blk, XI_MUL, &stub_int, add, c4);

    /* First pass folds add to 5 */
    xi_opt_const_fold(f);
    assert(add->op == XI_CONST && add->aux_int == 5);

    /* Second pass folds mul to 20 (add is now const) */
    xi_opt_const_fold(f);
    assert(mul->op == XI_CONST && mul->aux_int == 20);
    xi_func_free(f);
}

TEST(const_fold_no_fold_variable) {
    /* x + 3 should NOT be folded */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, x, c3);

    xi_opt_const_fold(f);

    assert(add->op == XI_ADD && "x + 3 should NOT be folded");
    xi_func_free(f);
}

static void setup_shared_const_slots(XiFunc *f, uint16_t count) {
    f->nshared = count;
    f->slot_owned_consts = (uint8_t *) xi_func_arena_alloc(f, count * sizeof(uint8_t));
    f->shared_const_literals =
        (XiConstLiteral *) xi_func_arena_alloc(f, count * sizeof(XiConstLiteral));
    f->shared_const_literal_count = count;
    assert(f->slot_owned_consts && f->shared_const_literals);
    memset(f->slot_owned_consts, 1, count * sizeof(uint8_t));
    memset(f->shared_const_literals, 0, count * sizeof(XiConstLiteral));
}

TEST(const_fold_shared_const_get) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    setup_shared_const_slots(f, 1);
    f->shared_const_literals[0].kind = XI_CONST_LITERAL_INT;
    f->shared_const_literals[0].type = &stub_int;
    f->shared_const_literals[0].int_value = 42;

    XiValue *load = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    load->aux_int = 0;

    xi_opt_const_fold(f);

    assert(load->op == XI_CONST && "const shared load should fold to CONST");
    assert(load->aux_int == 42);
    assert(load->nargs == 0);
    assert((load->flags & XI_FLAG_READS_MEM) == 0);
    xi_func_free(f);
}

TEST(const_fold_shared_const_set_records_expression_result) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;
    setup_shared_const_slots(f, 2);

    XiValue *c16 = xi_const_int(f, blk, 16, &stub_int);
    XiValue *set0 = xi_value_new(f, blk, XI_SET_SHARED, &stub_void, 1);
    set0->args[0] = c16;
    set0->aux_int = 0;

    XiValue *load0 = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    load0->aux_int = 0;
    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    XiValue *shl = xi_binary(f, blk, XI_SHL, &stub_int, load0, c2);
    XiValue *set1 = xi_value_new(f, blk, XI_SET_SHARED, &stub_void, 1);
    set1->args[0] = shl;
    set1->aux_int = 1;
    XiValue *load1 = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    load1->aux_int = 1;

    xi_opt_const_fold(f);

    assert(f->shared_const_literals[0].kind == XI_CONST_LITERAL_INT);
    assert(f->shared_const_literals[0].int_value == 16);
    assert(shl->op == XI_CONST && shl->aux_int == 64);
    assert(f->shared_const_literals[1].kind == XI_CONST_LITERAL_INT);
    assert(f->shared_const_literals[1].int_value == 64);
    assert(load1->op == XI_CONST && load1->aux_int == 64);
    xi_func_free(f);
}

TEST(const_fold_int_mod) {
    /* 17 % 5 -> 2 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c17 = xi_const_int(f, blk, 17, &stub_int);
    XiValue *c5 = xi_const_int(f, blk, 5, &stub_int);
    XiValue *mod = xi_binary(f, blk, XI_MOD, &stub_int, c17, c5);

    xi_opt_const_fold(f);

    assert(mod->op == XI_CONST && mod->aux_int == 2);
    xi_func_free(f);
}

TEST(const_fold_mod_by_zero) {
    /* 10 % 0 -> NOT folded */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c10 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *mod = xi_binary(f, blk, XI_MOD, &stub_int, c10, c0);

    xi_opt_const_fold(f);

    assert(mod->op == XI_MOD && "mod by zero should NOT be folded");
    xi_func_free(f);
}

TEST(const_fold_bnot) {
    /* Shared bitwise-not semantics must remain exact at both signed edges. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *cmin = xi_const_int(f, blk, INT64_MIN, &stub_int);
    XiValue *cmax = xi_const_int(f, blk, INT64_MAX, &stub_int);
    XiValue *bn0 = xi_unary(f, blk, XI_BNOT, &stub_int, c0);
    XiValue *bnmin = xi_unary(f, blk, XI_BNOT, &stub_int, cmin);
    XiValue *bnmax = xi_unary(f, blk, XI_BNOT, &stub_int, cmax);

    xi_opt_const_fold(f);

    assert(bn0->op == XI_CONST && bn0->aux_int == -1);
    assert(bnmin->op == XI_CONST && bnmin->aux_int == INT64_MAX);
    assert(bnmax->op == XI_CONST && bnmax->aux_int == INT64_MIN);
    xi_func_free(f);
}

TEST(const_fold_float_sub) {
    /* 5.0 - 1.5 -> 3.5 */
    XiFunc *f = make_func("test", &stub_float);
    XiBlock *blk = f->entry;

    XiValue *c5 = xi_const_float(f, blk, 5.0, &stub_float);
    XiValue *c1 = xi_const_float(f, blk, 1.5, &stub_float);
    XiValue *sub = xi_binary(f, blk, XI_SUB, &stub_float, c5, c1);

    xi_opt_const_fold(f);

    assert(sub->op == XI_CONST && "float sub should be folded");
    double result;
    memcpy(&result, &sub->aux_int, sizeof(double));
    assert(result == 3.5 && "5.0 - 1.5 should be 3.5");
    xi_func_free(f);
}

TEST(const_fold_float_compare) {
    /* 2.0 < 3.0 -> true */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *c2 = xi_const_float(f, blk, 2.0, &stub_float);
    XiValue *c3 = xi_const_float(f, blk, 3.0, &stub_float);
    XiValue *lt = xi_binary(f, blk, XI_LT, &stub_bool, c2, c3);

    xi_opt_const_fold(f);

    assert(lt->op == XI_CONST && lt->aux_int == 1 && "2.0 < 3.0 should be true");
    xi_func_free(f);
}

TEST(const_fold_int_eq) {
    /* 7 == 7 -> true */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *c7a = xi_const_int(f, blk, 7, &stub_int);
    XiValue *c7b = xi_const_int(f, blk, 7, &stub_int);
    XiValue *eq = xi_binary(f, blk, XI_EQ, &stub_bool, c7a, c7b);

    xi_opt_const_fold(f);

    assert(eq->op == XI_CONST && eq->aux_int == 1 && "7 == 7 should be true");
    xi_func_free(f);
}

TEST(const_fold_int_ne) {
    /* 3 != 5 -> true */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c5 = xi_const_int(f, blk, 5, &stub_int);
    XiValue *ne = xi_binary(f, blk, XI_NE, &stub_bool, c3, c5);

    xi_opt_const_fold(f);

    assert(ne->op == XI_CONST && ne->aux_int == 1 && "3 != 5 should be true");
    xi_func_free(f);
}

TEST(const_fold_bitwise_ops) {
    /* 0xFF & 0x0F -> 0x0F; 0xA0 | 0x05 -> 0xA5; 6 ^ 3 -> 5 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *cFF = xi_const_int(f, blk, 0xFF, &stub_int);
    XiValue *c0F = xi_const_int(f, blk, 0x0F, &stub_int);
    XiValue *band = xi_binary(f, blk, XI_BAND, &stub_int, cFF, c0F);

    XiValue *cA0 = xi_const_int(f, blk, 0xA0, &stub_int);
    XiValue *c05 = xi_const_int(f, blk, 0x05, &stub_int);
    XiValue *bor = xi_binary(f, blk, XI_BOR, &stub_int, cA0, c05);

    XiValue *c6 = xi_const_int(f, blk, 6, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *bxor = xi_binary(f, blk, XI_BXOR, &stub_int, c6, c3);
    XiValue *cneg1 = xi_const_int(f, blk, -1, &stub_int);
    XiValue *cmin = xi_const_int(f, blk, INT64_MIN, &stub_int);
    XiValue *band_negative = xi_binary(f, blk, XI_BAND, &stub_int, cneg1, c0F);
    XiValue *bor_edge = xi_binary(f, blk, XI_BOR, &stub_int, cmin, c05);
    XiValue *bxor_edge = xi_binary(f, blk, XI_BXOR, &stub_int, cneg1, cmin);

    xi_opt_const_fold(f);

    assert(band->op == XI_CONST && band->aux_int == 0x0F);
    assert(bor->op == XI_CONST && bor->aux_int == 0xA5);
    assert(bxor->op == XI_CONST && bxor->aux_int == 5);
    assert(band_negative->op == XI_CONST && band_negative->aux_int == 0x0F);
    assert(bor_edge->op == XI_CONST && bor_edge->aux_int == INT64_MIN + 5);
    assert(bxor_edge->op == XI_CONST && bxor_edge->aux_int == INT64_MAX);
    xi_func_free(f);
}

TEST(const_fold_shift) {
    /* Shared owner freezes mod-64 counts and signed/unsigned right shift. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *shl = xi_binary(f, blk, XI_SHL, &stub_int, c1, c4);

    XiValue *c32 = xi_const_int(f, blk, 32, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    XiValue *shr = xi_binary(f, blk, XI_SHR, &stub_int, c32, c2);
    XiValue *cneg1 = xi_const_int(f, blk, -1, &stub_int);
    XiValue *shl_neg_count = xi_binary(f, blk, XI_SHL, &stub_int, c1, cneg1);
    XiValue *cmin = xi_const_int(f, blk, INT64_MIN, &stub_int);
    XiValue *c63 = xi_const_int(f, blk, 63, &stub_int);
    XiValue *shr_signed = xi_binary(f, blk, XI_SHR, &stub_int, cmin, c63);
    XiValue *umin = xi_const_int(f, blk, INT64_MIN, &stub_uint64);
    XiValue *uone = xi_const_int(f, blk, 1, &stub_int);
    XiValue *shr_unsigned = xi_binary(f, blk, XI_SHR, &stub_uint64, umin, uone);

    xi_opt_const_fold(f);

    assert(shl->op == XI_CONST && shl->aux_int == 16);
    assert(shr->op == XI_CONST && shr->aux_int == 8);
    assert(shl_neg_count->op == XI_CONST && shl_neg_count->aux_int == INT64_MIN);
    assert(shr_signed->op == XI_CONST && shr_signed->aux_int == -1);
    assert(shr_unsigned->op == XI_CONST &&
           shr_unsigned->aux_int == INT64_C(4611686018427387904));
    xi_func_free(f);
}

/* ========== Copy Propagation Tests ========== */

TEST(copy_prop_basic) {
    /* x = COPY(a); y = x + 1 -> y = a + 1 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *a = xi_param(f, blk, 0, &stub_int);
    XiValue *copy = xi_value_new(f, blk, XI_COPY, &stub_int, 1);
    copy->args[0] = a;
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, copy, c1);

    xi_opt_copy_prop(f);

    assert(add->args[0] == a && "copy should be propagated");
    xi_func_free(f);
}

TEST(copy_prop_chain) {
    /* a -> COPY -> COPY -> use -> use(a) */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *a = xi_param(f, blk, 0, &stub_int);
    XiValue *cp1 = xi_value_new(f, blk, XI_COPY, &stub_int, 1);
    cp1->args[0] = a;
    XiValue *cp2 = xi_value_new(f, blk, XI_COPY, &stub_int, 1);
    cp2->args[0] = cp1;
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, cp2, c1);

    xi_opt_copy_prop(f);

    assert(add->args[0] == a && "chained copies should resolve to original");
    xi_func_free(f);
}

/* ========== One-shot Await Marking Tests ========== */

TEST(mark_one_shot_await_unique_go) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *await = xi_value_new(f, blk, XI_AWAIT, &stub_int, 1);
    await->args[0] = go;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    assert(chg.values_changed && "unique go->await should be marked");
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) != 0);
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0);
    assert((go->aux_int & XI_GO_AUX_DEFER_BATCH) == 0);
    xi_func_free(f);
}

TEST(mark_one_shot_await_through_copy) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *copy = xi_value_new(f, blk, XI_COPY, &stub_task, 1);
    copy->args[0] = go;
    XiValue *await = xi_value_new(f, blk, XI_AWAIT, &stub_int, 1);
    await->args[0] = copy;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    assert(chg.values_changed && "unique copied go->await should be marked");
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) != 0);
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0);
    xi_func_free(f);
}

TEST(mark_one_shot_await_completes_direct_lowering_pair) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *await = xi_value_new(f, blk, XI_AWAIT, &stub_int, 1);
    await->args[0] = go;
    await->aux_int |= XI_AWAIT_AUX_CONSUME_TASK;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    assert(chg.values_changed && "pre-marked await should still mark its go producer");
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) != 0);
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0);
    xi_func_free(f);
}

TEST(mark_one_shot_await_keeps_visible_task) {
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *await = xi_value_new(f, blk, XI_AWAIT, &stub_int, 1);
    await->args[0] = go;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    XiValue *done = xi_value_new(f, blk, XI_LOAD_FIELD, &stub_bool, 1);
    done->args[0] = go;
    done->aux = (void *) "done";

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    assert(!chg.values_changed && "observable Task use must not be one-shot");
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0);
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) == 0);
    xi_func_free(f);
}

TEST(mark_one_shot_await_skips_linked_go) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->aux_int = 1;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *await = xi_value_new(f, blk, XI_AWAIT, &stub_int, 1);
    await->args[0] = go;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    assert(!chg.values_changed && "linked go has observable propagation state");
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0);
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) == 0);
    xi_func_free(f);
}

TEST(mark_one_shot_await_all_fresh_literal) {
    XiFunc *f = make_func("test", &stub_task_array);
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *go1 = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go1->args[0] = callee;
    go1->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *go2 = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go2->args[0] = callee;
    go2->flags |= XI_FLAG_SIDE_EFFECT;

    XiValue *cap = xi_const_int(f, blk, 2, &stub_int);
    XiValue *arr = xi_value_new(f, blk, XI_ARRAY_NEW, &stub_task_array, 1);
    arr->args[0] = cap;
    XiValue *idx0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *set0 = xi_value_new(f, blk, XI_INDEX_SET, &stub_void, 3);
    set0->args[0] = arr;
    set0->args[1] = idx0;
    set0->args[2] = go1;
    set0->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *idx1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *set1 = xi_value_new(f, blk, XI_INDEX_SET, &stub_void, 3);
    set1->args[0] = arr;
    set1->args[1] = idx1;
    set1->args[2] = go2;
    set1->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    XiValue *await = xi_value_new(f, blk, XI_AWAIT, &stub_task_array, 1);
    await->args[0] = arr;
    await->aux_int |= XI_AWAIT_AUX_ALL;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    assert(chg.values_changed && "fresh await-all task literal should be aggregate one-shot");
    assert((await->aux_int & XI_AWAIT_AUX_AGGREGATE_ONE_SHOT) != 0);
    assert((go1->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0);
    assert((go2->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0);
    assert((go1->aux_int & XI_GO_AUX_DEFER_BATCH) != 0);
    assert((go2->aux_int & XI_GO_AUX_DEFER_BATCH) != 0);
    xi_func_free(f);
}

TEST(mark_one_shot_await_all_keeps_visible_array) {
    XiFunc *f = make_func("test", &stub_task_array);
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *cap = xi_const_int(f, blk, 1, &stub_int);
    XiValue *arr = xi_value_new(f, blk, XI_ARRAY_NEW, &stub_task_array, 1);
    arr->args[0] = cap;
    XiValue *idx0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *set0 = xi_value_new(f, blk, XI_INDEX_SET, &stub_void, 3);
    set0->args[0] = arr;
    set0->args[1] = idx0;
    set0->args[2] = go;
    set0->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *await = xi_value_new(f, blk, XI_AWAIT, &stub_task_array, 1);
    await->args[0] = arr;
    await->aux_int |= XI_AWAIT_AUX_ALL;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    XiValue *len = xi_value_new(f, blk, XI_LEN, &stub_int, 1);
    len->args[0] = arr;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    assert(!chg.values_changed && "observable task array must not be consumed");
    assert((await->aux_int & XI_AWAIT_AUX_AGGREGATE_ONE_SHOT) == 0);
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) == 0);
    assert((go->aux_int & XI_GO_AUX_DEFER_BATCH) == 0);
    xi_func_free(f);
}

TEST(mark_one_shot_sequential_await_fresh_cleared_task_array) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *cap = xi_const_int(f, blk, 1, &stub_int);
    XiValue *arr = xi_value_new(f, blk, XI_ARRAY_NEW, &stub_task_array, 1);
    arr->args[0] = cap;
    XiValue *clear = xi_value_new(f, blk, XI_CALL_METHOD, &stub_void, 1);
    clear->args[0] = arr;
    clear->aux = (void *) "clear";
    clear->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *push = xi_value_new(f, blk, XI_CALL_METHOD, &stub_void, 2);
    push->args[0] = arr;
    push->args[1] = go;
    push->aux = (void *) "push";
    push->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    XiValue *length = xi_value_new(f, blk, XI_LEN, &stub_int, 1);
    length->args[0] = arr;
    XiValue *idx = xi_const_int(f, blk, 0, &stub_int);
    XiValue *get = xi_value_new(f, blk, XI_INDEX_GET, &stub_task, 2);
    get->args[0] = arr;
    get->args[1] = idx;
    XiValue *await = xi_value_new(f, blk, XI_AWAIT, &stub_int, 1);
    await->args[0] = get;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    (void) length;
    assert(chg.values_changed && "cleared task array with sequential awaits should be batched");
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0);
    assert((go->aux_int & XI_GO_AUX_DEFER_BATCH) != 0);
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0);
    assert((await->aux_int & XI_AWAIT_AUX_SUBMIT_DEFERRED_BATCH) != 0);
    xi_func_free(f);
}

TEST(mark_one_shot_sequential_await_counted_task_array_loop) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *latch = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = body->sealed = latch->sealed = exit_blk->sealed = true;

    XiValue *cap = xi_const_int(f, entry, 0, &stub_int);
    XiValue *arr = xi_value_new(f, entry, XI_ARRAY_NEW, &stub_task_array, 1);
    arr->args[0] = cap;
    XiValue *clear = xi_value_new(f, entry, XI_CALL_METHOD, &stub_void, 1);
    clear->args[0] = arr;
    clear->aux = (void *) "clear";
    clear->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *callee = xi_param(f, entry, 0, &stub_func);
    XiValue *go = xi_value_new(f, entry, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *push = xi_value_new(f, entry, XI_CALL_METHOD, &stub_void, 2);
    push->args[0] = arr;
    push->args[1] = go;
    push->aux = (void *) "push";
    push->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(latch, header);
    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *length = xi_value_new(f, header, XI_LEN, &stub_int, 1);
    length->args[0] = arr;
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, length);
    xi_block_set_if(header, cond, body, exit_blk);

    XiValue *get = xi_value_new(f, body, XI_INDEX_GET, &stub_task, 2);
    get->args[0] = arr;
    get->args[1] = &iv->value;
    XiValue *await = xi_value_new(f, body, XI_AWAIT, &stub_int, 1);
    await->args[0] = get;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    xi_block_set_jump(body, latch);

    XiValue *one = xi_const_int(f, latch, 1, &stub_int);
    XiValue *next = xi_binary(f, latch, XI_ADD, &stub_int, &iv->value, one);
    int entry_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, latch);
    assert(entry_idx >= 0 && latch_idx >= 0);
    iv->value.args[entry_idx] = zero;
    iv->value.args[latch_idx] = next;
    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    assert(chg.values_changed && "counted task array await loop should be one-shot");
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0);
    assert((go->aux_int & XI_GO_AUX_DEFER_BATCH) != 0);
    assert((await->aux_int & XI_AWAIT_AUX_SUBMIT_DEFERRED_BATCH) != 0);
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) != 0);
    xi_func_free(f);
}

TEST(mark_one_shot_sequential_await_rejects_repeated_constant_index_loop) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *latch = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = body->sealed = latch->sealed = exit_blk->sealed = true;

    XiValue *cap = xi_const_int(f, entry, 0, &stub_int);
    XiValue *arr = xi_value_new(f, entry, XI_ARRAY_NEW, &stub_task_array, 1);
    arr->args[0] = cap;
    XiValue *clear = xi_value_new(f, entry, XI_CALL_METHOD, &stub_void, 1);
    clear->args[0] = arr;
    clear->aux = (void *) "clear";
    clear->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *callee = xi_param(f, entry, 0, &stub_func);
    XiValue *go = xi_value_new(f, entry, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *push = xi_value_new(f, entry, XI_CALL_METHOD, &stub_void, 2);
    push->args[0] = arr;
    push->args[1] = go;
    push->aux = (void *) "push";
    push->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);

    xi_block_set_jump(entry, header);
    xi_block_set_jump(latch, header);
    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *length = xi_value_new(f, header, XI_LEN, &stub_int, 1);
    length->args[0] = arr;
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, length);
    xi_block_set_if(header, cond, body, exit_blk);

    XiValue *get = xi_value_new(f, body, XI_INDEX_GET, &stub_task, 2);
    get->args[0] = arr;
    get->args[1] = zero;
    XiValue *await = xi_value_new(f, body, XI_AWAIT, &stub_int, 1);
    await->args[0] = get;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    xi_block_set_jump(body, latch);

    XiValue *one = xi_const_int(f, latch, 1, &stub_int);
    XiValue *next = xi_binary(f, latch, XI_ADD, &stub_int, &iv->value, one);
    int entry_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, latch);
    assert(entry_idx >= 0 && latch_idx >= 0);
    iv->value.args[entry_idx] = zero;
    iv->value.args[latch_idx] = next;
    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 0, &stub_int));

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    assert(chg.values_changed && "constant-index loop should still get deferred submit");
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0);
    assert((go->aux_int & XI_GO_AUX_DEFER_BATCH) != 0);
    assert((await->aux_int & XI_AWAIT_AUX_SUBMIT_DEFERRED_BATCH) != 0);
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0);
    xi_func_free(f);
}

TEST(mark_one_shot_sequential_await_skips_persistent_task_array) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *cap = xi_const_int(f, blk, 1, &stub_int);
    XiValue *arr = xi_value_new(f, blk, XI_ARRAY_NEW, &stub_task_array, 1);
    arr->args[0] = cap;
    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_task, 1);
    go->args[0] = callee;
    go->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *push = xi_value_new(f, blk, XI_CALL_METHOD, &stub_void, 2);
    push->args[0] = arr;
    push->args[1] = go;
    push->aux = (void *) "push";
    push->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    XiValue *length = xi_value_new(f, blk, XI_LEN, &stub_int, 1);
    length->args[0] = arr;
    XiValue *idx = xi_const_int(f, blk, 0, &stub_int);
    XiValue *get = xi_value_new(f, blk, XI_INDEX_GET, &stub_task, 2);
    get->args[0] = arr;
    get->args[1] = idx;
    XiValue *await = xi_value_new(f, blk, XI_AWAIT, &stub_int, 1);
    await->args[0] = get;
    await->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    (void) length;
    assert(!chg.values_changed && "persistent task arrays must not be deferred blindly");
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) == 0);
    assert((go->aux_int & XI_GO_AUX_DEFER_BATCH) == 0);
    assert((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0);
    assert((await->aux_int & XI_AWAIT_AUX_SUBMIT_DEFERRED_BATCH) == 0);
    xi_func_free(f);
}

TEST(mark_fire_and_forget_result_group_go_deferred) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *group = xi_param(f, blk, 1, &stub_result_group);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_void, 2);
    go->args[0] = callee;
    go->args[1] = group;
    go->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_FIRE_AND_FORGET;

    XiValue *recv = xi_value_new(f, blk, XI_CALL_METHOD, &stub_int, 1);
    recv->args[0] = group;
    recv->aux = (void *) "recv";
    recv->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND | XI_FLAG_READS_MEM;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    (void) recv;
    assert(chg.values_changed && "ResultGroup producer go should be deferred until recv");
    assert((go->aux_int & XI_GO_AUX_DEFER_BATCH) != 0);
    assert((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) == 0);
    xi_func_free(f);
}

TEST(mark_fire_and_forget_result_group_go_keeps_linked_go_immediate) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_func);
    XiValue *group = xi_param(f, blk, 1, &stub_result_group);
    XiValue *go = xi_value_new(f, blk, XI_GO, &stub_void, 2);
    go->args[0] = callee;
    go->args[1] = group;
    go->aux_int = 1;
    go->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_FIRE_AND_FORGET;

    XiValue *recv = xi_value_new(f, blk, XI_CALL_METHOD, &stub_int, 1);
    recv->args[0] = group;
    recv->aux = (void *) "recv";
    recv->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND | XI_FLAG_READS_MEM;

    XiPassChange chg = xi_opt_mark_one_shot_await(f);

    (void) recv;
    assert(!chg.values_changed && "linked fire-and-forget go has observable propagation state");
    assert((go->aux_int & XI_GO_AUX_DEFER_BATCH) == 0);
    xi_func_free(f);
}

/* ========== DCE Tests ========== */

TEST(dce_removes_unused) {
    /* dead: c1 = 42 (unused); live: return c2 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    xi_const_int(f, blk, 42, &stub_int);               /* dead */
    XiValue *c2 = xi_const_int(f, blk, 99, &stub_int); /* live */
    xi_block_set_return(blk, c2);

    uint32_t before = blk->nvalues;
    xi_opt_dce(f);

    assert(blk->nvalues < before && "dead value should be removed");
    /* c2 should remain (used by return) */
    bool found = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == c2)
            found = true;
    }
    assert(found && "live value should remain");
    xi_func_free(f);
}

TEST(dce_keeps_side_effects) {
    /* PRINT is side-effecting, should not be removed even with 0 uses */
    XiFunc *f = make_func("test", &stub_void);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 42, &stub_int);
    XiValue *pr = xi_value_new(f, blk, XI_PRINT, &stub_void, 1);
    pr->args[0] = c1;
    pr->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(blk, NULL);

    xi_opt_dce(f);

    /* Print should survive */
    bool found = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i]->op == XI_PRINT)
            found = true;
    }
    assert(found && "side-effecting value should not be removed");
    xi_func_free(f);
}

TEST(codegen_opaque_blocks_constant_folding_and_fence_survives_dce) {
    XiFunc *f = make_func("codegen_controls", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *constant = xi_const_int(f, blk, 42, &stub_int);
    XiValue *opaque = xi_value_new(f, blk, XI_CODEGEN_OPAQUE, &stub_int, 1);
    opaque->args[0] = constant;
    XiValue *fence = xi_value_new(f, blk, XI_CODEGEN_COMPILER_FENCE, &stub_void, 0);
    xi_block_set_return(blk, opaque);

    xi_opt_const_fold(f);
    xi_opt_dce(f);

    assert(opaque->op == XI_CODEGEN_OPAQUE &&
           "constant folding must not replace opacity with its known input");
    assert(fence->op == XI_CODEGEN_COMPILER_FENCE &&
           "the scheduling barrier must survive dead-code elimination");
    xi_func_free(f);
}

TEST(dce_cascading) {
    /* a = 1; b = a + 2; (b unused) -> both removed */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    xi_binary(f, blk, XI_ADD, &stub_int, c1, c2);       /* dead */
    XiValue *c99 = xi_const_int(f, blk, 99, &stub_int); /* live */
    xi_block_set_return(blk, c99);

    xi_opt_dce(f);

    /* Only c99 should remain */
    assert(blk->nvalues == 1 && "only the live return value should remain");
    assert(blk->values[0] == c99);
    xi_func_free(f);
}

/* ========== Phi Simplification Tests ========== */

TEST(phi_simplify_trivial) {
    /* phi(a, a) -> a */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_int(f, entry, 42, &stub_int);

    /* Create merge block with 2 preds */
    XiBlock *merge = xi_block_new(f);
    xi_block_add_pred(merge, entry);
    xi_block_add_pred(merge, entry);
    merge->sealed = true;

    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 2);
    phi->value.args[0] = a;
    phi->value.args[1] = a;

    /* Create a use of phi in merge */
    XiValue *use = xi_value_new(f, merge, XI_PRINT, &stub_void, 1);
    use->args[0] = &phi->value;
    use->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(merge, NULL);

    xi_opt_phi_simplify(f);

    /* The phi should be removed and the use should reference 'a' directly */
    assert(merge->phis == NULL && "trivial phi should be removed");
    assert(use->args[0] == a && "use should reference original value");
    xi_func_free(f);
}

TEST(phi_simplify_preserves_the_phi_type_view) {
    XiFunc *f = make_func("typed_phi", &stub_void);
    XiBlock *entry = f->entry;
    XiValue *null_value = xi_const_null(f, entry, &stub_null);
    REQUIRE(null_value != NULL);

    XiBlock *merge = xi_block_new(f);
    REQUIRE(merge != NULL);
    merge->sealed = true;
    xi_block_set_jump(entry, merge);

    /* The phi is the typed join.  Its sole surviving input may have a
     * different source type after SCCP removes an edge, but users still
     * consume the phi's declared type view. */
    XiPhi *phi = xi_phi_new(f, merge, &stub_str, 1);
    REQUIRE(phi != NULL);
    phi->value.args[0] = null_value;
    XiValue *forward = xi_value_new(f, merge, XI_OWNER_FORWARD, &stub_str, 1);
    REQUIRE(forward != NULL);
    forward->args[0] = &phi->value;
    XiValue *print = xi_value_new(f, merge, XI_PRINT, &stub_void, 1);
    REQUIRE(print != NULL);
    print->args[0] = forward;
    xi_block_set_return(merge, NULL);

    char errbuf[256] = {0};
    REQUIRE(xi_verify(f, errbuf, sizeof(errbuf)));
    XiPassChange change = xi_opt_phi_simplify(f);

    REQUIRE(!change.values_changed);
    REQUIRE(merge->phis == phi);
    REQUIRE(forward->args[0] == &phi->value);
    REQUIRE(xi_verify(f, errbuf, sizeof(errbuf)));
    xi_func_free(f);
}

/* ========== Combined Pass Test ========== */

TEST(opt_run_combined) {
    /* 3 + 4 (folded to 7); dead COPY; return 7 */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c3, c4);
    /* Dead copy */
    XiValue *cp = xi_value_new(f, blk, XI_COPY, &stub_int, 1);
    cp->args[0] = add;
    /* Return add directly */
    xi_block_set_return(blk, add);

    xi_opt_run(f);

    /* add should be folded to 7 */
    assert(add->op == XI_CONST && add->aux_int == 7);
    /* copy should be removed by DCE */
    bool found_copy = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == cp)
            found_copy = true;
    }
    assert(!found_copy && "dead copy should be removed");
    xi_func_free(f);
}

/* Strength reduction tests live in test_xi_strength.c (dedicated). */

TEST(phi_simplify_non_trivial) {
    /* phi(a, b) with a != b should NOT be simplified */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_int(f, entry, 1, &stub_int);
    XiValue *b = xi_const_int(f, entry, 2, &stub_int);

    XiBlock *merge = xi_block_new(f);
    xi_block_add_pred(merge, entry);
    xi_block_add_pred(merge, entry);
    merge->sealed = true;

    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 2);
    phi->value.args[0] = a;
    phi->value.args[1] = b;

    XiValue *use = xi_value_new(f, merge, XI_PRINT, &stub_void, 1);
    use->args[0] = &phi->value;
    use->flags |= XI_FLAG_SIDE_EFFECT;

    xi_opt_phi_simplify(f);

    assert(merge->phis != NULL && "non-trivial phi should NOT be removed");
    assert(use->args[0] == &phi->value && "use should still reference phi");
    xi_func_free(f);
}

/* GVN-PRE tests live in test_xi_gvn_pre.c (dedicated). */

/* ========== Verification Tests ========== */

TEST(verify_valid_func) {
    /* A well-formed function should pass verification */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c = xi_const_int(f, blk, 42, &stub_int);
    xi_block_set_return(blk, c);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    assert(ok && "well-formed func should pass verification");
    assert(errbuf[0] == '\0');
    xi_func_free(f);
}

TEST(verify_null_type) {
    /* A value with NULL type should fail verification */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *v = xi_value_new(f, blk, XI_CONST, &stub_int, 0);
    v->type = NULL; /* intentionally break invariant */

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    assert(!ok && "NULL type should fail verification");
    assert(errbuf[0] != '\0');
    xi_func_free(f);
}

TEST(verify_phi_arg_mismatch) {
    /* Phi with wrong arg count should fail */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_const_int(f, entry, 1, &stub_int);

    XiBlock *merge = xi_block_new(f);
    xi_block_add_pred(merge, entry);
    xi_block_add_pred(merge, entry);
    merge->sealed = true;

    /* Create phi with 1 arg but block has 2 preds */
    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 1);
    phi->value.args[0] = a;
    /* Manually set nargs to 1 (should be 2) */
    phi->value.nargs = 1;

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    assert(!ok && "phi arg mismatch should fail verification");
    xi_func_free(f);
}

TEST(verify_if_block_missing_control) {
    /* IF block with NULL control should fail */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);

    /* Manually set up an IF block with NULL control */
    blk->kind = XI_BLOCK_IF;
    blk->succs[0] = then_blk;
    blk->succs[1] = else_blk;
    blk->control = NULL; /* broken! */

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    assert(!ok && "IF with NULL control should fail");
    xi_func_free(f);
}

TEST(verify_after_optimization) {
    /* Function should be valid after running all optimizations */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c3, c4);
    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    xi_binary(f, blk, XI_MUL, &stub_int, add, c0); /* dead: x * 0 = 0, unused */
    xi_block_set_return(blk, add);

    xi_opt_run(f);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "function should be valid after optimization");
    xi_func_free(f);
}

/* ========== SelectRepresentations Tests ========== */

typedef struct ArrayHofRepFixture {
    XiFunc *root;
    XiValue *closure;
    XiValue *initial;
    XiValue *hof;
    XrSemanticOperationRecord *operation;
} ArrayHofRepFixture;

static uint32_t semantic_value_for_xi(const ArrayHofRepFixture *fixture,
                                      const XiValue *value) {
    if (!fixture || !fixture->root || !fixture->root->semantic_plan || !value)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(
        fixture->root->semantic_plan,
        fixture->root->semantic_plan_function_index);
    if (!function || value->id >= function->value_count ||
        function->value_begin > UINT32_MAX - value->id)
        return XR_SEMANTIC_INDEX_NONE;
    return function->value_begin + value->id;
}

static bool build_array_hof_rep_fixture(uint8_t kind,
                                        ArrayHofRepFixture *out,
                                        char *error, size_t error_size) {
    bool reduce = kind == XI_ARRAY_HOF_REDUCE;
    bool filter = kind == XI_ARRAY_HOF_FILTER;
    XrType *callback_type = reduce ? &stub_hof_reduce_callback
                            : filter ? &stub_hof_filter_callback
                                     : &stub_hof_map_callback;
    XrType *callback_result = filter ? &stub_hof_bool : &stub_hof_int;
    if (!out || (kind != XI_ARRAY_HOF_MAP &&
                 kind != XI_ARRAY_HOF_FILTER &&
                 kind != XI_ARRAY_HOF_REDUCE))
        return false;
    memset(out, 0, sizeof(*out));

    XiFunc *root = xi_func_new("array_hof_rep", &stub_hof_int);
    XiFunc *callback = xi_func_new("array_hof_rep_callback",
                                   callback_result);
    if (!root || !callback)
        return false;
    XiBlock *entry = xi_block_new(root);
    XiBlock *callback_entry = xi_block_new(callback);
    if (!entry || !callback_entry)
        return false;

    callback->nparams = callback->min_params = reduce ? 2 : 1;
    callback->params = (XiValue **) xr_calloc(
        callback->nparams, sizeof(*callback->params));
    if (!callback->params)
        return false;
    for (uint16_t i = 0; i < callback->nparams; i++) {
        callback->params[i] =
            xi_param(callback, callback_entry, i, &stub_hof_int);
        if (!callback->params[i])
            return false;
    }
    if (filter) {
        XiValue *accepted =
            xi_const_bool(callback, callback_entry, true, &stub_hof_bool);
        if (!accepted)
            return false;
        xi_block_set_return(callback_entry, accepted);
    } else if (reduce) {
        XiValue *sum = xi_binary(callback, callback_entry, XI_ADD,
                                 &stub_hof_int, callback->params[0],
                                 callback->params[1]);
        if (!sum)
            return false;
        xi_block_set_return(callback_entry, sum);
    } else {
        xi_block_set_return(callback_entry, callback->params[0]);
    }

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    if (!root->children)
        return false;
    root->children[0] = callback;
    root->nchildren = root->children_cap = 1;
    callback->parent_func = root;

    XiValue *capacity = xi_const_int(root, entry, 4, &stub_hof_int);
    XiValue *array = xi_value_new(root, entry, XI_ARRAY_NEW,
                                  &stub_hof_int_array, 1);
    XiValue *closure =
        xi_value_new(root, entry, XI_CLOSURE_NEW, callback_type, 0);
    XiValue *initial =
        reduce ? xi_const_int(root, entry, 0, &stub_hof_int) : NULL;
    XiValue *hof = xi_value_new(root, entry, XI_CALL_METHOD,
                                reduce ? &stub_hof_int : &stub_hof_int_array,
                                reduce ? 3 : 2);
    if (!capacity || !array || !closure || !hof || (reduce && !initial))
        return false;
    array->args[0] = capacity;
    array->array_element_storage = XR_ELEM_I64;
    closure->aux = callback;
    hof->args[0] = array;
    hof->args[1] = closure;
    if (reduce)
        hof->args[2] = initial;
    hof->aux = (void *) (reduce ? "reduce" : filter ? "filter" : "map");
    hof->array_hof_kind = kind;
    hof->array_element_storage = XR_ELEM_I64;
    hof->array_result_element_storage = XR_ELEM_I64;
    if (!reduce) {
        hof->call_return_ownership = (XiReturnOwnership) {
            .kind = XI_RETURN_OWNERSHIP_OWNED,
            .param_index = -1,
            .complete = true,
        };
        XiValue *release_result =
            xi_value_new(root, entry, XI_RELEASE, &stub_void, 1);
        if (!release_result)
            return false;
        release_result->args[0] = hof;
    }
    XiValue *release_array =
        xi_value_new(root, entry, XI_RELEASE, &stub_void, 1);
    if (!release_array)
        return false;
    release_array->args[0] = array;
    if (reduce)
        xi_block_set_return(entry, hof);
    else
        xi_block_set_return(entry,
                            xi_const_int(root, entry, 0, &stub_hof_int));
    root->stage = callback->stage = XI_STAGE_OPTIMIZED;
    if (!xr_semantic_plan_build_and_attach(root, error, error_size)) {
        xi_func_free(root);
        return false;
    }

    out->root = root;
    out->closure = closure;
    out->initial = initial;
    out->hof = hof;
    uint32_t semantic_value = semantic_value_for_xi(out, hof);
    XrSemanticPlan *plan = root->semantic_plan;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].function ==
                root->semantic_plan_function_index &&
            plan->operations[i].result_value == semantic_value) {
            if (out->operation)
                return false;
            out->operation = &plan->operations[i];
        }
    }
    return out->operation &&
           out->operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_HOF;
}

static uint32_t count_rep_adapter(const XiFunc *function, XiOp op,
                                  const XiValue *source) {
    uint32_t count = 0;
    for (uint32_t bi = 0; bi < function->nblocks; bi++) {
        const XiBlock *block = function->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (value && value->op == op && value->nargs == 1 &&
                value->args && value->args[0] == source)
                count++;
        }
    }
    return count;
}

TEST(select_rep_array_hof_exact_uses_native_contract) {
    for (uint8_t kind = XI_ARRAY_HOF_MAP;
         kind <= XI_ARRAY_HOF_REDUCE; kind++) {
        ArrayHofRepFixture fixture = {0};
        char error[512] = {0};
        bool built = build_array_hof_rep_fixture(kind, &fixture, error,
                                                 sizeof(error));
        if (!built)
            printf("  Array HOF fixture failed: %s\n", error);
        REQUIRE(built);
        XiRepPolicy policy = xi_rep_policy_tagged_boundary();
        xi_opt_select_rep_with_policy(fixture.root, &policy);

        REQUIRE(fixture.closure->rep == XR_REP_TAGGED);
        REQUIRE(fixture.hof->args[1] == fixture.closure);
        REQUIRE(count_rep_adapter(fixture.root, XI_UNBOX,
                                  fixture.closure) == 0);
        if (kind == XI_ARRAY_HOF_REDUCE) {
            REQUIRE(fixture.hof->rep == XR_REP_I64);
            REQUIRE(fixture.initial->rep == XR_REP_I64);
            REQUIRE(fixture.hof->args[2] == fixture.initial);
            REQUIRE(count_rep_adapter(fixture.root, XI_BOX,
                                      fixture.initial) == 0);
        } else {
            REQUIRE(fixture.hof->rep == XR_REP_TAGGED);
            REQUIRE(count_rep_adapter(fixture.root, XI_BOX,
                                      fixture.hof) == 0);
        }
        xi_func_free(fixture.root);
    }
}

typedef enum ArrayHofRepMutation {
    ARRAY_HOF_REP_MUTATE_SEMANTIC_KIND,
    ARRAY_HOF_REP_MUTATE_SEMANTIC_RESULT,
    ARRAY_HOF_REP_MUTATE_SEMANTIC_OPERAND,
    ARRAY_HOF_REP_MUTATE_XI_KIND,
    ARRAY_HOF_REP_MUTATION_COUNT,
} ArrayHofRepMutation;

TEST(select_rep_array_hof_mutation_cannot_authorize_native_seed) {
    for (uint8_t mutation = 0; mutation < ARRAY_HOF_REP_MUTATION_COUNT;
         mutation++) {
        ArrayHofRepFixture fixture = {0};
        char error[512] = {0};
        bool built = build_array_hof_rep_fixture(
            XI_ARRAY_HOF_REDUCE, &fixture, error, sizeof(error));
        if (!built)
            printf("  Array HOF mutation fixture failed: %s\n", error);
        REQUIRE(built);
        XrSemanticPlan *plan = fixture.root->semantic_plan;
        XrSemanticOperationRecord *operation = fixture.operation;
        if (mutation == ARRAY_HOF_REP_MUTATE_SEMANTIC_KIND) {
            operation->array_hof_kind = XR_SEM_ARRAY_HOF_MAP;
            REQUIRE(!xr_semantic_plan_verify(plan, error, sizeof(error)));
        } else if (mutation == ARRAY_HOF_REP_MUTATE_SEMANTIC_RESULT) {
            operation->result_value =
                semantic_value_for_xi(&fixture, fixture.initial);
            REQUIRE(!xr_semantic_plan_verify(plan, error, sizeof(error)));
        } else if (mutation == ARRAY_HOF_REP_MUTATE_SEMANTIC_OPERAND) {
            plan->operands[operation->operand_begin + 1u].value =
                plan->operands[operation->operand_begin].value;
            REQUIRE(!xr_semantic_plan_verify(plan, error, sizeof(error)));
        } else {
            fixture.hof->array_hof_kind = XI_ARRAY_HOF_MAP;
            XrSemanticPlan *rebuilt = NULL;
            REQUIRE(!xr_semantic_plan_build(fixture.root, &rebuilt, error,
                                            sizeof(error)));
            REQUIRE(rebuilt == NULL);
        }

        XiRepPolicy policy = xi_rep_policy_tagged_boundary();
        xi_opt_select_rep_with_policy(fixture.root, &policy);
        REQUIRE(fixture.hof->args[2] != fixture.initial);
        REQUIRE(fixture.hof->args[2]->op == XI_BOX);
        REQUIRE(fixture.hof->args[2]->args[0] == fixture.initial);
        REQUIRE(count_rep_adapter(fixture.root, XI_BOX,
                                  fixture.initial) == 1);
        xi_func_free(fixture.root);
    }
}

TEST(select_rep_box_const_for_return) {
    /* int constant returned: const(I64) -> return(TAGGED) needs BOX */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c42 = xi_const_int(f, blk, 42, &stub_int);
    xi_block_set_return(blk, c42);

    xi_opt_select_rep(f);

    /* Return control should now be a BOX wrapping c42 */
    assert(blk->control != c42 && "return should wrap const in BOX");
    assert(blk->control->op == XI_BOX && "wrapper should be XI_BOX");
    assert(blk->control->args[0] == c42 && "BOX arg should be the constant");
    xi_func_free(f);
}

TEST(select_rep_unbox_param_for_arith) {
    /* Typed int param gets I64 rep directly (no UNBOX needed for ADD).
     * Return value still needs BOX (I64 → TAGGED for caller). */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *p0 = xi_param(f, blk, 0, &stub_int);
    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, p0, c1);
    xi_block_set_return(blk, add);

    xi_opt_select_rep(f);

    /* Typed int param is already I64: ADD uses it directly */
    assert(add->args[0] == p0 && "typed param used directly by ADD");
    assert(p0->rep == XR_REP_I64 && "int param should have I64 rep");
    /* ADD result is I64, return needs TAGGED: should have BOX */
    assert(blk->control->op == XI_BOX && "return should BOX the ADD result");
    xi_func_free(f);
}

TEST(select_rep_no_change_for_call) {
    /* CALL with TAGGED-rep params: no BOX/UNBOX needed */
    XiFunc *f = make_func("test", &stub_func);
    XiBlock *blk = f->entry;

    XiValue *p0 = xi_param(f, blk, 0, &stub_func);
    XiValue *call = xi_value_new(f, blk, XI_CALL, &stub_func, 2);
    call->args[0] = p0; /* callee */
    call->args[1] = p0; /* arg */
    call->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(blk, call);

    uint32_t nv_before = blk->nvalues;
    xi_opt_select_rep(f);

    /* No conversions should be inserted (all TAGGED -> TAGGED) */
    assert(blk->nvalues == nv_before && "no BOX/UNBOX for all-tagged path");
    xi_func_free(f);
}

TEST(select_rep_arith_chain_stays_unboxed) {
    /* a + b + c: all int arithmetic stays I64, only final return needs BOX */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *add1 = xi_binary(f, blk, XI_ADD, &stub_int, c1, c2);
    XiValue *add2 = xi_binary(f, blk, XI_ADD, &stub_int, add1, c3);
    xi_block_set_return(blk, add2);

    xi_opt_select_rep(f);

    /* Intermediate arithmetic: no conversions (I64 -> I64) */
    assert(add1->args[0] == c1 && "const->add should stay direct");
    assert(add1->args[1] == c2 && "const->add should stay direct");
    assert(add2->args[0] == add1 && "add->add chain should stay unboxed");
    /* Only return needs BOX */
    assert(blk->control->op == XI_BOX && "return needs BOX");
    assert(blk->control->args[0] == add2 && "BOX wraps final add");
    xi_func_free(f);
}

TEST(select_rep_keeps_narrow_store_for_shared_typed_array) {
    XiFunc *f = make_func("test", &stub_void);
    XiBlock *blk = f->entry;

    XiValue *arr = xi_value_new(f, blk, XI_GET_SHARED, &stub_u8_array, 0);
    arr->aux_int = 0;
    XiValue *idx = xi_const_int(f, blk, 0, &stub_int);
    XiValue *val = xi_const_int(f, blk, 300, &stub_int);
    XiValue *narrow = xi_value_new(f, blk, XI_NARROW_U8, &stub_int, 1);
    narrow->args[0] = val;

    XiValue *store = xi_value_new(f, blk, XI_INDEX_SET, &stub_void, 3);
    store->args[0] = arr;
    store->args[1] = idx;
    store->args[2] = narrow;
    xi_block_set_return(blk, NULL);

    XiRepPolicy policy = xi_rep_policy_aot_transition();
    xi_opt_select_rep_with_policy(f, &policy);

    assert(store->args[2] == narrow && "typed array store must consume NARROW_U8 directly");
    assert(narrow->rep == XR_REP_I64 && "NARROW_U8 stays in an integer register");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "shared typed array store should verify after select_rep");
    xi_func_free(f);
}

TEST(select_rep_native_policy_keeps_return_unboxed) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *c42 = xi_const_int(f, blk, 42, &stub_int);
    xi_block_set_return(blk, c42);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    xi_opt_select_rep_with_policy(f, &policy);

    assert(blk->control == c42 && "native return policy should not BOX scalar return");
    assert(c42->rep == XR_REP_I64 && "int return should stay I64");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "native scalar return rep should verify");
    xi_func_free(f);
}

TEST(select_rep_aot_policy_keeps_scalar_phi_unboxed) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);

    XiValue *cond = xi_const_bool(f, entry, true, &stub_bool);
    xi_block_set_if(entry, cond, then_blk, else_blk);

    XiValue *c1 = xi_const_int(f, then_blk, 1, &stub_int);
    xi_block_set_jump(then_blk, merge);

    XiValue *c2 = xi_const_int(f, else_blk, 2, &stub_int);
    xi_block_set_jump(else_blk, merge);

    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 2);
    phi->value.args[0] = c1;
    phi->value.args[1] = c2;

    XiValue *c3 = xi_const_int(f, merge, 3, &stub_int);
    XiValue *add = xi_binary(f, merge, XI_ADD, &stub_int, &phi->value, c3);
    xi_block_set_return(merge, add);

    XiRepPolicy policy = xi_rep_policy_aot_transition();
    xi_opt_select_rep_with_policy(f, &policy);

    assert(phi->value.rep == XR_REP_I64 && "AOT transition policy should keep int phi I64");
    assert(phi->value.args[0] == c1 && "I64 phi arg should not be boxed");
    assert(phi->value.args[1] == c2 && "I64 phi arg should not be boxed");
    assert(add->args[0] == &phi->value && "arithmetic should consume native phi directly");
    assert(merge->nvalues == 3 && "only return BOX should be inserted in merge block");
    assert(merge->values[2]->op == XI_BOX && "AOT transition still boxes return for old cgen ABI");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "native scalar phi rep should verify");
    xi_func_free(f);
}

TEST(select_rep_advances_empty_func_tree) {
    XiFunc *root = make_func("empty_facade_init", &stub_void);
    XiFunc *child = make_func("empty_nested", &stub_void);
    xi_block_set_return(root->entry, NULL);
    xi_block_set_return(child->entry, NULL);
    child->parent_func = root;
    root->children = (XiFunc **) xr_malloc(sizeof(XiFunc *));
    assert(root->children != NULL);
    root->children[0] = child;
    root->nchildren = 1;
    root->children_cap = 1;

    assert(root->next_value_id == 0);
    assert(child->next_value_id == 0);

    char error[512] = {0};
    XiRawProgram *raw = xi_stage_adopt_raw(root, error, sizeof(error));
    assert(raw != NULL);
    XiCanonicalProgram *canonical = xi_program_canonicalize(raw, error, sizeof(error));
    assert(canonical != NULL);
    xi_pass_close(root);
    XiClosedProgram *closed = xi_program_close(canonical, error, sizeof(error));
    assert(closed != NULL);
    XiOwnedProgram *owned = xi_program_make_owned(closed, error, sizeof(error));
    assert(owned != NULL);
    XiSemanticLoweredProgram *semantic_lowered =
        xi_program_lower_semantics(owned, error, sizeof(error));
    assert(semantic_lowered != NULL);
    XiCoroLoweredProgram *coro =
        xi_program_lower_coroutines(semantic_lowered, NULL, error, sizeof(error));
    assert(coro != NULL);
    XiOptimizedProgram *optimized = xi_program_finish_optimization(coro, error, sizeof(error));
    assert(optimized != NULL);
    assert(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XiSemanticPlannedProgram *semantic =
        xi_program_freeze_semantics(optimized, error, sizeof(error));
    assert(semantic != NULL);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XiPassChange change = xi_opt_select_rep_with_policy(root, &policy);
    XiReppedProgram *repped = xi_program_select_reps(semantic, error, sizeof(error));
    assert(repped != NULL);

    assert(change.cfg_changed && change.values_changed && change.types_changed &&
           "empty pipeline root must record its stage transition");
    assert(root->stage == XI_STAGE_REPPED);
    assert(child->stage == XI_STAGE_REPPED);
    assert((root->invariant_mask & xi_stage_invariants(XI_STAGE_REPPED)) ==
           xi_stage_invariants(XI_STAGE_REPPED));
    assert((child->invariant_mask & xi_stage_invariants(XI_STAGE_REPPED)) ==
           xi_stage_invariants(XI_STAGE_REPPED));

    xi_func_free(xi_repped_program_release(repped));
}

TEST(full_pipeline_preserves_frozen_coroutine_plan) {
    XiFunc *f = make_func("coro_opt_rebase", &stub_int);
    XiValue *one = xi_const_int(f, f->entry, 1, &stub_int);
    XiValue *two = xi_const_int(f, f->entry, 2, &stub_int);
    XiValue *sum = xi_value_new(f, f->entry, XI_ADD, &stub_int, 2);
    XiValue *yield = xi_value_new(f, f->entry, XI_YIELD, &stub_void, 0);
    assert(one && two && sum && yield);
    sum->args[0] = one;
    sum->args[1] = two;
    xi_block_set_return(f->entry, sum);
    f->stage = XI_STAGE_SEMANTIC_LOWERED;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    assert(xi_coro_lower(f, NULL));
    f->stage = XI_STAGE_CORO_LOWERED;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_CORO_LOWERED);
    f->lowering_facts.initialized = true;
    f->lowering_facts.semantic_ops_lowered = true;
    f->lowering_facts.coroutine_required = true;
    f->lowering_facts.coroutine_lowered = true;
    f->lowering_facts.callable_lowered = true;

    uint64_t old_revision = f->ir_revision;
    XiOptResult result = xi_opt_run_pipeline(f, XI_OPT_FULL);
    if (!result.ok)
        fprintf(stderr, "coroutine optimizer failure: %s\n", result.detail);
    assert(result.ok);
    assert(f->ir_revision > old_revision);
    assert(f->coro_plan && xi_coro_plan_is_current(f, f->coro_plan));
    char error[256] = {0};
    assert(xi_verify_stage(f, XI_STAGE_CORO_LOWERED, error, sizeof(error)));
    xi_func_free(f);
}

TEST(non_coroutine_plan_keeps_full_optimizer_pipeline) {
    XiFunc *f = make_func("sync_opt_rebase", &stub_int);
    XiValue *dead = xi_const_int(f, f->entry, 41, &stub_int);
    XiValue *result = xi_const_int(f, f->entry, 42, &stub_int);
    assert(dead && result);
    xi_block_set_return(f->entry, result);
    f->stage = XI_STAGE_SEMANTIC_LOWERED;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    assert(xi_coro_lower(f, NULL));
    assert(f->coro_plan && !f->coro_plan->is_coroutine);
    f->stage = XI_STAGE_CORO_LOWERED;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_CORO_LOWERED);
    f->lowering_facts.initialized = true;
    f->lowering_facts.semantic_ops_lowered = true;
    f->lowering_facts.coroutine_lowered = true;
    f->lowering_facts.callable_lowered = true;

    uint32_t values_before = f->entry->nvalues;
    XiOptResult optimized = xi_opt_run_pipeline(f, XI_OPT_LIGHT);
    if (!optimized.ok)
        fprintf(stderr, "synchronous optimizer failure: %s\n", optimized.detail);
    assert(optimized.ok);
    assert(f->entry->nvalues < values_before && "DCE must not be disabled by a synchronous plan");
    assert(f->coro_plan && xi_coro_plan_is_current(f, f->coro_plan));
    xi_func_free(f);
}

TEST(the_none_level_runs_no_pass_and_reports_no_statistics) {
    XiFunc *f = make_func("none_level", &stub_int);
    XiValue *dead = xi_const_int(f, f->entry, 41, &stub_int);
    XiValue *result = xi_const_int(f, f->entry, 42, &stub_int);
    REQUIRE(dead && result);
    xi_block_set_return(f->entry, result);

    uint32_t values_before = f->entry->nvalues;
    XiPipelineStats stats;
    /* Poisoned on purpose: the driver reports statistics for a run in which
     * nothing ran, so it has to clear the buffer before it returns. */
    memset(&stats, 0xAB, sizeof(stats));

    XiOptResult opt = xi_opt_run_pipeline_ex(f, XI_OPT_NONE, &stats, 0);
    REQUIRE(opt.ok);
    /* The dead constant survives. At the light level DCE removes it, which is
     * what makes this an assertion about the level rather than about the
     * function being unoptimizable. */
    REQUIRE(f->entry->nvalues == values_before);
    REQUIRE(!opt.change.values_changed);
    REQUIRE(!opt.change.cfg_changed);
    REQUIRE(stats.npass == 0);
    REQUIRE(stats.total_rounds == 0);
    REQUIRE(stats.total_ns == 0);

    XiOptResult light = xi_opt_run_pipeline_ex(f, XI_OPT_LIGHT, &stats, 0);
    REQUIRE(light.ok);
    REQUIRE(f->entry->nvalues < values_before);
    REQUIRE(stats.npass > 0);

    xi_func_free(f);
}

static const XiPassStats *stats_for(const XiPipelineStats *stats, const char *name) {
    for (uint32_t i = 0; i < stats->npass; i++) {
        if (strcmp(stats->passes[i].name, name) == 0)
            return &stats->passes[i];
    }
    return NULL;
}

TEST(statistics_say_which_passes_do_not_count_values) {
    XiFunc *f = make_func("counts_declared", &stub_int);
    XiValue *dead = xi_const_int(f, f->entry, 41, &stub_int);
    XiValue *result = xi_const_int(f, f->entry, 42, &stub_int);
    REQUIRE(dead && result);
    xi_block_set_return(f->entry, result);

    XiPipelineStats stats;
    XiOptResult opt = xi_opt_run_pipeline_ex(f, XI_OPT_FULL, &stats, 0);
    REQUIRE(opt.ok);

    /* DCE counts what it removes, so its zero would be a measurement. */
    const XiPassStats *dce = stats_for(&stats, "dce");
    REQUIRE(dce && dce->invocations > 0);
    REQUIRE(dce->counts_reported);

    /* These rewrite the function without the counters moving. Reporting their
     * zero as a count is what let a reader conclude they had not fired. */
    static const char *const uncounted[] = {"constfold", "strength_reduce", "copy_prop",
                                            "licm",      "loop_split",      "inline",
                                            "tail_call", "ifconv"};
    for (size_t i = 0; i < sizeof(uncounted) / sizeof(uncounted[0]); i++) {
        const XiPassStats *ps = stats_for(&stats, uncounted[i]);
        REQUIRE(ps && ps->invocations > 0);
        REQUIRE(!ps->counts_reported);
        /* The driver refuses a declaration that has gone stale, so a pass
         * marked this way cannot be carrying real numbers. */
        REQUIRE(ps->n_removed == 0 && ps->n_added == 0);
    }

    xi_func_free(f);
}

/* ========== Tuple Projection Peephole Tests ========== */

/* TUPLE_GET(TUPLE_NEW(a, b), 0) collapses to COPY(a). */
TEST(tuple_get_of_tuple_new_first) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *e0 = xi_const_int(f, blk, 7, &stub_int);
    XiValue *e1 = xi_const_int(f, blk, 9, &stub_int);
    XiValue *tup = xi_value_new(f, blk, XI_TUPLE_NEW, &stub_int, 2);
    tup->args[0] = e0;
    tup->args[1] = e1;
    tup->aux_int = 2;

    XiValue *get = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get->args[0] = tup;
    get->aux_int = 0;
    xi_block_set_return(blk, get);

    xi_opt_const_fold(f);

    assert(get->op == XI_COPY && "TUPLE_GET(TUPLE_NEW, 0) should collapse to COPY");
    assert(get->args[0] == e0 && "COPY should forward to the original element");
    assert(get->aux_int == 0 && "COPY should clear the slot index");
    xi_func_free(f);
}

/* TUPLE_GET(TUPLE_NEW(a, b), 1) collapses to COPY(b). */
TEST(tuple_get_of_tuple_new_second) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *e0 = xi_const_int(f, blk, 11, &stub_int);
    XiValue *e1 = xi_const_int(f, blk, 13, &stub_int);
    XiValue *tup = xi_value_new(f, blk, XI_TUPLE_NEW, &stub_int, 2);
    tup->args[0] = e0;
    tup->args[1] = e1;
    tup->aux_int = 2;

    XiValue *get = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get->args[0] = tup;
    get->aux_int = 1;
    xi_block_set_return(blk, get);

    xi_opt_const_fold(f);

    assert(get->op == XI_COPY && "TUPLE_GET(TUPLE_NEW, 1) should collapse to COPY");
    assert(get->args[0] == e1 && "COPY should forward to element[1]");
    xi_func_free(f);
}

/* TUPLE_GET on a tuple that did NOT come from TUPLE_NEW must stay intact —
 * we cannot synthesise the source slot otherwise. */
TEST(tuple_get_unrelated_source_keeps_op) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *p0 = xi_param(f, blk, 0, &stub_int);
    XiValue *get = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get->args[0] = p0;
    get->aux_int = 0;
    xi_block_set_return(blk, get);

    xi_opt_const_fold(f);

    assert(get->op == XI_TUPLE_GET && "TUPLE_GET on non-NEW source must remain");
    assert(get->args[0] == p0 && "source should be untouched");
    xi_func_free(f);
}

/* DCE should reap the TUPLE_NEW once every TUPLE_GET has been collapsed
 * to copies that no longer reference it. */
TEST(tuple_new_eliminated_after_full_projection) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *e0 = xi_const_int(f, blk, 100, &stub_int);
    XiValue *e1 = xi_const_int(f, blk, 200, &stub_int);
    XiValue *tup = xi_value_new(f, blk, XI_TUPLE_NEW, &stub_int, 2);
    tup->args[0] = e0;
    tup->args[1] = e1;
    tup->aux_int = 2;

    XiValue *get0 = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get0->args[0] = tup;
    get0->aux_int = 0;
    XiValue *get1 = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get1->args[0] = tup;
    get1->aux_int = 1;
    XiValue *sum = xi_binary(f, blk, XI_ADD, &stub_int, get0, get1);
    xi_block_set_return(blk, sum);

    /* Run the standard pipeline so peephole + copy_prop + dce all apply. */
    xi_opt_run(f);

    bool tuple_new_alive = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] && blk->values[i]->op == XI_TUPLE_NEW) {
            tuple_new_alive = true;
            break;
        }
    }
    assert(!tuple_new_alive && "TUPLE_NEW should be DCE'd once all GETs project away");
    xi_func_free(f);
}

TEST(tuple_get_fold_clears_tbaa_metadata) {
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *e0 = xi_const_int(f, blk, 100, &stub_int);
    XiValue *tup = xi_value_new(f, blk, XI_TUPLE_NEW, &stub_int, 1);
    tup->args[0] = e0;
    tup->aux_int = 1;

    XiValue *get = xi_value_new(f, blk, XI_TUPLE_GET, &stub_int, 1);
    get->args[0] = tup;
    get->aux_int = 0;
    xi_block_set_return(blk, get);

    xi_tbaa_annotate(f);
    assert(get->mem_group == XI_MEM_TUPLE && "TUPLE_GET should be annotated before folding");

    xi_opt_const_fold(f);

    assert(get->op == XI_COPY && "TUPLE_GET(TUPLE_NEW, 0) should collapse to COPY");
    assert(get->mem_group == XI_MEM_NONE && "COPY must not keep TUPLE_GET memory group");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "folded COPY should satisfy TBAA verifier");
    xi_func_free(f);
}

/* ========== BOX/UNBOX Peephole Tests ========== */

TEST(box_elim_unbox_of_box) {
    /* UNBOX(BOX(x)) -> COPY(x) */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *box = xi_value_new(f, blk, XI_BOX, &stub_int, 1);
    box->args[0] = x;
    XiValue *unbox = xi_value_new(f, blk, XI_UNBOX, &stub_int, 1);
    unbox->args[0] = box;
    xi_block_set_return(blk, unbox);

    xi_opt_box_elim(f);

    assert(unbox->op == XI_COPY && "UNBOX(BOX(x)) should become COPY");
    assert(unbox->args[0] == x && "COPY should reference original x");
    xi_func_free(f);
}

TEST(box_elim_box_of_unbox) {
    /* BOX(UNBOX(x)) -> COPY(x) */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *unbox = xi_value_new(f, blk, XI_UNBOX, &stub_int, 1);
    unbox->args[0] = x;
    XiValue *box = xi_value_new(f, blk, XI_BOX, &stub_int, 1);
    box->args[0] = unbox;
    xi_block_set_return(blk, box);

    xi_opt_box_elim(f);

    assert(box->op == XI_COPY && "BOX(UNBOX(x)) should become COPY");
    assert(box->args[0] == x && "COPY should reference original x");
    xi_func_free(f);
}

TEST(box_elim_preserves_frozen_semantic_operation_identity) {
    XiFunc *f = make_func("frozen_box_unbox", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *box = xi_value_new(f, blk, XI_BOX, &stub_int, 1);
    XiValue *unbox = xi_value_new(f, blk, XI_UNBOX, &stub_int, 1);
    assert(x && box && unbox);
    box->args[0] = x;
    unbox->args[0] = box;
    xi_block_set_return(blk, unbox);
    f->stage = XI_STAGE_OPTIMIZED;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_OPTIMIZED);
    char error[256] = {0};
    assert(xr_semantic_plan_build_and_attach(f, error, sizeof(error)));

    xi_opt_box_elim(f);

    assert(unbox->op == XI_UNBOX &&
           "late representation cleanup must not rewrite a frozen semantic opcode");
    assert(unbox->args[0] == box &&
           "late representation cleanup must preserve the frozen operand identity");
    xi_func_free(f);
}

TEST(box_elim_preserves_backend_adapter_over_frozen_semantic_identity) {
    XiFunc *f = make_func("frozen_box_backend_unbox", &stub_int);
    XiBlock *blk = f->entry;
    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *box = xi_value_new(f, blk, XI_BOX, &stub_int, 1);
    assert(x && box);
    box->args[0] = x;
    xi_block_set_return(blk, box);
    f->stage = XI_STAGE_OPTIMIZED;
    f->invariant_mask = xi_stage_invariants(XI_STAGE_OPTIMIZED);
    char error[256] = {0};
    assert(xr_semantic_plan_build_and_attach(f, error, sizeof(error)));

    XiValue *unbox = xi_value_new(f, blk, XI_UNBOX, &stub_int, 1);
    assert(unbox);
    unbox->args[0] = box;
    unbox->backend_origin = XI_BACKEND_VALUE_REP_UNBOX;
    xi_block_set_return(blk, unbox);

    xi_opt_box_elim(f);

    assert(unbox->op == XI_UNBOX &&
           "late cleanup must preserve an adapter over a frozen semantic opcode");
    assert(unbox->args[0] == box &&
           "the adapter must retain its exact frozen semantic source");
    assert(unbox->backend_origin == XI_BACKEND_VALUE_REP_UNBOX &&
           "the preserved adapter must retain exact backend provenance");
    xi_func_free(f);
}

TEST(box_elim_no_false_positive) {
    /* BOX(x) where x is not UNBOX: should NOT be eliminated */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *blk = f->entry;

    XiValue *x = xi_param(f, blk, 0, &stub_int);
    XiValue *box = xi_value_new(f, blk, XI_BOX, &stub_int, 1);
    box->args[0] = x;
    xi_block_set_return(blk, box);

    xi_opt_box_elim(f);

    assert(box->op == XI_BOX && "BOX(param) should NOT be eliminated");
    xi_func_free(f);
}

/* ========== Block Simplify Tests ========== */

TEST(block_simplify_single_pred_empty_block) {
    /* entry --> empty --> exit
     * 'empty' is PLAIN, has no values and one pred, one succ → it can
     * be eliminated and the function can collapse into a single block. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiBlock *empty = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    empty->sealed = true;
    exit_blk->sealed = true;

    xi_block_set_jump(entry, empty);
    xi_block_set_jump(empty, exit_blk);
    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 7, &stub_int));

    XiPassChange chg = xi_opt_block_simplify(f);

    assert(chg.cfg_changed && "block_simplify should report a CFG change");
    assert(f->nblocks <= 2 && "single-pred chain should collapse");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR should remain valid after block_simplify");
    xi_func_free(f);
}

TEST(block_simplify_keeps_ir_valid_for_multi_pred_empty) {
    /* Two branches funnel through a shared empty PLAIN block before
     * reaching the join. block_simplify must not corrupt the CFG —
     * either it leaves the empty block in place or it correctly
     * stitches every predecessor through. The hard contract is: the
     * IR must remain a valid SSA program. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *cond = xi_param(f, entry, 0, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *funnel = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    funnel->sealed = true;
    exit_blk->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    xi_block_set_jump(then_blk, funnel);
    xi_block_set_jump(else_blk, funnel);
    xi_block_set_jump(funnel, exit_blk);
    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 9, &stub_int));

    (void) xi_opt_block_simplify(f);

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "block_simplify must preserve SSA validity on multi-pred empty");
    xi_func_free(f);
}

TEST(block_simplify_preserves_phi_when_merging) {
    /* exit block has a phi over its two preds. block_simplify must not
     * desync npreds and phi.nargs while operating on the shape. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *cond = xi_param(f, entry, 0, &stub_bool);

    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    exit_blk->sealed = true;

    xi_block_set_if(entry, cond, then_blk, else_blk);
    XiValue *t = xi_const_int(f, then_blk, 1, &stub_int);
    XiValue *e = xi_const_int(f, else_blk, 2, &stub_int);
    xi_block_set_jump(then_blk, exit_blk);
    xi_block_set_jump(else_blk, exit_blk);

    XiPhi *phi = xi_phi_new(f, exit_blk, &stub_int, 2);
    phi->value.args[0] = t;
    phi->value.args[1] = e;
    xi_block_set_return(exit_blk, &phi->value);

    (void) xi_opt_block_simplify(f);

    /* Whichever shape survives, all phis in surviving blocks must agree
     * with their block's npreds. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (XiPhi *p = blk->phis; p; p = p->next) {
            assert(p->value.nargs == blk->npreds &&
                   "phi.nargs must match block.npreds after block_simplify");
        }
    }

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok);
    xi_func_free(f);
}

TEST(block_simplify_merges_past_arena_value_capacity) {
    /* Block value arrays belong to the Xi arena. Merging a successor into a
     * full predecessor must grow by arena-copying, not by heap realloc. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *exit_blk = xi_block_new(f);
    exit_blk->sealed = true;

    uint32_t initial_cap = entry->values_cap;
    for (uint32_t i = 0; i < initial_cap; i++)
        (void) xi_const_int(f, entry, (int64_t) i, &stub_int);

    XiValue *ret = xi_const_int(f, exit_blk, 123, &stub_int);
    xi_block_set_jump(entry, exit_blk);
    xi_block_set_return(exit_blk, ret);

    XiPassChange chg = xi_opt_block_simplify(f);

    assert(chg.cfg_changed && "merge should report a CFG change");
    assert(f->nblocks == 1 && "linear return chain should merge into entry");
    assert(entry->nvalues == initial_cap + 1 && "merged value should be appended");
    assert(entry->values[initial_cap] == ret && "merged value should retain order");
    assert(ret->block == entry && "merged value should be rebound to predecessor");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR should remain valid after growing during block merge");
    xi_func_free(f);
}

TEST(block_simplify_redirects_try_handler_predecessor) {
    /* XI_TRY has an implicit exceptional edge to handler. Merging its block
     * into the predecessor must redirect handler->preds as well as the normal
     * successor edge, or RPO retains a predecessor that compact_blocks removed. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *try_blk = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    XiBlock *handler = xi_block_new(f);
    XiBlock *implicit_handler = xi_block_new(f);
    try_blk->sealed = exit_blk->sealed = handler->sealed = implicit_handler->sealed = true;

    xi_block_set_jump(entry, try_blk);
    XiValue *try_op = xi_value_new(f, try_blk, XI_TRY, &stub_void, 0);
    try_op->aux = handler;
    try_op->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_add_pred(handler, try_blk);
    xi_block_set_jump(try_blk, exit_blk);
    xi_block_set_return(exit_blk, xi_const_int(f, exit_blk, 7, &stub_int));

    XiValue *caught = xi_value_new(f, handler, XI_CATCH, &stub_int, 0);
    caught->aux = try_op;
    caught->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_return(handler, caught);

    /* Some exceptional paths are represented only by the target's pred side. */
    xi_block_add_pred(implicit_handler, try_blk);
    xi_block_set_return(implicit_handler, xi_const_int(f, implicit_handler, 9, &stub_int));

    XiPassChange chg = xi_opt_block_simplify(f);

    assert(chg.cfg_changed && "try block should merge into entry");
    assert(try_op->block == entry && "XI_TRY should move to the surviving block");
    assert(handler->npreds == 1 && handler->preds[0] == entry &&
           "handler predecessor must follow the moved XI_TRY");
    assert(implicit_handler->npreds == 1 && implicit_handler->preds[0] == entry &&
           "pred-only exceptional edges must follow the merged block");
    assert(xi_compute_rpo(f) == 3 && "normal and both exceptional blocks should be reachable");
    xi_compute_dominators(f);
    assert(handler->idom == entry && "exceptional handler should be dominated by try owner");
    assert(implicit_handler->idom == entry &&
           "pred-only exceptional handler should be dominated by try owner");

    xi_func_free(f);
}

/* ========== Jump Thread Tests ========== */

TEST(jump_thread_basic_redirect) {
    /* entry tests cmp; both branches funnel into 'merge' which tests
     * the same cmp again. Both incoming edges should be threaded to
     * the matching successor of merge, leaving merge unreachable. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *cmp = xi_binary(f, entry, XI_EQ, &stub_bool, x, zero);

    XiBlock *a_blk = xi_block_new(f);
    XiBlock *b_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    XiBlock *t_blk = xi_block_new(f);
    XiBlock *f_blk = xi_block_new(f);
    a_blk->sealed = true;
    b_blk->sealed = true;
    merge->sealed = true;
    t_blk->sealed = true;
    f_blk->sealed = true;

    xi_block_set_if(entry, cmp, a_blk, b_blk);
    xi_block_set_jump(a_blk, merge);
    xi_block_set_jump(b_blk, merge);
    xi_block_set_if(merge, cmp, t_blk, f_blk);
    xi_block_set_return(t_blk, xi_const_int(f, t_blk, 100, &stub_int));
    xi_block_set_return(f_blk, xi_const_int(f, f_blk, 200, &stub_int));

    XiPassChange chg = xi_opt_jump_thread(f);

    assert(chg.cfg_changed && "jump_thread should redirect both edges");
    assert(a_blk->succs[0] == t_blk && "a_blk should now jump straight to t_blk");
    assert(b_blk->succs[0] == f_blk && "b_blk should now jump straight to f_blk");

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR must remain valid after jump_thread");
    xi_func_free(f);
}

TEST(jump_thread_keeps_ir_valid_when_dest_has_phi) {
    /* Same threading shape, but t_blk holds a phi anchored to merge.
     * Naive redirect would attach a fresh predecessor to t_blk without
     * extending the phi argument list, breaking npreds == phi.nargs.
     * The pass must either correctly extend the phi or refuse to
     * thread; either way the verifier must pass. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *x = xi_param(f, entry, 0, &stub_int);
    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *cmp = xi_binary(f, entry, XI_EQ, &stub_bool, x, zero);

    XiBlock *a_blk = xi_block_new(f);
    XiBlock *b_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    XiBlock *t_blk = xi_block_new(f);
    XiBlock *f_blk = xi_block_new(f);
    a_blk->sealed = true;
    b_blk->sealed = true;
    merge->sealed = true;
    t_blk->sealed = true;
    f_blk->sealed = true;

    xi_block_set_if(entry, cmp, a_blk, b_blk);
    xi_block_set_jump(a_blk, merge);
    xi_block_set_jump(b_blk, merge);
    XiValue *m_val = xi_const_int(f, merge, 99, &stub_int);
    xi_block_set_if(merge, cmp, t_blk, f_blk);

    XiPhi *phi = xi_phi_new(f, t_blk, &stub_int, 1);
    phi->value.args[0] = m_val;
    xi_block_set_return(t_blk, &phi->value);
    xi_block_set_return(f_blk, xi_const_int(f, f_blk, 200, &stub_int));

    (void) xi_opt_jump_thread(f);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (XiPhi *p = blk->phis; p; p = p->next) {
            assert(p->value.nargs == blk->npreds &&
                   "phi.nargs must match block.npreds after jump_thread");
        }
    }

    char errbuf[256] = {0};
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok)
        printf("  verify error: %s\n", errbuf);
    assert(ok && "IR must remain valid after jump_thread on a phi-bearing dest");
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Opt Unit Tests ===\n\n");

    (void) stub_null;
    (void) stub_str;

    /* Constant folding */
    run_const_fold_int_add();
    run_const_fold_int_sub();
    run_const_fold_int_mul();
    run_const_fold_int_div();
    run_const_fold_div_by_zero();
    run_const_fold_int_compare();
    run_const_fold_uint64_compare();
    run_const_fold_uint64_type_view_copy();
    run_const_fold_neg();
    run_const_fold_not();
    run_const_fold_float_add();
    run_const_fold_chain();
    run_const_fold_no_fold_variable();
    run_const_fold_shared_const_get();
    run_const_fold_shared_const_set_records_expression_result();
    run_const_fold_int_mod();
    run_const_fold_mod_by_zero();
    run_const_fold_bnot();
    run_const_fold_float_sub();
    run_const_fold_float_compare();
    run_const_fold_int_eq();
    run_const_fold_int_ne();
    run_const_fold_bitwise_ops();
    run_const_fold_shift();

    /* Copy propagation */
    run_copy_prop_basic();
    run_copy_prop_chain();

    /* One-shot await marking */
    run_mark_one_shot_await_unique_go();
    run_mark_one_shot_await_through_copy();
    run_mark_one_shot_await_completes_direct_lowering_pair();
    run_mark_one_shot_await_keeps_visible_task();
    run_mark_one_shot_await_skips_linked_go();
    run_mark_one_shot_await_all_fresh_literal();
    run_mark_one_shot_await_all_keeps_visible_array();
    run_mark_one_shot_sequential_await_fresh_cleared_task_array();
    run_mark_one_shot_sequential_await_counted_task_array_loop();
    run_mark_one_shot_sequential_await_rejects_repeated_constant_index_loop();
    run_mark_one_shot_sequential_await_skips_persistent_task_array();
    run_mark_fire_and_forget_result_group_go_deferred();
    run_mark_fire_and_forget_result_group_go_keeps_linked_go_immediate();

    /* Dead code elimination */
    run_dce_removes_unused();
    run_dce_keeps_side_effects();
    run_codegen_opaque_blocks_constant_folding_and_fence_survives_dce();
    run_dce_cascading();

    /* Phi simplification */
    run_phi_simplify_trivial();
    run_phi_simplify_preserves_the_phi_type_view();
    run_phi_simplify_non_trivial();

    /* GVN-PRE: covered by test_xi_gvn_pre.c (dedicated). */
    /* Strength reduction: covered by test_xi_strength.c (dedicated). */

    /* Verification */
    run_verify_valid_func();
    run_verify_null_type();
    run_verify_phi_arg_mismatch();
    run_verify_if_block_missing_control();
    run_verify_after_optimization();

    /* Combined */
    run_opt_run_combined();

    /* SelectRepresentations */
    run_select_rep_box_const_for_return();
    run_select_rep_unbox_param_for_arith();
    run_select_rep_no_change_for_call();
    run_select_rep_arith_chain_stays_unboxed();
    run_select_rep_keeps_narrow_store_for_shared_typed_array();
    run_select_rep_native_policy_keeps_return_unboxed();
    run_select_rep_aot_policy_keeps_scalar_phi_unboxed();
    run_select_rep_array_hof_exact_uses_native_contract();
    run_select_rep_array_hof_mutation_cannot_authorize_native_seed();
    run_select_rep_advances_empty_func_tree();
    run_full_pipeline_preserves_frozen_coroutine_plan();
    run_non_coroutine_plan_keeps_full_optimizer_pipeline();
    run_the_none_level_runs_no_pass_and_reports_no_statistics();
    run_statistics_say_which_passes_do_not_count_values();

    /* Tuple projection peephole */
    run_tuple_get_of_tuple_new_first();
    run_tuple_get_of_tuple_new_second();
    run_tuple_get_unrelated_source_keeps_op();
    run_tuple_new_eliminated_after_full_projection();
    run_tuple_get_fold_clears_tbaa_metadata();

    /* BOX/UNBOX peephole */
    run_box_elim_unbox_of_box();
    run_box_elim_box_of_unbox();
    run_box_elim_preserves_frozen_semantic_operation_identity();
    run_box_elim_preserves_backend_adapter_over_frozen_semantic_identity();
    run_box_elim_no_false_positive();

    /* Block simplify */
    run_block_simplify_single_pred_empty_block();
    run_block_simplify_keeps_ir_valid_for_multi_pred_empty();
    run_block_simplify_preserves_phi_when_merging();
    run_block_simplify_merges_past_arena_value_capacity();
    run_block_simplify_redirects_try_handler_predecessor();

    /* Jump thread */
    run_jump_thread_basic_redirect();
    run_jump_thread_keeps_ir_valid_when_dest_has_phi();

    printf("\n=== %d/%d Xi Opt tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
