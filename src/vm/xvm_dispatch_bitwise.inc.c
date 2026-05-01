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

vmcase(OP_BAND) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // Fast path: integer bitwise operation
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        R(a) = xr_int(XR_TO_INT(vb) & XR_TO_INT(vc));
        vmbreak;
    }
    // Boolean AND (for pattern matching range tests)
    if (XR_IS_BOOL(vb) && XR_IS_BOOL(vc)) {
        R(a) = xr_bool(XR_TO_BOOL(vb) && XR_TO_BOOL(vc));
        vmbreak;
    }
    // BigInt bitwise AND
    if (XR_IS_BIGINT(vb) && XR_IS_BIGINT(vc)) {
        XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);
        XrBigInt *bb = (XrBigInt *) XR_TO_PTR(vc);
        XrBigInt *result = xr_bigint_and(VM_CURRENT_CORO, ba, bb);
        R(a) = XR_FROM_PTR(result);
        vmbreak;
    }

    // Operator overload
    if (xr_value_is_instance(vb)) {
        XrInstance *inst_obj = xr_value_to_instance(vb);
        XrClass *cls = xr_instance_get_class(inst_obj);
        if (XCLASS_HAS_OP(cls, XR_OP_BAND_FLAG)) {
            XrMethod *op_method = xr_class_lookup_method(cls, SYMBOL_OP_BAND);

            if (op_method != NULL && op_method->type == XMETHOD_OPERATOR &&
                op_method->as.closure != NULL) {
                XrClosure *closure = op_method->as.closure;
                XrProto *proto = closure->proto;

                if (1 + 1 != proto->numparams) {
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "operator & requires 1 argument");
                }
                if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                }

                R(a + 1) = vb;
                R(a + 2) = vc;
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
    }

    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bitwise AND requires integer types");
}

vmcase(OP_BOR) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // Fast path: integer bitwise operation
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        R(a) = xr_int(XR_TO_INT(vb) | XR_TO_INT(vc));
        vmbreak;
    }
    // Boolean OR (for pattern matching multi-value tests)
    if (XR_IS_BOOL(vb) && XR_IS_BOOL(vc)) {
        R(a) = xr_bool(XR_TO_BOOL(vb) || XR_TO_BOOL(vc));
        vmbreak;
    }
    // BigInt bitwise OR
    if (XR_IS_BIGINT(vb) && XR_IS_BIGINT(vc)) {
        XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);
        XrBigInt *bb = (XrBigInt *) XR_TO_PTR(vc);
        XrBigInt *result = xr_bigint_or(VM_CURRENT_CORO, ba, bb);
        R(a) = XR_FROM_PTR(result);
        vmbreak;
    }

    // Operator overload
    if (xr_value_is_instance(vb)) {
        XrInstance *inst_obj = xr_value_to_instance(vb);
        XrClass *cls = xr_instance_get_class(inst_obj);
        if (XCLASS_HAS_OP(cls, XR_OP_BOR_FLAG)) {
            XrMethod *op_method = xr_class_lookup_method(cls, SYMBOL_OP_BOR);

            if (op_method != NULL && op_method->type == XMETHOD_OPERATOR &&
                op_method->as.closure != NULL) {
                XrClosure *closure = op_method->as.closure;
                XrProto *proto = closure->proto;

                if (1 + 1 != proto->numparams) {
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "operator | requires 1 argument");
                }
                if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                }

                R(a + 1) = vb;
                R(a + 2) = vc;
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
    }

    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bitwise OR requires integer types");
}

vmcase(OP_BXOR) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // Fast path: integer bitwise operation
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        R(a) = xr_int(XR_TO_INT(vb) ^ XR_TO_INT(vc));
        vmbreak;
    }
    // BigInt bitwise XOR
    if (XR_IS_BIGINT(vb) && XR_IS_BIGINT(vc)) {
        XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);
        XrBigInt *bb = (XrBigInt *) XR_TO_PTR(vc);
        XrBigInt *result = xr_bigint_xor(VM_CURRENT_CORO, ba, bb);
        R(a) = XR_FROM_PTR(result);
        vmbreak;
    }

    // Operator overload
    if (xr_value_is_instance(vb)) {
        XrInstance *inst_obj = xr_value_to_instance(vb);
        XrClass *cls = xr_instance_get_class(inst_obj);
        if (XCLASS_HAS_OP(cls, XR_OP_BXOR_FLAG)) {
            XrMethod *op_method = xr_class_lookup_method(cls, SYMBOL_OP_BXOR);

            if (op_method != NULL && op_method->type == XMETHOD_OPERATOR &&
                op_method->as.closure != NULL) {
                XrClosure *closure = op_method->as.closure;
                XrProto *proto = closure->proto;

                if (1 + 1 != proto->numparams) {
                    VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "operator ^ requires 1 argument");
                }
                if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                }

                R(a + 1) = vb;
                R(a + 2) = vc;
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
    }

    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "bitwise XOR requires integer types");
}

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

    // Fast path: integer left shift
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        xr_Integer shift = XR_TO_INT(vc);
        if (shift >= 0 && shift < XR_INT64_BITS) {
            XR_SET_INT(R(a), XR_TO_INT(vb) << shift);
        } else {
            R(a) = xr_int(0);
        }
        vmbreak;
    }
    // BigInt left shift
    if (XR_IS_BIGINT(vb) && XR_IS_INT(vc)) {
        XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);
        uint32_t shift = (uint32_t) XR_TO_INT(vc);
        XrBigInt *result = xr_bigint_shl(VM_CURRENT_CORO, ba, shift);
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

    // Fast path: integer right shift
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        xr_Integer shift = XR_TO_INT(vc);
        if (shift >= 0 && shift < XR_INT64_BITS) {
            R(a) = xr_int(XR_TO_INT(vb) >> shift);
        } else {
            R(a) = xr_int(0);  // Shift out of range returns 0
        }
        vmbreak;
    }
    // BigInt right shift
    if (XR_IS_BIGINT(vb) && XR_IS_INT(vc)) {
        XrBigInt *ba = (XrBigInt *) XR_TO_PTR(vb);
        uint32_t shift = (uint32_t) XR_TO_INT(vc);
        XrBigInt *result = xr_bigint_shr(VM_CURRENT_CORO, ba, shift);
        R(a) = XR_FROM_PTR(result);
        vmbreak;
    }

    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "shift operation requires integer types");
}
