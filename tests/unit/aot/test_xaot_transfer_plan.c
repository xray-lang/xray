#include "../../../src/aot/xaot_prepare.h"
#include "../../../src/frontend/analyzer/xa_ownership.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>
#include <stdlib.h>

static XrType t_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType t_array = {.kind = XR_KIND_ARRAY, .id = 2, .frozen = true};
static XrType t_channel = {.kind = XR_KIND_CHANNEL, .id = 3, .frozen = true};

static void require(bool condition) {
    if (!condition) {
        fputs("test_xaot_transfer_plan: assertion failed\n", stderr);
        abort();
    }
}

static XiValue *make_go(XiFunc *func, XiValue *arg, uint8_t mode) {
    static uint8_t modes[1];
    XiValue *callee = xi_const_null(func, func->entry, &t_int);
    XiValue *go = xi_value_new(func, func->entry, XI_GO, &t_int, 2);
    require(callee && go);
    modes[0] = mode;
    go->args[0] = callee;
    go->args[1] = arg;
    go->aux = modes;
    return go;
}

static XaotTransferPlan derive(XiFunc *func, XiValue *go) {
    XaotTransferPlan plan;
    require(xaot_prepare_transfer_plan_for_site(func, go, 0, &plan));
    return plan;
}

static XiFunc *make_func(void) {
    XiFunc *func = xi_func_new("transfer_plan", &t_int);
    require(func && xi_block_new(func));
    func->entry->sealed = true;
    return func;
}

static void test_actions(void) {
    XiFunc *func = make_func();

    XiValue *local_array = xi_value_new(func, func->entry, XI_ARRAY_NEW, &t_array, 0);
    XaotTransferPlan implicit = derive(func, make_go(func, local_array, XR_TRANSFER_SHARE));
    require(implicit.action == XR_TRANSFER_REJECT);
    require(implicit.unproven_reason == XAOT_TRANSFER_UNPROVEN_CAPABILITY);

    XaotTransferPlan copied = derive(func, make_go(func, local_array, XR_TRANSFER_COPY));
    require(copied.action == XR_TRANSFER_EXPLICIT_COPY);
    require(copied.cost_class == XAOT_TRANSFER_COST_ON);
    require(copied.drop_action == XAOT_TRANSFER_DROP_CLONE);

    XiValue *source_move = xi_value_new(func, func->entry, XI_SOURCE_MOVE, &t_array, 1);
    source_move->args[0] = local_array;
    source_move->move_evidence_id = 41;
    source_move->move_storage_plan_id = 42;
    source_move->move_source_capability = XA_CAP_MUTABLE;
    source_move->move_target_capability = XA_CAP_MUTABLE;
    source_move->move_source_domain = XR_STORAGE_TRANSFERABLE;
    source_move->move_target_domain = XR_STORAGE_TRANSFERABLE;
    XaotTransferPlan moved = derive(func, make_go(func, source_move, XR_TRANSFER_MOVE));
    require(moved.action == XR_TRANSFER_MOVE_UNIQUE);
    require(moved.move_proof_id == 41 && moved.storage_plan_id == 42);
    require(moved.drop_action == XAOT_TRANSFER_DROP_HANDOFF);
    require(moved.cost_class == XAOT_TRANSFER_COST_O1);

    XiValue *channel = xi_value_new(func, func->entry, XI_CHAN_NEW, &t_channel, 0);
    XaotTransferPlan shared = derive(func, make_go(func, channel, XR_TRANSFER_SHARE));
    require(shared.action == XR_TRANSFER_SYNC_SHARE);
    require(shared.source_domain == XR_STORAGE_SYNC_SHARED);

    XiValue *integer = xi_const_int(func, func->entry, 7, &t_int);
    XaotTransferPlan scalar = derive(func, make_go(func, integer, XR_TRANSFER_SHARE));
    require(scalar.action == XR_TRANSFER_INLINE_COPY);
    require(scalar.cost_class == XAOT_TRANSFER_COST_O1);

    xi_func_free(func);
}

int main(void) {
    test_actions();
    puts("test_xaot_transfer_plan: 1 passed");
    return 0;
}
