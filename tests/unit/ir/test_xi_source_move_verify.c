/* Independent contract tests for explicit source ownership consumption. */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_source_move_verify.h"
#include "../../../src/frontend/analyzer/xa_ownership.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
#include <stdio.h>

static XrType t_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType t_array = {.kind = XR_KIND_ARRAY, .id = 2, .frozen = true};
static XrType t_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};

static uint32_t required_evidence(void) {
    return XA_OWNERSHIP_EV_BINDING_LIVE | XA_OWNERSHIP_EV_ROOT_UNIQUE | XA_OWNERSHIP_EV_LOAN_FREE |
           XA_OWNERSHIP_EV_ALIAS_FREE | XA_OWNERSHIP_EV_ESCAPE_FREE | XA_OWNERSHIP_EV_CAPABILITY |
           XA_OWNERSHIP_EV_CFG_CONSISTENT | XA_OWNERSHIP_EV_STORAGE;
}

static XiFunc *make_func(const char *name) {
    XiFunc *func = xi_func_new(name, &t_array);
    XiBlock *entry = xi_block_new(func);
    entry->sealed = true;
    return func;
}

static XiValue *array_new(XiFunc *func, XiBlock *block) {
    return xi_value_new(func, block, XI_ARRAY_NEW, &t_array, 0);
}

static XiValue *source_move(XiFunc *func, XiBlock *block, XiValue *source) {
    XiValue *move = xi_value_new(func, block, XI_SOURCE_MOVE, &t_array, 1);
    assert(move != NULL);
    move->args[0] = source;
    move->move_evidence_id = 11;
    move->move_source_root_id = 7;
    move->move_source_symbol_id = 5;
    move->move_storage_plan_id = 13;
    move->move_evidence_bits = required_evidence();
    move->move_source_capability = XA_CAP_MUTABLE;
    move->move_target_capability = XA_CAP_MUTABLE;
    move->move_source_domain = XR_STORAGE_TRANSFERABLE;
    move->move_target_domain = XR_STORAGE_TRANSFERABLE;
    return move;
}

static void expect_status(XiFunc *func, XiSourceMoveVerifyStatus status,
                          XiSourceMoveContract contract) {
    XiSourceMoveVerifyReport report;
    XiSourceMoveVerifyStatus actual = xi_source_move_verify(func, &report);
    if (actual != status || report.contract != contract) {
        fprintf(stderr, "expected status=%d contract=%d, got status=%d contract=%d: %s\n", status,
                contract, actual, report.contract, report.message);
        assert(false);
    }
}

static void test_valid_move(void) {
    XiFunc *func = make_func("valid_move");
    XiValue *source = array_new(func, func->entry);
    XiValue *move = source_move(func, func->entry, source);
    xi_block_set_return(func->entry, move);
    expect_status(func, XI_SOURCE_MOVE_PASS, XI_SOURCE_MOVE_CONTRACT_NONE);
    xi_func_free(func);
}

static void test_missing_evidence(void) {
    XiFunc *func = make_func("missing_evidence");
    XiValue *source = array_new(func, func->entry);
    XiValue *move = source_move(func, func->entry, source);
    move->move_evidence_bits &= ~XA_OWNERSHIP_EV_ALIAS_FREE;
    xi_block_set_return(func->entry, move);
    expect_status(func, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C1_EVIDENCE);
    xi_func_free(func);
}

static void test_post_consume_use(void) {
    XiFunc *func = make_func("post_consume_use");
    XiValue *source = array_new(func, func->entry);
    XiValue *move = source_move(func, func->entry, source);
    XiValue *index = xi_const_int(func, func->entry, 0, &t_int);
    XiValue *use = xi_value_new(func, func->entry, XI_INDEX_GET, &t_int, 2);
    use->args[0] = source;
    use->args[1] = index;
    xi_block_set_return(func->entry, move);
    expect_status(func, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C3_USE_AFTER_CONSUME);
    xi_func_free(func);
}

