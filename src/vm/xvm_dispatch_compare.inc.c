/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_compare.inc.c — comparison opcode dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, pc,
 * R, vmcase, vmbreak, vm_values_equal, ...) provided by the
 * surrounding scope. CMake excludes *.inc.c from the VM_SRC
 * glob.
 *
 * Owns:
 *   Branching comparisons (skip-on-false):
 *     OP_EQ / OP_EQK / OP_EQI
 *     OP_LT / OP_LTI / OP_LE / OP_LEI
 *     The shared VM_CMP_RR / VM_CMP_RI macros are defined here
 *     and used by OP_LE / OP_LEI right after.
 *
 *   Producing comparisons (write-bool):
 *     OP_CMP_EQ / CMP_NE / CMP_EQ_STRICT / CMP_NE_STRICT
 *     OP_CMP_LT / CMP_LE
 *
 *   Type predicates:
 *     OP_IS / OP_CHECKTYPE / OP_ISNULL / OP_ISNULL_SET
 */

/* ========================================================
** Comparison and Jump Instructions
** ======================================================== */

vmcase(OP_EQ) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int k_flag = GETARG_C(i);
    if (vm_values_equal(R(a), R(b)) != k_flag)
        pc++;
    vmbreak;
}

vmcase(OP_EQK) {
    // OP_EQK: constant equality comparison
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int k_flag = GETARG_C(i);
    if (vm_values_equal(R(a), k[b]) != k_flag)
        pc++;
    vmbreak;
}

vmcase(OP_EQI) {
    // OP_EQI: immediate value equality comparison
    int a = GETARG_A(i);
    int sb = GETARG_sB(i);
    int k_flag = GETARG_C(i);
    XrValue imm_val = xr_int(sb);
    if (vm_values_equal(R(a), imm_val) != k_flag)
        pc++;
    vmbreak;
}

// Register-register comparison macro (LE, GT, GE share identical structure)
#define VM_CMP_RR(op, int_op, float_op, str_cmp_op)                                                \
    vmcase(op) {                                                                                   \
        XrValue va = R(GETARG_A(i));                                                               \
        XrValue vb = R(GETARG_B(i));                                                               \
        int k_flag = GETARG_C(i);                                                                  \
        bool cond;                                                                                 \
        if (XR_IS_INT(va) && XR_IS_INT(vb)) {                                                      \
            cond = XR_TO_INT(va) int_op XR_TO_INT(vb);                                             \
        } else if ((XR_IS_INT(va) || XR_IS_FLOAT(va)) && (XR_IS_INT(vb) || XR_IS_FLOAT(vb))) {     \
            double na = XR_IS_INT(va) ? (double) XR_TO_INT(va) : XR_TO_FLOAT(va);                  \
            double nb = XR_IS_INT(vb) ? (double) XR_TO_INT(vb) : XR_TO_FLOAT(vb);                  \
            cond = na float_op nb;                                                                 \
        } else if (XR_IS_STRING(va) && XR_IS_STRING(vb)) {                                         \
            cond = xr_string_compare(xr_value_to_string(isolate, va),                              \
                                     xr_value_to_string(isolate, vb)) str_cmp_op 0;                \
        } else {                                                                                   \
            if (k_flag == 0)                                                                       \
                pc++;                                                                              \
            vmbreak;                                                                               \
        }                                                                                          \
        if (cond != k_flag)                                                                        \
            pc++;                                                                                  \
        vmbreak;                                                                                   \
    }

// Register-immediate comparison macro (LTI, LEI, GTI, GEI share identical structure)
#define VM_CMP_RI(op, int_op, float_op)                                                            \
    vmcase(op) {                                                                                   \
        XrValue va = R(GETARG_A(i));                                                               \
        int sb = GETARG_sB(i);                                                                     \
        int k_flag = GETARG_C(i);                                                                  \
        bool cond;                                                                                 \
        if (XR_IS_INT(va)) {                                                                       \
            cond = XR_TO_INT(va) int_op sb;                                                        \
        } else {                                                                                   \
            cond = XR_TO_FLOAT(va) float_op(double) sb;                                            \
        }                                                                                          \
        if (cond != k_flag)                                                                        \
            pc++;                                                                                  \
        vmbreak;                                                                                   \
    }

