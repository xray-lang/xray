/*
 * test_xi_emit.c - Unit tests for Xi IR to bytecode emitter
 *
 * Tests register allocation, instruction selection, block linearization,
 * phi elimination, and jump patching.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_emit.h"
#include "../../../src/runtime/value/xchunk.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* Minimal XrType stubs */
static XrType stub_int  = { .kind = XR_KIND_INT,    .id = 1, .frozen = true };
static XrType stub_float= { .kind = XR_KIND_FLOAT,  .id = 2, .frozen = true };
static XrType stub_bool = { .kind = XR_KIND_BOOL,   .id = 3, .frozen = true };
static XrType stub_null = { .kind = XR_KIND_NULL,   .id = 4, .frozen = true };
static XrType stub_void = { .kind = XR_KIND_VOID,   .id = 6, .frozen = true };
static XrType stub_string={ .kind = XR_KIND_STRING, .id = 5, .frozen = true };

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        printf("--- " #name " ---\n"); \
        test_##name(); \
        printf("  PASS\n"); \
        tests_passed++; \
    } \
    static void test_##name(void)

/* Helper: create function with sealed entry block */
static XiFunc *make_func(const char *name, XrType *ret) {
    XiFunc *f = xi_func_new(name, ret);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* ========== Basic Emission Tests ========== */

TEST(emit_return_const_int) {
    /* fn() { return 42 } */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *c = xi_const_int(f, entry, 42, &stub_int);
    xi_block_set_return(entry, c);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && "emit should succeed");
    assert(proto != NULL);

    /* Should have: LOADI rX, 42; RETURN1 rX */
    int count = PROTO_CODE_COUNT(proto);
    assert(count == 2 && "expected 2 instructions");

    XrInstruction i0 = PROTO_CODE(proto, 0);
    assert(GET_OPCODE(i0) == OP_LOADI && "first should be LOADI");
    assert(GETARG_sBx(i0) == 42 && "should load 42");

    XrInstruction i1 = PROTO_CODE(proto, 1);
    assert(GET_OPCODE(i1) == OP_RETURN1 && "second should be RETURN1");
    assert(GETARG_A(i1) == GETARG_A(i0) && "return same register as loaded");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_return_void) {
    /* fn() { return } */
    XiFunc *f = make_func("test", &stub_void);
    XiBlock *entry = f->entry;
    xi_block_set_return(entry, NULL);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK);
    assert(proto != NULL);

    int count = PROTO_CODE_COUNT(proto);
    assert(count == 1);
    assert(GET_OPCODE(PROTO_CODE(proto, 0)) == OP_RETURN0);

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_const_bool) {
    /* fn() { return true } */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *t = xi_const_bool(f, entry, true, &stub_bool);
    xi_block_set_return(entry, t);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    XrInstruction i0 = PROTO_CODE(proto, 0);
    assert(GET_OPCODE(i0) == OP_LOADTRUE);

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_const_null) {
    /* fn() { return null } */
    XiFunc *f = make_func("test", &stub_null);
    XiBlock *entry = f->entry;

    XiValue *n = xi_value_new(f, entry, XI_CONST, &stub_null, 0);
    n->aux_int = 0;
    xi_block_set_return(entry, n);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    XrInstruction i0 = PROTO_CODE(proto, 0);
    assert(GET_OPCODE(i0) == OP_LOADNULL);

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Arithmetic Tests ========== */

TEST(emit_add) {
    /* fn(a, b) { return a + b } */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, a, b);
    xi_block_set_return(entry, add);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);
    assert(proto->numparams == 2);

    /* Should have: PARAM, PARAM (no-ops), ADD, RETURN1 */
    /* Find ADD instruction */
    bool found_add = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_ADD) {
            found_add = true;
            XrInstruction inst = PROTO_CODE(proto, i);
            /* B and C should be param registers (0, 1) */
            assert(GETARG_B(inst) == 0 && "first arg should be R[0]");
            assert(GETARG_C(inst) == 1 && "second arg should be R[1]");
            break;
        }
    }
    assert(found_add && "should emit ADD instruction");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_sub_mul_div) {
    /* fn(a, b) { return (a - b) * (a / b) } */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *sub = xi_binary(f, entry, XI_SUB, &stub_int, a, b);
    XiValue *div = xi_binary(f, entry, XI_DIV, &stub_int, a, b);
    XiValue *mul = xi_binary(f, entry, XI_MUL, &stub_int, sub, div);
    xi_block_set_return(entry, mul);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    /* Verify all opcodes are present */
    bool has_sub = false, has_div = false, has_mul = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, i));
        if (op == OP_SUB) has_sub = true;
        if (op == OP_DIV) has_div = true;
        if (op == OP_MUL) has_mul = true;
    }
    assert(has_sub && has_div && has_mul && "should emit SUB, DIV, MUL");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_unary_neg) {
    /* fn(a) { return -a } */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *neg = xi_unary(f, entry, XI_NEG, &stub_int, a);
    xi_block_set_return(entry, neg);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_UNM) { found = true; break; }
    }
    assert(found && "should emit UNM");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Comparison Tests ========== */

