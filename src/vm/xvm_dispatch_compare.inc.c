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
 *     OP_CMP_EQ / CMP_NE
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

/* Register-register branch comparison. The opcode names a relation and the
 * shared owner answers it on whichever lane the operands route to; the VM keeps
 * only the carrier work and the branch. */
#define VM_CMP_RR(op, kind)                                                                        \
    vmcase(op) {                                                                                   \
        XrValue va = R(GETARG_A(i));                                                               \
        XrValue vb = R(GETARG_B(i));                                                               \
        int k_flag = GETARG_C(i);                                                                  \
        bool cond;                                                                                 \
        if (XR_IS_INT(va) && XR_IS_INT(vb)) {                                                      \
            cond = VM_COMPARE_I64((kind), XR_TO_INT(va), XR_TO_INT(vb));                           \
        } else if ((XR_IS_INT(va) || XR_IS_FLOAT(va)) && (XR_IS_INT(vb) || XR_IS_FLOAT(vb))) {     \
            cond = VM_COMPARE_F64((kind),                                                          \
                                  XR_IS_INT(va) ? (double) XR_TO_INT(va) : XR_TO_FLOAT(va),        \
                                  XR_IS_INT(vb) ? (double) XR_TO_INT(vb) : XR_TO_FLOAT(vb));       \
        } else if (XR_IS_STRING(va) && XR_IS_STRING(vb)) {                                         \
            cond = VM_COMPARE_ORDERING((kind),                                                     \
                                       xr_string_compare(xr_value_to_string(isolate, va),          \
                                                         xr_value_to_string(isolate, vb)));        \
        } else {                                                                                   \
            if (k_flag == 0)                                                                       \
                pc++;                                                                              \
            vmbreak;                                                                               \
        }                                                                                          \
        if (cond != k_flag)                                                                        \
            pc++;                                                                                  \
        vmbreak;                                                                                   \
    }

/* Register-immediate branch comparison: the immediate is a signed operand of
 * the same lane the register carries. */
#define VM_CMP_RI(op, kind)                                                                        \
    vmcase(op) {                                                                                   \
        XrValue va = R(GETARG_A(i));                                                               \
        int sb = GETARG_sB(i);                                                                     \
        int k_flag = GETARG_C(i);                                                                  \
        bool cond;                                                                                 \
        if (XR_IS_INT(va)) {                                                                       \
            cond = VM_COMPARE_I64((kind), XR_TO_INT(va), sb);                                      \
        } else {                                                                                   \
            cond = VM_COMPARE_F64((kind), XR_TO_FLOAT(va), (double) sb);                           \
        }                                                                                          \
        if (cond != k_flag)                                                                        \
            pc++;                                                                                  \
        vmbreak;                                                                                   \
    }

VM_CMP_RR(OP_LT, XR_COMPARE_LT)
VM_CMP_RI(OP_LTI, XR_COMPARE_LT)
VM_CMP_RR(OP_LE, XR_COMPARE_LE)
VM_CMP_RI(OP_LEI, XR_COMPARE_LE)

/* The unsigned branch opcodes read the same i64 slot on the unsigned lane. A
 * non-integer pair has no unsigned reading, so it falls back to the signed
 * relation the ordering helpers publish. */
#define VM_CMP_UNSIGNED_RR(op, kind)                                                               \
    vmcase(op) {                                                                                   \
        XrValue va = R(GETARG_A(i));                                                               \
        XrValue vb = R(GETARG_B(i));                                                               \
        int k_flag = GETARG_C(i);                                                                  \
        bool cond;                                                                                 \
        if (XR_IS_INT(va) && XR_IS_INT(vb)) {                                                      \
            cond = VM_COMPARE_U64((kind), (uint64_t) XR_TO_INT(va), (uint64_t) XR_TO_INT(vb));     \
        } else if ((kind) == XR_COMPARE_LT) {                                                      \
            cond = vm_numeric_less(va, vb);                                                        \
        } else {                                                                                   \
            cond = vm_numeric_less_equal(va, vb);                                                  \
        }                                                                                          \
        if (cond != k_flag)                                                                        \
            pc++;                                                                                  \
        vmbreak;                                                                                   \
    }

