/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_boundary_fixture.h - Borrowed inputs, independent deaths and landings
 */
#ifndef LIR_SLICE_BOUNDARY_FIXTURE_H
#define LIR_SLICE_BOUNDARY_FIXTURE_H
#include "lir/xlir_internal.h"
#include "lir/xlir_array.h"

typedef struct SliceBoundaryFixture {
    XrLirModule *module;
    XrLirType slice, element;
    XrLirFunction leaf, relay, fanout, read;
    XrLirValue formal, call_result;
} SliceBoundaryFixture;

static XrLirValue slice_boundary_value(SliceBoundaryFixture *c, XrLirBlock block, XrLirInstruction op) {
    XrLirValue value = {0}; OK(xr_lir_emit_value(c->module,block,&op,&value)); return value;
}
static XrLirValue slice_boundary_role(SliceBoundaryFixture *c, XrLirBlock block,
    XrLirSliceOperation role, XrLirType type, XrLirValue input) {
    uint32_t id = 0; OK(xr_lir_slice_operation(c->module,role,c->element,0,&id));
    XrLirInstruction op = {.op=XR_LIR_CALL_INTRINSIC,.type=type,.intrinsic=id,.operands=&input,.operand_count=1};
    if (type == XR_LIR_UNIT) { OK(xr_lir_emit_effect(c->module,block,&op)); return (XrLirValue){0}; }
    return slice_boundary_value(c,block,op);
}
static void slice_boundary_return(SliceBoundaryFixture *c, XrLirBlock block, XrLirValue input) {
    XrLirInstruction op = {.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&input,.operand_count=1};
    OK(xr_lir_set_terminator(c->module,block,&op));
}
static void slice_boundary_cleanup(SliceBoundaryFixture *c, XrLirBlock block, XrLirValue input) {
    XrLirInstruction op = {.op=XR_LIR_SLICE_FORMAL_CLEANUP,.type=XR_LIR_UNIT,.bits=input.id};
    OK(xr_lir_emit_effect(c->module,block,&op));
}
static XrLirBlock slice_boundary_function(SliceBoundaryFixture *c, XrLirFunction *function,
    const XrLirType *parameters, uint32_t count, XrLirType result) {
    static const char *names[] = {"leaf","relay","fanout","read"};
    uint32_t ordinal = c->module->function_count; CHECK(ordinal < 4);
    XrLirFunctionSpec spec = {.name=names[ordinal],.name_length=strlen(names[ordinal]),
        .parameters=parameters,.parameter_count=count,.result=result};
    OK(xr_lir_declare_function(c->module,&spec,function)); OK(xr_lir_define_function(c->module,*function));
    XrLirBlock block = {0}; OK(xr_lir_add_block(c->module,*function,NULL,0,&block));
    OK(xr_lir_set_entry(c->module,*function,block)); return block;
}
static XrLirValue slice_boundary_parameter(SliceBoundaryFixture *c, XrLirFunction function, uint32_t ordinal) {
    XrLirValue value = {0}; OK(xr_lir_function_parameter(c->module,function,ordinal,&value)); return value;
}
static void slice_boundary_contracts_for_root(SliceBoundaryFixture *c, XrLirFunction root) {
    XrLirSliceFunctionFact rows[6] = {{0}};
    XrLirFunction functions[] = {c->leaf,c->relay,c->fanout,c->read};
    for (size_t n = 0; n < 4; ++n) rows[functions[n].id] = (XrLirSliceFunctionFact){.function=functions[n].id,
        .parameter_count=n == 1 ? 2u : 1u,.result_parameter=UINT32_MAX,.available=true};
    const XrLirSliceEffect terms[] = {{XR_LIR_SLICE_EFFECT_PARAMETER,0},{XR_LIR_SLICE_EFFECT_PARAMETER,1},
        {XR_LIR_SLICE_EFFECT_UNKNOWN,0},{XR_LIR_SLICE_EFFECT_FIELD,17}};
    rows[c->leaf.id].result_parameter = 0; rows[c->leaf.id].result_origin = (XrLirSliceEffectSpan){0,1};
    rows[c->relay.id].result_parameter = 1; rows[c->relay.id].result_origin = (XrLirSliceEffectSpan){1,1};
    /* The original metadata fixture keeps conservative symbolic facts on an
     * unused helper. The executable root instead calls its actual pure body. */
    if (!root.id) {
        rows[c->read.id].write = (XrLirSliceEffectSpan){2,1}; rows[c->read.id].access = (XrLirSliceEffectSpan){3,1};
    }
    if (root.id) rows[root.id]=(XrLirSliceFunctionFact){.function=root.id,.result_parameter=UINT32_MAX,.available=true};
    OK(xr_lir_internal_slice_effects_set(c->module,(XrLirSliceFactsView){.functions=rows,
        .function_count=root.id ? 5u : 4u,.terms=terms,.term_count=4}));
}
static SliceBoundaryFixture slice_boundary_fixture_body(XrLirType element, bool read_after_cleanup) {
    SliceBoundaryFixture c = {.element=element}; OK(xr_lir_module_create(NULL,&c.module));
    OK(xr_lir_slice_catalog_install(c.module)); OK(xr_lir_slice_type(c.module,element,&c.slice));
    XrLirBlock block = slice_boundary_function(&c,&c.leaf,&c.slice,1,c.slice);
    XrLirValue formal = slice_boundary_parameter(&c,c.leaf,0);
    XrLirValue kept = slice_boundary_role(&c,block,XR_LIR_SLICE_OP_BIND,c.slice,formal);
    slice_boundary_cleanup(&c,block,formal); slice_boundary_cleanup(&c,block,formal);
    if (read_after_cleanup) (void)slice_boundary_role(&c,block,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,formal);
    slice_boundary_return(&c,block,kept);
    XrLirType parameters[] = {XR_LIR_I64,c.slice};
    block = slice_boundary_function(&c,&c.relay,parameters,2,c.slice);
    formal = slice_boundary_parameter(&c,c.relay,1);
    kept = slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CALL,.type=c.slice,
        .callee=c.leaf,.operands=&formal,.operand_count=1});
    slice_boundary_cleanup(&c,block,formal); slice_boundary_return(&c,block,kept);
    block = slice_boundary_function(&c,&c.fanout,&c.slice,1,XR_LIR_I64);
    c.formal = slice_boundary_parameter(&c,c.fanout,0);
    XrLirValue answer = slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=42});
    XrLirValue args[] = {answer,c.formal};
    c.call_result = slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CALL,.type=c.slice,
        .callee=c.relay,.operands=args,.operand_count=2});
    (void)slice_boundary_role(&c,block,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,c.formal);
    (void)slice_boundary_role(&c,block,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,c.call_result);
    XrLirType pair[] = {c.slice,c.slice}; XrLirBlock join = {0};
    OK(xr_lir_add_block(c.module,c.fanout,pair,2,&join)); XrLirValue copies[] = {c.formal,c.formal};
    XrLirInstruction branch = {.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,.edges={{join,copies,2}}};
    OK(xr_lir_set_terminator(c.module,block,&branch));
    (void)slice_boundary_role(&c,join,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,c.formal);
    slice_boundary_cleanup(&c,join,c.formal); slice_boundary_cleanup(&c,join,c.formal);
    for (uint32_t p = 0; p < 2; ++p) {
        XrLirValue value = {0}; OK(xr_lir_block_parameter(c.module,join,p,&value));
        (void)slice_boundary_role(&c,join,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,value);
    }
    slice_boundary_return(&c,join,answer);
    block = slice_boundary_function(&c,&c.read,&c.slice,1,XR_LIR_I64);
    formal = slice_boundary_parameter(&c,c.read,0);
    kept = slice_boundary_role(&c,block,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,formal);
    slice_boundary_cleanup(&c,block,formal); slice_boundary_cleanup(&c,block,formal);
    slice_boundary_return(&c,block,kept); return c;
}
static SliceBoundaryFixture slice_boundary_fixture(XrLirType element, bool read_after_cleanup) {
    SliceBoundaryFixture c=slice_boundary_fixture_body(element,read_after_cleanup);
    slice_boundary_contracts_for_root(&c,(XrLirFunction){0}); return c;
}
#endif // LIR_SLICE_BOUNDARY_FIXTURE_H
