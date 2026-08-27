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
#include "../../../src/runtime/class/xclass_descriptor.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/frontend/analyzer/xa_intrinsic_registry.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"
#include "../../../src/plan/semantic/xr_semantic_number_parse_error_shape.h"
#include "../../../include/xray_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* Minimal XrType stubs */
static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_float = {.kind = XR_KIND_FLOAT, .id = 2, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 3, .frozen = true};
static XrType stub_null = {.kind = XR_KIND_NULL, .id = 4, .frozen = true};
static XrType stub_void = {.kind = XR_KIND_UNIT, .id = 6, .frozen = true};
#define XI_EMIT_TEST_NULL_PROJECTION_OPCODE OP_LOADNULL
#define XI_EMIT_TEST_VOID_RETURN_OPCODE OP_RETURN0
static XrType stub_string = {.kind = XR_KIND_STRING, .id = 5, .frozen = true};
static XrType stub_uint64 = {
    .kind = XR_KIND_INT, .id = 8, .frozen = true, .scalar_rep = XR_NATIVE_U64};
static XrType stub_float64 = {
    .kind = XR_KIND_FLOAT, .id = 9, .frozen = true, .scalar_rep = XR_NATIVE_F64};

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

#define TEST_REQUIRE(condition)                                                                    \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "test requirement failed: %s (%s:%d)\n", #condition, __FILE__,       \
                    __LINE__);                                                                     \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* Helper: create function with sealed entry block */