VM_CMP_UNSIGNED_RR(OP_LTU, XR_COMPARE_LT)
VM_CMP_UNSIGNED_RR(OP_LEU, XR_COMPARE_LE)

#undef VM_CMP_UNSIGNED_RR

/* ========================================================
** Comparison Instructions (Produce Boolean Value)
** ======================================================== */

#define XVM_COMPARE_IS_PRIMITIVE(v)                                                                \
    (XR_IS_INT(v) || XR_IS_FLOAT(v) || XR_IS_BOOL(v) || XR_IS_NULL(v))

/* The generated `negate` flag names the relation, not a second rule: the owner
 * turns the equality verdict into the opcode's answer. */
#define XVM_TEMPLATE_COMPARE_DEEP_CASE(op, negate, op_flag, op_symbol, op_name, deep_fn)           \
    vmcase(op) {                                                                                   \
        int dest = GETARG_A(i);                                                                    \
        int left = GETARG_B(i);                                                                    \
        int right = GETARG_C(i);                                                                   \
        XrValue vl = R(left);                                                                      \
        XrValue vr = R(right);                                                                     \
        XrCompareKind kind = (negate) ? XR_COMPARE_NE : XR_COMPARE_EQ;                             \
        if (XVM_COMPARE_IS_PRIMITIVE(vl) && XVM_COMPARE_IS_PRIMITIVE(vr)) {                        \
            R(dest) = xr_bool(VM_COMPARE_EQUAL(kind, vm_values_equal(vl, vr)));                    \
            vmbreak;                                                                               \
        }                                                                                          \
        VM_TRY_BINARY_OP_OVERLOAD(vl, vr, dest, op_flag, op_symbol, op_name);                      \
        R(dest) = xr_bool(VM_COMPARE_EQUAL(kind, deep_fn(isolate, vl, vr)));                       \
        vmbreak;                                                                                   \
    }

#define XVM_TEMPLATE_COMPARE_ORDER_CASE(op, op_flag, op_symbol, op_name, compare_fn)               \
    vmcase(op) {                                                                                   \
        int dest = GETARG_A(i);                                                                    \
        int left = GETARG_B(i);                                                                    \
        int right = GETARG_C(i);                                                                   \
        XrValue vl = R(left);                                                                      \
        XrValue vr = R(right);                                                                     \
        VM_TRY_BINARY_OP_OVERLOAD(vl, vr, dest, op_flag, op_symbol, op_name);                      \
        R(dest) = xr_bool(compare_fn(vl, vr));                                                     \
        vmbreak;                                                                                   \
    }

#include "xvm_template_compare_gen.inc.c"

vmcase(OP_CMP_LTU) {
    int dest = GETARG_A(i);
    XrValue vl = R(GETARG_B(i));
    XrValue vr = R(GETARG_C(i));
    if (XR_IS_INT(vl) && XR_IS_INT(vr)) {
        R(dest) = xr_bool(
            VM_COMPARE_U64(XR_COMPARE_LT, (uint64_t) XR_TO_INT(vl), (uint64_t) XR_TO_INT(vr)));
        vmbreak;
    }
    VM_TRY_BINARY_OP_OVERLOAD(vl, vr, dest, XR_OP_LT_FLAG, SYMBOL_OP_LT, "<");
    R(dest) = xr_bool(vm_numeric_less(vl, vr));
    vmbreak;
}

vmcase(OP_CMP_LEU) {
    int dest = GETARG_A(i);
    XrValue vl = R(GETARG_B(i));
    XrValue vr = R(GETARG_C(i));
    if (XR_IS_INT(vl) && XR_IS_INT(vr)) {
        R(dest) = xr_bool(
            VM_COMPARE_U64(XR_COMPARE_LE, (uint64_t) XR_TO_INT(vl), (uint64_t) XR_TO_INT(vr)));
        vmbreak;
    }
    VM_TRY_BINARY_OP_OVERLOAD(vl, vr, dest, XR_OP_LE_FLAG, SYMBOL_OP_LE, "<=");
    R(dest) = xr_bool(vm_numeric_less_equal(vl, vr));
    vmbreak;
}

