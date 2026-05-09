/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_arith.inc.c — arithmetic and unary opcode dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, pc,
 * R, vmcase, vmbreak, VM_RUNTIME_ERROR, VM_BIN_*, the cl /
 * frame / base register window, ...) provided by the
 * surrounding scope. CMake excludes *.inc.c from the VM_SRC
 * glob.
 *
 * Owns the hot arithmetic dispatch:
 *   OP_ADD / ADDI / ADDK
 *   OP_SUB / SUBI / SUBK
 *   OP_MUL / MULI / MULK
 *   OP_DIV / DIVK
 *   OP_MOD / MODK
 *   OP_UNM (unary minus, with operator-overload fallback)
 *   OP_NOT (logical not)
 */

/* ========================================================
** Arithmetic Instructions (Hot Path Inlined)
** ======================================================== */

vmcase(OP_ADD) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // Fast path: integer addition (wrap on overflow)
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XR_SET_INT(R(a), (int64_t) ((uint64_t) XR_TO_INT(vb) + (uint64_t) XR_TO_INT(vc)));
        vmbreak;
    }
    // Fast path: float + float (skip XR_TONUMBER overhead)
    if (XR_IS_FLOAT(vb) && XR_IS_FLOAT(vc)) {
        XR_SET_FLOAT(R(a), vb.f + vc.f);
        vmbreak;
    }
    // Mixed/extended numeric addition
    {
        double nb = 0, nc = 0;
        if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
            XR_SET_FLOAT(R(a), nb + nc);
            vmbreak;
        }
    }
    // BigInt addition (auto-promote int operands)
    if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
        R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, vc, xr_bigint_add);
        vmbreak;
    }
    // Operator overload (unified macro)
    VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_ADD_FLAG, SYMBOL_OP_ADD, "+");
    // String concatenation: only reachable via any+any runtime path
    // (compiler guarantees typed string+string uses STRBUF sequence)
    if (XR_IS_STRING(vb) || XR_IS_STRING(vc)) {
        // Fast path: both are strings — use str_data/len directly, no promote
        if (XR_IS_STRING(vb) && XR_IS_STRING(vc)) {
            const char *db = xr_value_str_data(&vb);
            uint32_t lb = xr_value_str_len(&vb);
            const char *dc = xr_value_str_data(&vc);
            uint32_t lc = xr_value_str_len(&vc);
            size_t total_len = lb + lc;
            if (total_len < XR_SHORT_STRING_THRESHOLD) {
                char stack_buf[XR_SHORT_STRING_THRESHOLD];
                memcpy(stack_buf, db, lb);
                memcpy(stack_buf + lb, dc, lc);
                R(a) = xr_string_value(xr_string_intern(isolate, stack_buf, total_len, 0));
                vmbreak;
            }
            XrStrBuf *sb = xr_strbuf_tmp(isolate);
            xr_strbuf_append_cstr(sb, db, lb);
            xr_strbuf_append_cstr(sb, dc, lc);
            R(a) = xr_string_value(xr_strbuf_to_string(sb));
            vmbreak;
        }
        // Slow path: one operand needs toString conversion
        XrString *str_b = xr_value_to_string(isolate, vb);
        XrString *str_c = xr_value_to_string(isolate, vc);
        size_t total_len = str_b->length + str_c->length;

        if (total_len < XR_SHORT_STRING_THRESHOLD) {
            char stack_buf[XR_SHORT_STRING_THRESHOLD];
            memcpy(stack_buf, str_b->data, str_b->length);
            memcpy(stack_buf + str_b->length, str_c->data, str_c->length);
            R(a) = xr_string_value(xr_string_intern(isolate, stack_buf, total_len, 0));
            vmbreak;
        }
        XrStrBuf *sb = xr_strbuf_tmp(isolate);
        xr_strbuf_append_str(sb, str_b);
        xr_strbuf_append_str(sb, str_c);
        R(a) = xr_string_value(xr_strbuf_to_string(sb));
        vmbreak;
    }
    // TypeError: incompatible operand types for '+'
    VM_RUNTIME_ERROR(
        XR_ERR_TYPE_MISMATCH,
        "operator '+' requires both operands to be numeric or both string, got '%s' and '%s'",
        xr_typeid_name(xr_value_typeid(vb)), xr_typeid_name(xr_value_typeid(vc)));
    vmbreak;
}