static XiFunc *make_func(const char *name, XrType *ret) {
    XiFunc *f = xi_func_new(name, ret);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

static XrVMRuntime *new_test_isolate(void) {
    XrVMConfig p = {0};
    return xray_vm_new_full(&p);
}

/* ========== Basic Emission Tests ========== */

TEST(class_descriptor_constants_use_opaque_pointer_identity) {
    XrProto *proto = xr_instruction_unit_new();
    assert(proto != NULL);

    /* The first bytes of a descriptor are a class-name pointer, not an
     * XrObjHeader.  These hostile values would make XR_FROM_PTR misclassify
     * both descriptors as strings and permit deep-equality to merge them. */
    XrClassDescriptor first = {0};
    XrClassDescriptor second = {0};
    first.class_name = (const char *) (uintptr_t) XR_TSTRING;
    second.class_name = (const char *) (uintptr_t) XR_TSTRING;

    int first_index = xr_instruction_unit_add_class_descriptor_constant(proto, &first);
    int first_again = xr_instruction_unit_add_class_descriptor_constant(proto, &first);
    int second_index = xr_instruction_unit_add_class_descriptor_constant(proto, &second);

    assert(first_index == 0);
    assert(first_again == first_index);
    assert(second_index == 1);
    assert(PROTO_CONST_COUNT(proto) == 2);
    XrValue first_value = PROTO_CONSTANT(proto, first_index);
    XrValue second_value = PROTO_CONSTANT(proto, second_index);
    assert(XR_IS_PTR(first_value) && first_value.heap_type == 0 &&
           XR_TO_PTR(first_value) == &first);
    assert(XR_IS_PTR(second_value) && second_value.heap_type == 0 &&
           XR_TO_PTR(second_value) == &second);

    xr_instruction_unit_free(proto);
}

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

    xr_instruction_unit_free(proto);
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
    assert(GET_OPCODE(PROTO_CODE(proto, 0)) == XI_EMIT_TEST_VOID_RETURN_OPCODE);

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_target_layout_queries_use_canonical_target_layout) {
    XiFunc *f = make_func("target-layout", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *size = xi_value_new(f, entry, XI_TARGET_SIZEOF, &stub_int, 0);
    XiValue *align = xi_value_new(f, entry, XI_TARGET_ALIGNOF, &stub_int, 0);
    assert(size != NULL && align != NULL);
    size->aux_int = XR_NATIVE_USIZE;
    align->aux_int = XR_NATIVE_ISIZE;
    XiValue *sum = xi_value_new(f, entry, XI_ADD, &stub_int, 2);
    assert(sum != NULL);
    sum->args[0] = size;
    sum->args[1] = align;
    xi_block_set_return(entry, sum);

    XrProto *proto = NULL;
    XiEmitStatus status = xi_emit(f, NULL, &proto);
    assert(status == XI_EMIT_OK && proto != NULL);
    int loads = 0;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (GET_OPCODE(inst) == OP_LOADI) {
            assert(GETARG_sBx(inst) == (int) sizeof(void *));
            loads++;
        }
    }
    assert(loads == 2);

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_unreachable_is_terminator) {
    XiFunc *f = make_func("unreachable", &stub_void);
    f->entry->kind = XI_BLOCK_UNREACHABLE;

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK);
    assert(proto != NULL);
    assert(PROTO_CODE_COUNT(proto) == 1);
    assert(GET_OPCODE(PROTO_CODE(proto, 0)) == OP_RETURN0 &&
           "UNREACHABLE bytecode must not fall through");

    xr_instruction_unit_free(proto);
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

    xr_instruction_unit_free(proto);
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
    assert(GET_OPCODE(i0) == XI_EMIT_TEST_NULL_PROJECTION_OPCODE);

    xr_instruction_unit_free(proto);
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

    xr_instruction_unit_free(proto);
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
        if (op == OP_SUB)
            has_sub = true;
        if (op == OP_DIV)
            has_div = true;
        if (op == OP_MUL)
            has_mul = true;
    }
    assert(has_sub && has_div && has_mul && "should emit SUB, DIV, MUL");

    xr_instruction_unit_free(proto);
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
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_UNM) {
            found = true;
            break;
        }
    }
    assert(found && "should emit UNM");

    xr_instruction_unit_free(proto);
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
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_CMP_EQ) {
            found = true;
            break;
        }
    }
    assert(found && "should emit CMP_EQ");

    xr_instruction_unit_free(proto);
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

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_uint64_cmp_uses_unsigned_opcode) {
    XiFunc *f = make_func("test", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_uint64);
    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *gt = xi_binary(f, entry, XI_GT, &stub_bool, a, zero);
    xi_block_set_return(entry, gt);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (GET_OPCODE(inst) == OP_CMP_LTU) {
            found = true;
            assert(GETARG_C(inst) == 0 && "u64 GT should compare 0 < param with unsigned op");
            break;
        }
    }
    assert(found && "u64 compare should emit CMP_LTU");

    xr_instruction_unit_free(proto);
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
        if (op == OP_TEST)
            has_test = true;
        if (op == OP_JMP)
            has_jmp = true;
        if (op == OP_RETURN1)
            ret_count++;
    }
    assert(has_test && "should emit TEST");
    assert(has_jmp && "should emit JMP");
    assert(ret_count == 2 && "should have 2 RETURN1 instructions");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_reused_cmp_control_materializes_bool) {
    /* A compare used by more than one block control must produce a bool value.
     * Branch fusion is valid only when the current block's control is the
     * compare's sole consumer. */
    XiFunc *f = make_func("reused_cmp", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *gt = xi_binary(f, entry, XI_GT, &stub_bool, a, zero);

    XiBlock *then_once = xi_block_new(f);
    then_once->sealed = true;
    XiBlock *merge = xi_block_new(f);
    merge->sealed = true;
    XiBlock *then_b = xi_block_new(f);
    then_b->sealed = true;
    XiBlock *else_b = xi_block_new(f);
    else_b->sealed = true;

    xi_block_set_if(entry, gt, then_once, merge);
    xi_block_set_jump(then_once, merge);
    xi_block_set_if(merge, gt, then_b, else_b);

    XiValue *one = xi_const_int(f, then_b, 1, &stub_int);
    xi_block_set_return(then_b, one);
    XiValue *two = xi_const_int(f, else_b, 2, &stub_int);
    xi_block_set_return(else_b, two);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found_cmp = false;
    uint32_t cmp_reg = 0;
    int tests_of_cmp = 0;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        OpCode op = GET_OPCODE(inst);
        if (op == OP_CMP_LT) {
            found_cmp = true;
            cmp_reg = GETARG_A(inst);
        } else if (op == OP_TEST && found_cmp && GETARG_A(inst) == cmp_reg) {
            tests_of_cmp++;
        }
    }
    assert(found_cmp && "reused compare must emit a materialized CMP_* bool");
    assert(tests_of_cmp == 2 && "both branches should test the same materialized compare");

    xr_instruction_unit_free(proto);
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
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_JMP)
            jmp_count++;
    }
    assert(jmp_count == 0 && "should elide fallthrough JMP");

    xr_instruction_unit_free(proto);
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
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_MOVE) {
            found_move = true;
            break;
        }
    }
    assert(found_move && "COPY should emit MOVE");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_codegen_compiler_fence_projects_to_void_without_runtime_effect) {
    XiFunc *f = make_func("compiler_fence", &stub_void);
    XiBlock *entry = f->entry;
    (void) xi_value_new(f, entry, XI_CODEGEN_COMPILER_FENCE, &stub_void, 0);
    xi_block_set_return(entry, NULL);

    XrProto *proto = NULL;
    XiEmitStatus status = xi_emit(f, NULL, &proto);
    assert(status == XI_EMIT_OK);
    assert(proto != NULL);
    assert(PROTO_CODE_COUNT(proto) == 2);
    assert(GET_OPCODE(PROTO_CODE(proto, 0)) == XI_EMIT_TEST_NULL_PROJECTION_OPCODE);
    assert(GET_OPCODE(PROTO_CODE(proto, 1)) == XI_EMIT_TEST_VOID_RETURN_OPCODE);

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_numeric_conversion_packs_typed_witness) {
    XiFunc *f = make_func("convert", &stub_float64);
    XiBlock *entry = f->entry;
    XiValue *source = xi_param(f, entry, 0, &stub_uint64);
    XiValue *convert = xi_value_new(f, entry, XI_CONVERT, &stub_float64, 1);
    convert->args[0] = source;
    convert->conversion = (XrConversionWitness) {
        .kind = XR_CONVERSION_EXPLICIT_INT_FLOAT,
        .source_scalar_rep = XR_NATIVE_U64,
        .target_scalar_rep = XR_NATIVE_F64,
        .is_implicit = false,
        .is_compile_time = false,
    };
    xi_block_set_return(entry, convert);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (GET_OPCODE(inst) != OP_TOFLOAT)
            continue;
        uint16_t packed = (uint16_t) GETARG_C(inst);
        assert(xr_conversion_bytecode_is_numeric(packed));
        assert(!xr_conversion_bytecode_is_parse_required(packed));
        assert(!xr_conversion_bytecode_is_parse_optional(packed));
        assert(xr_conversion_bytecode_kind(packed) == XR_CONVERSION_EXPLICIT_INT_FLOAT);
        assert(xr_conversion_bytecode_source_rep(packed) == XR_NATIVE_U64);
        assert(xr_conversion_bytecode_target_rep(packed) == XR_NATIVE_F64);
        assert(xr_conversion_bytecode_pointer_bits(packed) == 64);
        found = true;
    }
    assert(found && "typed integer-to-f64 witness must reach VM bytecode");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(conversion_bytecode_modes_are_disjoint) {
    for (int kind = XR_CONVERSION_IDENTITY; kind <= XR_CONVERSION_EXPLICIT_INT_FLOAT; kind++) {
        XrConversionWitness witness = {
            .kind = (XrConversionKind) kind,
            .source_scalar_rep = XR_NATIVE_F64,
            .target_scalar_rep = XR_NATIVE_I64,
        };
        uint16_t packed = xr_conversion_bytecode_pack(&witness, 64);
        assert(xr_conversion_bytecode_is_numeric(packed));
        assert(!xr_conversion_bytecode_is_parse_required(packed));
        assert(!xr_conversion_bytecode_is_parse_optional(packed));
    }
    assert(!xr_conversion_bytecode_is_numeric(XR_CONVERSION_BC_PARSE_REQUIRED));
    assert(xr_conversion_bytecode_is_parse_required(XR_CONVERSION_BC_PARSE_REQUIRED));
    assert(!xr_conversion_bytecode_is_parse_optional(XR_CONVERSION_BC_PARSE_REQUIRED));
    assert(!xr_conversion_bytecode_is_numeric(XR_CONVERSION_BC_PARSE_OPTIONAL));
    assert(!xr_conversion_bytecode_is_parse_required(XR_CONVERSION_BC_PARSE_OPTIONAL));
    assert(xr_conversion_bytecode_is_parse_optional(XR_CONVERSION_BC_PARSE_OPTIONAL));
}

