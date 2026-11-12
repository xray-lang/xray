/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_fixed_call_fixture.h - A first aggregate return preceding any NEW
 */
#include "base/xmalloc.h"
#include "vm/register/xrbc_internal.h"

typedef struct RawFixedCalls {
    XrRbcModule module;
    XrRbcFunction functions[3];
    XrRbcAggregateType type;
    XrRbcAggregateValue root_homes[2], callee_home;
    XrRbcAggregateAccess root_accesses[5], callee_accesses[2], forward_access;
    XrRbcArrayLocation index, *operands;
    uint32_t root_code[11], callee_code[2], forward_code, argument, parameter;
    XrRbcRegisterKind kinds[4];
    uint64_t last_index;
} RawFixedCalls;

static uint32_t fixed_call_word(XrRbcOp op, uint32_t a, uint32_t b, uint32_t c, int64_t immediate) {
    uint32_t word = 0;
    XR_CHECK(xr_rbc_encode(&word, (XrRbcOperands){op, a, b, c, immediate}), "fixed call fixture encoding");
    return word;
}

static void fixed_call_fixture(RawFixedCalls *f, uint32_t count) {
    *f = (RawFixedCalls){0};
    f->type = xr_rbc_fixed_type(count);
    XrRbcAggregateLayout layout;
    XR_CHECK(xr_rbc_aggregate_layout(f->type, &layout), "fixed call layout");
    f->operands = xr_calloc(count, sizeof(*f->operands));
    XR_CHECK(f->operands, "fixed call real operand span");
    f->index = (XrRbcArrayLocation){1, false};
    f->last_index = count - 1;
    f->root_homes[1].offset = layout.size;
    f->root_accesses[0] = (XrRbcAggregateAccess){.operation=XR_RBC_AGGREGATE_CALL, .callee=1, .window=2};
    f->root_accesses[1] = f->root_accesses[0]; f->root_accesses[1].destination = 1;
    f->root_accesses[2] = (XrRbcAggregateAccess){.operation=XR_RBC_AGGREGATE_CALL,
        .callee=2, .window=3, .destination=1, .count=1};
    f->root_accesses[3] = (XrRbcAggregateAccess){.operation=XR_RBC_AGGREGATE_GET, .count=1};
    f->root_accesses[4] = (XrRbcAggregateAccess){.operation=XR_RBC_AGGREGATE_GET,
        .source=1, .result={1, false}, .count=1};
    f->callee_accesses[0] = (XrRbcAggregateAccess){.operation=XR_RBC_AGGREGATE_NEW, .count=count};
    f->callee_accesses[1] = (XrRbcAggregateAccess){.operation=XR_RBC_AGGREGATE_RETURN};
    f->forward_access = f->callee_accesses[1];
    f->root_code[0] = fixed_call_word(XR_RBC_LOADI, 3, 0, 0, 37);
    f->root_code[1] = fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 0);
    f->root_code[2] = fixed_call_word(XR_RBC_LOADI, 3, 0, 0, 91);
    f->root_code[3] = fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 1);
    f->root_code[4] = fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 2);
    f->root_code[5] = count <= 32768 ? fixed_call_word(XR_RBC_LOADI, 1, 0, 0, count - 1) :
        fixed_call_word(XR_RBC_LOADK, 1, 0, 0, 0);
    f->root_code[6] = fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 3);
    f->root_code[7] = fixed_call_word(XR_RBC_MULI, 0, 0, 0, 100);
    f->root_code[8] = fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 4);
    f->root_code[9] = fixed_call_word(XR_RBC_ADD, 0, 0, 1, 0);
    f->root_code[10] = fixed_call_word(XR_RBC_RET, 0, 0, 0, 0);
    f->callee_code[0] = fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 0);
    f->callee_code[1] = fixed_call_word(XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 1);
    f->forward_code = f->callee_code[0];
    f->kinds[2] = f->kinds[3] = XR_RBC_CALL_WINDOW;
    f->functions[0] = (XrRbcFunction){.code=f->root_code, .code_count=11, .register_count=4,
        .constants=&f->last_index, .constant_count=1,
        .register_kinds=f->kinds, .register_kind_count=4, .aggregate_values=f->root_homes,
        .aggregate_value_count=2, .aggregate_size=layout.size * 2,
        .aggregate_accesses=f->root_accesses, .aggregate_access_count=5,
        .aggregate_operands=&f->index, .aggregate_operand_count=1,
        .aggregate_arguments=&f->argument, .aggregate_argument_count=1};
    f->functions[1] = (XrRbcFunction){.code=f->callee_code, .code_count=2, .register_count=255,
        .parameter_count=1, .aggregate_values=&f->callee_home, .aggregate_value_count=1,
        .aggregate_size=layout.size, .aggregate_result_type=1, .aggregate_accesses=f->callee_accesses,
        .aggregate_access_count=2, .aggregate_operands=f->operands, .aggregate_operand_count=count};
    f->functions[2] = (XrRbcFunction){.code=&f->forward_code, .code_count=1, .register_count=255,
        .aggregate_values=&f->callee_home, .aggregate_value_count=1, .aggregate_size=layout.size,
        .aggregate_result_type=1, .aggregate_parameters=&f->parameter, .aggregate_parameter_count=1,
        .aggregate_accesses=&f->forward_access, .aggregate_access_count=1};
    f->module = (XrRbcModule){.functions=f->functions, .function_count=3, .aggregate_types=&f->type, .aggregate_type_count=1};
}