vmcase(OP_ADDI) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int sc = GETARG_sC(i);
    XrValue vb = R(b);

    if (XR_IS_INT(vb)) {
        XR_SET_INT(R(a), (int64_t) ((uint64_t) XR_TO_INT(vb) + (uint64_t) (int64_t) sc));
    } else if (XR_IS_FLOAT(vb)) {
        XR_SET_FLOAT(R(a), vb.f + (double) sc);
    } else if (XR_IS_BIGINT(vb)) {
        R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, xr_int(sc), xr_bigint_add);
    } else {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "addition requires numeric types");
    }
    vmbreak;
}

vmcase(OP_ADDK) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue kc = k[c];

    // BigInt + constant mixed
    if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(kc)) {
        R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, kc, xr_bigint_add);
        vmbreak;
    }

    if (XR_IS_INT(vb) && XR_IS_INT(kc)) {
        XR_SET_INT(R(a), (int64_t) ((uint64_t) XR_TO_INT(vb) + (uint64_t) XR_TO_INT(kc)));
    } else {
        double nb = XR_IS_INT(vb) ? (double) XR_TO_INT(vb) : XR_TO_FLOAT(vb);
        double nc = XR_IS_INT(kc) ? (double) XR_TO_INT(kc) : XR_TO_FLOAT(kc);
        R(a) = xr_float(nb + nc);
    }
    vmbreak;
}

vmcase(OP_SUB) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // Fast path: integer subtraction (wrap on overflow)
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XR_SET_INT(R(a), (int64_t) ((uint64_t) XR_TO_INT(vb) - (uint64_t) XR_TO_INT(vc)));
        vmbreak;
    }
    // Fast path: float - float
    if (XR_IS_FLOAT(vb) && XR_IS_FLOAT(vc)) {
        XR_SET_FLOAT(R(a), vb.f - vc.f);
        vmbreak;
    }
    // Mixed/extended numeric subtraction
    {
        double nb = 0, nc = 0;
        if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
            XR_SET_FLOAT(R(a), nb - nc);
            vmbreak;
        }
    }
    // BigInt subtraction (auto-promote int operands)
    if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
        R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, vc, xr_bigint_sub);
        vmbreak;
    }
    // Operator overload (unified macro)
    VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_SUB_FLAG, SYMBOL_OP_SUB, "-");
    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "subtraction requires numeric types");
}

vmcase(OP_SUBI) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int sc = GETARG_sC(i);
    XrValue vb = R(b);

    if (XR_IS_INT(vb)) {
        XR_SET_INT(R(a), (int64_t) ((uint64_t) XR_TO_INT(vb) - (uint64_t) (int64_t) sc));
    } else if (XR_IS_FLOAT(vb)) {
        XR_SET_FLOAT(R(a), vb.f - (double) sc);
    } else if (XR_IS_BIGINT(vb)) {
        R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, xr_int(sc), xr_bigint_sub);
    } else {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "subtraction requires numeric types");
    }
    vmbreak;
}

vmcase(OP_SUBK) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue kc = k[c];

    // BigInt - constant mixed
    if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(kc)) {
        R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, kc, xr_bigint_sub);
        vmbreak;
    }

    if (XR_IS_INT(vb) && XR_IS_INT(kc)) {
        XR_SET_INT(R(a), (int64_t) ((uint64_t) XR_TO_INT(vb) - (uint64_t) XR_TO_INT(kc)));
    } else {
        double nb = XR_IS_INT(vb) ? (double) XR_TO_INT(vb) : XR_TO_FLOAT(vb);
        double nc = XR_IS_INT(kc) ? (double) XR_TO_INT(kc) : XR_TO_FLOAT(kc);
        XR_SET_FLOAT(R(a), nb - nc);
    }
    vmbreak;
}

