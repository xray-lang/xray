/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_region_loop_fixture.h - Independent loops with distinct value spans
 */
#ifndef LIR_ARRAY_REGION_LOOP_FIXTURE_H
#define LIR_ARRAY_REGION_LOOP_FIXTURE_H
#include "lir_array_region_range_fixture.h"
static XrLirBlock array_region_binding_block(ArraySetRegionFixture *c) {
    const XrLirType types[]={XR_LIR_ARRAY,XR_LIR_I64}; XrLirBlock block={0}; XrLirValue array={0};
    OK(xr_lir_add_block(c->module,c->function,types,2,&block));
    OK(xr_lir_block_parameter(c->module,block,0,&array));
    OK(xr_lir_set_array_shape(c->module,array,&(XrLirArrayShape){XR_LIR_I64}));
    return block;
}
static XrLirFunction array_region_borrow_observer(ArraySetRegionFixture *c) {
    XrLirFunction observer={0}; XrLirBlock block={0}; XrLirValue array={0};
    XrLirType type=XR_LIR_ARRAY;
    OK(xr_lir_declare_function(c->module,&(XrLirFunctionSpec){.name="observe_array",.name_length=13,
        .parameters=&type,.parameter_count=1,.result=XR_LIR_UNIT},&observer));
    OK(xr_lir_function_parameter(c->module,observer,0,&array));
    OK(xr_lir_set_array_shape(c->module,array,&(XrLirArrayShape){XR_LIR_I64}));
    OK(xr_lir_define_function(c->module,observer));
    OK(xr_lir_add_block(c->module,observer,NULL,0,&block)); OK(xr_lir_set_entry(c->module,observer,block));
    (void)ars_array_op(c,block,XR_LIR_ARRAY_OP_LEN,&array,1);
    OK(xr_lir_set_terminator(c->module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT}));
    return observer;
}
static ArraySetRegionFixture array_region_binding_loops(bool shared,bool reverse,bool range,unsigned read_mask,
    bool observe_second) {
    CHECK(read_mask<=3 && (!read_mask || range) && (!observe_second || (!shared && range && !read_mask)));
    ArraySetRegionFixture c={.arity=2,.mode=ARRAY_SET_REGION_LOCAL};
    OK(xr_lir_array_module_create(NULL,&c.module));
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="two_regions",.name_length=11,
        .result=XR_LIR_I64},&c.function));
    if (observe_second) c.helper=array_region_borrow_observer(&c);
    OK(xr_lir_define_function(c.module,c.function));
    OK(xr_lir_add_block(c.module,c.function,NULL,0,&c.entry)); OK(xr_lir_set_entry(c.module,c.function,c.entry));
    XrLirBlock current=c.entry,next_header={0};
    if (reverse) next_header=array_region_binding_block(&c);
    for (uint32_t loop=0;loop<2;++loop) {
        c.count=loop+2;
        if (shared && loop) c.header=next_header;
        else c.header=array_region_binding_block(&c);
        for (uint32_t n=0;n<c.count;++n) c.body[n]=array_region_binding_block(&c);
        if (shared && !loop && reverse) c.exit=next_header;
        else c.exit=array_region_binding_block(&c);
        if (shared && !loop) next_header=c.exit;
        XrLirValue values[4]={{0}};
        if (!shared || !loop) {
            XrLirValue item=ars_number(&c,current,7);
            values[0]=ars_array_op(&c,current,XR_LIR_ARRAY_OP_NEW,&item,1); values[1]=ars_number(&c,current,0);
            if (observe_second && loop)
                OK(xr_lir_emit_effect(c.module,current,&(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,
                    .callee=c.helper,.operands=values,.operand_count=1}));
            ars_branch(&c,current,c.header,values);
        }
        ars_parameters(&c,c.header,values); c.header_array=values[0];
        XrLirValue length=ars_array_op(&c,c.header,XR_LIR_ARRAY_OP_LEN,values,1);
        XrLirValue limit=range ? length : ars_number(&c,c.header,shared && loop ? 8 : 4),operands[]={values[1],limit};
        XrLirValue condition=ars_value(&c,c.header,(XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,
            .predicate=XR_LIR_LT,.operands=operands,.operand_count=2});
        OK(xr_lir_set_terminator(c.module,c.header,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
            .operands=&condition,.operand_count=1,.edge_count=2,.edges={{c.body[0],values,2},{c.exit,values,2}}}));
        for (uint32_t n=0;n<c.count;++n) {
            ars_parameters(&c,c.body[n],values);
            if (!n) values[0]=range ? ars_range_mutate(&c,values,(read_mask&(1u<<loop)) ?
                ARRAY_REGION_RANGE_READ_ONLY : ARRAY_REGION_RANGE_UNIT) : ars_mutate(&c,c.body[n],values[0]);
            if (n+1==c.count) {
                XrLirValue increment[]={values[1],ars_number(&c,c.body[n],1)};
                values[1]=ars_value(&c,c.body[n],(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,
                    .operands=increment,.operand_count=2});
            }
            ars_branch(&c,c.body[n],n+1==c.count ? c.header : c.body[n+1],values);
        }
        if (!shared || loop) {
            ars_parameters(&c,c.exit,values); c.result=ars_array_op(&c,c.exit,XR_LIR_ARRAY_OP_LEN,values,1);
            ars_effect(&c,c.exit,XR_LIR_ARRAY_DROP,values[0]);
        }
        current=c.exit;
    }
    OK(xr_lir_set_terminator(c.module,current,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&c.result,.operand_count=1}));
    XrLirDiagnostic diagnostic={0}; XrLirStatus status=xr_lir_module_finish(c.module,&diagnostic);
    if (status!=XR_LIR_OK) fprintf(stderr,"two regions f%u b%u i%u: %s\n",diagnostic.function,
        diagnostic.block,diagnostic.instruction,diagnostic.message);
    OK(status); return c;
}
static ArraySetRegionFixture array_region_binding_two_loops(bool shared,bool reverse,bool range) {
    return array_region_binding_loops(shared,reverse,range,0,false);
}
#endif // LIR_ARRAY_REGION_LOOP_FIXTURE_H