vmcase(OP_LT) {
    // LT A B k: if (R[A] < R[B]) != k then pc++
    XrValue va = R(GETARG_A(i));
    XrValue vb = R(GETARG_B(i));
    int k_flag = GETARG_C(i);

    // Fast path: int direct comparison
    if (XR_IS_INT(va) && XR_IS_INT(vb)) {
        if ((XR_TO_INT(va) < XR_TO_INT(vb)) != k_flag)
            pc++;
    } else if ((XR_IS_INT(va) || XR_IS_FLOAT(va)) && (XR_IS_INT(vb) || XR_IS_FLOAT(vb))) {
        double na = XR_IS_INT(va) ? (double) XR_TO_INT(va) : XR_TO_FLOAT(va);
        double nb = XR_IS_INT(vb) ? (double) XR_TO_INT(vb) : XR_TO_FLOAT(vb);
        if ((na < nb) != k_flag)
            pc++;
    } else if (XR_IS_STRING(va) && XR_IS_STRING(vb)) {
        const char *da = xr_value_str_data(&va);
        uint32_t la = xr_value_str_len(&va);
        const char *db = xr_value_str_data(&vb);
        uint32_t lb = xr_value_str_len(&vb);
        uint32_t ml = la < lb ? la : lb;
        int cmp = memcmp(da, db, ml);
        if (cmp == 0)
            cmp = (la > lb) - (la < lb);
        if ((cmp < 0) != k_flag)
            pc++;
    } else {
        // Non-comparable types: treat as false
        if (k_flag == 0)
            pc++;
    }
    vmbreak;
}

vmcase(OP_LTI) {
    // LTI A sB k: if (R[A] < sB) != k then pc++
    XrValue va = R(GETARG_A(i));
    int sb = GETARG_sB(i);
    int k_flag = GETARG_C(i);
    bool cond;

    if (XR_IS_INT(va)) {
        cond = XR_TO_INT(va) < sb;
    } else {
        cond = XR_TO_FLOAT(va) < (double) sb;
    }
    if (cond != k_flag)
        pc++;
    vmbreak;
}

VM_CMP_RR(OP_LE, <=, <=, <=)
VM_CMP_RI(OP_LEI, <=, <=)

/* ========================================================
** Comparison Instructions (Produce Boolean Value)
** ======================================================== */

vmcase(OP_CMP_EQ) {
    // OP_CMP_EQ: equality comparison
    int dest = GETARG_A(i);
    int left = GETARG_B(i);
    int right = GETARG_C(i);
    // Fast path: primitive types
    if ((XR_IS_INT(R(left)) || XR_IS_FLOAT(R(left)) || XR_IS_BOOL(R(left)) ||
         XR_IS_NULL(R(left))) &&
        (XR_IS_INT(R(right)) || XR_IS_FLOAT(R(right)) || XR_IS_BOOL(R(right)) ||
         XR_IS_NULL(R(right)))) {
        R(dest) = xr_bool(vm_values_equal(R(left), R(right)));
        vmbreak;
    }
    // Operator overload
    VM_TRY_BINARY_OP_OVERLOAD(R(left), R(right), dest, XR_OP_EQ_FLAG, SYMBOL_OP_EQ, "==");
    // Deep comparison
    R(dest) = xr_bool(vm_values_equal_deep(isolate, R(left), R(right)));
    vmbreak;
}

vmcase(OP_CMP_NE) {
    // OP_CMP_NE: inequality comparison
    int dest = GETARG_A(i);
    int left = GETARG_B(i);
    int right = GETARG_C(i);
    if ((XR_IS_INT(R(left)) || XR_IS_FLOAT(R(left)) || XR_IS_BOOL(R(left)) ||
         XR_IS_NULL(R(left))) &&
        (XR_IS_INT(R(right)) || XR_IS_FLOAT(R(right)) || XR_IS_BOOL(R(right)) ||
         XR_IS_NULL(R(right)))) {
        R(dest) = xr_bool(!vm_values_equal(R(left), R(right)));
        vmbreak;
    }
    // Operator overload for != (uses == operator, negated)
    VM_TRY_BINARY_OP_OVERLOAD(R(left), R(right), dest, XR_OP_NE_FLAG, SYMBOL_OP_NE, "!=");
    R(dest) = xr_bool(!vm_values_equal_deep(isolate, R(left), R(right)));
    vmbreak;
}

vmcase(OP_CMP_EQ_STRICT) {
    // OP_CMP_EQ_STRICT: strict equality comparison
    int dest = GETARG_A(i);
    int left = GETARG_B(i);
    int right = GETARG_C(i);
    R(dest) = xr_bool(vm_values_equal(R(left), R(right)));
    vmbreak;
}