vmcase(OP_MUL) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // Fast path: integer multiplication (wrap on overflow)
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XR_SET_INT(R(a), (int64_t) ((uint64_t) XR_TO_INT(vb) * (uint64_t) XR_TO_INT(vc)));
        vmbreak;
    }
    // Fast path: float * float
    if (XR_IS_FLOAT(vb) && XR_IS_FLOAT(vc)) {
        XR_SET_FLOAT(R(a), vb.f * vc.f);
        vmbreak;
    }
    // Mixed/extended numeric multiplication
    {
        double nb = 0, nc = 0;
        if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
            XR_SET_FLOAT(R(a), nb * nc);
            vmbreak;
        }
    }
    // BigInt multiplication (auto-promote int operands)
    if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
        R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, vc, xr_bigint_mul);
        vmbreak;
    }
    // String repeat: string * int or int * string
    if (XR_IS_STRING(vb) && XR_IS_INT(vc)) {
        XrString *str = xr_value_to_string(isolate, vb);
        xr_Integer count = XR_TO_INT(vc);
        XrString *result = xr_string_repeat(isolate, str, count);
        R(a) = result ? xr_string_value(result) : xr_null();
        vmbreak;
    }
    if (XR_IS_INT(vb) && XR_IS_STRING(vc)) {
        xr_Integer count = XR_TO_INT(vb);
        XrString *str = xr_value_to_string(isolate, vc);
        XrString *result = xr_string_repeat(isolate, str, count);
        R(a) = result ? xr_string_value(result) : xr_null();
        vmbreak;
    }
    // Operator overload (unified macro)
    VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_MUL_FLAG, SYMBOL_OP_MUL, "*");
    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "multiplication requires numeric types");
}

vmcase(OP_MULI) {
    // MULI A B sC: R[A] = R[B] * sC
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int sc = GETARG_sC(i);
    XrValue vb = R(b);

    if (XR_IS_INT(vb)) {
        XR_SET_INT(R(a), (int64_t) ((uint64_t) XR_TO_INT(vb) * (uint64_t) (int64_t) sc));
        vmbreak;
    }
    if (XR_IS_FLOAT(vb)) {
        XR_SET_FLOAT(R(a), vb.f * (double) sc);
        vmbreak;
    }
    if (XR_IS_BIGINT(vb)) {
        R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, xr_int(sc), xr_bigint_mul);
        vmbreak;
    }
    /* String repeat: "str" * N */
    if (XR_IS_STRING(vb)) {
        XrString *str = xr_value_to_string(isolate, vb);
        XrString *result = xr_string_repeat(isolate, str, (xr_Integer) sc);
        R(a) = result ? xr_string_value(result) : xr_null();
        vmbreak;
    }
    // Operator overload: convert immediate to XrValue
    {
        XrValue vc = xr_int(sc);
        VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_MUL_FLAG, SYMBOL_OP_MUL, "*");
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "multiplication requires numeric types");
}

