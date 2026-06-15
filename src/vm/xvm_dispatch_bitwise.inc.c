/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_bitwise.inc.c — bitwise opcode dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, R, vmcase,
 * vmbreak, VM_RUNTIME_ERROR, ...) provided by the surrounding
 * scope. CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns the integer / bigint bitwise dispatch:
 *   OP_BAND / OP_BOR / OP_BXOR  — binary bitwise
 *   OP_BNOT                     — unary bitwise complement
 *   OP_SHL / OP_SHR             — shift left / arithmetic shift right
 */

/* ========================================================
** Bitwise Operations
** ======================================================== */

#define XVM_TEMPLATE_BITWISE_BINARY_CASE(op, int_op, bigint_fn, op_flag, op_symbol, op_name,       \
                                         error_msg)                                                \
    vmcase(op) {                                                                                   \
        int a = GETARG_A(i);                                                                       \
        int b = GETARG_B(i);                                                                       \
        int c = GETARG_C(i);                                                                       \
        XrValue vb = R(b);                                                                         \
        XrValue vc = R(c);                                                                         \
        if (XR_IS_INT(vb) && XR_IS_INT(vc)) {                                                      \
            R(a) = xr_int(XR_TO_INT(vb) int_op XR_TO_INT(vc));                                     \
            vmbreak;                                                                               \
        }                                                                                          \
        if (XR_IS_BIGINT(vb) && XR_IS_BIGINT(vc)) {                                                \
            XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);                                             \
            XrBigInt *bb = (XrBigInt *) XR_TO_PTR(vc);                                             \
            XrBigInt *result = bigint_fn(VM_CURRENT_CORO, ba, bb);                                 \
            R(a) = XR_FROM_PTR(result);                                                            \
            vmbreak;                                                                               \
        }                                                                                          \
        VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, op_flag, op_symbol, op_name);                         \
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, error_msg);                                         \
    }

#define XVM_TEMPLATE_BITWISE_BINARY_BOOL_CASE(op, int_op, bool_op, bigint_fn, op_flag, op_symbol,  \
                                              op_name, error_msg)                                  \
    vmcase(op) {                                                                                   \
        int a = GETARG_A(i);                                                                       \
        int b = GETARG_B(i);                                                                       \
        int c = GETARG_C(i);                                                                       \
        XrValue vb = R(b);                                                                         \
        XrValue vc = R(c);                                                                         \
        if (XR_IS_INT(vb) && XR_IS_INT(vc)) {                                                      \
            R(a) = xr_int(XR_TO_INT(vb) int_op XR_TO_INT(vc));                                     \
            vmbreak;                                                                               \
        }                                                                                          \
        if (XR_IS_BOOL(vb) && XR_IS_BOOL(vc)) {                                                    \
            R(a) = xr_bool(XR_TO_BOOL(vb) bool_op XR_TO_BOOL(vc));                                 \
            vmbreak;                                                                               \
        }                                                                                          \
        if (XR_IS_BIGINT(vb) && XR_IS_BIGINT(vc)) {                                                \
            XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);                                             \
            XrBigInt *bb = (XrBigInt *) XR_TO_PTR(vc);                                             \
            XrBigInt *result = bigint_fn(VM_CURRENT_CORO, ba, bb);                                 \
            R(a) = XR_FROM_PTR(result);                                                            \
            vmbreak;                                                                               \
        }                                                                                          \
        VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, op_flag, op_symbol, op_name);                         \
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, error_msg);                                         \
    }

#include "xvm_template_bitwise_binary_gen.inc.c"

#undef XVM_TEMPLATE_BITWISE_BINARY_BOOL_CASE
#undef XVM_TEMPLATE_BITWISE_BINARY_CASE

vmcase(OP_BNOT) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue vb = R(b);

    // Fast path: integer bitwise NOT
    if (XR_IS_INT(vb)) {
        R(a) = xr_int(~XR_TO_INT(vb));
        vmbreak;
    }

    // Operator overload: bitwise NOT uses "~" symbol
    if (xr_value_is_instance(vb)) {
        XrInstance *inst_obj = xr_value_to_instance(vb);
        XrClass *cls = xr_instance_get_class(inst_obj);
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        int op_sym = xr_symbol_register_in_table(sym_table, "~");
        XrMethod *op_method = xr_class_lookup_method(cls, op_sym);

        if (op_method != NULL && op_method->type == XMETHOD_OPERATOR &&
            op_method->as.closure != NULL) {
            XrClosure *closure = op_method->as.closure;
            XrProto *proto = closure->proto;

            if (1 != proto->numparams) {
                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "unary operator ~ takes no arguments");
            }
            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
            }
            VM_STACK_CHECK(a + 1 + proto->maxstacksize);

            R(a + 1) = vb;
            savepc();
            int _fidx = VM_FRAME_COUNT;
            VM_INC_FRAME_COUNT;
            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(proto);
            new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);
            goto startfunc;
        }
    }

    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bitwise NOT requires integer type");
}

vmcase(OP_SHL) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // Fast path: integer left shift (count taken mod 64 per language spec;
    // matches JIT hardware shifts, AOT xrt_i64_shl, and constant folding)
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XR_SET_INT(R(a), xr_int_shl_wrap(XR_TO_INT(vb), XR_TO_INT(vc)));
        vmbreak;
    }
    // BigInt left shift (arbitrary precision: count is NOT masked; negative
    // counts are rejected — the old uint32 cast turned them into multi-GB
    // allocations)
    if (XR_IS_BIGINT(vb) && XR_IS_INT(vc)) {
        xr_Integer count = XR_TO_INT(vc);
        if (count < 0 || count > UINT32_MAX) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bigint shift count out of range");
        }
        XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);
        XrBigInt *result = xr_bigint_shl(VM_CURRENT_CORO, ba, (uint32_t) count);
        if (XR_UNLIKELY(result == NULL)) {
            VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "bigint shift allocation failed");
        }
        R(a) = XR_FROM_PTR(result);
        vmbreak;
    }

    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "shift operation requires integer types");
}

vmcase(OP_SHR) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // Fast path: integer arithmetic right shift (count mod 64 per spec;
    // matches JIT hardware shifts, AOT xrt_i64_shr, and constant folding)
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        R(a) = xr_int(xr_int_shr_wrap(XR_TO_INT(vb), XR_TO_INT(vc)));
        vmbreak;
    }
    // BigInt right shift (arbitrary precision: count is NOT masked; reject
    // negative counts instead of letting the uint32 cast wrap them)
    if (XR_IS_BIGINT(vb) && XR_IS_INT(vc)) {
        xr_Integer count = XR_TO_INT(vc);
        if (count < 0 || count > UINT32_MAX) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bigint shift count out of range");
        }
        XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);
        XrBigInt *result = xr_bigint_shr(VM_CURRENT_CORO, ba, (uint32_t) count);
        if (XR_UNLIKELY(result == NULL)) {
            VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "bigint shift allocation failed");
        }
        R(a) = XR_FROM_PTR(result);
        vmbreak;
    }

    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "shift operation requires integer types");
}
