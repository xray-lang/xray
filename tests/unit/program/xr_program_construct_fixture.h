/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_construct_fixture.h - Shared aggregate owner-transfer programs
 */

#ifndef XR_PROGRAM_CONSTRUCT_FIXTURE_H
#define XR_PROGRAM_CONSTRUCT_FIXTURE_H

#include "../../../src/base/xchecks.h"
#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"
#include "../../../src/program/xr_program_verify.h"

#include <stdio.h>
#include <string.h>

#define XR_PROGRAM_CONSTRUCT_CASES(X)                                                              \
    X(NESTED_TRANSFER, "nested owner transfer", NONE)                                              \
    X(EXPLICIT_COPY_PAIR, "explicit copy into distinct fields", NONE)                              \
    X(TRIVIAL_REUSE, "trivial field reuse", NONE)                                                  \
    X(FORBIDDEN_MOVE, "copy-forbidden owner transfer", NONE)                                       \
    X(DUPLICATE_OWNER, "duplicate owner fields", VALUE_USE)                                        \
    X(USE_AFTER_CONSTRUCT, "use after owner transfer", VALUE_USE)                                  \
    X(DROP_AFTER_CONSTRUCT, "drop after owner transfer", VALUE_USE)                                \
    X(BORROWED_FIELD, "borrowed affine field", OPERATION_TYPE)                                     \
    X(FORBIDDEN_COPY, "copy-forbidden explicit copy", OPERATION_TYPE)

typedef enum XrProgramConstructCaseKind {
#define XR_CONSTRUCT_KIND(kind, name, diagnostic) XR_PROGRAM_CONSTRUCT_##kind,
    XR_PROGRAM_CONSTRUCT_CASES(XR_CONSTRUCT_KIND)
#undef XR_CONSTRUCT_KIND
    XR_PROGRAM_CONSTRUCT_CASE_COUNT,
} XrProgramConstructCaseKind;

typedef struct XrProgramConstructCase {
    XrProgramConstructCaseKind kind;
    const char *name;
    XrProgramDiagnosticKind diagnostic;
} XrProgramConstructCase;

