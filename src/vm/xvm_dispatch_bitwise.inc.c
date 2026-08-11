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

#define XVM_TEMPLATE_BITWISE_UNARY_CASE(op, error_msg)                                            \
    vmcase(op) {                                                                                   \
        int a = GETARG_A(i);                                                                       \
        int b = GETARG_B(i);                                                                       \
        XrValue vb = R(b);                                                                         \
        if (!XR_IS_INT(vb))                                                                        \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, error_msg);                                     \
        XR_SET_INT(R(a), XR_BITS_NOT_OWNER_APPLY(                                                  \
                             XR_SEM_OWNER_ID_SHARED_BITS_NOT_HI,                                  \
                             XR_SEM_OWNER_ID_SHARED_BITS_NOT_LO, XR_SEM_CONSUMER_VM,              \
                             XR_TO_INT(vb)));                                                      \
        vmbreak;                                                                                   \
    }

#include "xvm_template_bitwise_unary_gen.inc.c"

#undef XVM_TEMPLATE_BITWISE_UNARY_CASE

#define XVM_TEMPLATE_SHIFT_CASE(op, shift_kind)                                                    \
    vmcase(op) {                                                                                   \
        int a = GETARG_A(i);                                                                       \
        int b = GETARG_B(i);                                                                       \
        int c = GETARG_C(i);                                                                       \
        XrValue vb = R(b);                                                                         \
        XrValue vc = R(c);                                                                         \
        if (XR_IS_INT(vb) && XR_IS_INT(vc)) {                                                      \
            XR_SET_INT(R(a),                                                                       \
                       XR_SHIFT_OWNER_APPLY(                                                       \
                           XR_SEM_OWNER_ID_SHARED_SHIFT_HI,                                       \
                           XR_SEM_OWNER_ID_SHARED_SHIFT_LO, XR_SEM_CONSUMER_VM, shift_kind,        \
                           XR_TO_INT(vb), XR_TO_INT(vc)));                                        \
            vmbreak;                                                                               \
        }                                                                                          \
        if (XR_IS_BIGINT(vb) && XR_IS_INT(vc)) {                                                   \
            XrShiftStatus shift_status = XR_SHIFT_STATUS_OK;                                       \
            XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);                                             \
            XrBigInt *result =                                                                    \
                xr_bigint_shift(VM_CURRENT_CORO, ba, XR_TO_INT(vc), shift_kind, &shift_status);    \
            if (shift_status == XR_SHIFT_STATUS_COUNT_RANGE) {                                     \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bigint shift count out of range");         \
            }                                                                                      \
            if (shift_status == XR_SHIFT_STATUS_CAPACITY_OVERFLOW || XR_UNLIKELY(result == NULL)) {\
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
XVM_TEMPLATE_SHIFT_CASE(OP_SHR_U, XR_SHIFT_RIGHT_UNSIGNED)

#undef XVM_TEMPLATE_SHIFT_CASE

/* Exact-width bit intrinsics are statically typed Xi operations. C carries
 * the receiver's XrNativeType; rotations place receiver/count contiguously at
 * B/B+1 so one compact opcode still retains the width contract. */
#define XVM_EXACT_BIT_UNARY_CASE(op, kernel)                                                       \
    vmcase(op) {                                                                                   \
        int a = GETARG_A(i);                                                                       \
        int b = GETARG_B(i);                                                                       \
        uint8_t native_type = (uint8_t) GETARG_C(i);                                               \
        XrValue value = R(b);                                                                      \
        if (!XR_IS_INT(value))                                                                     \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "exact-width bit intrinsic requires int");      \
        XR_SET_INT(R(a),                                                                           \
                   XR_BITS_EXACT_OWNER_APPLY(                                                      \
                       XR_SEM_OWNER_ID_SHARED_BITS_HI, XR_SEM_OWNER_ID_SHARED_BITS_LO,             \
                       XR_SEM_CONSUMER_VM, kernel, XR_TO_INT(value), 0, native_type));             \
        vmbreak;                                                                                   \
    }

XVM_EXACT_BIT_UNARY_CASE(OP_BIT_BSWAP, xr_bits_exact_kernel_bswap)
XVM_EXACT_BIT_UNARY_CASE(OP_BIT_POPCOUNT, xr_bits_exact_kernel_popcount)
XVM_EXACT_BIT_UNARY_CASE(OP_BIT_CLZ, xr_bits_exact_kernel_clz)
XVM_EXACT_BIT_UNARY_CASE(OP_BIT_CTZ, xr_bits_exact_kernel_ctz)

#undef XVM_EXACT_BIT_UNARY_CASE

#define XVM_EXACT_BIT_ROTATE_CASE(op, kernel)                                                      \
    vmcase(op) {                                                                                   \
        int a = GETARG_A(i);                                                                       \
        int b = GETARG_B(i);                                                                       \
        uint8_t native_type = (uint8_t) GETARG_C(i);                                               \
        XrValue value = R(b);                                                                      \
        XrValue count = R(b + 1);                                                                  \
        if (!XR_IS_INT(value) || !XR_IS_INT(count))                                                \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "exact-width rotate requires integer inputs");  \
        XR_SET_INT(R(a),                                                                           \
                   XR_BITS_EXACT_OWNER_APPLY(                                                      \
                       XR_SEM_OWNER_ID_SHARED_BITS_HI, XR_SEM_OWNER_ID_SHARED_BITS_LO,             \
                       XR_SEM_CONSUMER_VM, kernel, XR_TO_INT(value), XR_TO_INT(count),             \
                       native_type));                                                              \
        vmbreak;                                                                                   \
    }

XVM_EXACT_BIT_ROTATE_CASE(OP_BIT_ROTL, xr_bits_exact_kernel_rotl)
XVM_EXACT_BIT_ROTATE_CASE(OP_BIT_ROTR, xr_bits_exact_kernel_rotr)

#undef XVM_EXACT_BIT_ROTATE_CASE

vmcase(OP_BIT_MUL_HIGH) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    uint8_t native_type = (uint8_t) GETARG_C(i);
    XrValue lhs_value = R(b);
    XrValue rhs_value = R(b + 1);
    if (!XR_IS_INT(lhs_value) || !XR_IS_INT(rhs_value))
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "unsigned mulHigh requires exact-width integer inputs");
    switch (native_type) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_USIZE:
        case XR_NATIVE_U64:
            break;
        default:
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                             "mulHigh receiver must be an unsigned exact-width integer");
    }
    XR_SET_INT(R(a),
               XR_BITS_EXACT_OWNER_APPLY(
                   XR_SEM_OWNER_ID_SHARED_BITS_HI, XR_SEM_OWNER_ID_SHARED_BITS_LO,
                   XR_SEM_CONSUMER_VM, xr_bits_exact_kernel_mul_high, XR_TO_INT(lhs_value),
                   XR_TO_INT(rhs_value), native_type));
    vmbreak;
}