vmcase(OP_CMP_NE_STRICT) {
    // OP_CMP_NE_STRICT: strict inequality comparison
    int dest = GETARG_A(i);
    int left = GETARG_B(i);
    int right = GETARG_C(i);
    R(dest) = xr_bool(!vm_values_equal(R(left), R(right)));
    vmbreak;
}

vmcase(OP_CMP_LT) {
    // OP_CMP_LT: less than comparison
    int dest = GETARG_A(i);
    int left = GETARG_B(i);
    int right = GETARG_C(i);
    VM_TRY_BINARY_OP_OVERLOAD(R(left), R(right), dest, XR_OP_LT_FLAG, SYMBOL_OP_LT, "<");
    R(dest) = xr_bool(vm_numeric_less(R(left), R(right)));
    vmbreak;
}

vmcase(OP_CMP_LE) {
    // OP_CMP_LE: less than or equal comparison
    int dest = GETARG_A(i);
    int left = GETARG_B(i);
    int right = GETARG_C(i);
    VM_TRY_BINARY_OP_OVERLOAD(R(left), R(right), dest, XR_OP_LE_FLAG, SYMBOL_OP_LE, "<=");
    R(dest) = xr_bool(vm_numeric_less_equal(R(left), R(right)));
    vmbreak;
}

vmcase(OP_IS) {
    // OP_IS: runtime type check - R[A] = (R[B] is R[C])
    // R[C] is either an int (XrTypeId for primitive check) or a class value
    int dest = GETARG_A(i);
    int src = GETARG_B(i);
    int type_reg = GETARG_C(i);
    XrValue val = R(src);
    XrValue type_val = R(type_reg);
    bool result = false;

    if (XR_IS_INT(type_val)) {
        /* Primitive type ID check */
        int expected_type = (int) XR_TO_INT(type_val);
        result = (xr_value_typeid(val) == (XrTypeId) expected_type);
    } else if (xr_value_is_class(type_val)) {
        /* Class instanceof check via inheritance chain */
        XrClass *target_cls = xr_value_to_class(type_val);
        if (xr_value_is_instance(val)) {
            XrClass *inst_cls = xr_instance_get_class(xr_value_to_instance(val));
            result = xr_class_instanceof(inst_cls, target_cls);
        }
    }

    R(dest) = xr_bool(result);
    vmbreak;
}

vmcase(OP_CHECKTYPE) {
    /* OP_CHECKTYPE A B: assert R[A] matches type bitmask K(B).
     * K(B) is a bitmask where bit[tid] = 1 for each allowed type.
     * Single type: mask = (1 << tid).  Union: OR of member bits. */
    int src = GETARG_A(i);
    int type_idx = GETARG_B(i);
    XrValue val = R(src);
    XrValue type_val = K(type_idx);

    if (XR_IS_INT(type_val)) {
        int64_t expected_mask = XR_TO_INT(type_val);
        XrTypeId actual_tid = xr_value_typeid(val);
        if (!((1LL << actual_tid) & expected_mask)) {
            savepc();
            // Build human-readable expected type list from bitmask
            char expect_buf[128];
            int pos = 0;
            for (int tid = 0; tid < XR_TID_COUNT && pos < 110; tid++) {
                if (!((1LL << tid) & expected_mask))
                    continue;
                if (pos > 0) {
                    expect_buf[pos++] = ' ';
                    expect_buf[pos++] = '|';
                    expect_buf[pos++] = ' ';
                }
                const char *n = xr_typeid_name((XrTypeId) tid);
                int nl = (int) strlen(n);
                if (pos + nl >= 120) {
                    memcpy(expect_buf + pos, "...", 3);
                    pos += 3;
                    break;
                }
                memcpy(expect_buf + pos, n, nl);
                pos += nl;
            }
            expect_buf[pos] = '\0';
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "TypeError: expected '%s', got '%s'", expect_buf,
                             xr_typeid_name(actual_tid));
        }
    }
    vmbreak;
}

vmcase(OP_ISNULL) {
    // ISNULL A k: if (R[A] == null) != k then pc++
    XrValue va = R(GETARG_A(i));
    int k_flag = GETARG_B(i);
    bool is_null = XR_IS_NULL(va);
    if (is_null != k_flag)
        pc++;
    vmbreak;
}

vmcase(OP_ISNULL_SET) {
    // ISNULL_SET A B: R[A] = (R[B] == null)
    int dest = GETARG_A(i);
    int src = GETARG_B(i);
    R(dest) = xr_bool(XR_IS_NULL(R(src)));
    vmbreak;
}
