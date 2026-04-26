/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xemit_typed.h - Strongly typed per-opcode emitter API (generated).
 *
 * GENERATED FILE — DO NOT EDIT BY HAND.
 *
 *   Source of truth : src/runtime/value/xopcode_def.h
 *   Generator       : scripts/gen_xemit_typed.py
 *   Re-generate     : python3 scripts/gen_xemit_typed.py
 *
 * One inline function per VM opcode. The function signature mirrors
 * the KOP_* field-kind triple declared in xopcode_def.h, so the call
 * site documents — and the compiler enforces — which slot is a
 * register, a K-index, a symbol index, a literal flag, etc.
 *
 *   xemit_move(e, dst, src)           // OP_MOVE   : KOP_AB_UNARY
 *   xemit_dump(e, val_reg, indent)    // OP_DUMP   : KOP_DUMP
 *   xemit_add(e, dst, lhs, rhs)       // OP_ADD    : KOP_ABC_BIN
 *   xemit_invoke(e, base, sym_idx, nargs)  // OP_INVOKE : KOP_INVOKE_SYM
 *
 * The generic emit_abc / emit_abx / emit_asbx helpers in xemit.h
 * remain available for opcodes flagged KOP_SPECIAL or for emitter
 * internals such as peephole rewriting and patching.
 */

#ifndef XEMIT_TYPED_H
#define XEMIT_TYPED_H

// Intentionally NOT including xemit.h here: this header is included
// at the *end* of xemit.h so the inline bodies below see the already
// declared emit_abc / emit_abx / emit_asbx prototypes. Including
// xemit.h from here would create a chicken-and-egg cycle.

// FMT_AB / KOP_AB_UNARY : R[A] = R[B]
static inline int xemit_move(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_MOVE, dst, src, 0);
}

// FMT_AsBx / KOP_AsBx_LITS : R[A] = sBx
static inline int xemit_loadi(XrEmitter *e, int dst, int sbx) {
    return emit_asbx(e, OP_LOADI, dst, sbx);
}

// FMT_AsBx / KOP_AsBx_LITS : R[A] = (float)sBx
static inline int xemit_loadf(XrEmitter *e, int dst, int sbx) {
    return emit_asbx(e, OP_LOADF, dst, sbx);
}

// FMT_ABx / KOP_ABx_K : R[A] = K[Bx]
static inline int xemit_loadk(XrEmitter *e, int dst, int k_idx) {
    return emit_abx(e, OP_LOADK, dst, k_idx);
}

// FMT_A / KOP_A_LOAD : R[A] = null
static inline int xemit_loadnull(XrEmitter *e, int dst) {
    return emit_abc(e, OP_LOADNULL, dst, 0, 0);
}

// FMT_A / KOP_A_LOAD : R[A] = true
static inline int xemit_loadtrue(XrEmitter *e, int dst) {
    return emit_abc(e, OP_LOADTRUE, dst, 0, 0);
}

// FMT_A / KOP_A_LOAD : R[A] = false
static inline int xemit_loadfalse(XrEmitter *e, int dst) {
    return emit_abc(e, OP_LOADFALSE, dst, 0, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] + R[C]
static inline int xemit_add(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_ADD, dst, lhs, rhs);
}

// FMT_AB_sC / KOP_ABC_BIN_S : R[A] = R[B] + sC
static inline int xemit_addi(XrEmitter *e, int dst, int src, int sliteral) {
    return emit_abc(e, OP_ADDI, dst, src, sliteral);
}

// FMT_ABC / KOP_ABC_BIN_K : R[A] = R[B] + K[C]
static inline int xemit_addk(XrEmitter *e, int dst, int src, int k_idx) {
    return emit_abc(e, OP_ADDK, dst, src, k_idx);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] - R[C]
static inline int xemit_sub(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_SUB, dst, lhs, rhs);
}

// FMT_AB_sC / KOP_ABC_BIN_S : R[A] = R[B] - sC
static inline int xemit_subi(XrEmitter *e, int dst, int src, int sliteral) {
    return emit_abc(e, OP_SUBI, dst, src, sliteral);
}

// FMT_ABC / KOP_ABC_BIN_K : R[A] = R[B] - K[C]
static inline int xemit_subk(XrEmitter *e, int dst, int src, int k_idx) {
    return emit_abc(e, OP_SUBK, dst, src, k_idx);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] * R[C]
static inline int xemit_mul(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_MUL, dst, lhs, rhs);
}

// FMT_AB_sC / KOP_ABC_BIN_S : R[A] = R[B] * sC
static inline int xemit_muli(XrEmitter *e, int dst, int src, int sliteral) {
    return emit_abc(e, OP_MULI, dst, src, sliteral);
}

// FMT_ABC / KOP_ABC_BIN_K : R[A] = R[B] * K[C]
static inline int xemit_mulk(XrEmitter *e, int dst, int src, int k_idx) {
    return emit_abc(e, OP_MULK, dst, src, k_idx);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] / R[C]