TEST(number_parse_error_builtin_identity_requires_matching_typed_metadata) {
    const XrNumberParseErrorRegistryRow *row =
        xr_number_parse_error_registry_row(XR_GLOBAL_VAR_NUMBER_PARSE_ERROR);
    assert(row != NULL);
    assert(row->global_index == 30);
    assert(xr_number_parse_error_registry_row(29) == NULL);
    assert(xr_number_parse_error_registry_row(31) == NULL);

    char namespace_type_key[192];
    int written = snprintf(namespace_type_key, sizeof(namespace_type_key),
                           "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:%u:%s[0]",
                           (unsigned) XR_KIND_CLASS, (unsigned) XR_TID_NULL,
                           (unsigned) XR_SCALAR_REP_NONE, (unsigned) strlen(row->enum_name),
                           row->enum_name);
    assert(written > 0 && (size_t) written < sizeof(namespace_type_key));

    XrSemanticTypeRecord namespace_type = {
        .canonical_key = namespace_type_key,
        .kind = XR_KIND_CLASS,
        .builtin_type = XR_TID_NULL,
        .source_class = XR_SEMANTIC_INDEX_NONE,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };
    const char *metadata[] = {row->enum_name};
    XrSemanticOperationRecord namespace_load = {
        .result_type = 0,
        .opcode = XI_GET_BUILTIN,
        .metadata_count = 1,
        .semantic_immediate = XR_GLOBAL_VAR_NUMBER_PARSE_ERROR,
        .constant = XR_SEMANTIC_INDEX_NONE,
        .callable_function = XR_SEMANTIC_INDEX_NONE,
        .effects = xi_generated_op_effects(XI_GET_BUILTIN),
        .flags = xi_generated_op_default_flags(XI_GET_BUILTIN),
        .result_alias_operand = -1,
    };
    XrSemanticPlan plan = {0};
    plan.types = &namespace_type;
    plan.type_count = 1;
    plan.operations = &namespace_load;
    plan.operation_count = 1;
    plan.metadata = metadata;
    plan.metadata_count = 1;

    assert(xr_semantic_number_parse_error_namespace_is_exact(&plan, &namespace_load));

    namespace_load.semantic_immediate = XR_GLOBAL_VAR_NUMBER_PARSE_ERROR + 1;
    assert(!xr_semantic_number_parse_error_namespace_is_exact(&plan, &namespace_load));
    namespace_load.semantic_immediate = XR_GLOBAL_VAR_NUMBER_PARSE_ERROR;

    const char *saved_metadata = metadata[0];
    metadata[0] = "Utf8Error";
    assert(!xr_semantic_number_parse_error_namespace_is_exact(&plan, &namespace_load));
    metadata[0] = saved_metadata;
    namespace_load.metadata_count = 0;
    assert(!xr_semantic_number_parse_error_namespace_is_exact(&plan, &namespace_load));
    namespace_load.metadata_count = 1;

    const char *saved_type_key = namespace_type.canonical_key;
    namespace_type.canonical_key = "type-v3:forged:NumberParseError";
    assert(!xr_semantic_number_parse_error_namespace_is_exact(&plan, &namespace_load));
    namespace_type.canonical_key = saved_type_key;
    namespace_type.kind = XR_KIND_ENUM;
    assert(!xr_semantic_number_parse_error_namespace_is_exact(&plan, &namespace_load));
    namespace_type.kind = XR_KIND_CLASS;

    assert(xr_semantic_number_parse_error_namespace_is_exact(&plan, &namespace_load));

    char enum_key[256];
    written = snprintf(enum_key, sizeof(enum_key),
                       "source-enum-v1:schema=%u:owner=%u:%s:name=%u:%s:members=2:"
                       "m0=%u:%s:payloads=0:m1=%u:%s:payloads=0",
                       (unsigned) XR_SEMANTIC_SCHEMA_VERSION,
                       (unsigned) strlen(row->nominal_owner), row->nominal_owner,
                       (unsigned) strlen(row->enum_name), row->enum_name,
                       (unsigned) strlen(row->members[XR_NUMBER_PARSE_ERROR_INVALID_SYNTAX]),
                       row->members[XR_NUMBER_PARSE_ERROR_INVALID_SYNTAX],
                       (unsigned) strlen(row->members[XR_NUMBER_PARSE_ERROR_OUT_OF_RANGE]),
                       row->members[XR_NUMBER_PARSE_ERROR_OUT_OF_RANGE]);
    assert(written > 0 && (size_t) written < sizeof(enum_key));
    XrStableId enum_identity = {{0}};
    XrFingerprint enum_digest = {{0}};
    assert(xr_stable_id_from_key(enum_key, &enum_identity, &enum_digest));
    XrSemanticTypeRecord enum_type = {
        .source_enum_identity = enum_identity,
        .source_enum_key = enum_key,
        .kind = XR_KIND_ENUM,
        .builtin_type = XR_TID_NULL,
        .source_class = XR_SEMANTIC_INDEX_NONE,
        .enum_layout_id = row->enum_layout_id,
        .enum_member_count = XR_NUMBER_PARSE_ERROR_MEMBER_COUNT,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .enum_flags = XR_SEM_ENUM_DECLARATION_EXACT | XR_SEM_ENUM_UNIT,
    };
    plan.types = &enum_type;
    assert(xr_semantic_number_parse_error_type_is_exact(&plan, 0));

    enum_type.source_enum_identity = (XrStableId) {{0}};
    assert(!xr_semantic_number_parse_error_type_is_exact(&plan, 0));
    enum_type.source_enum_identity = enum_identity;
    const char *saved_enum_key = enum_type.source_enum_key;
    enum_type.source_enum_key = "source-enum-v1:forged:NumberParseError";
    assert(!xr_semantic_number_parse_error_type_is_exact(&plan, 0));
    enum_type.source_enum_key = saved_enum_key;
    enum_type.enum_member_count = XR_NUMBER_PARSE_ERROR_MEMBER_COUNT - 1;
    assert(!xr_semantic_number_parse_error_type_is_exact(&plan, 0));
    enum_type.enum_member_count = XR_NUMBER_PARSE_ERROR_MEMBER_COUNT;
    enum_type.enum_layout_id ^= 1u;
    assert(!xr_semantic_number_parse_error_type_is_exact(&plan, 0));
    enum_type.enum_layout_id = row->enum_layout_id;
    assert(xr_semantic_number_parse_error_type_is_exact(&plan, 0));
}

