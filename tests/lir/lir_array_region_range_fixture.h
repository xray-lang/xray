/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_region_range_fixture.h - Exact arithmetic shapes on real owner routes
 */
#ifndef LIR_ARRAY_REGION_RANGE_FIXTURE_H
#define LIR_ARRAY_REGION_RANGE_FIXTURE_H
#include "lir_array_set_region_fixture.h"
#include "base/xhash.h"

enum ArrayRegionRangeMode {
    ARRAY_REGION_RANGE_UNIT, ARRAY_REGION_RANGE_PARALLEL_BAD,
    ARRAY_REGION_RANGE_BOUND, ARRAY_REGION_RANGE_PREDICATE, ARRAY_REGION_RANGE_FALSE_BODY,
    ARRAY_REGION_RANGE_COMPUTED_GET, ARRAY_REGION_RANGE_CONSTANT_SET,
    ARRAY_REGION_RANGE_STEP_TWO, ARRAY_REGION_RANGE_STEP_DYNAMIC,
    ARRAY_REGION_RANGE_COMMUTED, ARRAY_REGION_RANGE_DOUBLE_STEP, ARRAY_REGION_RANGE_LATCH_CURRENT,
    ARRAY_REGION_RANGE_NEXT_COPIES, ARRAY_REGION_RANGE_C_HEADER_LIVEOUT, ARRAY_REGION_RANGE_FAILING_RHS,
    ARRAY_REGION_RANGE_READ_ONLY, ARRAY_REGION_RANGE_READ_EXTRA, ARRAY_REGION_RANGE_READ_INDEX,
    ARRAY_REGION_RANGE_READ_STEP, ARRAY_REGION_RANGE_READ_EFFECT
};
#define ARS_RANGE_FIELDS(r) (r).kind,(r).iv,(r).iv_argument,(r).entry_iv,(r).length,(r).length_instruction, \
    (r).condition,(r).condition_instruction,(r).increment,(r).increment_block,(r).increment_instruction,(r).one,(r).latch_iv
