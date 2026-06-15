/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xm_eligibility.c - JIT eligibility metadata tests
 */

#include "../../../src/jit/xm_eligibility.h"
#include "../../../src/ir/xi.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/runtime/symbol/xsymbol_table.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
#include <stdio.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};
static XrType stub_channel_int = {
    .kind = XR_KIND_CHANNEL,
    .id = 3,
    .frozen = true,
    .container = {.element_type = &stub_int},
};

static int tests_passed = 0;

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

static XiFunc *make_func(const char *name, XrType *ret) {
    XiFunc *f = xi_func_new(name, ret);
    assert(f != NULL);
    XiBlock *entry = xi_block_new(f);
    assert(entry != NULL);
    entry->sealed = true;
    return f;
}

static void add_params(XiFunc *f, XrType **types, uint16_t nparams) {
    f->params = (XiValue **) xr_calloc(nparams, sizeof(XiValue *));
    assert(f->params != NULL);
    f->nparams = nparams;
    for (uint16_t i = 0; i < nparams; i++) {
        XiValue *param = xi_param(f, f->entry, i, types[i]);
        assert(param != NULL);
        f->params[i] = param;
    }
}

TEST(seeds_typed_xi_signature) {
    XrType *param_types[] = {&stub_int, &stub_bool};
    XiFunc *f = make_func("typed_helper", &stub_int);
    add_params(f, param_types, 2);
    xi_block_set_return(f->entry, f->params[0]);

    XrProto proto = {0};
    proto.numparams = 2;
    proto.xi_func = f;

    xm_eligibility_prepare(&proto);

    assert(proto.param_types != NULL);
    assert(proto.param_types_count == 2);
    assert(proto.param_types[0] == &stub_int);
    assert(proto.param_types[1] == &stub_bool);
    assert(proto.return_type_info == &stub_int);
    assert(is_jit_eligible(&proto, false));

    xr_free(proto.param_types);
    xi_func_free(f);
}

TEST(does_not_seed_params_past_jit_limit) {
    XrType *param_types[9];
    for (int i = 0; i < 9; i++)
        param_types[i] = &stub_int;

    XiFunc *f = make_func("too_many_params", &stub_int);
    add_params(f, param_types, 9);
    xi_block_set_return(f->entry, f->params[0]);

    XrProto proto = {0};
    proto.numparams = 9;
    proto.xi_func = f;

    xm_eligibility_prepare(&proto);

    assert(proto.param_types == NULL);
    assert(proto.return_type_info == &stub_int);
    assert(!is_jit_eligible(&proto, false));

    xi_func_free(f);
}

TEST(keeps_suspend_channel_helpers_on_vm) {
    XrType *param_types[] = {&stub_channel_int};
    XiFunc *f = make_func("channel_recv", &stub_int);
    add_params(f, param_types, 1);

    XiValue *recv = xi_value_new(f, f->entry, XI_CALL_METHOD, &stub_int, 1);
    assert(recv != NULL);
    recv->args[0] = f->params[0];
    recv->aux_int = (int64_t) SYMBOL_RECV << 1;
    xi_block_set_return(f->entry, recv);

    XrProto proto = {0};
    proto.numparams = 1;
    proto.xi_func = f;

    xm_eligibility_prepare(&proto);

    assert(proto.param_types != NULL);
    assert(proto.param_types_count == 1);
    assert(proto.param_types[0] == &stub_channel_int);
    assert(proto.return_type_info == &stub_int);
    assert(!is_jit_eligible(&proto, false));

    xr_free(proto.param_types);
    xi_func_free(f);
}

TEST(allows_direct_channel_ops_to_reach_jit) {
    XrType *param_types[] = {&stub_channel_int, &stub_int};
    XiFunc *f = make_func("direct_channel_ops", &stub_int);
    add_params(f, param_types, 2);

    XiValue *send = xi_value_new(f, f->entry, XI_CHAN_SEND, &stub_int, 2);
    assert(send != NULL);
    send->args[0] = f->params[0];
    send->args[1] = f->params[1];

    XiValue *recv = xi_value_new(f, f->entry, XI_CHAN_RECV, &stub_int, 1);
    assert(recv != NULL);
    recv->args[0] = f->params[0];

    XiValue *status = xi_value_new(f, f->entry, XI_CHAN_RECV_STATUS, &stub_bool, 1);
    assert(status != NULL);
    status->args[0] = recv;
    xi_block_set_return(f->entry, status);

    XrProto proto = {0};
    proto.numparams = 2;
    proto.xi_func = f;

    xm_eligibility_prepare(&proto);

    assert(proto.param_types != NULL);
    assert(proto.param_types_count == 2);
    assert(is_jit_eligible(&proto, false));

    xr_free(proto.param_types);
    xi_func_free(f);
}

TEST(blocks_callers_of_suspendable_callees) {
    XrType *callee_param_types[] = {&stub_channel_int, &stub_int};
    XiFunc *callee = make_func("recv_int", &stub_int);
    add_params(callee, callee_param_types, 2);

    XiValue *recv = xi_value_new(callee, callee->entry, XI_CHAN_RECV, &stub_int, 1);
    assert(recv != NULL);
    recv->args[0] = callee->params[0];
    xi_block_set_return(callee->entry, recv);

    XrType *caller_param_types[] = {&stub_channel_int, &stub_int};
    XiFunc *caller = make_func("consumer", &stub_int);
    add_params(caller, caller_param_types, 2);
    caller->shared_slot_funcs = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    assert(caller->shared_slot_funcs != NULL);
    caller->shared_slot_func_count = 1;
    caller->shared_slot_funcs[0] = callee;

    XiValue *get = xi_value_new(caller, caller->entry, XI_GET_SHARED, &stub_int, 0);
    assert(get != NULL);
    get->aux_int = 0;

    XiValue *call = xi_value_new(caller, caller->entry, XI_CALL, &stub_int, 3);
    assert(call != NULL);
    call->args[0] = get;
    call->args[1] = caller->params[0];
    call->args[2] = caller->params[1];
    xi_block_set_return(caller->entry, call);

    XrProto proto = {0};
    proto.numparams = 2;
    proto.xi_func = caller;

    xm_eligibility_prepare(&proto);

    assert(proto.param_types != NULL);
    assert(proto.param_types_count == 2);
    assert(!is_jit_eligible(&proto, false));

    xr_free(proto.param_types);
    xr_free(caller->shared_slot_funcs);
    xi_func_free(caller);
    xi_func_free(callee);
}

int main(void) {
    printf("=== test_xm_eligibility ===\n");

    run_seeds_typed_xi_signature();
    run_does_not_seed_params_past_jit_limit();
    run_keeps_suspend_channel_helpers_on_vm();
    run_allows_direct_channel_ops_to_reach_jit();
    run_blocks_callers_of_suspendable_callees();

    printf("\nAll %d xm_eligibility tests passed\n", tests_passed);
    return 0;
}