TEST(emit_scalar_parse_intrinsics_use_exact_vm_modes) {
    XiFunc *f = make_func("parse-modes", &stub_float64);
    XiBlock *entry = f->entry;
    XiValue *source = xi_param(f, entry, 0, &stub_string);
    const XaIntrinsicId ids[] = {
        XA_INTRINSIC_I64_PARSE,
        XA_INTRINSIC_I64_TRY_PARSE,
        XA_INTRINSIC_F64_PARSE,
        XA_INTRINSIC_F64_TRY_PARSE,
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        bool integer = ids[i] == XA_INTRINSIC_I64_PARSE ||
                       ids[i] == XA_INTRINSIC_I64_TRY_PARSE;
        XiValue *convert =
            xi_value_new(f, entry, XI_CONVERT, integer ? &stub_int : &stub_float64, 1);
        assert(convert != NULL);
        convert->args[0] = source;
        convert->xa_intrinsic_id = ids[i];
        if (ids[i] == XA_INTRINSIC_I64_PARSE || ids[i] == XA_INTRINSIC_F64_PARSE)
            convert->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        if (i == 3)
            xi_block_set_return(entry, convert);
    }

    XrProto *proto = NULL;
    XiEmitStatus status = xi_emit(f, NULL, &proto);
    assert(status == XI_EMIT_OK && proto != NULL);
    int required_i64 = 0, optional_i64 = 0, required_f64 = 0, optional_f64 = 0;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction instruction = PROTO_CODE(proto, i);
        OpCode opcode = GET_OPCODE(instruction);
        uint16_t mode = (uint16_t) GETARG_C(instruction);
        if (opcode == OP_TOINT && xr_conversion_bytecode_is_parse_required(mode))
            required_i64++;
        else if (opcode == OP_TOINT && xr_conversion_bytecode_is_parse_optional(mode))
            optional_i64++;
        else if (opcode == OP_TOFLOAT && xr_conversion_bytecode_is_parse_required(mode))
            required_f64++;
        else if (opcode == OP_TOFLOAT && xr_conversion_bytecode_is_parse_optional(mode))
            optional_f64++;
    }
    assert(required_i64 == 1 && optional_i64 == 1 && required_f64 == 1 && optional_f64 == 1 &&
           "each stable scalar parse intrinsic must select one disjoint VM byte mode");

    xr_instruction_unit_free(proto);
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
    assert(GET_OPCODE(i0) == OP_LOADF && "small f64 should use LOADF");

    xr_instruction_unit_free(proto);
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
    assert(GET_OPCODE(i0) == OP_LOADK && "non-integer f64 should use LOADK");
    assert(PROTO_CONST_COUNT(proto) == 1 && "should have 1 constant");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

/* ========== Large Constants ========== */

