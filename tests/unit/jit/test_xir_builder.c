/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_builder.c - Unit tests for XIR builder (bytecode â†?XIR)
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../../../src/jit/xir.h"
#include "../../../src/jit/xir_builder.h"
#include "../../../src/jit/xir_printer.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/runtime/value/xslot_type.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/value/xtype_pool.h"
#include "../test_win_compat.h"

static XrTypePool *g_test_pool = NULL;

static void setup_type_pool(void) {
    g_test_pool = xr_type_pool_new();
    xr_type_set_current_pool(g_test_pool, NULL);
}

static void teardown_type_pool(void) {
    xr_type_pool_free(g_test_pool);
    g_test_pool = NULL;
}

/* Set param_types for parameters from an array of XrSlotType values */
static void proto_set_types(XrProto *proto, const uint8_t *types, int count) {
    // Set param_types for parameter slots
    int nparams = proto->numparams;
    if (nparams > 0) {
        proto->param_types = (struct XrType **)calloc(nparams, sizeof(struct XrType *));
        proto->param_types_count = nparams;
        for (int i = 0; i < nparams && i < count; i++) {
            switch (types[i]) {
            case XR_SLOT_I64:
                proto->param_types[i] = xr_type_new_int(NULL);
                break;
            case XR_SLOT_F64:
                proto->param_types[i] = xr_type_new_float(NULL);
                break;
            case XR_SLOT_BOOL:
                proto->param_types[i] = xr_type_new_bool(NULL);
                break;
            default:
                proto->param_types[i] = NULL;
                break;
            }
        }
    }
}

/*
 * Test: simple i64 addition
 * Source: fn add(a: int, b: int) -> int { return a + b }
 * Bytecode:
 *   0: OP_ADD    A=2 B=0 C=1    ; R[2] = R[0] + R[1]
 *   1: OP_RETURN1 A=2            ; return R[2]
 * param_types: [int, int]
 */
static void test_simple_add(void) {
    fprintf(stderr, "  test_simple_add...");

    XrProto *proto = xr_vm_proto_new();
    assert(proto != NULL);

    // Set up function metadata
    proto->numparams = 2;
    proto->maxstacksize = 3;
    proto->return_type_info = xr_type_new_int(NULL);

    // param_types: both params are i64
    uint8_t types_add[] = {XR_SLOT_I64, XR_SLOT_I64, XR_SLOT_I64};
    proto_set_types(proto, types_add, 3);

    // Emit bytecode: ADD A=2 B=0 C=1
    xr_vm_proto_write(proto, CREATE_ABC(OP_ADD, 2, 0, 1), 1);
    // RETURN1 A=2
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN1, 2, 0, 0), 2);

    // Build XIR
    XirFunc *func = xir_build_from_proto(proto);
    assert(func != NULL);

    // Print for inspection
    fprintf(stderr, "\n");
    xir_print_func(stderr, func);

    // Verify: should have 1 block, raw i64 add (not CALL_C)
    assert(func->nblk >= 1);
    XirBlock *entry = func->entry;
    assert(entry != NULL);

    // Should contain an ADD instruction (not CALL_C)
    bool found_add = false;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].op == XIR_ADD) {
            found_add = true;
            assert(entry->ins[i].rep == XR_REP_I64);
        }
    }
    assert(found_add);

    // Should end with RET
    assert(entry->jmp.type == XIR_JMP_RET);

    xir_func_destroy(func);
    xr_vm_proto_free(proto);
    fprintf(stderr, "  PASS\n");
}

/*
 * Test: tagged (dynamic) addition falls back to CALL_C
 * Source: fn add(a, b) { return a + b }
 * param_types: NULL (all ANY)
 */
static void test_tagged_add(void) {
    fprintf(stderr, "  test_tagged_add...");

    XrProto *proto = xr_vm_proto_new();
    proto->numparams = 2;
    proto->maxstacksize = 3;
    // No param_types â†?all slots are ANY

    xr_vm_proto_write(proto, CREATE_ABC(OP_ADD, 2, 0, 1), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN1, 2, 0, 0), 2);

    XirFunc *func = xir_build_from_proto(proto);
    assert(func != NULL);

    fprintf(stderr, "\n");
    xir_print_func(stderr, func);

    // Unknown-type ADD emits XIR_CALL_C to xr_jit_rt_add helper directly.
    // Builder inlines the runtime call instead of generating XIR_RT_ADD.
    XirBlock *entry = func->entry;
    bool found_call_c = false;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].op == XIR_CALL_C) {
            found_call_c = true;
        }
    }
    assert(found_call_c);

    xir_func_destroy(func);
    xr_vm_proto_free(proto);
    fprintf(stderr, "  PASS\n");
}

/*
 * Test: LOADI + arithmetic
 * Source: fn foo(x: int) -> int { return x + 10 }
 * Bytecode:
 *   0: OP_ADDI A=1 B=0 sC=10  ; R[1] = R[0] + 10
 *   1: OP_RETURN1 A=1
 */