static inline int xemit_div(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_DIV, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN_K : R[A] = R[B] / K[C]
static inline int xemit_divk(XrEmitter *e, int dst, int src, int k_idx) {
    return emit_abc(e, OP_DIVK, dst, src, k_idx);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] % R[C]
static inline int xemit_mod(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_MOD, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN_K : R[A] = R[B] % K[C]
static inline int xemit_modk(XrEmitter *e, int dst, int src, int k_idx) {
    return emit_abc(e, OP_MODK, dst, src, k_idx);
}

// FMT_AB / KOP_AB_UNARY : R[A] = -R[B]
static inline int xemit_unm(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_UNM, dst, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = !R[B]
static inline int xemit_not(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_NOT, dst, src, 0);
}

// FMT_A / KOP_A_LOAD : R[A] = new_strbuf()
static inline int xemit_strbuf_new(XrEmitter *e, int dst) {
    return emit_abc(e, OP_STRBUF_NEW, dst, 0, 0);
}

// FMT_AB / KOP_AB_INPLACE : strbuf(R[A]).append(R[B])
static inline int xemit_strbuf_append(XrEmitter *e, int target, int src) {
    return emit_abc(e, OP_STRBUF_APPEND, target, src, 0);
}

// FMT_A / KOP_A_INOUT : R[A] = strbuf(R[A]).to_string()
static inline int xemit_strbuf_finish(XrEmitter *e, int reg) {
    return emit_abc(e, OP_STRBUF_FINISH, reg, 0, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] & R[C]
static inline int xemit_band(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_BAND, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] | R[C]
static inline int xemit_bor(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_BOR, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] ^ R[C]
static inline int xemit_bxor(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_BXOR, dst, lhs, rhs);
}

// FMT_AB / KOP_AB_UNARY : R[A] = ~R[B]
static inline int xemit_bnot(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_BNOT, dst, src, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] << R[C]
static inline int xemit_shl(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_SHL, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] >> R[C]
static inline int xemit_shr(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_SHR, dst, lhs, rhs);
}

// FMT_AB_IMM / KOP_AB_TEST : if (R[A] == R[B]) != k then PC++
static inline int xemit_eq(XrEmitter *e, int lhs, int rhs, int k_flag) {
    return emit_abc(e, OP_EQ, lhs, rhs, k_flag);
}

// FMT_AB_IMM / KOP_AB_TEST_K : if (R[A] == K[B]) != k then PC++
static inline int xemit_eqk(XrEmitter *e, int lhs, int k_idx, int k_flag) {
    return emit_abc(e, OP_EQK, lhs, k_idx, k_flag);
}

// FMT_AsB_C / KOP_AB_TEST_S : if (R[A] == sB) != k then PC++
static inline int xemit_eqi(XrEmitter *e, int lhs, int sliteral, int k_flag) {
    return emit_abc(e, OP_EQI, lhs, sliteral, k_flag);
}

// FMT_AB_IMM / KOP_AB_TEST : if (R[A] < R[B]) != k then PC++
static inline int xemit_lt(XrEmitter *e, int lhs, int rhs, int k_flag) {
    return emit_abc(e, OP_LT, lhs, rhs, k_flag);
}

// FMT_AsB_C / KOP_AB_TEST_S : if (R[A] < sB) != k then PC++
static inline int xemit_lti(XrEmitter *e, int lhs, int sliteral, int k_flag) {
    return emit_abc(e, OP_LTI, lhs, sliteral, k_flag);
}

// FMT_AB_IMM / KOP_AB_TEST : if (R[A] <= R[B]) != k then PC++
static inline int xemit_le(XrEmitter *e, int lhs, int rhs, int k_flag) {
    return emit_abc(e, OP_LE, lhs, rhs, k_flag);
}

// FMT_AsB_C / KOP_AB_TEST_S : if (R[A] <= sB) != k then PC++
static inline int xemit_lei(XrEmitter *e, int lhs, int sliteral, int k_flag) {
    return emit_abc(e, OP_LEI, lhs, sliteral, k_flag);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = (R[B] == R[C])
static inline int xemit_cmp_eq(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_CMP_EQ, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = (R[B] != R[C])
static inline int xemit_cmp_ne(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_CMP_NE, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = (R[B] === R[C])
static inline int xemit_cmp_eq_strict(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_CMP_EQ_STRICT, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = (R[B] !== R[C])
static inline int xemit_cmp_ne_strict(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_CMP_NE_STRICT, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = (R[B] < R[C])
static inline int xemit_cmp_lt(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_CMP_LT, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = (R[B] <= R[C])
static inline int xemit_cmp_le(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_CMP_LE, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = (R[B] is Type[C])
static inline int xemit_is(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_IS, dst, src, literal);
}

// FMT_AB / KOP_AB_K : assert R[A] is Type[K(B)]
static inline int xemit_checktype(XrEmitter *e, int src, int k_idx) {
    return emit_abc(e, OP_CHECKTYPE, src, k_idx, 0);
}

// FMT_AB_IMM / KOP_AB_FLAG : if (R[A]==null) != k then PC++
static inline int xemit_isnull(XrEmitter *e, int src, int k_flag) {
    return emit_abc(e, OP_ISNULL, src, k_flag, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = (R[B] == null)
static inline int xemit_isnull_set(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_ISNULL_SET, dst, src, 0);
}

// FMT_sJ / KOP_NONE : PC += sJ
static inline int xemit_jmp(XrEmitter *e) {
    return emit_abc(e, OP_JMP, 0, 0, 0);
}

// FMT_AB_IMM / KOP_A_TEST : if R[A] != k then PC++
static inline int xemit_test(XrEmitter *e, int cond_reg, int k_flag) {
    return emit_abc(e, OP_TEST, cond_reg, 0, k_flag);
}

// FMT_ABC / KOP_ABC_BIN_LIT : if R[B] != k then PC++ else R[A]=R[B]
static inline int xemit_testset(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_TESTSET, dst, src, literal);
}

// FMT_ABC / KOP_CALL : R[A] = R[A](R[A+1]...R[A+B-1])
static inline int xemit_call(XrEmitter *e, int base, int nargs, int nresults) {
    return emit_abc(e, OP_CALL, base, nargs, nresults);
}

// FMT_ABC / KOP_CALL_KEEP : R[C] = R[A](R[A+1]...R[A+B]); R[A] kept
static inline int xemit_call_keep(XrEmitter *e, int base, int nargs, int keep_dst) {
    return emit_abc(e, OP_CALL_KEEP, base, nargs, keep_dst);
}

// FMT_ABC / KOP_CALL : R[A](R[A+1]...R[A+B-1]) - known closure
static inline int xemit_call_static(XrEmitter *e, int base, int nargs, int nresults) {
    return emit_abc(e, OP_CALL_STATIC, base, nargs, nresults);
}

// FMT_ABC / KOP_CALL : recursive call opt
static inline int xemit_callself(XrEmitter *e, int base, int nargs, int nresults) {
    return emit_abc(e, OP_CALLSELF, base, nargs, nresults);
}

// FMT_ABC / KOP_CALL : tail call opt
static inline int xemit_tailcall(XrEmitter *e, int base, int nargs, int nresults) {
    return emit_abc(e, OP_TAILCALL, base, nargs, nresults);
}

// FMT_ABC / KOP_RETURN : return R[A]...R[A+B-2]
static inline int xemit_return(XrEmitter *e, int base, int nret) {
    return emit_abc(e, OP_RETURN, base, nret, 0);
}

// FMT_NONE / KOP_NONE : return (fast)
static inline int xemit_return0(XrEmitter *e) {
    return emit_abc(e, OP_RETURN0, 0, 0, 0);
}

// FMT_A / KOP_A_USE : return R[A] (fast)
static inline int xemit_return1(XrEmitter *e, int src) {
    return emit_abc(e, OP_RETURN1, src, 0, 0);
}

// FMT_ABC / KOP_NEW_CONTAINER : R[A] = [], B=capacity, C=storage
static inline int xemit_newarray(XrEmitter *e, int dst, int capacity, int storage) {
    return emit_abc(e, OP_NEWARRAY, dst, capacity, storage);
}

// FMT_ABC / KOP_NEW_CONTAINER : R[A] = #{}, B=capacity, C=storage
static inline int xemit_newmap(XrEmitter *e, int dst, int capacity, int storage) {
    return emit_abc(e, OP_NEWMAP, dst, capacity, storage);
}

// FMT_AB / KOP_AB_NEW_LIT : R[A] = #[], B=storage
static inline int xemit_newset(XrEmitter *e, int dst, int storage) {
    return emit_abc(e, OP_NEWSET, dst, storage, 0);
}

// FMT_AB / KOP_AB_NEW_LIT : R[A] = new StringBuilder(), B=storage
static inline int xemit_newstringbuilder(XrEmitter *e, int dst, int storage) {
    return emit_abc(e, OP_NEWSTRINGBUILDER, dst, storage, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = Range(R[B], R[C])
static inline int xemit_newrange(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_NEWRANGE, dst, lhs, rhs);
}

// FMT_AB / KOP_AB_RECV : R[A]=start, R[A+1]=end, R[A+2]=step from Range R[B]
static inline int xemit_range_unpack(XrEmitter *e, int dst_base, int src) {
    return emit_abc(e, OP_RANGE_UNPACK, dst_base, src, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B]:Array[R[C]]
static inline int xemit_array_get(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_ARRAY_GET, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = R[B]:Array[C]
static inline int xemit_array_getc(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_ARRAY_GETC, dst, src, literal);
}

// FMT_ABC / KOP_ABC_INPLACE : R[A]:Array[R[B]] = R[C]
static inline int xemit_array_set(XrEmitter *e, int target, int key, int value) {
    return emit_abc(e, OP_ARRAY_SET, target, key, value);
}

// FMT_ABC / KOP_ABC_INPLACE_LIT : R[A]:Array[B] = R[C]
static inline int xemit_array_setc(XrEmitter *e, int target, int literal, int value) {
    return emit_abc(e, OP_ARRAY_SETC, target, literal, value);
}

// FMT_AB / KOP_AB_INPLACE : R[A]:Array.push(R[B])
static inline int xemit_array_push(XrEmitter *e, int target, int src) {
    return emit_abc(e, OP_ARRAY_PUSH, target, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = len(R[B]:Array)
static inline int xemit_array_len(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_ARRAY_LEN, dst, src, 0);
}

// FMT_AB_IMM / KOP_AB_BASE_LIT : R[A][1..B] = R[A+1..A+B]
static inline int xemit_array_init(XrEmitter *e, int base, int count) {
    return emit_abc(e, OP_ARRAY_INIT, base, count, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B]:Map[R[C]]
static inline int xemit_map_get(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_MAP_GET, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN_K : R[A] = R[B]:Map[K[C]]
static inline int xemit_map_getk(XrEmitter *e, int dst, int src, int k_idx) {
    return emit_abc(e, OP_MAP_GETK, dst, src, k_idx);
}

// FMT_ABC / KOP_ABC_INPLACE : R[A]:Map[R[B]] = R[C]
static inline int xemit_map_set(XrEmitter *e, int target, int key, int value) {
    return emit_abc(e, OP_MAP_SET, target, key, value);
}

// FMT_ABC / KOP_ABC_INPLACE_K : R[A]:Map[K[B]] = R[C]
static inline int xemit_map_setk(XrEmitter *e, int target, int k_idx, int value) {
    return emit_abc(e, OP_MAP_SETK, target, k_idx, value);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B][R[C]] (runtime dispatch)
static inline int xemit_index_get(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_INDEX_GET, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_INPLACE : R[A][R[B]] = R[C] (runtime dispatch)
static inline int xemit_index_set(XrEmitter *e, int target, int key, int value) {
    return emit_abc(e, OP_INDEX_SET, target, key, value);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B][R[C]:R[C+1]] (slice)
static inline int xemit_slice(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_SLICE, dst, lhs, rhs);
}

// FMT_PROTO / KOP_PROTO : R[A] = closure(XrProto[Bx])
static inline int xemit_closure(XrEmitter *e, int dst, int proto_idx) {
    return emit_abx(e, OP_CLOSURE, dst, proto_idx);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = cl->upvals[B]
static inline int xemit_upval_get(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_UPVAL_GET, dst, src, literal);
}

// FMT_A / KOP_A_INOUT : R[A] = new_cell(R[A])
static inline int xemit_cell_new(XrEmitter *e, int reg) {
    return emit_abc(e, OP_CELL_NEW, reg, 0, 0);
}

// FMT_ABC / KOP_AB_UNARY : R[A] = cell_deref(R[B])
static inline int xemit_cell_get(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_CELL_GET, dst, src, 0);
}

// FMT_ABC / KOP_ABC_INPLACE : cell_store(R[A], R[B])
static inline int xemit_cell_set(XrEmitter *e, int target, int key, int value) {
    return emit_abc(e, OP_CELL_SET, target, key, value);
}

// FMT_ABx / KOP_ABx_K : R[A] = Class.from_descriptor(K[Bx])
static inline int xemit_class_create_from_descriptor(XrEmitter *e, int dst, int k_idx) {
    return emit_abx(e, OP_CLASS_CREATE_FROM_DESCRIPTOR, dst, k_idx);
}

// FMT_A / KOP_A_LIT : call static init <clinit>
static inline int xemit_clinit_call(XrEmitter *e, int lit) {
    return emit_abc(e, OP_CLINIT_CALL, lit, 0, 0);
}

// FMT_ABC / KOP_ABC_BIN_SYM : R[A] = R[B].symbol[C]
static inline int xemit_getprop(XrEmitter *e, int dst, int src, int sym_idx) {
    return emit_abc(e, OP_GETPROP, dst, src, sym_idx);
}

// FMT_ABC / KOP_ABC_INPLACE_SYM : R[A].symbol[B] = R[C]
static inline int xemit_setprop(XrEmitter *e, int target, int sym_idx, int value) {
    return emit_abc(e, OP_SETPROP, target, sym_idx, value);
}

// FMT_ABC / KOP_ABC_BIN_SYM : R[A] = R[B].super.symbol[C]
static inline int xemit_getsuper(XrEmitter *e, int dst, int src, int sym_idx) {
    return emit_abc(e, OP_GETSUPER, dst, src, sym_idx);
}

// FMT_ABC / KOP_INVOKE_SYM : R[A] = R[B]:method(...)
static inline int xemit_invoke(XrEmitter *e, int base, int sym_idx, int nargs) {
    return emit_abc(e, OP_INVOKE, base, sym_idx, nargs);
}

// FMT_ABC / KOP_INVOKE_SYM : tail: R[A+1]:method(...) reuse frame
static inline int xemit_invoke_tail(XrEmitter *e, int base, int sym_idx, int nargs) {
    return emit_abc(e, OP_INVOKE_TAIL, base, sym_idx, nargs);
}

// FMT_ABC / KOP_INVOKE_K : super.K[B](...)
static inline int xemit_superinvoke(XrEmitter *e, int base, int k_idx, int nargs) {
    return emit_abc(e, OP_SUPERINVOKE, base, k_idx, nargs);
}

// FMT_ABC / KOP_INVOKE_DIRECT : R[A] = R[B]:methods[C](...)
static inline int xemit_invoke_direct(XrEmitter *e, int base, int recv_reg, int method_slot) {
    return emit_abc(e, OP_INVOKE_DIRECT, base, recv_reg, method_slot);
}

// FMT_ABC / KOP_INVOKE_BUILTIN : R[A] = R[A+1]:builtin[B](nargs=C)
static inline int xemit_invoke_builtin(XrEmitter *e, int base, int builtin_idx, int nargs) {
    return emit_abc(e, OP_INVOKE_BUILTIN, base, builtin_idx, nargs);
}

// FMT_NONE / KOP_NONE : runtime: called abstract method
static inline int xemit_abstract_error(XrEmitter *e) {
    return emit_abc(e, OP_ABSTRACT_ERROR, 0, 0, 0);
}

// FMT_A / KOP_A_LIT : set storage mode context A=mode
static inline int xemit_set_storage_ctx(XrEmitter *e, int lit) {
    return emit_abc(e, OP_SET_STORAGE_CTX, lit, 0, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = to_shared(R[B])
static inline int xemit_to_shared(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_TO_SHARED, dst, src, 0);
}

// FMT_AB / KOP_AB_BASE_LIT : R[A].fields[0..B-1] = R[A+1]..R[A+B]
static inline int xemit_map_setks(XrEmitter *e, int base, int count) {
    return emit_abc(e, OP_MAP_SETKS, base, count, 0);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = R[B]:Instance.fields[C]
static inline int xemit_getfield(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_GETFIELD, dst, src, literal);
}

// FMT_ABC / KOP_ABC_INPLACE_LIT : R[A]:Instance.fields[B] = R[C]
static inline int xemit_setfield(XrEmitter *e, int target, int literal, int value) {
    return emit_abc(e, OP_SETFIELD, target, literal, value);
}

// FMT_ABC / KOP_SPECIAL : R[A] = R[B].K[C] (IC)
static inline int xemit_getfield_ic(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_GETFIELD_IC, a, b, c);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = new Json(K[B]:Shape, storage_mode=C)
static inline int xemit_newjson(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_NEWJSON, dst, src, literal);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = R[B].fields[C]
static inline int xemit_json_get(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_JSON_GET, dst, src, literal);
}

// FMT_ABC / KOP_ABC_INPLACE_LIT : R[A].fields[B] = R[C]
static inline int xemit_json_set(XrEmitter *e, int target, int literal, int value) {
    return emit_abc(e, OP_JSON_SET, target, literal, value);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = R[B].get(symbol=C)
static inline int xemit_json_getk(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_JSON_GETK, dst, src, literal);
}

// FMT_ABC / KOP_ABC_INPLACE_LIT : R[A].set(symbol=B, R[C])
static inline int xemit_json_setk(XrEmitter *e, int target, int literal, int value) {
    return emit_abc(e, OP_JSON_SETK, target, literal, value);
}

// FMT_ABC / KOP_ABC_INPLACE_LIT : R[A].fields[B] = R[C]
static inline int xemit_json_init(XrEmitter *e, int target, int literal, int value) {
    return emit_abc(e, OP_JSON_INIT, target, literal, value);
}

// FMT_ABC / KOP_SPECIAL : R[A].fields[B] = C
static inline int xemit_json_init_i(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_JSON_INIT_I, a, b, c);
}

// FMT_ABC / KOP_SPECIAL : R[A].fields[B] = null
static inline int xemit_json_init_n(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_JSON_INIT_N, a, b, c);
}

// FMT_GLOBAL / KOP_GLOBAL_GET : R[A] = builtins[Bx]
static inline int xemit_getbuiltin(XrEmitter *e, int dst, int global_idx) {
    return emit_abx(e, OP_GETBUILTIN, dst, global_idx);
}

// FMT_A / KOP_PRINT : print(R[A], add_space=B, packed=C)
static inline int xemit_print(XrEmitter *e, int val_reg, int add_space, int packed) {
    return emit_abc(e, OP_PRINT, val_reg, add_space, packed);
}

// FMT_AB / KOP_AB_UNARY_HINT : R[A] = typeof(R[B]) -> int
static inline int xemit_typeof(XrEmitter *e, int dst, int src, int slot_hint) {
    return emit_abc(e, OP_TYPEOF, dst, src, slot_hint);
}

// FMT_AB / KOP_AB_UNARY_HINT : R[A] = typename(R[B]) -> string
static inline int xemit_typename(XrEmitter *e, int dst, int src, int slot_hint) {
    return emit_abc(e, OP_TYPENAME, dst, src, slot_hint);
}

// FMT_AB / KOP_DUMP : dump(R[A], indent=B)
static inline int xemit_dump(XrEmitter *e, int val_reg, int indent) {
    return emit_abc(e, OP_DUMP, val_reg, indent, 0);
}

// FMT_AB / KOP_AB_UNARY_HINT : R[A] = int(R[B])
static inline int xemit_toint(XrEmitter *e, int dst, int src, int slot_hint) {
    return emit_abc(e, OP_TOINT, dst, src, slot_hint);
}

// FMT_AB / KOP_AB_UNARY_HINT : R[A] = float(R[B])
static inline int xemit_tofloat(XrEmitter *e, int dst, int src, int slot_hint) {
    return emit_abc(e, OP_TOFLOAT, dst, src, slot_hint);
}

// FMT_AB / KOP_AB_UNARY_HINT : R[A] = string(R[B])
static inline int xemit_tostring(XrEmitter *e, int dst, int src, int slot_hint) {
    return emit_abc(e, OP_TOSTRING, dst, src, slot_hint);
}

// FMT_AB / KOP_AB_UNARY : R[A] = bool(R[B])
static inline int xemit_tobool(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_TOBOOL, dst, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = copy(R[B])
static inline int xemit_copy(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_COPY, dst, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = chr(R[B])
static inline int xemit_chr(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_CHR, dst, src, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B].variant[R[C]]
static inline int xemit_enum_access(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_ENUM_ACCESS, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = enum_convert(R[B], R[C])
static inline int xemit_enum_convert(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_ENUM_CONVERT, dst, lhs, rhs);
}

// FMT_AB / KOP_AB_UNARY : R[A] = enum_name(R[B])
static inline int xemit_enum_name(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_ENUM_NAME, dst, src, 0);
}

// FMT_SPECIAL / KOP_SPECIAL : try block start
static inline int xemit_try(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_TRY, a, b, c);
}

// FMT_A / KOP_A_LOAD : catch block
static inline int xemit_catch(XrEmitter *e, int dst) {
    return emit_abc(e, OP_CATCH, dst, 0, 0);
}

// FMT_NONE / KOP_NONE : finally block
static inline int xemit_finally(XrEmitter *e) {
    return emit_abc(e, OP_FINALLY, 0, 0, 0);
}

// FMT_NONE / KOP_NONE : try block end
static inline int xemit_end_try(XrEmitter *e) {
    return emit_abc(e, OP_END_TRY, 0, 0, 0);
}

// FMT_A / KOP_A_USE : throw R[A]
static inline int xemit_throw(XrEmitter *e, int src) {
    return emit_abc(e, OP_THROW, src, 0, 0);
}

// FMT_AB / KOP_AB_UNARY : S[A] = R[B] (spill register to slot)
static inline int xemit_spill(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_SPILL, dst, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = S[B] (reload from slot)
static inline int xemit_reload(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_RELOAD, dst, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = box(R[B] as i64)
static inline int xemit_box_i64(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_BOX_I64, dst, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = box(R[B] as f64)
static inline int xemit_box_f64(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_BOX_F64, dst, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = unbox(R[B]) as i64
static inline int xemit_unbox_i64(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_UNBOX_I64, dst, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = unbox(R[B]) as f64
static inline int xemit_unbox_f64(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_UNBOX_F64, dst, src, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B]:Array[R[C]] (no check)
static inline int xemit_array_get_nocheck(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_ARRAY_GET_NOCHECK, dst, lhs, rhs);
}

// FMT_AB / KOP_AB_INPLACE : R[A]:Map[R[B]]++
static inline int xemit_map_increment(XrEmitter *e, int target, int src) {
    return emit_abc(e, OP_MAP_INCREMENT, target, src, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B].substring(R[C], R[C+1])
static inline int xemit_substring(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_SUBSTRING, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B] * R[C] (string repeat)
static inline int xemit_str_repeat(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_STR_REPEAT, dst, lhs, rhs);
}

// FMT_ABx / KOP_ABx_K : R[A] = import(K[Bx])
static inline int xemit_import(XrEmitter *e, int dst, int k_idx) {
    return emit_abx(e, OP_IMPORT, dst, k_idx);
}

// FMT_ABC / KOP_SPECIAL : export(K[A], R[B], C=const?)
static inline int xemit_export(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_EXPORT, a, b, c);
}

// FMT_A / KOP_A_USE : export * from R[A]
static inline int xemit_export_all(XrEmitter *e, int src) {
    return emit_abc(e, OP_EXPORT_ALL, src, 0, 0);
}

// FMT_ABC / KOP_SPECIAL : if !R[A] throw AssertError(K[B]); C=1: negate
static inline int xemit_assert(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_ASSERT, a, b, c);
}

// FMT_ABC / KOP_SPECIAL : if R[A] != R[B] throw AssertError(K[C])
static inline int xemit_assert_eq(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_ASSERT_EQ, a, b, c);
}

// FMT_ABC / KOP_SPECIAL : if R[A] == R[B] throw AssertError(K[C])
static inline int xemit_assert_ne(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_ASSERT_NE, a, b, c);
}

// FMT_ABC / KOP_ABC_BIN_K : R[A] = regex.compile(K[B], K[C])
static inline int xemit_regex_compile(XrEmitter *e, int dst, int src, int k_idx) {
    return emit_abc(e, OP_REGEX_COMPILE, dst, src, k_idx);
}

// FMT_ABC / KOP_SPECIAL : R[A] = go R[B](R[B+1]..R[B+C])
static inline int xemit_go(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_GO, a, b, c);
}

// FMT_ABC / KOP_SPECIAL : R[A] = go R[B].method(args)
static inline int xemit_go_invoke(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_GO_INVOKE, a, b, c);
}

// FMT_ABC / KOP_ABC_BIN_LIT : spawn continuation: R[A]=task, R[B]=fn, C=nargs|flags
static inline int xemit_spawn_cont(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_SPAWN_CONT, dst, src, literal);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = await R[B], C=discard
static inline int xemit_await(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_AWAIT, dst, src, literal);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = await(timeout: R[C]) R[B]
static inline int xemit_await_timeout(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_AWAIT_TIMEOUT, dst, lhs, rhs);
}

// FMT_AB / KOP_AB_UNARY : R[A] = await R[B]:Array
static inline int xemit_await_all(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_AWAIT_ALL, dst, src, 0);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = await.any R[B]:Array, C=mode
static inline int xemit_await_any(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_AWAIT_ANY, dst, src, literal);
}

// FMT_A / KOP_A_LIT : yield (A=poll_threshold for select default)
static inline int xemit_yield(XrEmitter *e, int lit) {
    return emit_abc(e, OP_YIELD, lit, 0, 0);
}

// FMT_A / KOP_A_LOAD : R[A] = cancelled()
static inline int xemit_cancelled(XrEmitter *e, int dst) {
    return emit_abc(e, OP_CANCELLED, dst, 0, 0);
}

// FMT_NONE / KOP_NONE : Coro.lockThread()
static inline int xemit_lock_thread(XrEmitter *e) {
    return emit_abc(e, OP_LOCK_THREAD, 0, 0, 0);
}

// FMT_NONE / KOP_NONE : Coro.unlockThread()
static inline int xemit_unlock_thread(XrEmitter *e) {
    return emit_abc(e, OP_UNLOCK_THREAD, 0, 0, 0);
}

// FMT_AB / KOP_AB_INOUT_IN : Coro.setLocal(R[A], R[B])
static inline int xemit_set_local(XrEmitter *e, int target, int src) {
    return emit_abc(e, OP_SET_LOCAL, target, src, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = Coro.getLocal(R[B])
static inline int xemit_get_local(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_GET_LOCAL, dst, src, 0);
}

// FMT_AB / KOP_AB_INOUT_IN : Coro.setPriority(R[A], R[B])
static inline int xemit_set_priority(XrEmitter *e, int target, int src) {
    return emit_abc(e, OP_SET_PRIORITY, target, src, 0);
}

// FMT_ABC / KOP_SPECIAL : coro monitoring, C=sub_op
static inline int xemit_coro_ctrl(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_CORO_CTRL, a, b, c);
}

// FMT_AB / KOP_AB_NEW_LIT : R[A] = Channel(B)
static inline int xemit_chan_new(XrEmitter *e, int dst, int storage) {
    return emit_abc(e, OP_CHAN_NEW, dst, storage, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = Channel(R[B], R[C]) - named
static inline int xemit_chan_new_named(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_CHAN_NEW_NAMED, dst, lhs, rhs);
}

// FMT_ABC / KOP_SPECIAL : R[B].send(R[C])
static inline int xemit_chan_send(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_CHAN_SEND, a, b, c);
}

// FMT_AB / KOP_AB_RECV : R[A], R[A+1] = R[B].recv()
static inline int xemit_chan_recv(XrEmitter *e, int dst_base, int src) {
    return emit_abc(e, OP_CHAN_RECV, dst_base, src, 0);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B].trySend(R[C])
static inline int xemit_chan_try_send(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_CHAN_TRY_SEND, dst, lhs, rhs);
}

// FMT_AB / KOP_AB_RECV : R[A], R[A+1] = R[B].tryRecv()
static inline int xemit_chan_try_recv(XrEmitter *e, int dst_base, int src) {
    return emit_abc(e, OP_CHAN_TRY_RECV, dst_base, src, 0);
}

// FMT_ABC / KOP_SPECIAL : R[A] = R[B].send(R[C], timeout: R[C+1])
static inline int xemit_chan_send_timeout(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_CHAN_SEND_TIMEOUT, a, b, c);
}

// FMT_ABC / KOP_ABC_BIN : R[A] = R[B].recv(timeout: R[C])
static inline int xemit_chan_recv_timeout(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_CHAN_RECV_TIMEOUT, dst, lhs, rhs);
}

// FMT_A / KOP_A_USE : R[A].close()
static inline int xemit_chan_close(XrEmitter *e, int src) {
    return emit_abc(e, OP_CHAN_CLOSE, src, 0, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = R[B].isClosed()
static inline int xemit_chan_is_closed(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_CHAN_IS_CLOSED, dst, src, 0);
}

// FMT_NONE / KOP_NONE : select start
static inline int xemit_select_start(XrEmitter *e) {
    return emit_abc(e, OP_SELECT_START, 0, 0, 0);
}

// FMT_ABC / KOP_SPECIAL : case A=type, B=channel, C=value
static inline int xemit_select_case(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_SELECT_CASE, a, b, c);
}

// FMT_NONE / KOP_NONE : select end
static inline int xemit_select_end(XrEmitter *e) {
    return emit_abc(e, OP_SELECT_END, 0, 0, 0);
}

// FMT_AB / KOP_AB_BASE_LIT : defer R[A](args R[A+1..A+B-1])
static inline int xemit_defer(XrEmitter *e, int base, int count) {
    return emit_abc(e, OP_DEFER, base, count, 0);
}

// FMT_AB / KOP_AB_NEW_LIT : R[A] = Bytes(B args)
static inline int xemit_bytes_new(XrEmitter *e, int dst, int storage) {
    return emit_abc(e, OP_BYTES_NEW, dst, storage, 0);
}

// FMT_A / KOP_A_LIT : enter scope, A=mode(0=wait,1=linked,2=supervisor)
static inline int xemit_scope_enter(XrEmitter *e, int lit) {
    return emit_abc(e, OP_SCOPE_ENTER, lit, 0, 0);
}

// FMT_AB / KOP_AB_NEW_LIT : exit scope, A=mode, B=result_reg
static inline int xemit_scope_exit(XrEmitter *e, int dst, int storage) {
    return emit_abc(e, OP_SCOPE_EXIT, dst, storage, 0);
}

// FMT_A / KOP_A_USE : time.sleep(R[A]) ms
static inline int xemit_sleep(XrEmitter *e, int src) {
    return emit_abc(e, OP_SLEEP, src, 0, 0);
}

// FMT_AB / KOP_AB_UNARY : R[A] = time.after(R[B]) ms
static inline int xemit_time_after(XrEmitter *e, int dst, int src) {
    return emit_abc(e, OP_TIME_AFTER, dst, src, 0);
}

// FMT_ABC / KOP_ABC_BIN_LIT : select block wait: A=ch_base, B=ch_count, C=case_count
static inline int xemit_select_block(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_SELECT_BLOCK, dst, src, literal);
}

// FMT_GLOBAL / KOP_GLOBAL_GET : R[A] = shared[Bx]
static inline int xemit_getshared(XrEmitter *e, int dst, int global_idx) {
    return emit_abx(e, OP_GETSHARED, dst, global_idx);
}

// FMT_GLOBAL / KOP_GLOBAL_SET : shared[Bx] = R[A]
static inline int xemit_setshared(XrEmitter *e, int src, int global_idx) {
    return emit_abx(e, OP_SETSHARED, src, global_idx);
}

// FMT_ABC / KOP_ABC_BIN : R[A].i = R[B]:TypedArray[R[C]]
static inline int xemit_tarray_get(XrEmitter *e, int dst, int lhs, int rhs) {
    return emit_abc(e, OP_TARRAY_GET, dst, lhs, rhs);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A].i = R[B]:TypedArray[C]
static inline int xemit_tarray_getc(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_TARRAY_GETC, dst, src, literal);
}