TEST(emit_const_int_large) {
    /* fn() { return 100000 } - fits the widened sBx range, so uses LOADI. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *c = xi_const_int(f, entry, 100000, &stub_int);
    xi_block_set_return(entry, c);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    XrInstruction i0 = PROTO_CODE(proto, 0);
    assert(GET_OPCODE(i0) == OP_LOADI && "widened sBx i64 should use LOADI");
    assert(GETARG_sBx(i0) == 100000);
    assert(PROTO_CONST_COUNT(proto) == 0);

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_const_int_beyond_sbx) {
    /* fn() { return MAXARG_sBx + 1 } - uses LOADK outside sBx range. */
    XiFunc *f = make_func("test", &stub_int);
    XiBlock *entry = f->entry;

    int64_t large = (int64_t) MAXARG_sBx + 1;
    XiValue *c = xi_const_int(f, entry, large, &stub_int);
    xi_block_set_return(entry, c);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    XrInstruction i0 = PROTO_CODE(proto, 0);
    assert(GET_OPCODE(i0) == OP_LOADK && "i64 beyond sBx should use LOADK");
    assert(PROTO_CONST_COUNT(proto) == 1);

    xr_instruction_unit_free(proto);
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

    xr_instruction_unit_free(proto);
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
    assert(proto->maxstacksize <= 4 && "register recycling should keep maxstacksize <= 4");

    xr_instruction_unit_free(proto);
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
    assert(proto->maxstacksize <= 4 && "sequential chain should recycle intermediates");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_param_register_above_255) {
    XiFunc *f = make_func("wide_params", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *last = NULL;
    for (uint16_t i = 0; i <= 300; i++)
        last = xi_param(f, entry, i, &stub_int);
    xi_block_set_return(entry, last);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);
    assert(proto->numparams == 301);
    assert(proto->maxstacksize >= 301);

    XrInstruction ret = PROTO_CODE(proto, PROTO_CODE_COUNT(proto) - 1);
    assert(GET_OPCODE(ret) == OP_RETURN1);
    assert(GETARG_A(ret) == 300);

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_coalesces_var_id_above_255) {
    XiFunc *f = make_func("wide_var_id", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *p0 = xi_param(f, entry, 0, &stub_int);
    p0->var_id = 300;
    XiValue *copy = xi_value_new(f, entry, XI_COPY, &stub_int, 1);
    assert(copy != NULL);
    copy->args[0] = p0;
    copy->var_id = 300;
    xi_block_set_return(entry, copy);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);
    assert(proto->maxstacksize == 1 && "same high var_id should reuse the param register");

    XrInstruction ret = PROTO_CODE(proto, PROTO_CODE_COUNT(proto) - 1);
    assert(GET_OPCODE(ret) == OP_RETURN1);
    assert(GETARG_A(ret) == 0);

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_select_preserves_param_slot_alias) {
    XiFunc *f = make_func("select_alias", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *p0 = xi_param(f, entry, 0, &stub_int);
    p0->var_id = 7;
    XiValue *cond = xi_const_bool(f, entry, false, &stub_bool);
    XiValue *default_val = xi_const_int(f, entry, 99, &stub_int);
    XiValue *select = xi_value_new(f, entry, XI_SELECT, &stub_int, 3);
    assert(select != NULL);
    select->args[0] = cond;
    select->args[1] = default_val;
    select->args[2] = p0;
    select->var_id = 7;
    xi_block_set_return(entry, select);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    int test_pc = -1;
    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        if (GET_OPCODE(inst) == OP_TEST) {
            test_pc = pc;
            break;
        }
        assert(!(GET_OPCODE(inst) == OP_MOVE && GETARG_A(inst) == 0) &&
               "SELECT must not overwrite the parameter slot before testing the condition");
    }
    assert(test_pc >= 0 && test_pc + 1 < PROTO_CODE_COUNT(proto));
    XrInstruction test_inst = PROTO_CODE(proto, test_pc);
    assert(GETARG_B(test_inst) == 1 &&
           "dst=false alias should move true arm only when cond is true");
    XrInstruction move_inst = PROTO_CODE(proto, test_pc + 1);
    assert(GET_OPCODE(move_inst) == OP_MOVE);
    assert(GETARG_A(move_inst) == 0 && "SELECT result should write back to the parameter slot");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_symbol_index_above_255) {
    XrVMRuntime *iso = new_test_isolate();
    assert(iso != NULL);

    XiFunc *f = make_func("wide_symbols", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *obj = xi_param(f, entry, 0, &stub_int);

    char names[302][32];
    XiValue *last_prop = NULL;
    for (int i = 0; i <= 300; i++) {
        snprintf(names[i], sizeof(names[i]), "prop_%03d", i);
        XiValue *load = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
        assert(load != NULL);
        load->args[0] = obj;
        load->aux = names[i];
        last_prop = load;
    }

    snprintf(names[301], sizeof(names[301]), "method_301");
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    assert(call != NULL);
    call->args[0] = obj;
    call->aux = names[301];
    call->aux_int = 0;
    xi_block_set_return(entry, call);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, iso, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);
    assert(PROTO_SYMBOL_COUNT(proto) == 302);

    bool found_getprop = false;
    bool found_invoke = false;
    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        if (GET_OPCODE(inst) == OP_GETPROP && GETARG_C(inst) == 300)
            found_getprop = true;
        if (GET_OPCODE(inst) == OP_INVOKE && GETARG_B(inst) == 301)
            found_invoke = true;
    }
    assert(last_prop != NULL);
    assert(found_getprop && "GETPROP should preserve a proto-local symbol index above 255");
    assert(found_invoke && "INVOKE should preserve a proto-local symbol index above 255");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
    xray_vm_delete(iso);
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

    xr_instruction_unit_free(proto);
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

    xr_instruction_unit_free(proto);
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

    xr_instruction_unit_free(proto);
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

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_addk_large_const) {
    /* fn(a) { return a + 40000 } -> ADDK (too large for ADDI's int16_t) */
    XiFunc *f = make_func("addk", &stub_int);
    XiBlock *entry = f->entry;

    XiValue *a = xi_param(f, entry, 0, &stub_int);
    XiValue *ck = xi_const_int(f, entry, 40000, &stub_int);
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
    assert(found_addk && "a + 40000 should use ADDK (const pool)");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

/* ========== New Op Coverage ========== */

TEST(emit_str_concat) {
    /* Two parts fit the bounded range form exactly. */
    XiFunc *f = make_func("concat", &stub_string);
    XiBlock *entry = f->entry;

    XiValue *s1 = xi_const_str(f, entry, "hello", &stub_string);
    XiValue *s2 = xi_const_str(f, entry, " world", &stub_string);

    XiValue *v = xi_value_new(f, entry, XI_STR_CONCAT, &stub_string, 2);
    TEST_REQUIRE(v != NULL);
    v->args[0] = s1;
    v->args[1] = s2;
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    TEST_REQUIRE(s == XI_EMIT_OK && proto != NULL);

    int concat_count = 0;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        OpCode op = GET_OPCODE(inst);
        if (op == OP_STR_CONCAT_N) {
            concat_count++;
            TEST_REQUIRE(GETARG_C(inst) == 2);
        }
    }
    TEST_REQUIRE(concat_count == 1);

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_str_concat_uint64_formats_before_range_concat) {
    XiFunc *f = make_func("concat_u64", &stub_string);
    XiBlock *entry = f->entry;

    XiValue *prefix = xi_const_str(f, entry, "u=", &stub_string);
    XiValue *u = xi_param(f, entry, 0, &stub_uint64);
    XiValue *v = xi_value_new(f, entry, XI_STR_CONCAT, &stub_string, 2);
    TEST_REQUIRE(v != NULL);
    v->args[0] = prefix;
    v->args[1] = u;
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    TEST_REQUIRE(s == XI_EMIT_OK && proto != NULL);

    bool found_tostring_u64 = false;
    int concat_count = 0;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (GET_OPCODE(inst) == OP_TOSTRING && GETARG_C(inst) == 3)
            found_tostring_u64 = true;
        if (GET_OPCODE(inst) == OP_STR_CONCAT_N) {
            concat_count++;
            TEST_REQUIRE(GETARG_C(inst) == 2);
        }
    }
    TEST_REQUIRE(found_tostring_u64);
    TEST_REQUIRE(concat_count == 1);

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_closure_new) {
    /* CLOSURE_NEW -> OP_CLOSURE with recursive child emit */
    XiFunc *f = make_func("parent", &stub_int);
    XiBlock *entry = f->entry;

    /* Create a minimal child func that just returns a constant */
    XiFunc *child = xi_func_new("child", &stub_int);
    assert(child != NULL);
    XiBlock *child_entry = xi_block_new(child);
    child_entry->sealed = true;
    XiValue *c42 = xi_const_int(child, child_entry, 42, &stub_int);
    xi_block_set_return(child_entry, c42);

    /* Register child in parent's children array (mirrors real lowering) */
    if (f->nchildren >= f->children_cap) {
        uint16_t nc = f->children_cap ? f->children_cap * 2 : 4;
        XiFunc **tmp = (XiFunc **) xr_realloc(f->children, nc * sizeof(XiFunc *));
        assert(tmp != NULL);
        f->children = tmp;
        f->children_cap = nc;
    }
    f->children[f->nchildren++] = child;
    uint16_t child_idx = (uint16_t) (f->nchildren - 1);

    XiValue *v = xi_value_new(f, entry, XI_CLOSURE_NEW, &stub_int, 0);
    assert(v != NULL);
    v->aux = (void *) child;
    v->aux_int = child_idx;
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    /* Raw direct-emitter tests do not publish unverified IR to the AOT proto. */
    assert(f->children[child_idx] == child && "raw child should remain owned by parent");

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_CLOSURE) {
            found = true;
            break;
        }
    }
    assert(found && "CLOSURE_NEW should emit OP_CLOSURE");

    xr_instruction_unit_free(proto);
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
            found = true;
            break;
        }
    }
    assert(found && "SET_NEW should emit OP_NEWSET");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_is_check) {
    /* IS -> OP_IS: args[0]=value, args[1]=type constant */
    XiFunc *f = make_func("typecheck", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *p0 = xi_param(f, entry, 0, &stub_int);
    XiValue *type_const = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    assert(type_const != NULL);
    type_const->aux_int = 8; /* XR_TID_I64 */
    XiValue *v = xi_value_new(f, entry, XI_IS, &stub_bool, 2);
    assert(v != NULL);
    v->args[0] = p0;
    v->args[1] = type_const;
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_IS) {
            found = true;
            break;
        }
    }
    assert(found && "XI_IS should emit OP_IS");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_identity_as_establishes_owned_result) {
    XiFunc *f = make_func("identity_as", &stub_string);
    XiBlock *entry = f->entry;

    XiValue *source = xi_param(f, entry, 0, &stub_string);
    XiValue *cast = xi_value_new(f, entry, XI_AS, &stub_string, 1);
    assert(cast != NULL);
    cast->args[0] = source;
    cast->aux_int = ((int64_t) (uint32_t) -1 << 1);
    xi_block_set_return(entry, cast);

    XrProto *proto = NULL;
    XiEmitStatus status = xi_emit(f, NULL, &proto);
    assert(status == XI_EMIT_OK && proto != NULL);

    bool found_dup = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_DUP) {
            found_dup = true;
            break;
        }
    }
    assert(found_dup && "identity XI_AS must retain its independently owned result");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_cancelled_builtin) {
    /* CALL_BUILTIN(0) -> OP_CANCELLED */
    XiFunc *f = make_func("chk", &stub_bool);
    XiBlock *entry = f->entry;

    XiValue *v = xi_value_new(f, entry, XI_CALL_BUILTIN, &stub_bool, 0);
    assert(v != NULL);
    v->aux_int = 0; /* cancelled() */
    xi_block_set_return(entry, v);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found = false;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_CANCELLED) {
            found = true;
            break;
        }
    }
    assert(found && "CALL_BUILTIN(0) should emit OP_CANCELLED");

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_local_addr_pins_source_slot) {
    XiFunc *f = make_func("borrow", &stub_int);
    XiBlock *entry = f->entry;
    XiValue *param = xi_param(f, entry, 0, &stub_int);
    XiValue *place = xi_value_new(f, entry, XI_LOCAL_ADDR, &stub_int, 1);
    place->args[0] = param;
    XiValue *load = xi_value_new(f, entry, XI_PLACE_LOAD, &stub_int, 1);
    load->args[0] = place;
    xi_block_set_return(entry, load);

    XrProto *proto = NULL;
    XiEmitStatus s = xi_emit(f, NULL, &proto);
    assert(s == XI_EMIT_OK && proto != NULL);

    bool found_pinning_move = false;
    bool found_addr = false;
    uint32_t storage = 0;
    for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (GET_OPCODE(inst) == OP_MOVE && GETARG_B(inst) == 0) {
            storage = GETARG_A(inst);
            found_pinning_move = storage != 0;
        } else if (GET_OPCODE(inst) == OP_LOCAL_ADDR) {
            found_addr = true;
            assert(found_pinning_move);
            assert(GETARG_B(inst) == storage);
        }
    }
    assert(found_addr);

    xr_instruction_unit_free(proto);
    xi_func_free(f);
}

