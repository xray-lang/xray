#include "../../../src/ir/xi_cfg_edit.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                                     \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_cfg_edit", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

TEST(remove_pred_updates_phi_args) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *a = xi_block_new(f);
    XiBlock *b = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    a->sealed = b->sealed = merge->sealed = true;

    xi_block_set_if(entry, xi_const_int(f, entry, 1, &stub_int), a, b);
    xi_block_set_jump(a, merge);
    xi_block_set_jump(b, merge);

    XiValue *va = xi_const_int(f, a, 10, &stub_int);
    XiValue *vb = xi_const_int(f, b, 20, &stub_int);
    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 2);
    phi->value.args[0] = va;
    phi->value.args[1] = vb;

    ASSERT(merge->npreds == 2);
    ASSERT(xi_cfg_remove_pred(merge, a));
    ASSERT(merge->npreds == 1);
    ASSERT(merge->preds[0] == b);
    ASSERT(phi->value.nargs == 1);
    ASSERT(phi->value.args[0] == vb);

    xi_func_free(f);
}

TEST(append_pred_updates_phi_args) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *a = xi_block_new(f);
    XiBlock *b = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    a->sealed = b->sealed = merge->sealed = true;

    xi_block_set_jump(entry, a);
    xi_block_set_jump(a, merge);

    XiValue *va = xi_const_int(f, a, 10, &stub_int);
    XiValue *vb = xi_const_int(f, b, 20, &stub_int);
    XiPhi *phi = xi_phi_new(f, merge, &stub_int, 1);
    phi->value.args[0] = va;

    XiValue *args[1] = {vb};
    ASSERT(xi_cfg_append_pred(merge, b, args, 1));
    ASSERT(merge->npreds == 2);
    ASSERT(merge->preds[1] == b);
    ASSERT(phi->value.nargs == 2);
    ASSERT(phi->value.args[1] == vb);

    xi_func_free(f);
}

TEST(redirect_edge_moves_pred_and_updates_phi) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *old_target = xi_block_new(f);
    XiBlock *new_target = xi_block_new(f);
    old_target->sealed = new_target->sealed = true;

    xi_block_set_jump(entry, old_target);
    XiValue *old_val = xi_const_int(f, old_target, 1, &stub_int);
    XiValue *new_val = xi_const_int(f, entry, 2, &stub_int);
    XiPhi *old_phi = xi_phi_new(f, old_target, &stub_int, 1);
    old_phi->value.args[0] = old_val;
    XiPhi *new_phi = xi_phi_new(f, new_target, &stub_int, 0);
    XiValue *args[1] = {new_val};

    ASSERT(xi_cfg_redirect_edge(entry, old_target, new_target, args, 1));
    ASSERT(entry->succs[0] == new_target);
    ASSERT(old_target->npreds == 0);
    ASSERT(old_phi->value.nargs == 0);
    ASSERT(new_target->npreds == 1);
    ASSERT(new_target->preds[0] == entry);
    ASSERT(new_phi->value.nargs == 1);
    ASSERT(new_phi->value.args[0] == new_val);

    xi_func_free(f);
}

TEST(redirect_edge_rejects_phi_arity_mismatch) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *old_target = xi_block_new(f);
    XiBlock *new_target = xi_block_new(f);
    old_target->sealed = new_target->sealed = true;

    xi_block_set_jump(entry, old_target);
    xi_phi_new(f, new_target, &stub_int, 0);

    ASSERT(!xi_cfg_redirect_edge(entry, old_target, new_target, NULL, 0));
    ASSERT(entry->succs[0] == old_target);
    ASSERT(old_target->npreds == 1);
    ASSERT(new_target->npreds == 0);

    xi_func_free(f);
}

TEST(mark_unreachable_removes_all_stale_preds) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *dead = xi_block_new(f);
    XiBlock *succ = xi_block_new(f);
    dead->sealed = succ->sealed = true;

    xi_block_set_jump(entry, succ);
    dead->kind = XI_BLOCK_IF;
    dead->succs[0] = succ;
    dead->succs[1] = succ;
    xi_block_add_pred(succ, dead);
    xi_block_add_pred(succ, dead);

    ASSERT(succ->npreds == 3);
    ASSERT(xi_cfg_mark_unreachable_if_isolated(f, dead));
    ASSERT(dead->kind == XI_BLOCK_UNREACHABLE);
    ASSERT(succ->npreds == 1);
    ASSERT(succ->preds[0] == entry);

    xi_func_free(f);
}

TEST(compact_blocks_removes_unreachable_non_entry) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *dead = xi_block_new(f);
    XiBlock *live = xi_block_new(f);
    dead->sealed = live->sealed = true;

    dead->kind = XI_BLOCK_UNREACHABLE;
    xi_block_set_jump(entry, live);
    uint32_t removed = xi_cfg_compact_blocks(f);

    ASSERT(removed == 1);
    ASSERT(f->nblocks == 2);
    ASSERT(f->blocks[0] == entry);
    ASSERT(f->blocks[1] == live);
    ASSERT(live->id == 1);

    xi_func_free(f);
}

int main(void) {
    printf("=== Xi CFG Edit Tests ===\n\n");

    run_remove_pred_updates_phi_args();
    run_append_pred_updates_phi_args();
    run_redirect_edge_moves_pred_and_updates_phi();
    run_redirect_edge_rejects_phi_arity_mismatch();
    run_mark_unreachable_removes_all_stale_preds();
    run_compact_blocks_removes_unreachable_non_entry();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