// FMT_ABC / KOP_ABC_INPLACE : R[A]:TypedArray[R[B]] = R[C].i
static inline int xemit_tarray_set(XrEmitter *e, int target, int key, int value) {
    return emit_abc(e, OP_TARRAY_SET, target, key, value);
}

// FMT_AB / KOP_AB_INPLACE : R[A]:TypedArray.push(R[B].i)
static inline int xemit_tarray_push(XrEmitter *e, int target, int src) {
    return emit_abc(e, OP_TARRAY_PUSH, target, src, 0);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A].i = R[B]:compact_fields[C]
static inline int xemit_tfield_get(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_TFIELD_GET, dst, src, literal);
}

// FMT_ABC / KOP_ABC_INPLACE_LIT : R[A]:compact_fields[B] = R[C].i
static inline int xemit_tfield_set(XrEmitter *e, int target, int literal, int value) {
    return emit_abc(e, OP_TFIELD_SET, target, literal, value);
}

// FMT_ABx / KOP_ABx_LAYOUT : R[A]:Instance.gc.extra = packed type args from Bx
static inline int xemit_inst_type_args(XrEmitter *e, int dst, int layout_id) {
    return emit_abx(e, OP_INST_TYPE_ARGS, dst, layout_id);
}

// FMT_ABC / KOP_SPECIAL : tail recursion: R[0..B-1]=R[A+1..A+B]; PC=entry
static inline int xemit_loop_back(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_LOOP_BACK, a, b, c);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = alloc struct in struct_area (B=class reg, C=slot offset)
static inline int xemit_new_struct(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_NEW_STRUCT, dst, src, literal);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = struct(R[B]).field[C]
static inline int xemit_struct_get(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_STRUCT_GET, dst, src, literal);
}

// FMT_ABC / KOP_ABC_INPLACE_LIT : struct(R[A]).field[B] = R[C]
static inline int xemit_struct_set(XrEmitter *e, int target, int literal, int value) {
    return emit_abc(e, OP_STRUCT_SET, target, literal, value);
}

// FMT_ABC / KOP_ABC_BIN_LIT : R[A] = memcpy struct R[B] into struct_area slot C
static inline int xemit_struct_copy(XrEmitter *e, int dst, int src, int literal) {
    return emit_abc(e, OP_STRUCT_COPY, dst, src, literal);
}

// FMT_SPECIAL / KOP_SPECIAL : no-op / spawn metadata
static inline int xemit_nop(XrEmitter *e, int a, int b, int c) {
    return emit_abc(e, OP_NOP, a, b, c);
}


#endif // XEMIT_TYPED_H