TEST(emit_assertion_action_scratch_has_explicit_ownership) {
    XrVMRuntime *isolate = new_test_isolate();
    TEST_REQUIRE(isolate != NULL);
    XiFunc *f = make_func("assertion_action_ownership", &stub_void);
    XiBlock *entry = f->entry;
    XiValue *action = xi_param(f, entry, 0, &stub_string);
    XiValue *assertion = xi_value_new(f, entry, XI_ASSERTION, &stub_void, 1);
    TEST_REQUIRE(action != NULL && assertion != NULL);
    assertion->args[0] = action;
    assertion->source_span = (XiSourceSpan) {4, 5, 4, 31};
    XrAssertionPlan plan;
    XrLocation source = {"assertion_action_ownership.xr", 4, 5, 4, 31};
    TEST_REQUIRE(xr_assertion_plan_build(XR_CORE_BUILTIN_ASSERT_PANICS, 1, source,
                                         XR_CORE_INTRINSIC_TARGET_VM,
                                         XR_ASSERTION_CAPABILITY_ALL,
                                         &plan) == XR_ASSERTION_PLAN_OK);
    TEST_REQUIRE(xi_value_set_assertion_plan(f, assertion, &plan));
    xi_block_set_return(entry, NULL);

    XrProto *proto = NULL;
    XiEmitStatus status = xi_emit(f, isolate, &proto);
    TEST_REQUIRE(status == XI_EMIT_OK && proto != NULL);

    int call_pc = -1;
    int catch_pc = -1;
    int first_error_catch_pc = -1;
    int dup_count = 0;
    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, pc));
        if (op == OP_CALL)
            call_pc = pc;
        else if (op == OP_CATCH)
            catch_pc = pc;
        else if (op == OP_ERR_CATCH && first_error_catch_pc < 0)
            first_error_catch_pc = pc;
        else if (op == OP_DUP)
            dup_count++;
    }
    TEST_REQUIRE(call_pc >= 9 && catch_pc >= 3 && first_error_catch_pc >= 2);
    uint32_t call_base = GETARG_A(PROTO_CODE(proto, call_pc));
    TEST_REQUIRE(call_base >= 5u);
    uint32_t base = call_base - 5u;
    TEST_REQUIRE(dup_count == 1);
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, call_pc - 4)) == OP_DUP &&
                 GETARG_A(PROTO_CODE(proto, call_pc - 4)) == base + 4u);
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, call_pc - 5)) == OP_MOVE &&
                 GETARG_A(PROTO_CODE(proto, call_pc - 5)) == base + 4u &&
                 GETARG_B(PROTO_CODE(proto, call_pc - 5)) == GETARG_B(PROTO_CODE(proto, 0)));
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, call_pc - 3)) == OP_MOVE &&
                 GETARG_A(PROTO_CODE(proto, call_pc - 3)) == base + 5u &&
                 GETARG_B(PROTO_CODE(proto, call_pc - 3)) == GETARG_B(PROTO_CODE(proto, 0)));

    /* The normal path releases the retained action guard before probing the
     * typed-error channel.  The typed path consumes the ordinary result before
     * ERR_CATCH replaces the same slot. */
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, call_pc + 2)) == OP_DROP &&
                 GETARG_A(PROTO_CODE(proto, call_pc + 2)) == base + 4u);
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, call_pc + 3)) == OP_LOADNULL &&
                 GETARG_A(PROTO_CODE(proto, call_pc + 3)) == base + 4u);
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, call_pc + 4)) == OP_MOVE &&
                 GETARG_A(PROTO_CODE(proto, call_pc + 4)) == base &&
                 GETARG_B(PROTO_CODE(proto, call_pc + 4)) == base + 5u);
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, call_pc + 5)) == OP_LOADNULL &&
                 GETARG_A(PROTO_CODE(proto, call_pc + 5)) == base + 5u);
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, first_error_catch_pc - 2)) == OP_DROP &&
                 GETARG_A(PROTO_CODE(proto, first_error_catch_pc - 2)) == base);
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, first_error_catch_pc - 1)) == OP_LOADNULL &&
                 GETARG_A(PROTO_CODE(proto, first_error_catch_pc - 1)) == base);

    /* A panic skips the normal continuation, so the catch frontier releases
     * the guard and clears the borrowed action alias before binding the owned
     * panic observation. */
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, catch_pc - 3)) == OP_DROP &&
                 GETARG_A(PROTO_CODE(proto, catch_pc - 3)) == base + 4u);
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, catch_pc - 2)) == OP_LOADNULL &&
                 GETARG_A(PROTO_CODE(proto, catch_pc - 2)) == base + 4u);
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, catch_pc - 1)) == OP_LOADNULL &&
                 GETARG_A(PROTO_CODE(proto, catch_pc - 1)) == base + 5u);
    TEST_REQUIRE(GETARG_A(PROTO_CODE(proto, catch_pc)) == base + 1u);

    int return_pc = PROTO_CODE_COUNT(proto) - 1;
    TEST_REQUIRE(GET_OPCODE(PROTO_CODE(proto, return_pc)) == OP_RETURN0);
    for (uint32_t slot = 0; slot < 6u; slot++) {
        XrInstruction clear = PROTO_CODE(proto, return_pc - 6 + (int) slot);
        TEST_REQUIRE(GET_OPCODE(clear) == OP_LOADNULL && GETARG_A(clear) == base + slot);
    }

    xr_instruction_unit_free(proto);
    xi_func_free(f);
    xray_vm_delete(isolate);
}

