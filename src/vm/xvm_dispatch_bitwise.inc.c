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

#define XVM_TEMPLATE_BITWISE_UNARY_CASE(op, int_op, op_symbol, op_name, error_msg)                 \
    vmcase(op) {                                                                                   \
        int a = GETARG_A(i);                                                                       \
        int b = GETARG_B(i);                                                                       \
        XrValue vb = R(b);                                                                         \
        if (XR_IS_INT(vb)) {                                                                       \
            R(a) = xr_int(int_op XR_TO_INT(vb));                                                   \
            vmbreak;                                                                               \
        }                                                                                          \
        VM_TRY_UNARY_OP_OVERLOAD(vb, a, op_symbol, op_name);                                       \
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, error_msg);                                         \
    }

#include "xvm_template_bitwise_unary_gen.inc.c"

#undef XVM_TEMPLATE_BITWISE_UNARY_CASE

#define XVM_TEMPLATE_SHIFT_CASE(op, int_fn, bigint_fn)                                             \
    vmcase(op) {                                                                                   \
        int a = GETARG_A(i);                                                                       \
        int b = GETARG_B(i);                                                                       \
        int c = GETARG_C(i);                                                                       \
        XrValue vb = R(b);                                                                         \
        XrValue vc = R(c);                                                                         \
        if (XR_IS_INT(vb) && XR_IS_INT(vc)) {                                                      \
            XR_SET_INT(R(a), int_fn(XR_TO_INT(vb), XR_TO_INT(vc)));                                \
            vmbreak;                                                                               \
        }                                                                                          \
        if (XR_IS_BIGINT(vb) && XR_IS_INT(vc)) {                                                   \
            xr_Integer count = XR_TO_INT(vc);                                                      \
            if (count < 0 || count > UINT32_MAX) {                                                 \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bigint shift count out of range");         \
            }                                                                                      \
            XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);                                             \
            XrBigInt *result = bigint_fn(VM_CURRENT_CORO, ba, (uint32_t) count);                   \
            if (XR_UNLIKELY(result == NULL)) {                                                     \
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "bigint shift allocation failed");          \
            }                                                                                      \
            R(a) = XR_FROM_PTR(result);                                                            \
            vmbreak;                                                                               \
        }                                                                                          \
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "shift operation requires integer types");          \
    }

#include "xvm_template_shift_gen.inc.c"

/* OP_SHR_U: logical (zero-extending) right shift. Emitted by xi_emit_arith
 * for XI_SHR whose lhs static type is an unsigned integer — uint64 payloads
 * occupy all 64 bits, so the arithmetic OP_SHR would sign-extend and diverge
 * from AOT/C. Not part of the generated shift template because there is no
 * distinct xi op: the split happens at bytecode emission on the static type. */
XVM_TEMPLATE_SHIFT_CASE(OP_SHR_U, xr_int_shr_u_wrap, xr_bigint_shr)

#undef XVM_TEMPLATE_SHIFT_CASE