vmcase(OP_MULK) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = k[c];

    // BigInt * constant mixed
    if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
        R(a) = vm_bigint_binop(VM_CURRENT_CORO, vb, vc, xr_bigint_mul);
        vmbreak;
    }

    // Fast path: integer multiplication (wrap on overflow)
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XR_SET_INT(R(a), (int64_t) ((uint64_t) XR_TO_INT(vb) * (uint64_t) XR_TO_INT(vc)));
        vmbreak;
    }
    // Float/mixed multiplication
    {
        double nb = 0, nc = 0;
        if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
            XR_SET_FLOAT(R(a), nb * nc);
            vmbreak;
        }
    }
    /* String repeat: "str" * K or K * "str" */
    if (XR_IS_STRING(vb) && XR_IS_INT(vc)) {
        XrString *str = xr_value_to_string(isolate, vb);
        XrString *result = xr_string_repeat(isolate, str, XR_TO_INT(vc));
        R(a) = result ? xr_string_value(result) : xr_null();
        vmbreak;
    }
    if (XR_IS_INT(vb) && XR_IS_STRING(vc)) {
        XrString *str = xr_value_to_string(isolate, vc);
        XrString *result = xr_string_repeat(isolate, str, XR_TO_INT(vb));
        R(a) = result ? xr_string_value(result) : xr_null();
        vmbreak;
    }
    // Operator overload
    VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_MUL_FLAG, SYMBOL_OP_MUL, "*");
    R(a) = xr_null();
    vmbreak;
}

vmcase(OP_DIV) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // BigInt division (auto-promote, with zero check)
    if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
        R(a) = vm_bigint_divop(VM_CURRENT_CORO, vb, vc, xr_bigint_div);
        if (unlikely(XR_IS_NOTFOUND(R(a)))) {
            VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
        }
        vmbreak;
    }

    // Fast path: int / int → int (type determines result)
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        xr_Integer nb = XR_TO_INT(vb);
        xr_Integer nc = XR_TO_INT(vc);
        if (nc == 0) {
            VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
        }
        R(a) = xr_int(nb / nc);
        vmbreak;
    }

    // Mixed or float: promote to float
    if ((XR_IS_INT(vb) || XR_IS_FLOAT(vb)) && (XR_IS_INT(vc) || XR_IS_FLOAT(vc))) {
        double nb = XR_IS_INT(vb) ? (double) XR_TO_INT(vb) : XR_TO_FLOAT(vb);
        double nc = XR_IS_INT(vc) ? (double) XR_TO_INT(vc) : XR_TO_FLOAT(vc);
        if (nc == 0.0) {
            VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
        }
        R(a) = xr_float(nb / nc);
        vmbreak;
    }

    // Operator overload (unified macro)
    VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_DIV_FLAG, SYMBOL_OP_DIV, "/");
    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "division requires numeric types");
}

vmcase(OP_DIVK) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = k[c];

    // BigInt / constant mixed
    if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
        R(a) = vm_bigint_divop(VM_CURRENT_CORO, vb, vc, xr_bigint_div);
        if (unlikely(XR_IS_NOTFOUND(R(a)))) {
            VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
        }
        vmbreak;
    }

    // int / int → int (type determines result)
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        xr_Integer nb = XR_TO_INT(vb);
        xr_Integer nc = XR_TO_INT(vc);
        if (nc == 0) {
            VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
        }
        R(a) = xr_int(nb / nc);
    } else if ((XR_IS_INT(vb) || XR_IS_FLOAT(vb)) && (XR_IS_INT(vc) || XR_IS_FLOAT(vc))) {
        // Mixed or float: promote to float
        double nb = XR_IS_INT(vb) ? (double) XR_TO_INT(vb) : XR_TO_FLOAT(vb);
        double nc = XR_IS_INT(vc) ? (double) XR_TO_INT(vc) : XR_TO_FLOAT(vc);
        if (nc == 0.0) {
            VM_RUNTIME_ERROR(XR_ERR_DIV_BY_ZERO, "division by zero");
        }
        R(a) = xr_float(nb / nc);
    } else {
        // Operator overload
        VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_DIV_FLAG, SYMBOL_OP_DIV, "/");
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "division requires numeric types");
    }
    vmbreak;
}