TEST(emit_cmp_eq) {
    /* fn(a, b) { return a == b } */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *eq = xi_binary(f, entry, XI_EQ, &stub_bool, a, b);
    xi_block_set_return(entry, eq);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_CMP_EQ) { found = true; break; }
    }
    assert(found && "should emit CMP_EQ");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_cmp_gt) {
    /* fn(a, b) { return a > b } -- emits CMP_LT with swapped args */
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *b = xi_param(f, entry, 1, &stub_int);
    XiValue *gt = xi_binary(f, entry, XI_GT, &stub_bool, a, b);
    xi_block_set_return(entry, gt);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (GET_OPCODE(inst) == OP_CMP_LT) {
            found = true;
            /* a > b = b < a: B=b_reg(1), C=a_reg(0) */
            assert(GETARG_B(inst) == 1 && GETARG_C(inst) == 0 &&
                   "GT swaps to LT with reversed args");
            break;
        }
    }
    assert(found && "should emit CMP_LT for GT");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Control Flow Tests ========== */

TEST(emit_if_then_else) {
    /* fn(cond) { if cond then return 1 else return 2 } */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *cond = xi_param(f, entry, 0, &stub_bool);

    XiBlock *then_b = xi_block_new(f);
    then_b->sealed = true;
    XiBlock *else_b = xi_block_new(f);
    else_b->sealed = true;

    xi_block_set_if(entry, cond, then_b, else_b);

    XiValue *c1 = xi_const_int(f, then_b, 1, &stub_int);
    xi_block_set_return(then_b, c1);

    XiValue *c2 = xi_const_int(f, else_b, 2, &stub_int);
    xi_block_set_return(else_b, c2);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    /* Should have: TEST, JMP, LOADI 1, RETURN1, LOADI 2, RETURN1 */
    bool has_test = false, has_jmp = false;
    int ret_count = 0;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, i));
        if (op == OP_TEST) has_test = true;
        if (op == OP_JMP) has_jmp = true;
        if (op == OP_RETURN1) ret_count++;
    }
    assert(has_test && "should emit TEST");
    assert(has_jmp && "should emit JMP");
    assert(ret_count == 2 && "should have 2 RETURN1 instructions");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_jump_fallthrough) {
    /* entry -> b1 -> return; test that unnecessary JMP is elided */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;
    XiBlock *b1 = xi_block_new(f);
    b1->sealed = true;

    xi_block_set_jump(entry, b1);
    XiValue *c = xi_const_int(f, b1, 99, &stub_int);
    xi_block_set_return(b1, c);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    /* Should have: LOADI 99, RETURN1 — no JMP since b1 is the next block */
    int jmp_count = 0;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_JMP) jmp_count++;
    }
    assert(jmp_count == 0 && "should elide fallthrough JMP");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Copy / Move Tests ========== */

TEST(emit_copy_becomes_move) {
    /* fn(a) { b = copy(a); return b } */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *cp = xi_value_new(f, entry, XI_COPY, &stub_int, 1);
    cp->args[0] = a;
    xi_block_set_return(entry, cp);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    /* Should have MOVE + RETURN1 */
    bool found_move = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_MOVE) { found_move = true; break; }
    }
    assert(found_move && "COPY should emit MOVE");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Float Constants ========== */

