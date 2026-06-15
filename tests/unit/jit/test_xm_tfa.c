/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xm_tfa.c - Type-flow analysis tests
 */

#include "../test_framework.h"

#include "jit/xm_tfa.h"
#include "runtime/value/xchunk.h"

TEST(tfa_tracks_call_arguments_above_legacy_255_register_limit) {
    XrProto *caller = xr_vm_proto_new();
    XrProto *callee = xr_vm_proto_new();
    ASSERT_NOT_NULL(caller);
    ASSERT_NOT_NULL(callee);

    callee->numparams = 1;
    callee->maxstacksize = 1;

    int child_idx = xr_vm_proto_add_proto(caller, callee);
    ASSERT_EQ_INT(child_idx, 0);

    caller->maxstacksize = 302;
    xr_vm_proto_write(caller, CREATE_ABx(OP_CLOSURE, 300, (uint32_t) child_idx), 1);
    xr_vm_proto_write(caller, CREATE_AsBx(OP_LOADI, 301, 42), 1);
    xr_vm_proto_write(caller, CREATE_ABC(OP_CALL, 300, 2, 1), 1);
    xr_vm_proto_write(caller, CREATE_ABC(OP_RETURN, 0, 0, 0), 1);

    TfaState tfa;
    tfa_analyze_module(&tfa, caller);

    ASSERT_NOT_NULL(callee->param_types);
    ASSERT_EQ_INT(callee->param_types_count, 1);
    ASSERT_NOT_NULL(callee->param_types[0]);
    ASSERT_EQ_INT(callee->param_types[0]->kind, XR_KIND_INT);

    tfa_free(&tfa);
    xr_vm_proto_free(caller);
}

static void run_all_tests(void) {
    RUN_TEST_SUITE("Type Flow Analysis");
    RUN_TEST(tfa_tracks_call_arguments_above_legacy_255_register_limit);
}

TEST_MAIN_BEGIN()
printf("=== xray TFA Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