#undef XVM_TEMPLATE_COMPARE_ORDER_CASE
#undef XVM_TEMPLATE_COMPARE_DEEP_CASE
#undef XVM_COMPARE_IS_PRIMITIVE

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
        result = xr_value_is_type_id(val, (XrTypeId) expected_type);
    } else if (xr_value_is_class(type_val)) {
        /* Class instanceof check via inheritance chain */
        XrClass *target_cls = xr_value_to_class(type_val);
        if (xr_value_is_instance(val)) {
            XrClass *inst_cls = xr_instance_get_class(xr_value_to_instance(val));
            result = xr_class_instanceof(inst_cls, target_cls);
        }
    } else if (XR_IS_ENUM_TYPE(type_val)) {
        /* Enum type identity check (`e is SomeEnum`).  The bare enum name
         * resolves to an XrEnumType value; user enum values are enum
         * aggregates whose class is the enum's class. */
        if (xr_value_is_instance(val)) {
            XrEnumType *et = XR_TO_ENUM_TYPE(type_val);
            XrClass *vcls = xr_instance_get_class(xr_value_to_instance(val));
            if (vcls != NULL && et->enum_class != NULL)
                result = (vcls == et->enum_class);
        }
    }

    R(dest) = xr_bool(result);
    vmbreak;
}

vmcase(OP_IS_ENUM_DESCRIPTOR) {
    int dest = GETARG_A(i);
    int src = GETARG_B(i);
    int type_reg = GETARG_C(i);
    int64_t token = XR_TO_INT(R(type_reg));
    uint32_t layout_id = (uint32_t) ((uint64_t) token >> 8);
    uint8_t metadata_kind = (uint8_t) (token & 0xff);
    R(dest) = xr_bool(xr_enum_descriptor_matches(R(src), layout_id, metadata_kind));
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
        bool matches = ((1LL << actual_tid) & expected_mask) != 0;
        if (!matches && XR_TID_IS_NUMBER(actual_tid)) {
            /* An erased value reports only its family, so a fixed-width member
             * of the mask is answered by representability instead. */
            for (int tid = 0; tid < XR_TID_COUNT && !matches; tid++) {
                if (((1LL << tid) & expected_mask) != 0)
                    matches = xr_value_is_type_id(val, (XrTypeId) tid);
            }
        }
        if (!matches) {
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
            char err_buf[XR_ERROR_CORE_TYPE_MISMATCH_BUFSZ];
            xr_error_core_format_type_mismatch(err_buf, sizeof(err_buf), expect_buf,
                                               xr_typeid_name(actual_tid));
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "%s", err_buf);
        }
    }
    vmbreak;
}

vmcase(OP_ISNULL) {
    // ISNULL A k: if (R[A] == null) != k then pc++
    XrValue va = R(GETARG_A(i));
    int k_flag = GETARG_B(i);
    bool is_null = XR_NULL_TEST_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_NULL_TEST_HI, XR_SEM_OWNER_ID_SHARED_NULL_TEST_LO,
        XR_SEM_CONSUMER_VM, xr_null_test_tagged_core(va.tag));
    if (is_null != k_flag)
        pc++;
    vmbreak;
}

vmcase(OP_ISNULL_SET) {
    // ISNULL_SET A B: R[A] = (R[B] == null)
    int dest = GETARG_A(i);
    int src = GETARG_B(i);
    R(dest) = xr_bool(XR_NULL_TEST_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_NULL_TEST_HI, XR_SEM_OWNER_ID_SHARED_NULL_TEST_LO,
        XR_SEM_CONSUMER_VM, xr_null_test_tagged_core(R(src).tag)));
    vmbreak;
}