TEST(emit_const_float_small) {
    /* fn() { return 3.0 } - uses LOADF */
    XiFunc *f = make_func("test", &stub_float);
    XiBlock *entry = f->entry;

    XiValue *cf = xi_const_float(f, entry, 3.0, &stub_float);
    xi_block_set_return(entry, cf);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    XrInstruction i0 = PROTO_CODE(proto, 0);
    assert(GET_OPCODE(i0) == OP_LOADF && "small float should use LOADF");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_const_float_large) {
    /* fn() { return 3.14 } - uses LOADK (not integer-representable) */
    XiFunc *f = make_func("test", &stub_float);
    XiBlock *entry = f->entry;

    XiValue *cf = xi_const_float(f, entry, 3.14, &stub_float);
    xi_block_set_return(entry, cf);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    XrInstruction i0 = PROTO_CODE(proto, 0);
    assert(GET_OPCODE(i0) == OP_LOADK && "non-integer float should use LOADK");
    assert(PROTO_CONST_COUNT(proto) == 1 && "should have 1 constant");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Large Constants ========== */

TEST(emit_const_int_large) {
    /* fn() { return 100000 } - uses LOADK since > sBx range */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *c = xi_const_int(f, entry, 100000, &stub_int);
    xi_block_set_return(entry, c);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    XrInstruction i0 = PROTO_CODE(proto, 0);
    assert(GET_OPCODE(i0) == OP_LOADK && "large int should use LOADK");
    assert(PROTO_CONST_COUNT(proto) == 1);

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Optimization + Emit ========== */

TEST(emit_after_optimization) {
    /* fn(a) { return a + 0 }
     * Strength reduction: a + 0 -> copy(a)
     * Copy propagation: copy(a) -> a
     * Result: just return a */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *c0 = xi_const_int(f, entry, 0, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, a, c0);
    xi_block_set_return(entry, add);

    xi_opt_run(f);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    /* After optimization, should have minimal instructions */
    int count = PROTO_CODE_COUNT(proto);
    /* Should be 1-2 instructions: maybe just RETURN1 R[0], or MOVE + RETURN1 */
    assert(count <= 3 && "optimized emit should be compact");

    /* Should NOT have ADD */
    for (int i = 0; i < count; i++) {
        assert(GET_OPCODE(PROTO_CODE(proto, i)) != OP_ADD &&
               "ADD should be eliminated by strength reduction");
    }

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Register Recycling ========== */

TEST(emit_reg_recycling) {
    /* Build a chain: p0, p1 are params.
     * t1 = p0 + p1    (p0, p1 last used here -> freed)
     * t2 = t1 + t1    (reuses recycled regs for t2)
     * t3 = t2 + t2
     * return t3
     * With recycling, maxstacksize should be <= 4 (2 params + 2 temps)
     * Without recycling it would be 6 (2 params + 4 temps). */
    XiFunc *f = make_func("recyc", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *p0 = xi_param(f, entry, 0, &stub_int);
    XiValue *p1 = xi_param(f, entry, 1, &stub_int);
    XiValue *t1 = xi_binary(f, entry, XI_ADD, &stub_int, p0, p1);
    XiValue *t2 = xi_binary(f, entry, XI_ADD, &stub_int, t1, t1);
    XiValue *t3 = xi_binary(f, entry, XI_ADD, &stub_int, t2, t2);
    xi_block_set_return(entry, t3);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    /* After recycling, dead temps' regs are reused.
     * maxstacksize should be at most 4. */
    assert(proto->maxstacksize <= 4 &&
           "register recycling should keep maxstacksize <= 4");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_reg_pressure) {
    /* Many independent values in sequence — all die immediately.
     * With recycling, only need ~3 registers at any time. */
    XiFunc *f = make_func("pressure", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *p0 = xi_param(f, entry, 0, &stub_int);
    XiValue *p1 = xi_param(f, entry, 1, &stub_int);

    /* Build 20 sequential adds: each uses prev + p1, prev dies */
    XiValue *prev = p0;
    for (int i = 0; i < 20; i++) {
        prev = xi_binary(f, entry, XI_ADD, &stub_int, prev, p1);
    }
    xi_block_set_return(entry, prev);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    /* p1 is live throughout (used in every ADD), so we need:
     * 2 params + at most 2 temps = 4 regs max. */
    assert(proto->maxstacksize <= 4 &&
           "sequential chain should recycle intermediates");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Instruction Fusion ========== */

TEST(emit_addi_rhs_const) {
    /* fn(a) { return a + 5 } -> should emit ADDI, not LOADI + ADD */
    XiFunc *f = make_func("addi", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *c5 = xi_const_int(f, entry, 5, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, a, c5);
    xi_block_set_return(entry, add);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found_addi = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_ADDI) {
            found_addi = true;
            XrInstruction inst = PROTO_CODE(proto, i);
            assert(GETARG_B(inst) == 0 && "src should be param R[0]");
            assert(GETARG_sC(inst) == 5 && "immediate should be 5");
        }
    }
    assert(found_addi && "a + 5 should fuse into ADDI");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_addi_lhs_const) {
    /* fn(a) { return 3 + a } -> commutative, should emit ADDI */
    XiFunc *f = make_func("addi_swap", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *c3 = xi_const_int(f, entry, 3, &stub_int);
    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, c3, a);
    xi_block_set_return(entry, add);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found_addi = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_ADDI) {
            found_addi = true;
            assert(GETARG_sC(PROTO_CODE(proto, i)) == 3);
        }
    }
    assert(found_addi && "3 + a should fuse into ADDI (commutative)");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_subi_muli) {
    /* fn(a) { return (a - 1) * 2 } -> SUBI then MULI */
    XiFunc *f = make_func("sub_mul", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *c1 = xi_const_int(f, entry, 1, &stub_int);
    XiValue *c2 = xi_const_int(f, entry, 2, &stub_int);
    XiValue *sub = xi_binary(f, entry, XI_SUB, &stub_int, a, c1);
    XiValue *mul = xi_binary(f, entry, XI_MUL, &stub_int, sub, c2);
    xi_block_set_return(entry, mul);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found_subi = false, found_muli = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, i));
        if (op == OP_SUBI) {
            found_subi = true;
            assert(GETARG_sC(PROTO_CODE(proto, i)) == 1);
        }
        if (op == OP_MULI) {
            found_muli = true;
            assert(GETARG_sC(PROTO_CODE(proto, i)) == 2);
        }
    }
    assert(found_subi && "a - 1 should fuse into SUBI");
    assert(found_muli && "(a-1) * 2 should fuse into MULI");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_addi_negative) {
    /* fn(a) { return a + (-10) } -> ADDI with negative immediate */
    XiFunc *f = make_func("addi_neg", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *cn = xi_const_int(f, entry, -10, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, a, cn);
    xi_block_set_return(entry, add);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found_addi = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_ADDI) {
            found_addi = true;
            assert(GETARG_sC(PROTO_CODE(proto, i)) == -10);
        }
    }
    assert(found_addi && "a + (-10) should fuse into ADDI");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_addk_large_const) {
    /* fn(a) { return a + 1000 } -> ADDK (too large for ADDI's int8_t) */
    XiFunc *f = make_func("addk", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *ck = xi_const_int(f, entry, 1000, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, a, ck);
    xi_block_set_return(entry, add);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found_addk = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_ADDK) {
            found_addk = true;
        }
    }
    assert(found_addk && "a + 1000 should use ADDK (const pool)");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== New Op Coverage ========== */