/* ========== Error Handling ========== */

TEST(emit_status_str) {
    assert(strcmp(xi_emit_status_str(XI_EMIT_OK), "OK") == 0);
    assert(strcmp(xi_emit_status_str(XI_EMIT_ERR_TOO_MANY_REGS),
                  "too many registers (>65535 encoded slots, with one sentinel reserved)") == 0);
    assert(strcmp(xi_emit_status_str(XI_EMIT_ERR_UNSUPPORTED_OP), "unsupported Xi IR operation") ==
           0);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Emit Unit Tests ===\n\n");

    (void) stub_null;
    (void) stub_void;

    run_number_parse_error_builtin_identity_requires_matching_typed_metadata();

    /* Basic emission */
    run_class_descriptor_constants_use_opaque_pointer_identity();
    run_emit_return_const_int();
    run_emit_return_void();
    run_emit_target_layout_queries_use_canonical_target_layout();
    run_emit_unreachable_is_terminator();
    run_emit_const_bool();
    run_emit_const_null();

    /* Arithmetic */
    run_emit_add();
    run_emit_sub_mul_div();
    run_emit_unary_neg();

    /* Comparison */
    run_emit_cmp_eq();
    run_emit_cmp_gt();
    run_emit_uint64_cmp_uses_unsigned_opcode();

    /* Control flow */
    run_emit_if_then_else();
    run_emit_reused_cmp_control_materializes_bool();
    run_emit_jump_fallthrough();

    /* Copy / Move */
    run_emit_copy_becomes_move();
    run_emit_codegen_compiler_fence_projects_to_void_without_runtime_effect();
    run_emit_numeric_conversion_packs_typed_witness();
    run_conversion_bytecode_modes_are_disjoint();
    run_emit_scalar_parse_intrinsics_use_exact_vm_modes();

    /* Float constants */
    run_emit_const_float_small();
    run_emit_const_float_large();

    /* Large constants */
    run_emit_const_int_large();
    run_emit_const_int_beyond_sbx();

    /* Optimization + emit */
    run_emit_after_optimization();

    /* Register recycling */
    run_emit_reg_recycling();
    run_emit_reg_pressure();
    run_emit_param_register_above_255();
    run_emit_coalesces_var_id_above_255();
    run_emit_select_preserves_param_slot_alias();
    run_emit_symbol_index_above_255();

    /* Instruction fusion */
    run_emit_addi_rhs_const();
    run_emit_addi_lhs_const();
    run_emit_subi_muli();
    run_emit_addi_negative();
    run_emit_addk_large_const();

    /* New op coverage */
    run_emit_str_concat();
    run_emit_str_concat_uint64_formats_before_range_concat();
    run_emit_closure_new();
    run_emit_set_new();
    run_emit_is_check();
    run_emit_identity_as_establishes_owned_result();
    run_emit_cancelled_builtin();
    run_emit_local_addr_pins_source_slot();
    run_emit_assertion_action_scratch_has_explicit_ownership();

    /* Error handling */
    run_emit_status_str();

    printf("\n=== %d/%d Xi Emit tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