static void test_balanced_retain_does_not_authorize_semantic_use(void) {
    XiFunc *func = make_func("balanced_retain_does_not_authorize_semantic_use");
    XiValue *source = array_new(func, func->entry);
    XiValue *retain = xi_value_new(func, func->entry, XI_RETAIN, &t_array, 1);
    retain->args[0] = source;
    XiValue *move = source_move(func, func->entry, source);
    XiValue *index = xi_const_int(func, func->entry, 0, &t_int);
    XiValue *use = xi_value_new(func, func->entry, XI_INDEX_GET, &t_int, 2);
    use->args[0] = source;
    use->args[1] = index;
    xi_block_set_return(func->entry, move);
    expect_status(func, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C3_USE_AFTER_CONSUME);
    xi_func_free(func);
}

static void test_balanced_arc_cleanup_after_move(void) {
    XiFunc *func = make_func("balanced_arc_cleanup_after_move");
    XiValue *source = array_new(func, func->entry);
    XiValue *retain = xi_value_new(func, func->entry, XI_RETAIN, &t_array, 1);
    retain->args[0] = source;
    XiValue *move = source_move(func, func->entry, source);
    XiValue *release = xi_value_new(func, func->entry, XI_RELEASE, &t_array, 1);
    release->args[0] = source;
    xi_block_set_return(func->entry, move);
    expect_status(func, XI_SOURCE_MOVE_PASS, XI_SOURCE_MOVE_CONTRACT_NONE);
    xi_func_free(func);
}

static void test_unbalanced_arc_cleanup_after_move(void) {
    XiFunc *func = make_func("unbalanced_arc_cleanup_after_move");
    XiValue *source = array_new(func, func->entry);
    XiValue *move = source_move(func, func->entry, source);
    XiValue *release = xi_value_new(func, func->entry, XI_RELEASE, &t_array, 1);
    release->args[0] = source;
    xi_block_set_return(func->entry, move);
    expect_status(func, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C3_USE_AFTER_CONSUME);
    xi_func_free(func);
}

static void test_balanced_cross_block_cleanup_after_move(void) {
    XiFunc *func = make_func("balanced_cross_block_cleanup_after_move");
    XiBlock *cleanup = xi_block_new(func);
    cleanup->sealed = true;
    XiValue *source = array_new(func, func->entry);
    XiValue *retain = xi_value_new(func, func->entry, XI_RETAIN, &t_array, 1);
    retain->args[0] = source;
    XiValue *move = source_move(func, func->entry, source);
    xi_block_set_jump(func->entry, cleanup);
    XiValue *release = xi_value_new(func, cleanup, XI_RELEASE, &t_array, 1);
    release->args[0] = source;
    xi_block_set_return(cleanup, move);
    expect_status(func, XI_SOURCE_MOVE_VIOLATION, XI_SOURCE_MOVE_C3_USE_AFTER_CONSUME);
    xi_func_free(func);
}

static void test_branch_exclusive_use(void) {
    XiFunc *func = make_func("branch_exclusive_use");
    XiBlock *entry = func->entry;
    XiBlock *move_block = xi_block_new(func);
    XiBlock *use_block = xi_block_new(func);
    move_block->sealed = true;
    use_block->sealed = true;

    XiValue *source = array_new(func, entry);
    XiValue *cond = xi_const_bool(func, entry, true, &t_bool);
    xi_block_set_if(entry, cond, move_block, use_block);

    XiValue *move = source_move(func, move_block, source);
    xi_block_set_return(move_block, move);
    xi_block_set_return(use_block, source);

    expect_status(func, XI_SOURCE_MOVE_PASS, XI_SOURCE_MOVE_CONTRACT_NONE);
    xi_func_free(func);
}

int main(void) {
    test_valid_move();
    test_missing_evidence();
    test_post_consume_use();
    test_balanced_retain_does_not_authorize_semantic_use();
    test_balanced_arc_cleanup_after_move();
    test_unbalanced_arc_cleanup_after_move();
    test_balanced_cross_block_cleanup_after_move();
    test_branch_exclusive_use();
    printf("test_xi_source_move_verify: 8 passed\n");
    return 0;
}
