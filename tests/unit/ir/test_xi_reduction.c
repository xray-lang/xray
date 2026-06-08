#include "../../../src/ir/xi_opt_reduction.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"
#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};
static int passed, failed;

#define ASSERT(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d\n", #c, __LINE__);                                                  \
            failed++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void test_null(void) {
    XiPassChange c = xi_opt_reduction(NULL);
    ASSERT(!c.values_changed);
    passed++;
}

static int pred_index(const XiBlock *blk, const XiBlock *pred) {
    if (!blk || !pred)
        return -1;
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] == pred)
            return (int) i;
    }
    return -1;
}

static XiFunc *build_sum_loop(XiPhi **out_acc) {
    XiFunc *f = xi_func_new("sum_reduction", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = body->sealed = exit_blk->sealed = true;

    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 8, &stub_int);
    XiValue *one = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_jump(entry, header);
    xi_block_set_jump(body, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiPhi *sum = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, body, exit_blk);

    XiValue *sum_next = xi_binary(f, body, XI_ADD, &stub_int, &sum->value, &iv->value);
    XiValue *iv_next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, one);

    int entry_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[entry_idx] = zero;
    iv->value.args[latch_idx] = iv_next;
    sum->value.args[entry_idx] = zero;
    sum->value.args[latch_idx] = sum_next;

    xi_block_set_return(exit_blk, &sum->value);
    *out_acc = sum;
    return f;
}

static XiFunc *build_product_loop(XiPhi **out_acc) {
    XiFunc *f = xi_func_new("product_reduction", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = body->sealed = exit_blk->sealed = true;

    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *one = xi_const_int(f, entry, 1, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 8, &stub_int);
    xi_block_set_jump(entry, header);
    xi_block_set_jump(body, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiPhi *product = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, body, exit_blk);

    XiValue *product_next = xi_binary(f, body, XI_MUL, &stub_int, &product->value, &iv->value);
    XiValue *iv_next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, one);

    int entry_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[entry_idx] = zero;
    iv->value.args[latch_idx] = iv_next;
    product->value.args[entry_idx] = one;
    product->value.args[latch_idx] = product_next;

    xi_block_set_return(exit_blk, &product->value);
    *out_acc = product;
    return f;
}

static XiFunc *build_sub_loop(XiPhi **out_acc) {
    XiFunc *f = xi_func_new("sub_not_reduction", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);
    entry->sealed = header->sealed = body->sealed = exit_blk->sealed = true;

    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *limit = xi_const_int(f, entry, 8, &stub_int);
    XiValue *one = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_jump(entry, header);
    xi_block_set_jump(body, header);

    XiPhi *iv = xi_phi_new(f, header, &stub_int, header->npreds);
    XiPhi *acc = xi_phi_new(f, header, &stub_int, header->npreds);
    XiValue *cond = xi_binary(f, header, XI_LT, &stub_bool, &iv->value, limit);
    xi_block_set_if(header, cond, body, exit_blk);

    XiValue *acc_next = xi_binary(f, body, XI_SUB, &stub_int, &acc->value, &iv->value);
    XiValue *iv_next = xi_binary(f, body, XI_ADD, &stub_int, &iv->value, one);

    int entry_idx = pred_index(header, entry);
    int latch_idx = pred_index(header, body);
    iv->value.args[entry_idx] = zero;
    iv->value.args[latch_idx] = iv_next;
    acc->value.args[entry_idx] = zero;
    acc->value.args[latch_idx] = acc_next;

    xi_block_set_return(exit_blk, &acc->value);
    *out_acc = acc;
    return f;
}

static void test_sum_reduction(void) {
    XiPhi *sum = NULL;
    XiFunc *f = build_sum_loop(&sum);
    XiPassChange c = xi_opt_reduction(f);
    ASSERT(c.values_changed);
    ASSERT(sum->value.aux_int == XI_REDUCE_SUM);
    xi_func_free(f);
    passed++;
}

static void test_product_reduction(void) {
    XiPhi *product = NULL;
    XiFunc *f = build_product_loop(&product);
    XiPassChange c = xi_opt_reduction(f);
    ASSERT(c.values_changed);
    ASSERT(product->value.aux_int == XI_REDUCE_PRODUCT);
    xi_func_free(f);
    passed++;
}

static void test_sub_is_not_reduction(void) {
    XiPhi *acc = NULL;
    XiFunc *f = build_sub_loop(&acc);
    XiPassChange c = xi_opt_reduction(f);
    ASSERT(!c.values_changed);
    ASSERT(acc->value.aux_int == 0);
    xi_func_free(f);
    passed++;
}

int main(void) {
    test_null();
    test_sum_reduction();
    test_product_reduction();
    test_sub_is_not_reduction();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
