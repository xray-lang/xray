/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_boundary_root_fixture.h - Executable calls around independent loans
 */
#ifndef LIR_SLICE_BOUNDARY_ROOT_FIXTURE_H
#define LIR_SLICE_BOUNDARY_ROOT_FIXTURE_H
#include "lir_slice_boundary_fixture.h"
static SliceBoundaryFixture slice_boundary_root_fixture(XrLirType element, XrLirFunction *root) {
    SliceBoundaryFixture c=slice_boundary_fixture_body(element,false);
    XrLirFunctionSpec spec={.name="entry",.name_length=5,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,root)); OK(xr_lir_define_function(c.module,*root));
    XrLirBlock block={0}; OK(xr_lir_add_block(c.module,*root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,*root,block));
    XrLirValue values[3];
    for (uint32_t n=0;n<3;++n) values[n]=slice_boundary_value(&c,block,
        (XrLirInstruction){.op=XR_LIR_CONST,.type=element,.bits=10u*(n+1)});
    uint32_t id=0; OK(xr_lir_array_operation(c.module,element,XR_LIR_ARRAY_OP_NEW,&id));
    XrLirValue array=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=XR_LIR_ARRAY,.intrinsic=id,.operands=values,.operand_count=3});
    XrLirValue cell=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CELL_NEW,
        .type=XR_LIR_ARRAY_CELL,.operands=&array,.operand_count=1});
    /* Constructed values receive their family from the catalog and input.
     * The explicit shape setter belongs only to unbound formal parameters. */
    const XrLirFunctionData *entry=&c.module->functions[root->id-1];
    CHECK(entry->values[array.id-1].array_element==element && entry->values[cell.id-1].array_element==element);
    XrLirValue first=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=0});
    XrLirValue last=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=3});
    XrLirValue range[]={cell,first,last}; OK(xr_lir_slice_operation(c.module,XR_LIR_SLICE_OP_NEW,element,XR_LIR_ARRAY_CELL,&id));
    XrLirValue view=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=c.slice,.intrinsic=id,.operands=range,.operand_count=3});
    XrLirValue answer=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CALL,
        .type=XR_LIR_I64,.callee=c.fanout,.operands=&view,.operand_count=1});
    XrLirValue args[]={answer,view};
    XrLirValue result=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CALL,
        .type=c.slice,.callee=c.relay,.operands=args,.operand_count=2});
    XrLirValue length=slice_boundary_role(&c,block,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,result);
    XrLirValue index=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=1});
    XrLirValue get[]={result,index}; OK(xr_lir_slice_operation(c.module,XR_LIR_SLICE_OP_GET,element,0,&id));
    XrLirValue item=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=element,.intrinsic=id,.operands=get,.operand_count=2});
    if (element==XR_LIR_U8) item=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_INTCAST,
        .type=XR_LIR_I64,.operands=&item,.operand_count=1});
    XrLirValue caller_length=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_CALL,
        .type=XR_LIR_I64,.callee=c.read,.operands=&view,.operand_count=1});
    (void)slice_boundary_role(&c,block,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,view);
    XrLirValue totals[]={answer,item,length,caller_length},sum=totals[0];
    for (uint32_t n=1;n<4;++n) {
        XrLirValue add[]={sum,totals[n]};
        sum=slice_boundary_value(&c,block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    }
    (void)slice_boundary_role(&c,block,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,result);
    (void)slice_boundary_role(&c,block,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,view);
    XrLirInstruction drop={.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
    OK(xr_lir_emit_effect(c.module,block,&drop)); drop.op=XR_LIR_ARRAY_DROP; drop.operands=&array;
    OK(xr_lir_emit_effect(c.module,block,&drop)); slice_boundary_return(&c,block,sum);
    slice_boundary_contracts_for_root(&c,*root); return c;
}
#endif // LIR_SLICE_BOUNDARY_ROOT_FIXTURE_H