static inline bool ars_range_same(XrLirArraySetRegionRange a, XrLirArraySetRegionRange b) {
    uint32_t left[] = {ARS_RANGE_FIELDS(a)}, right[] = {ARS_RANGE_FIELDS(b)};
    for (size_t n = 0; n < sizeof(left) / sizeof(*left); ++n) if (left[n] != right[n]) return false;
    return true;
}
static inline uint64_t ars_range_digest(XrLirArraySetRegionRange range) {
    uint32_t fields[] = {ARS_RANGE_FIELDS(range)};
    return xr_hash_bytes64(fields, sizeof(fields));
}
static inline bool ars_owner_home_same(XrLirArraySetRegionOwnerHome a, XrLirArraySetRegionOwnerHome b) {
    return a.kind == b.kind && a.first_active_step == b.first_active_step && a.exit_step == b.exit_step;
}
static inline uint64_t ars_owner_home_digest(XrLirArraySetRegionOwnerHome home) {
    uint32_t fields[] = {home.kind,home.first_active_step,home.exit_step};
    return xr_hash_bytes64(fields,sizeof(fields));
}
static void ars_range_edge(ArraySetRegionFixture *c, XrLirBlock block, XrLirBlock target, XrLirValue values[4]) {
    OK(xr_lir_set_terminator(c->module, block, &(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{target,values,4}}}));
}
static void ars_range_parameters(ArraySetRegionFixture *c, XrLirBlock block, XrLirValue values[4]) {
    for (uint32_t n = 0; n < 4; ++n) OK(xr_lir_block_parameter(c->module, block, n, &values[n]));
}
static XrLirValue ars_range_mutate(ArraySetRegionFixture *c, XrLirValue values[4],
    enum ArrayRegionRangeMode mode) {
    XrLirBlock block = c->body[0];
    XrLirValue index = values[1], args[] = {values[0],index};
    if (mode == ARRAY_REGION_RANGE_COMPUTED_GET || mode == ARRAY_REGION_RANGE_READ_INDEX) {
        XrLirValue zero = ars_number(c, block, 0), add[] = {index,zero};
        args[1] = ars_value(c, block, (XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    }
    XrLirValue item = ars_array_op(c, block, XR_LIR_ARRAY_OP_GET, args, 2);
    if (mode >= ARRAY_REGION_RANGE_READ_ONLY) {
        if (mode == ARRAY_REGION_RANGE_READ_EXTRA) (void)ars_array_op(c,block,XR_LIR_ARRAY_OP_GET,args,2);
        if (mode == ARRAY_REGION_RANGE_READ_EFFECT) {
            XrLirValue operands[]={item,ars_number(c,block,1)};
            (void)ars_value(c,block,(XrLirInstruction){.op=XR_LIR_DIV,.type=XR_LIR_I64,.operands=operands,.operand_count=2});
        }
        return values[0];
    }
    (void)ars_array_op(c, block, XR_LIR_ARRAY_OP_GET, args, 2);
    if (mode == ARRAY_REGION_RANGE_C_HEADER_LIVEOUT || mode == ARRAY_REGION_RANGE_FAILING_RHS) {
        XrLirValue external = {0}; OK(xr_lir_function_parameter(c->module,c->function,0,&external));
        XrLirValue sum[] = {item,external};
        item = ars_value(c,block,(XrLirInstruction){.op=mode == ARRAY_REGION_RANGE_C_HEADER_LIVEOUT ? XR_LIR_ADD : XR_LIR_DIV,
            .type=XR_LIR_I64,.operands=sum,.operand_count=2});
    }
    XrLirValue cell = ars_value(c, block, (XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,
        .operands=values,.operand_count=1});
    ars_effect(c, block, XR_LIR_ARRAY_DROP, values[0]);
    if (mode == ARRAY_REGION_RANGE_CONSTANT_SET) index = ars_number(c, block, 0);
    uint32_t set = 0; OK(xr_lir_array_operation(c->module, XR_LIR_I64, XR_LIR_ARRAY_OP_SET, &set));
    XrLirValue write[] = {cell,index,item};
    OK(xr_lir_emit_effect(c->module, block, &(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
        .intrinsic=set,.operands=write,.operand_count=3}));
    XrLirValue result = ars_value(c, block, (XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_ARRAY,
        .operands=&cell,.operand_count=1});
    ars_effect(c, block, XR_LIR_ARRAY_CELL_DROP, cell);
    return result;
}
static ArraySetRegionFixture ars_range_build(XrLirModule *module, uint32_t count, enum ArrayRegionRangeMode mode) {
    CHECK(count && count <= ARRAY_SET_REGION_BODY_CAPACITY);
    ArraySetRegionFixture c = {.module=module,.count=count,.arity=4};
    if (!module) OK(xr_lir_array_module_create(NULL, &c.module));
    XrLirType scalar = XR_LIR_I64, types[] = {XR_LIR_ARRAY,XR_LIR_I64,XR_LIR_I64,XR_LIR_I64};
    char name[40]; int length = snprintf(name,sizeof(name),"range_%u_%u",(unsigned)mode,(unsigned)count);
    CHECK(length > 0 && (size_t)length < sizeof(name));
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name=name,.name_length=(size_t)length,
        .parameters=&scalar,.parameter_count=1,.result=XR_LIR_I64},&c.function));
    OK(xr_lir_define_function(c.module,c.function));
    OK(xr_lir_add_block(c.module,c.function,NULL,0,&c.entry)); OK(xr_lir_set_entry(c.module,c.function,c.entry));
    c.region_entry = c.entry;
    OK(xr_lir_add_block(c.module,c.function,types,4,&c.header));
    for (uint32_t n = 0; n < count; ++n) OK(xr_lir_add_block(c.module,c.function,types,4,&c.body[count - 1 - n]));
    OK(xr_lir_add_block(c.module,c.function,types,4,&c.exit));
    for (uint32_t n = 0; n < count + 2; ++n) {
        XrLirBlock block = n == count ? c.header : n == count + 1 ? c.exit : c.body[n];
        XrLirValue array = {0}; OK(xr_lir_block_parameter(c.module,block,0,&array));
        OK(xr_lir_set_array_shape(c.module,array,&(XrLirArrayShape){XR_LIR_I64}));
    }
    XrLirValue seed = ars_number(&c,c.entry,7), start = {0};
    OK(xr_lir_function_parameter(c.module,c.function,0,&start));
    XrLirValue incoming[] = {ars_array_op(&c,c.entry,XR_LIR_ARRAY_OP_NEW,&seed,1),
        ars_number(&c,c.entry,-1),ars_number(&c,c.entry,0),start};
    ars_range_edge(&c,c.entry,c.header,incoming);
    XrLirValue header[4]; ars_range_parameters(&c,c.header,header); c.header_array = header[0];
    XrLirValue bound = ars_array_op(&c,c.header,XR_LIR_ARRAY_OP_LEN,header,1);
    if (mode == ARRAY_REGION_RANGE_BOUND) bound = ars_number(&c,c.header,4);
    XrLirValue compared[] = {header[3],bound};
    XrLirValue condition = ars_value(&c,c.header,(XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,
        .predicate=mode == ARRAY_REGION_RANGE_PREDICATE ? XR_LIR_LE : XR_LIR_LT,.operands=compared,.operand_count=2});
    XrLirValue routed[] = {header[0],header[3],header[3],header[1]};
    bool reverse = mode == ARRAY_REGION_RANGE_FALSE_BODY;
    OK(xr_lir_set_terminator(c.module,c.header,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&condition,.operand_count=1,.edge_count=2,
        .edges={{reverse ? c.exit : c.body[0],routed,4},{reverse ? c.body[0] : c.exit,routed,4}}}));
    for (uint32_t n = 0; n < count; ++n) {
        XrLirValue values[4]; ars_range_parameters(&c,c.body[n],values);
        uint32_t iv_slot = 1 + n % 3;
        if (!n) values[0] = ars_range_mutate(&c,values,mode);
        XrLirValue next = values[iv_slot];
        if ((n + 1 == count && mode != ARRAY_REGION_RANGE_NEXT_COPIES) || (!n && mode == ARRAY_REGION_RANGE_NEXT_COPIES)) {
            XrLirValue one = mode == ARRAY_REGION_RANGE_STEP_DYNAMIC ? start :
                ars_number(&c,c.body[n],mode == ARRAY_REGION_RANGE_STEP_TWO || mode == ARRAY_REGION_RANGE_READ_STEP ? 2 : 1);
            XrLirValue add[] = {next,one};
            if (mode == ARRAY_REGION_RANGE_COMMUTED) { add[0] = one; add[1] = next; }
            next = ars_value(&c,c.body[n],(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
            if (mode == ARRAY_REGION_RANGE_DOUBLE_STEP)
                (void)ars_value(&c,c.body[n],(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
        }
        if (n + 1 == count) {
            XrLirValue latch[] = {values[0],next,header[1],mode == ARRAY_REGION_RANGE_PARALLEL_BAD ? header[2] :
                mode == ARRAY_REGION_RANGE_LATCH_CURRENT ? values[iv_slot] : next};
            ars_range_edge(&c,c.body[n],c.header,latch);
        } else {
            uint32_t next_slot = 1 + (n + 1) % 3;
            XrLirValue copied[] = {values[0],header[1],header[1],header[1]};
            copied[next_slot] = next; copied[1 + next_slot % 3] = next;
            ars_range_edge(&c,c.body[n],c.body[n + 1],copied);
        }
    }
    XrLirValue exited[4]; ars_range_parameters(&c,c.exit,exited);
    c.result = ars_array_op(&c,c.exit,XR_LIR_ARRAY_OP_LEN,exited,1);
    if (mode == ARRAY_REGION_RANGE_C_HEADER_LIVEOUT) {
        XrLirValue sum[] = {bound,exited[1]};
        c.result = ars_value(&c,c.exit,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=sum,.operand_count=2});
    }
    ars_effect(&c,c.exit,XR_LIR_ARRAY_DROP,exited[0]);
    OK(xr_lir_set_terminator(c.module,c.exit,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&c.result,.operand_count=1}));
    return c;
}
#endif // LIR_ARRAY_REGION_RANGE_FIXTURE_H
