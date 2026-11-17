/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_set_region_fixture.h - Real local loop ownership and closed gaps
 */
#ifndef LIR_ARRAY_SET_REGION_FIXTURE_H
#define LIR_ARRAY_SET_REGION_FIXTURE_H
#include "lir/xlir_array.h"
#include <stdio.h>
enum ArraySetRegionMode {
    ARRAY_SET_REGION_LOCAL, ARRAY_SET_REGION_CALL, ARRAY_SET_REGION_SNAPSHOT,
    ARRAY_SET_REGION_ALIAS, ARRAY_SET_REGION_RESIZE, ARRAY_SET_REGION_GAP,
    ARRAY_SET_REGION_HEADER_WRITE, ARRAY_SET_REGION_EXTRA_ENTRY,
    ARRAY_SET_REGION_EXTRA_BACKEDGE, ARRAY_SET_REGION_EARLY_EXIT, ARRAY_SET_REGION_TRAP, ARRAY_SET_REGION_PADDED
};
typedef struct ArraySetRegionFixture {
    XrLirModule *module;
    XrLirFunction function, helper;
    XrLirBlock entry, header, exit, body[8];
    XrLirValue header_array, result;
    enum ArraySetRegionMode mode;
    uint32_t count, arity;
} ArraySetRegionFixture;
static XrLirValue ars_value(ArraySetRegionFixture *c, XrLirBlock block, XrLirInstruction instruction) {
    XrLirValue value = {0}; OK(xr_lir_emit_value(c->module, block, &instruction, &value)); return value;
}
static XrLirValue ars_number(ArraySetRegionFixture *c, XrLirBlock block, int64_t number) {
    return ars_value(c, block, (XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=(uint64_t)number});
}
static void ars_effect(ArraySetRegionFixture *c, XrLirBlock block, XrLirOp op, XrLirValue value) {
    OK(xr_lir_emit_effect(c->module, block, &(XrLirInstruction){.op=op,.type=XR_LIR_UNIT,
        .operands=&value,.operand_count=1}));
}
static XrLirValue ars_array_op(ArraySetRegionFixture *c, XrLirBlock block, XrLirArrayOperation operation,
    const XrLirValue *arguments, uint32_t count) {
    uint32_t intrinsic = 0; OK(xr_lir_array_operation(c->module, XR_LIR_I64, operation, &intrinsic));
    return ars_value(c, block, (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=operation == XR_LIR_ARRAY_OP_NEW ? XR_LIR_ARRAY : XR_LIR_I64,
        .intrinsic=intrinsic,.operands=arguments,.operand_count=count});
}
static void ars_parameters(ArraySetRegionFixture *c, XrLirBlock block, XrLirValue values[3]) {
    for (uint32_t n = 0; n < c->arity; ++n) OK(xr_lir_block_parameter(c->module, block, n, &values[n]));
}
static void ars_branch(ArraySetRegionFixture *c, XrLirBlock block, XrLirBlock target, XrLirValue values[3]) {
    OK(xr_lir_set_terminator(c->module, block, &(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{target,values,c->arity}}}));
}
static XrLirValue ars_mutate(ArraySetRegionFixture *c, XrLirBlock block, XrLirValue array) {
    XrLirValue zero = ars_number(c, block, 0), arguments[] = {array,zero};
    XrLirValue item = ars_array_op(c, block, XR_LIR_ARRAY_OP_GET, arguments, 2);
    XrLirValue cell = ars_value(c, block, (XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,
        .operands=&array,.operand_count=1});
    if (c->mode == ARRAY_SET_REGION_GAP) (void)ars_number(c, block, 9);
    ars_effect(c, block, XR_LIR_ARRAY_DROP, array);
    if (c->mode == ARRAY_SET_REGION_CALL)
        OK(xr_lir_emit_effect(c->module, block, &(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=c->helper}));
    if (c->mode == ARRAY_SET_REGION_ALIAS)
        (void)ars_value(c, block, (XrLirInstruction){.op=XR_LIR_ARRAY_BORROW,.type=XR_LIR_ARRAY_REF,
            .operands=&cell,.operand_count=1});
    if (c->mode == ARRAY_SET_REGION_RESIZE) {
        uint32_t push = 0; OK(xr_lir_array_operation(c->module, XR_LIR_I64, XR_LIR_ARRAY_OP_PUSH, &push));
        XrLirValue operands[] = {cell,item};
        OK(xr_lir_emit_effect(c->module, block, &(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
            .intrinsic=push,.operands=operands,.operand_count=2}));
    }
    if (c->mode == ARRAY_SET_REGION_TRAP) {
        XrLirValue one = ars_number(c, block, 1), operands[] = {item,one};
        item = ars_value(c, block, (XrLirInstruction){.op=XR_LIR_DIV,.type=XR_LIR_I64,.operands=operands,.operand_count=2});
    }
    uint32_t set = 0; OK(xr_lir_array_operation(c->module, XR_LIR_I64, XR_LIR_ARRAY_OP_SET, &set));
    XrLirValue operands[] = {cell,zero,item};
    OK(xr_lir_emit_effect(c->module, block, &(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
        .intrinsic=set,.operands=operands,.operand_count=3}));
    XrLirValue result = ars_value(c, block, (XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_ARRAY,
        .operands=&cell,.operand_count=1});
    ars_effect(c, block, XR_LIR_ARRAY_CELL_DROP, cell);
    return result;
}
static void ars_declare(ArraySetRegionFixture *c) {
    if (c->mode == ARRAY_SET_REGION_CALL || c->mode == ARRAY_SET_REGION_PADDED) {
        OK(xr_lir_declare_function(c->module, &(XrLirFunctionSpec){.name="region_observer",.name_length=15,
            .result=XR_LIR_UNIT}, &c->helper));
        OK(xr_lir_define_function(c->module, c->helper)); XrLirBlock block = {0};
        OK(xr_lir_add_block(c->module, c->helper, NULL, 0, &block)); OK(xr_lir_set_entry(c->module, c->helper, block));
        OK(xr_lir_set_terminator(c->module, block, &(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT}));
    }
    char name[40]; int length = snprintf(name, sizeof(name), "local_set_loop_%u", (unsigned)c->count);
    CHECK(length > 0 && (size_t)length < sizeof(name));
    OK(xr_lir_declare_function(c->module, &(XrLirFunctionSpec){.name=name,.name_length=(size_t)length,
        .result=XR_LIR_I64}, &c->function)); OK(xr_lir_define_function(c->module, c->function));
    OK(xr_lir_add_block(c->module, c->function, NULL, 0, &c->entry)); OK(xr_lir_set_entry(c->module, c->function, c->entry));
    XrLirType parameters[] = {XR_LIR_ARRAY,XR_LIR_I64,XR_LIR_ARRAY};
    OK(xr_lir_add_block(c->module, c->function, parameters, c->arity, &c->header));
    for (uint32_t n = 0; n < c->count; ++n)
        OK(xr_lir_add_block(c->module, c->function, parameters, c->arity, &c->body[c->count - 1 - n]));
    OK(xr_lir_add_block(c->module, c->function, parameters, c->arity, &c->exit));
    for (uint32_t n = 0; n < c->count + 2; ++n) {
        XrLirBlock block = n == c->count ? c->header : n == c->count + 1 ? c->exit : c->body[n];
        XrLirValue values[3] = {{0}}; ars_parameters(c, block, values);
        OK(xr_lir_set_array_shape(c->module, values[0], &(XrLirArrayShape){XR_LIR_I64}));
        if (c->arity == 3) OK(xr_lir_set_array_shape(c->module, values[2], &(XrLirArrayShape){XR_LIR_I64}));
    }
}
static void ars_entry_header(ArraySetRegionFixture *c) {
    XrLirValue seed = ars_number(c, c->entry, 7), values[3] = {{0}};
    values[0] = ars_array_op(c, c->entry, XR_LIR_ARRAY_OP_NEW, &seed, 1);
    values[1] = ars_number(c, c->entry, 0);
    if (c->arity == 3) values[2] = ars_value(c, c->entry, (XrLirInstruction){.op=XR_LIR_ARRAY_RETAIN,
        .type=XR_LIR_ARRAY,.operands=&values[0],.operand_count=1});
    if (c->mode == ARRAY_SET_REGION_EXTRA_ENTRY) {
        XrLirValue condition = ars_value(c, c->entry, (XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1});
        OK(xr_lir_set_terminator(c->module, c->entry, &(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
            .operands=&condition,.operand_count=1,.edge_count=2,.edges={{c->header,values,c->arity},{c->body[0],values,c->arity}}}));
    } else ars_branch(c, c->entry, c->header, values);
    ars_parameters(c, c->header, values); c->header_array = values[0];
    if (c->mode == ARRAY_SET_REGION_HEADER_WRITE) values[0] = ars_mutate(c, c->header, values[0]);
    (void)ars_array_op(c, c->header, XR_LIR_ARRAY_OP_LEN, values, 1);
    XrLirValue limit = ars_number(c, c->header, 4), compared[] = {values[1],limit};
    XrLirValue condition = ars_value(c, c->header, (XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,
        .predicate=XR_LIR_LT,.operands=compared,.operand_count=2});
    OK(xr_lir_set_terminator(c->module, c->header, &(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&condition,.operand_count=1,.edge_count=2,.edges={{c->body[0],values,c->arity},{c->exit,values,c->arity}}}));
}
static void ars_body_exit(ArraySetRegionFixture *c) {
    for (uint32_t n = 0; n < c->count; ++n) {
        XrLirValue values[3] = {{0}}; ars_parameters(c, c->body[n], values);
        if (!n && c->mode != ARRAY_SET_REGION_HEADER_WRITE) values[0] = ars_mutate(c, c->body[n], values[0]);
        if (n + 1 == c->count) {
            XrLirValue one = ars_number(c, c->body[n], 1), operands[] = {values[1],one};
            values[1] = ars_value(c, c->body[n], (XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,
                .operands=operands,.operand_count=2});
        }
        if (c->mode == ARRAY_SET_REGION_EARLY_EXIT || c->mode == ARRAY_SET_REGION_EXTRA_BACKEDGE) {
            XrLirValue condition = ars_value(c, c->body[n], (XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1});
            XrLirBlock other = c->mode == ARRAY_SET_REGION_EARLY_EXIT ? c->exit : c->header;
            XrLirBlock next = n + 1 == c->count ? c->header : c->body[n + 1];
            OK(xr_lir_set_terminator(c->module, c->body[n], &(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
                .operands=&condition,.operand_count=1,.edge_count=2,.edges={{next,values,c->arity},{other,values,c->arity}}}));
        } else ars_branch(c, c->body[n], n + 1 == c->count ? c->header : c->body[n + 1], values);
    }
    XrLirValue values[3] = {{0}}; ars_parameters(c, c->exit, values);
    c->result = ars_array_op(c, c->exit, XR_LIR_ARRAY_OP_LEN, values, 1);
    ars_effect(c, c->exit, XR_LIR_ARRAY_DROP, values[0]);
    if (c->arity == 3) ars_effect(c, c->exit, XR_LIR_ARRAY_DROP, values[2]);
    OK(xr_lir_set_terminator(c->module, c->exit, &(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&c->result,.operand_count=1}));
}
static ArraySetRegionFixture array_set_region_build(XrLirModule *module, uint32_t count, enum ArraySetRegionMode mode) {
    CHECK(count && count <= 8);
    ArraySetRegionFixture c = {.module=module,.count=count,.mode=mode,.arity=mode == ARRAY_SET_REGION_SNAPSHOT ? 3u : 2u};
    if (!c.module) OK(xr_lir_array_module_create(NULL, &c.module));
    ars_declare(&c); ars_entry_header(&c); ars_body_exit(&c);
    return c;
}
static ArraySetRegionFixture array_set_region_fixture(uint32_t count, enum ArraySetRegionMode mode) {
    ArraySetRegionFixture c = array_set_region_build(NULL, count, mode);
    OK(xr_lir_module_finish(c.module, NULL));
    return c;
}
#endif // LIR_ARRAY_SET_REGION_FIXTURE_H