TEST(emit_str_concat) {
    /* STR_CONCAT with 2 parts -> STRBUF_NEW + 2*STRBUF_APPEND + STRBUF_FINISH */
    XiFunc *f = make_func("concat", &stub_string);
    XiBlock *entry = f->entry;

    XiValue *s1 = xi_const_str(f, entry, "hello", &stub_string);
    XiValue *s2 = xi_const_str(f, entry, " world", &stub_string);

    XiValue *v = xi_value_new(f, entry, XI_STR_CONCAT, &stub_string, 2);
    assert(v != NULL);
    v->args[0] = s1;
    v->args[1] = s2;
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    /* Verify sequence: STRBUF_NEW, STRBUF_APPEND, STRBUF_APPEND, STRBUF_FINISH */
    bool found_new = false, found_append = false, found_finish = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, i));
        if (op == OP_STRBUF_NEW) found_new = true;
        if (op == OP_STRBUF_APPEND) found_append = true;
        if (op == OP_STRBUF_FINISH) found_finish = true;
    }
    assert(found_new && "should have STRBUF_NEW");
    assert(found_append && "should have STRBUF_APPEND");
    assert(found_finish && "should have STRBUF_FINISH");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_closure_new) {
    /* CLOSURE_NEW -> OP_CLOSURE */
    XiFunc *f = make_func("parent", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *v = xi_value_new(f, entry, XI_CLOSURE_NEW, &stub_int, 0);
    assert(v != NULL);
    v->aux_int = 0;  /* proto index 0 */
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_CLOSURE) {
            found = true; break;
        }
    }
    assert(found && "CLOSURE_NEW should emit OP_CLOSURE");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_set_new) {
    /* SET_NEW -> OP_NEWSET */
    XiFunc *f = make_func("mkset", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *cap = xi_const_int(f, entry, 4, &stub_int);
    XiValue *v = xi_value_new(f, entry, XI_SET_NEW, &stub_int, 1);
    assert(v != NULL);
    v->args[0] = cap;
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_NEWSET) {
            found = true; break;
        }
    }
    assert(found && "SET_NEW should emit OP_NEWSET");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_is_check) {
    /* IS -> OP_IS */
    XiFunc *f = make_func("typecheck", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *p0 = xi_param(f, entry, 0, &stub_int);
    XiValue *v = xi_value_new(f, entry, XI_IS, &stub_bool, 1);
    assert(v != NULL);
    v->args[0] = p0;
    v->aux_int = 1;  /* type_id for int */
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_IS) {
            found = true; break;
        }
    }
    assert(found && "XI_IS should emit OP_IS");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