static const XrProgramConstructCase xr_program_construct_cases[] = {
#define XR_CONSTRUCT_CASE(kind, name, diagnostic)                                                  \
    {XR_PROGRAM_CONSTRUCT_##kind, name, XR_PROGRAM_DIAGNOSTIC_##diagnostic},
    XR_PROGRAM_CONSTRUCT_CASES(XR_CONSTRUCT_CASE)
#undef XR_CONSTRUCT_CASE
};
#undef XR_PROGRAM_CONSTRUCT_CASES

_Static_assert(sizeof(xr_program_construct_cases) / sizeof(xr_program_construct_cases[0]) ==
                   XR_PROGRAM_CONSTRUCT_CASE_COUNT,
               "construct fixture registration is incomplete");

enum {
    XR_CONSTRUCT_LEAF = 100,
    XR_CONSTRUCT_MIDDLE,
    XR_CONSTRUCT_OUTER,
    XR_CONSTRUCT_TOKEN,
    XR_CONSTRUCT_READ,
    XR_CONSTRUCT_CHOICE,
    XR_CONSTRUCT_TYPE_COUNT = 6,
    XR_CONSTRUCT_INSTRUCTION_LIMIT = 24,
};

typedef enum XrConstructValue {
    XR_CONSTRUCT_SCALAR,
    XR_CONSTRUCT_LEAF_VALUE,
    XR_CONSTRUCT_COPY,
    XR_CONSTRUCT_PLACE,
    XR_CONSTRUCT_BORROW,
    XR_CONSTRUCT_MIDDLE_VALUE,
    XR_CONSTRUCT_OUTER_VALUE,
    XR_CONSTRUCT_MIDDLE_BORROW,
    XR_CONSTRUCT_FIRST_BORROW,
    XR_CONSTRUCT_SECOND_BORROW,
    XR_CONSTRUCT_FIRST,
    XR_CONSTRUCT_SECOND,
    XR_CONSTRUCT_RESULT,
    XR_CONSTRUCT_INVALID_USE,
    XR_CONSTRUCT_VALUE_COUNT,
} XrConstructValue;

typedef struct XrConstructFixture {
    XrCoreIrTypeInput types[XR_CONSTRUCT_TYPE_COUNT];
    uint16_t scalar_fields[1];
    uint16_t middle_fields[2];
    uint16_t outer_fields[3];
    uint16_t token_fields[1];
    XrCoreIrVariantInput variants[2];
    uint16_t receiver_type;
    XrParamMode receiver_mode;
    XrCoreIrCallableSignatureInput interface_slot;
    XrCoreIrInterfaceInput interface;
    XrCoreIrKey values[XR_CONSTRUCT_VALUE_COUNT];
    XrCoreIrKey operands[XR_CONSTRUCT_INSTRUCTION_LIMIT][3];
    XrCoreIrInstructionInput instructions[XR_CONSTRUCT_INSTRUCTION_LIMIT];
    uint32_t instruction_count;
    XrCoreIrConstantInput constant;
} XrConstructFixture;

static XrCoreIrKey xr_construct_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static bool xr_construct_has_forbidden_type(XrProgramConstructCaseKind kind) {
    return kind == XR_PROGRAM_CONSTRUCT_FORBIDDEN_MOVE ||
           kind == XR_PROGRAM_CONSTRUCT_FORBIDDEN_COPY;
}

static bool xr_construct_has_two_leaf_fields(XrProgramConstructCaseKind kind) {
    return kind == XR_PROGRAM_CONSTRUCT_EXPLICIT_COPY_PAIR ||
           kind == XR_PROGRAM_CONSTRUCT_DUPLICATE_OWNER ||
           kind == XR_PROGRAM_CONSTRUCT_TRIVIAL_REUSE;
}

static void xr_construct_init_types(XrConstructFixture *fixture, XrProgramConstructCaseKind kind) {
    bool trivial = kind == XR_PROGRAM_CONSTRUCT_TRIVIAL_REUSE;
    bool forbidden = xr_construct_has_forbidden_type(kind);
    const char *keys[] = {"construct:type:leaf",  "construct:type:middle", "construct:type:outer",
                          "construct:type:token", "construct:type:read",   "construct:type:choice"};
    for (uint32_t index = 0u; index < XR_CONSTRUCT_TYPE_COUNT; ++index) {
        fixture->types[index].key = xr_construct_key(keys[index]);
        fixture->types[index].local_id = (uint16_t) (XR_CONSTRUCT_LEAF + index);
    }
    fixture->scalar_fields[0] = XR_CORE_TYPE_I64;
    fixture->middle_fields[0] = forbidden ? XR_CONSTRUCT_CHOICE : XR_CONSTRUCT_LEAF;
    fixture->middle_fields[1] =
        xr_construct_has_two_leaf_fields(kind) ? XR_CONSTRUCT_LEAF : XR_CORE_TYPE_I64;
    fixture->outer_fields[0] = XR_CONSTRUCT_MIDDLE;
    fixture->outer_fields[1] = fixture->outer_fields[2] = XR_CORE_TYPE_I64;
    fixture->token_fields[0] = XR_CONSTRUCT_TOKEN;
    for (uint32_t index = 0u; index < 3u; ++index) {
        XrCoreIrTypeInput *type = &fixture->types[index];
        type->kind = XR_CORE_IR_TYPE_AGGREGATE;
        type->ownership =
            trivial ? XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL : XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        type->copy_contract = trivial                    ? XR_CORE_IR_COPY_TRIVIAL
                              : forbidden && index != 0u ? XR_CORE_IR_COPY_FORBIDDEN
                                                         : XR_CORE_IR_COPY_EXPLICIT;
    }
    fixture->types[0].nominal_kind = trivial ? XR_CORE_IR_NOMINAL_STRUCT : XR_CORE_IR_NOMINAL_NONE;
    fixture->types[0].field_types = fixture->scalar_fields;
    fixture->types[0].field_count = 1u;
    fixture->types[1].field_types = fixture->middle_fields;
    fixture->types[1].field_count = 2u;
    fixture->types[2].field_types = fixture->outer_fields;
    fixture->types[2].field_count = 3u;
    fixture->interface.key = xr_construct_key("construct:interface");
    for (uint32_t index = 3u; index <= 4u; ++index) {
        XrCoreIrTypeInput *type = &fixture->types[index];
        type->kind = XR_CORE_IR_TYPE_EXISTENTIAL;
        type->existential_interface = fixture->interface.key;
        type->ownership =
            index == 3u ? XR_CORE_IR_TYPE_OWNERSHIP_AFFINE : XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
        type->copy_contract = index == 3u ? XR_CORE_IR_COPY_FORBIDDEN : XR_CORE_IR_COPY_TRIVIAL;
        type->interface_use_kind = index == 3u ? XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE
                                               : XR_CORE_IR_INTERFACE_EXISTENTIAL_READ;
    }
    // An unselected owned-storage alternative makes the inhabited i64 case move-only.
    fixture->variants[0] = (XrCoreIrVariantInput) {fixture->scalar_fields, 1u};
    fixture->variants[1] = (XrCoreIrVariantInput) {fixture->token_fields, 1u};
    fixture->types[5].kind = XR_CORE_IR_TYPE_VARIANT;
    fixture->types[5].ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    fixture->types[5].copy_contract = XR_CORE_IR_COPY_FORBIDDEN;
    fixture->types[5].variants = fixture->variants;
    fixture->types[5].variant_count = 2u;
    fixture->receiver_type = XR_CONSTRUCT_READ;
    fixture->receiver_mode = XR_PARAM_READ;
    fixture->interface_slot = (XrCoreIrCallableSignatureInput) {
        .parameter_types = &fixture->receiver_type,
        .parameter_modes = &fixture->receiver_mode,
        .parameter_count = 1u,
        .has_receiver = true,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_I64,
    };
    fixture->interface.slots = &fixture->interface_slot;
    fixture->interface.slot_count = 1u;
}

static XrCoreIrInstructionInput *xr_construct_op(XrConstructFixture *fixture, uint16_t operation,
                                                 uint16_t type, XrConstructValue result,
                                                 XrConstructValue operand, bool owner) {
    XR_CHECK(fixture->instruction_count < XR_CONSTRUCT_INSTRUCTION_LIMIT,
             "construct fixture instruction capacity exhausted");
    XR_CHECK(result < XR_CONSTRUCT_VALUE_COUNT, "construct fixture result is invalid");
    uint32_t index = fixture->instruction_count++;
    XrCoreIrInstructionInput *instruction = &fixture->instructions[index];
    instruction->operation_id = operation;
    instruction->result_type_id = type;
    instruction->result_ownership = owner ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER;
    if (type != XR_CORE_TYPE_VOID)
        instruction->result = fixture->values[result];
    if (operand < XR_CONSTRUCT_VALUE_COUNT) {
        fixture->operands[index][0] = fixture->values[operand];
        instruction->operands = fixture->operands[index];
        instruction->operand_count = 1u;
    }
    return instruction;
}

static void xr_construct_project(XrConstructFixture *fixture, uint16_t type,
                                 XrConstructValue result, XrConstructValue source, uint32_t field) {
    XrCoreIrInstructionInput *instruction =
        xr_construct_op(fixture, XR_CORE_OP_CORE_AGGREGATE_PROJECT, type, result, source, false);
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
    instruction->immediate.field_ordinal = field;
}

static void xr_construct_finish(XrConstructFixture *fixture, XrProgramConstructCaseKind kind) {
    bool trivial = kind == XR_PROGRAM_CONSTRUCT_TRIVIAL_REUSE;
    bool forbidden = xr_construct_has_forbidden_type(kind);
    XrCoreIrInstructionInput *instruction =
        xr_construct_op(fixture, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, XR_CONSTRUCT_OUTER,
                        XR_CONSTRUCT_OUTER_VALUE, XR_CONSTRUCT_MIDDLE_VALUE, !trivial);
    fixture->operands[fixture->instruction_count - 1u][1] = fixture->values[XR_CONSTRUCT_SCALAR];
    fixture->operands[fixture->instruction_count - 1u][2] = fixture->values[XR_CONSTRUCT_SCALAR];
    instruction->operand_count = 3u;
    xr_construct_project(fixture, XR_CONSTRUCT_MIDDLE, XR_CONSTRUCT_MIDDLE_BORROW,
                         XR_CONSTRUCT_OUTER_VALUE, 0u);
    xr_construct_project(fixture, forbidden ? XR_CONSTRUCT_CHOICE : XR_CONSTRUCT_LEAF,
                         XR_CONSTRUCT_FIRST_BORROW, XR_CONSTRUCT_MIDDLE_BORROW, 0u);
    if (forbidden) {
        instruction = xr_construct_op(fixture, XR_CORE_OP_CORE_VARIANT_PROJECT, XR_CORE_TYPE_I64,
                                      XR_CONSTRUCT_FIRST, XR_CONSTRUCT_FIRST_BORROW, false);
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT_FIELD;
        instruction->immediate.variant_field.variant_ordinal = 0u;
        instruction->immediate.variant_field.field_ordinal = 0u;
    } else {
        xr_construct_project(fixture, XR_CORE_TYPE_I64, XR_CONSTRUCT_FIRST,
                             XR_CONSTRUCT_FIRST_BORROW, 0u);
    }
    if (xr_construct_has_two_leaf_fields(kind)) {
        xr_construct_project(fixture, XR_CONSTRUCT_LEAF, XR_CONSTRUCT_SECOND_BORROW,
                             XR_CONSTRUCT_MIDDLE_BORROW, 1u);
        xr_construct_project(fixture, XR_CORE_TYPE_I64, XR_CONSTRUCT_SECOND,
                             XR_CONSTRUCT_SECOND_BORROW, 0u);
    } else {
        xr_construct_project(fixture, XR_CORE_TYPE_I64, XR_CONSTRUCT_SECOND,
                             XR_CONSTRUCT_MIDDLE_BORROW, 1u);
    }
    instruction = xr_construct_op(fixture, XR_CORE_OP_CORE_ADD_I64, XR_CORE_TYPE_I64,
                                  XR_CONSTRUCT_RESULT, XR_CONSTRUCT_FIRST, false);
    fixture->operands[fixture->instruction_count - 1u][1] = fixture->values[XR_CONSTRUCT_SECOND];
    instruction->operand_count = 2u;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    if (!trivial)
        (void) xr_construct_op(fixture, XR_CORE_OP_CORE_OWNER_DROP, XR_CORE_TYPE_VOID,
                               XR_CONSTRUCT_RESULT, XR_CONSTRUCT_OUTER_VALUE, false);
    (void) xr_construct_op(fixture, XR_CORE_OP_CORE_RETURN, XR_CORE_TYPE_VOID, XR_CONSTRUCT_RESULT,
                           XR_CONSTRUCT_RESULT, false);
}

static void xr_construct_init_instructions(XrConstructFixture *fixture,
                                           XrProgramConstructCaseKind kind) {
    bool trivial = kind == XR_PROGRAM_CONSTRUCT_TRIVIAL_REUSE;
    bool forbidden = xr_construct_has_forbidden_type(kind);
    uint16_t leaf_type = forbidden ? XR_CONSTRUCT_CHOICE : XR_CONSTRUCT_LEAF;
    for (uint32_t index = 0u; index < XR_CONSTRUCT_VALUE_COUNT; ++index) {
        char name[48];
        (void) snprintf(name, sizeof(name), "construct:value:%u", index);
        fixture->values[index] = xr_construct_key(name);
    }
    fixture->constant = (XrCoreIrConstantInput) {
        .key = xr_construct_key("construct:constant:21"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 21,
    };
    (void) xr_construct_op(fixture, XR_CORE_OP_CORE_BLOCK_ARGUMENT, XR_CORE_TYPE_VOID,
                           XR_CONSTRUCT_SCALAR, XR_CONSTRUCT_VALUE_COUNT, false);
    XrCoreIrInstructionInput *instruction =
        xr_construct_op(fixture, XR_CORE_OP_CORE_CONSTANT_I64, XR_CORE_TYPE_I64,
                        XR_CONSTRUCT_SCALAR, XR_CONSTRUCT_VALUE_COUNT, false);
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT;
    instruction->immediate.key = fixture->constant.key;
    instruction = xr_construct_op(
        fixture,
        forbidden ? XR_CORE_OP_CORE_VARIANT_CONSTRUCT : XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
        leaf_type, XR_CONSTRUCT_LEAF_VALUE, XR_CONSTRUCT_SCALAR, !trivial);
    if (forbidden)
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT;
    if (kind == XR_PROGRAM_CONSTRUCT_EXPLICIT_COPY_PAIR ||
        kind == XR_PROGRAM_CONSTRUCT_FORBIDDEN_COPY)
        (void) xr_construct_op(fixture, XR_CORE_OP_CORE_OWNER_COPY, leaf_type, XR_CONSTRUCT_COPY,
                               XR_CONSTRUCT_LEAF_VALUE, true);
    if (kind == XR_PROGRAM_CONSTRUCT_BORROWED_FIELD) {
        instruction = xr_construct_op(fixture, XR_CORE_OP_CORE_PLACE_LOCAL, leaf_type,
                                      XR_CONSTRUCT_PLACE, XR_CONSTRUCT_LEAF_VALUE, false);
        instruction->result_category = XR_CORE_IR_PLACE;
        (void) xr_construct_op(fixture, XR_CORE_OP_CORE_PLACE_LOAD, leaf_type, XR_CONSTRUCT_BORROW,
                               XR_CONSTRUCT_PLACE, false);
    }
    instruction = xr_construct_op(
        fixture, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, XR_CONSTRUCT_MIDDLE,
        XR_CONSTRUCT_MIDDLE_VALUE,
        kind == XR_PROGRAM_CONSTRUCT_BORROWED_FIELD ? XR_CONSTRUCT_BORROW : XR_CONSTRUCT_LEAF_VALUE,
        !trivial);
    XrConstructValue second = kind == XR_PROGRAM_CONSTRUCT_EXPLICIT_COPY_PAIR ? XR_CONSTRUCT_COPY
                              : xr_construct_has_two_leaf_fields(kind) ? XR_CONSTRUCT_LEAF_VALUE
                                                                       : XR_CONSTRUCT_SCALAR;
    fixture->operands[fixture->instruction_count - 1u][1] = fixture->values[second];
    instruction->operand_count = 2u;
    if (kind == XR_PROGRAM_CONSTRUCT_USE_AFTER_CONSTRUCT)
        xr_construct_project(fixture, XR_CORE_TYPE_I64, XR_CONSTRUCT_INVALID_USE,
                             XR_CONSTRUCT_LEAF_VALUE, 0u);
    if (kind == XR_PROGRAM_CONSTRUCT_DROP_AFTER_CONSTRUCT)
        (void) xr_construct_op(fixture, XR_CORE_OP_CORE_OWNER_DROP, XR_CORE_TYPE_VOID,
                               XR_CONSTRUCT_INVALID_USE, XR_CONSTRUCT_LEAF_VALUE, false);
    xr_construct_finish(fixture, kind);
}

static XrProgramBuildStatus xr_program_construct_fixture_write(XrProgramConstructCaseKind kind,
                                                               XrProgramArtifact *artifact,
                                                               char *diagnostic,
                                                               size_t diagnostic_size) {
    if (!artifact || (unsigned) kind >= XR_PROGRAM_CONSTRUCT_CASE_COUNT) {
        if (diagnostic && diagnostic_size != 0u)
            (void) snprintf(diagnostic, diagnostic_size, "invalid construct fixture request");
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    XrConstructFixture fixture = {0};
    xr_construct_init_types(&fixture, kind);
    xr_construct_init_instructions(&fixture, kind);
    XrCoreIrBlockInput block = {
        .key = xr_construct_key("construct:block"),
        .instructions = fixture.instructions,
        .instruction_count = fixture.instruction_count,
    };
    XrCoreIrFunctionInput function = {
        .key = xr_construct_key("construct:entry"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_TRAP,
        .entry_block = block.key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = xr_construct_key("construct:module"),
        .constants = &fixture.constant,
        .constant_count = 1u,
        .functions = &function,
        .function_count = 1u,
    };
    XrCoreIrKey semantic = xr_construct_key("construct:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = fixture.types,
        .type_count = XR_CONSTRUCT_TYPE_COUNT,
        .interfaces = &fixture.interface,
        .interface_count = 1u,
        .modules = &module,
        .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

#endif  // XR_PROGRAM_CONSTRUCT_FIXTURE_H