vmcase(OP_MOD) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = R(c);

    // Fast path: integer modulo
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        xr_Integer divisor = XR_TO_INT(vc);
        if (divisor == 0) {
            VM_RUNTIME_ERROR(XR_ERR_MOD_BY_ZERO, "modulo by zero");
        }
        R(a) = xr_int(XR_TO_INT(vb) % divisor);
        vmbreak;
    }

    // BigInt modulo (auto-promote, with zero check)
    if (XR_IS_BIGINT(vb) || XR_IS_BIGINT(vc)) {
        R(a) = vm_bigint_divop(VM_CURRENT_CORO, vb, vc, xr_bigint_mod);
        if (unlikely(XR_IS_NOTFOUND(R(a)))) {
            VM_RUNTIME_ERROR(XR_ERR_MOD_BY_ZERO, "modulo by zero");
        }
        vmbreak;
    }

    // Operator overload (unified macro)
    VM_TRY_BINARY_OP_OVERLOAD(vb, vc, a, XR_OP_MOD_FLAG, SYMBOL_OP_MOD, "%%");
    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "modulo requires integer types");
}

vmcase(OP_MODK) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue vb = R(b);
    XrValue vc = k[c];

    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        xr_Integer divisor = XR_TO_INT(vc);
        if (divisor == 0) {
            VM_RUNTIME_ERROR(XR_ERR_MOD_BY_ZERO, "modulo by zero");
        }
        R(a) = xr_int(XR_TO_INT(vb) % divisor);
    } else {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "modulo requires integer types");
    }
    vmbreak;
}

vmcase(OP_UNM) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue vb = R(b);

    // Fast path: numeric negation (wrap on overflow)
    if (XR_IS_INT(vb)) {
        XR_SET_INT(R(a), (int64_t) (-(uint64_t) XR_TO_INT(vb)));
        vmbreak;
    }
    if (XR_IS_FLOAT(vb)) {
        R(a) = xr_float(-XR_TO_FLOAT(vb));
        vmbreak;
    }

    // BigInt negation
    if (XR_IS_BIGINT(vb)) {
        XrBigInt *bigint = (XrBigInt *) XR_TO_PTR(vb);
        XrBigInt *result = xr_bigint_neg(VM_CURRENT_CORO, bigint);
        R(a) = XR_FROM_PTR(result);
        vmbreak;
    }

    // Operator overload: unary minus uses "-" symbol
    if (xr_value_is_instance(vb)) {
        XrInstance *inst_obj = xr_value_to_instance(vb);
        XrClass *cls = xr_instance_get_class(inst_obj);
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        int op_sym = xr_symbol_register_in_table(sym_table, "-");
        XrMethod *op_method = xr_class_lookup_method(cls, op_sym);

        if (op_method != NULL && op_method->type == XMETHOD_OPERATOR &&
            op_method->as.closure != NULL) {
            XrClosure *closure = op_method->as.closure;
            XrProto *proto = closure->proto;

            if (1 != proto->numparams) {
                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "unary operator - takes no arguments");
            }
            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
            }

            R(a + 1) = vb;  // this
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

    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "operand must be numeric");
}

vmcase(OP_NOT) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue vb = R(b);

    // Fast path: boolean negation
    if (XR_IS_BOOL(vb)) {
        R(a) = xr_bool(!XR_TO_BOOL(vb));
        vmbreak;
    }

    // Operator overload: logical not uses "!" symbol
    if (xr_value_is_instance(vb)) {
        XrInstance *inst_obj = xr_value_to_instance(vb);
        XrClass *cls = xr_instance_get_class(inst_obj);
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        int op_sym = xr_symbol_register_in_table(sym_table, "!");
        XrMethod *op_method = xr_class_lookup_method(cls, op_sym);

        if (op_method != NULL && op_method->type == XMETHOD_OPERATOR &&
            op_method->as.closure != NULL) {
            XrClosure *closure = op_method->as.closure;
            XrProto *proto = closure->proto;

            if (1 != proto->numparams) {
                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "unary operator ! takes no arguments");
            }
            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
            }

            R(a + 1) = vb;  // this
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

    // Default: negate truthiness
    R(a) = xr_bool(!xr_vm_is_truthy(vb));
    vmbreak;
}