TEST(emit_cancelled_builtin) {
    /* CALL_BUILTIN(0) -> OP_CANCELLED */
    XiFunc *f = make_func("chk", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *v = xi_value_new(f, entry, XI_CALL_BUILTIN, &stub_bool, 0);
    assert(v != NULL);
    v->aux_int = 0;  /* cancelled() */
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_CANCELLED) {
            found = true; break;
        }
    }
    assert(found && "CALL_BUILTIN(0) should emit OP_CANCELLED");

    xr_vm_proto_free(proto);
    xi_func_free(f);
}

/* ========== Error Handling ========== */

TEST(emit_status_str) {
    assert(strcmp(xi_emit_status_str(XI_EMIT_OK), "OK") == 0);
    assert(strcmp(xi_emit_status_str(XI_EMIT_ERR_TOO_MANY_REGS),
                  "too many registers (>255)") == 0);
    assert(strcmp(xi_emit_status_str(XI_EMIT_ERR_UNSUPPORTED_OP),
                  "unsupported Xi IR operation") == 0);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Emit Unit Tests ===\n\n");

    (void)stub_null;
    (void)stub_void;

    /* Basic emission */
    run_emit_return_const_int();
    run_emit_return_void();
    run_emit_const_bool();
    run_emit_const_null();

    /* Arithmetic */
    run_emit_add();
    run_emit_sub_mul_div();
    run_emit_unary_neg();

    /* Comparison */
    run_emit_cmp_eq();
    run_emit_cmp_gt();

    /* Control flow */
    run_emit_if_then_else();
    run_emit_jump_fallthrough();

    /* Copy / Move */
    run_emit_copy_becomes_move();

    /* Float constants */
    run_emit_const_float_small();
    run_emit_const_float_large();

    /* Large constants */
    run_emit_const_int_large();

    /* Optimization + emit */
    run_emit_after_optimization();

    /* Register recycling */
    run_emit_reg_recycling();
    run_emit_reg_pressure();

    /* Instruction fusion */
    run_emit_addi_rhs_const();
    run_emit_addi_lhs_const();
    run_emit_subi_muli();
    run_emit_addi_negative();
    run_emit_addk_large_const();

    /* New op coverage */
    run_emit_str_concat();
    run_emit_closure_new();
    run_emit_set_new();
    run_emit_is_check();
    run_emit_cancelled_builtin();

    /* Error handling */
    run_emit_status_str();

    printf("\n=== %d/%d Xi Emit tests passed ===\n",
           tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