static void test_loadi_arith(void) {
    fprintf(stderr, "  test_loadi_arith...");

    XrProto *proto = xr_vm_proto_new();
    proto->numparams = 1;
    proto->maxstacksize = 2;

    uint8_t types_arith[] = {XR_SLOT_I64, XR_SLOT_I64};
    proto_set_types(proto, types_arith, 2);

    xr_vm_proto_write(proto, CREATE_ABC(OP_ADDI, 1, 0, 10), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN1, 1, 0, 0), 2);

    XirFunc *func = xir_build_from_proto(proto);
    assert(func != NULL);

    fprintf(stderr, "\n");
    xir_print_func(stderr, func);

    // Should have raw ADD (not CALL_C)
    XirBlock *entry = func->entry;
    bool found_add = false;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].op == XIR_ADD) {
            found_add = true;
        }
    }
    assert(found_add);

    xir_func_destroy(func);
    xr_vm_proto_free(proto);
    fprintf(stderr, "  PASS\n");
}

/*
 * Test: control flow (TEST + JMP pattern)
 * Source: fn abs(x: int) -> int { if (x < 0) return -x; return x; }
 * Simplified bytecode:
 *   0: OP_LOADI   A=1 sBx=0     ; R[1] = 0
 *   1: OP_CMP_LT  A=2 B=0 C=1   ; R[2] = (R[0] < R[1])
 *   2: OP_TEST    A=2 k=1        ; if R[2] == false skip
 *   3: OP_JMP     sJ=2           ; goto pc=6
 *   4: OP_UNM     A=0 B=0        ; R[0] = -R[0]
 *   5: OP_RETURN1 A=0            ; return R[0]
 *   6: OP_RETURN1 A=0            ; return R[0]
 */
static void test_control_flow(void) {
    fprintf(stderr, "  test_control_flow...");

    XrProto *proto = xr_vm_proto_new();
    proto->numparams = 1;
    proto->maxstacksize = 3;

    uint8_t types_cf[] = {XR_SLOT_I64, XR_SLOT_I64, XR_SLOT_I64};
    proto_set_types(proto, types_cf, 3);

    xr_vm_proto_write(proto, CREATE_AsBx(OP_LOADI, 1, 0), 1);         // R[1] = 0
    xr_vm_proto_write(proto, CREATE_ABC(OP_CMP_LT, 2, 0, 1), 2);     // R[2] = R[0] < R[1]
    xr_vm_proto_write(proto, CREATE_ABC(OP_TEST, 2, 0, 1), 3);        // if !R[2] skip
    xr_vm_proto_write(proto, CREATE_sJ(OP_JMP, 2), 4);                // goto pc=6
    xr_vm_proto_write(proto, CREATE_ABC(OP_UNM, 0, 0, 0), 5);        // R[0] = -R[0]
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN1, 0, 0, 0), 6);     // return R[0]
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN1, 0, 0, 0), 7);     // return R[0]

    XirFunc *func = xir_build_from_proto(proto);
    assert(func != NULL);

    fprintf(stderr, "\n");
    xir_print_func(stderr, func);

    // Should have multiple blocks (entry, then, else)
    assert(func->nblk >= 2);

    // Entry should have a branch terminator
    XirBlock *entry = func->entry;
    (void)entry;
    // Entry block may or may not have the branch directly depending on
    // how blocks are split. Just verify we have blocks and a branch somewhere.
    bool found_branch = false;
    for (uint32_t i = 0; i < func->nblk; i++) {
        if (func->blocks[i]->jmp.type == XIR_JMP_BR) {
            found_branch = true;
        }
    }
    assert(found_branch);

    xir_func_destroy(func);
    xr_vm_proto_free(proto);
    fprintf(stderr, "  PASS\n");
}

/*
 * Test: BOX/UNBOX instructions
 */
static void test_box_unbox(void) {
    fprintf(stderr, "  test_box_unbox...");

    XrProto *proto = xr_vm_proto_new();
    proto->numparams = 1;
    proto->maxstacksize = 3;

    uint8_t types_box[] = {XR_SLOT_I64, XR_SLOT_ANY, XR_SLOT_I64};
    proto_set_types(proto, types_box, 3);

    xr_vm_proto_write(proto, CREATE_ABC(OP_BOX_I64, 1, 0, 0), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_UNBOX_I64, 2, 1, 0), 2);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN1, 2, 0, 0), 3);

    XirFunc *func = xir_build_from_proto(proto);
    assert(func != NULL);

    fprintf(stderr, "\n");
    xir_print_func(stderr, func);

    // In JIT mode, BOX/UNBOX are no-ops: builder directly maps slots
    // without emitting XIR_BOX_I64 or XIR_UNBOX_I64. The function
    // reduces to a pass-through: v0 (param) is returned via v2.
    // Verify no BOX/UNBOX instructions were emitted.
    XirBlock *entry = func->entry;
    for (uint32_t i = 0; i < entry->nins; i++) {
        assert(entry->ins[i].op != XIR_BOX_I64);
        assert(entry->ins[i].op != XIR_UNBOX_I64);
    }
    // Verify the function has a valid return (jmp.type == XIR_JMP_RET)
    assert(entry->jmp.type == XIR_JMP_RET);

    xir_func_destroy(func);
    xr_vm_proto_free(proto);
    fprintf(stderr, "  PASS\n");
}

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_xir_builder ===\n");

    setup_type_pool();

    test_simple_add();
    test_tagged_add();
    test_loadi_arith();
    test_control_flow();
    test_box_unbox();

    teardown_type_pool();

    fprintf(stderr, "All tests passed!\n");
    return 0;
}
