/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_frame_scalar_cell_fixture.h - Actual late scalar identities and callbacks
 */
#ifndef LIR_FRAME_SCALAR_CELL_FIXTURE_H
#define LIR_FRAME_SCALAR_CELL_FIXTURE_H
typedef enum FrameScalarCellCase {
    FRAME_SCALAR_CELL_VALID, FRAME_SCALAR_CELL_LOOP, FRAME_SCALAR_CELL_UNINITIALIZED
} FrameScalarCellCase;
typedef struct FrameScalarCellFixture {
    XrLirModule *module;
    XrLirFunction root, choose;
    XrLirBlock entry, left, right, join;
} FrameScalarCellFixture;

static XrLirValue frame_scalar_cell_call(CArrayFixture *c,XrLirBlock block,XrLirFunction target,
    XrLirValue argument) {
    XrLirValue result={0};
    OK(xr_lir_emit_value(c->module,block,&(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,
        .callee=target,.operands=&argument,.operand_count=1,.source=ca_source()},&result));
    return result;
}
static XrLirValue frame_scalar_cell_callback(CArrayFixture *c,XrLirBlock block,
    XrLirFunction worker,XrLirType callable,XrLirValue cell) {
    XrLirValue callback={0};
    OK(xr_lir_emit_value(c->module,block,&(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,.type=callable,
        .callee=worker,.operands=&cell,.operand_count=1,.bits=XR_LIR_CLOSURE_FRAME,.source=ca_source()},&callback));
    return callback;
}
static FrameScalarCellFixture frame_scalar_cell_fixture(FrameScalarCellCase kind) {
    FrameScalarCellFixture f={0};CArrayFixture c={0};OK(xr_lir_module_create(NULL,&c.module));f.module=c.module;
    XrLirType callable=0,cell_type=XR_LIR_I64_CELL;
    OK(xr_lir_callable_type(c.module,&(XrLirCallableType){.result=XR_LIR_I64},&callable));
    XrLirFunction worker={0};
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="increment",.name_length=9,
        .parameters=&cell_type,.parameter_count=1,.result=XR_LIR_I64},&worker));
    OK(xr_lir_set_closure_target(c.module,worker,(XrLirClosureTarget){callable,1,false}));
    OK(xr_lir_define_function(c.module,worker));XrLirBlock body={0};XrLirValue formal={0};
    OK(xr_lir_add_block(c.module,worker,NULL,0,&body));OK(xr_lir_set_entry(c.module,worker,body));
    OK(xr_lir_function_parameter(c.module,worker,0,&formal));
    XrLirValue old=ca_value(&c,body,XR_LIR_CELL_READ,XR_LIR_I64,&formal,1);
    XrLirValue next=ca_binary(&c,body,XR_LIR_ADD,old,ca_constant(&c,body,1)),write[]={formal,next};
    OK(xr_lir_emit_effect(c.module,body,&(XrLirInstruction){.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,
        .operands=write,.operand_count=2,.source=ca_source()}));ca_return(&c,body,next);
    XrLirType flag_type=XR_LIR_I1;
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="choose",.name_length=6,
        .parameters=&flag_type,.parameter_count=1,.result=XR_LIR_I64},&f.choose));
    OK(xr_lir_define_function(c.module,f.choose));
    OK(xr_lir_add_block(c.module,f.choose,NULL,0,&f.entry));OK(xr_lir_set_entry(c.module,f.choose,f.entry));
    OK(xr_lir_add_block(c.module,f.choose,NULL,0,&f.left));OK(xr_lir_add_block(c.module,f.choose,NULL,0,&f.right));
    XrLirType join_type=kind==FRAME_SCALAR_CELL_UNINITIALIZED?XR_LIR_I64_CELL:XR_LIR_I64;
    OK(xr_lir_add_block(c.module,f.choose,&join_type,1,&f.join));XrLirValue flag={0};
    OK(xr_lir_function_parameter(c.module,f.choose,0,&flag));
    OK(xr_lir_set_terminator(c.module,f.entry,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&flag,.operand_count=1,.edge_count=2,.edges={{f.left,NULL,0},{f.right,NULL,0}}}));
    XrLirValue seed=ca_constant(&c,f.left,10);
    XrLirValue first=ca_value(&c,f.left,XR_LIR_CELL_NEW,XR_LIR_I64_CELL,&seed,1);
    seed=ca_constant(&c,f.left,20);
    XrLirValue second=ca_value(&c,f.left,XR_LIR_CELL_NEW,XR_LIR_I64_CELL,&seed,1);
    XrLirValue callback=frame_scalar_cell_callback(&c,f.left,worker,callable,first);
    (void)ca_value(&c,f.left,XR_LIR_CALL_INDIRECT,XR_LIR_I64,&callback,1);
    XrLirValue value=ca_value(&c,f.left,XR_LIR_CALL_INDIRECT,XR_LIR_I64,&callback,1);
    XrLirValue other=ca_value(&c,f.left,XR_LIR_CELL_READ,XR_LIR_I64,&second,1);
    XrLirValue left=kind==FRAME_SCALAR_CELL_UNINITIALIZED?first:ca_binary(&c,f.left,XR_LIR_ADD,value,other);
    XrLirInstruction branch={.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,.edges={{f.join,&left,1}}};
    if (kind==FRAME_SCALAR_CELL_LOOP) branch.edges[0]=(XrLirEdge){f.left,NULL,0};
    OK(xr_lir_set_terminator(c.module,f.left,&branch));
    seed=ca_constant(&c,f.right,40);
    XrLirValue later=ca_value(&c,f.right,XR_LIR_CELL_NEW,XR_LIR_I64_CELL,&seed,1);
    callback=frame_scalar_cell_callback(&c,f.right,worker,callable,later);
    XrLirValue right=ca_value(&c,f.right,XR_LIR_CALL_INDIRECT,XR_LIR_I64,&callback,1);
    if (kind==FRAME_SCALAR_CELL_UNINITIALIZED) right=first;
    branch.edges[0]=(XrLirEdge){f.join,&right,1};OK(xr_lir_set_terminator(c.module,f.right,&branch));
    XrLirValue joined={0};OK(xr_lir_block_parameter(c.module,f.join,0,&joined));
    if (kind==FRAME_SCALAR_CELL_UNINITIALIZED) joined=ca_value(&c,f.join,XR_LIR_CELL_READ,XR_LIR_I64,&joined,1);
    ca_return(&c,f.join,joined);
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_I64},&f.root));
    OK(xr_lir_define_function(c.module,f.root));OK(xr_lir_add_block(c.module,f.root,NULL,0,&body));
    OK(xr_lir_set_entry(c.module,f.root,body));
    XrLirValue yes={0},no={0};XrLirInstruction boolean={.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1};
    OK(xr_lir_emit_value(c.module,body,&boolean,&yes));boolean.bits=0;OK(xr_lir_emit_value(c.module,body,&boolean,&no));
    XrLirValue a=frame_scalar_cell_call(&c,body,f.choose,yes),b=frame_scalar_cell_call(&c,body,f.choose,no);
    ca_return(&c,body,ca_binary(&c,body,XR_LIR_ADD,a,b));return f;
}
#endif // LIR_FRAME_SCALAR_CELL_FIXTURE_H
