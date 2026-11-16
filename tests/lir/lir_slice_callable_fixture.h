/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_callable_fixture.h - Real captures across a live local Array loan
 */
#ifndef LIR_SLICE_CALLABLE_FIXTURE_H
#define LIR_SLICE_CALLABLE_FIXTURE_H
#include "lir/xlir_internal.h"
typedef enum SliceCallableCase {
    SLICE_CALLABLE_DIRECT, SLICE_CALLABLE_INDIRECT, SLICE_CALLABLE_OPAQUE,
    SLICE_CALLABLE_ARRAY, SLICE_CALLABLE_UNAVAILABLE, SLICE_CALLABLE_FORMAL,
    SLICE_CALLABLE_ALIAS, SLICE_CALLABLE_PHI, SLICE_CALLABLE_INDIRECT_ARGUMENT,
    SLICE_CALLABLE_DIRECT_LOCAL_WRITE, SLICE_CALLABLE_INDIRECT_LOCAL_WRITE,
    SLICE_CALLABLE_CAPTURE_CONTEXTS, SLICE_CALLABLE_CASE_COUNT
} SliceCallableCase;
static XrLirValue slice_callable_value(XrLirBlock block,XrLirInstruction instruction) {
    XrLirValue value={0};OK(xr_lir_emit_value((XrLirModule *)block.module,block,&instruction,&value));return value;
}
static void slice_callable_effect(XrLirBlock block,XrLirInstruction instruction) {
    OK(xr_lir_emit_effect((XrLirModule *)block.module,block,&instruction));
}
static void slice_callable_return(XrLirBlock block,XrLirValue value) {
    OK(xr_lir_set_terminator((XrLirModule *)block.module,block,&(XrLirInstruction){
        .op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1}));
}
static XrLirBlock slice_callable_function(XrLirModule *module,const char *name,
    const XrLirType *parameters,uint32_t count,XrLirFunction *function) {
    XrLirFunctionSpec spec={.name=name,.name_length=strlen(name),.parameters=parameters,
        .parameter_count=count,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(module,&spec,function));OK(xr_lir_define_function(module,*function));
    XrLirBlock block={0};OK(xr_lir_add_block(module,*function,NULL,0,&block));
    OK(xr_lir_set_entry(module,*function,block));return block;
}
static XrLirValue slice_callable_role(XrLirModule *module,XrLirBlock block,
    XrLirSliceOperation role,XrLirType type,const XrLirValue *arguments,uint32_t count) {
    uint32_t intrinsic=0;
    OK(xr_lir_slice_operation(module,role,XR_LIR_I64,role==XR_LIR_SLICE_OP_NEW?XR_LIR_ARRAY_CELL:0,&intrinsic));
    XrLirInstruction op={.op=XR_LIR_CALL_INTRINSIC,.type=type,.intrinsic=intrinsic,
        .operands=arguments,.operand_count=count};
    if (type!=XR_LIR_UNIT) return slice_callable_value(block,op);
    slice_callable_effect(block,op);return (XrLirValue){0};
}
static XrLirModule *slice_callable_fixture(SliceCallableCase shape) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));OK(xr_lir_slice_catalog_install(m));
    bool contexts=shape==SLICE_CALLABLE_CAPTURE_CONTEXTS;
    bool higher=shape>=SLICE_CALLABLE_INDIRECT_ARGUMENT;
    bool indirect_argument=shape==SLICE_CALLABLE_INDIRECT_ARGUMENT || shape==SLICE_CALLABLE_INDIRECT_LOCAL_WRITE;
    bool array_capture=shape==SLICE_CALLABLE_ARRAY || shape==SLICE_CALLABLE_DIRECT_LOCAL_WRITE ||
        shape==SLICE_CALLABLE_INDIRECT_LOCAL_WRITE || contexts;
    XrLirType callable=0,slice_type=0;
    OK(xr_lir_callable_type(m,&(XrLirCallableType){.result=XR_LIR_I64},&callable));
    OK(xr_lir_slice_type(m,XR_LIR_I64,&slice_type));
    XrLirType parameter=shape==SLICE_CALLABLE_OPAQUE?callable:XR_LIR_I64_CELL;
    uint32_t root_count=shape==SLICE_CALLABLE_OPAQUE || shape==SLICE_CALLABLE_FORMAL;
    XrLirFunction root={0},target={0},apply={0};
    XrLirBlock body=slice_callable_function(m,"caller",&parameter,root_count,&root);
    XrLirType capture_type=array_capture?XR_LIR_ARRAY_REF:XR_LIR_I64_CELL;
    XrLirBlock worker=slice_callable_function(m,"captured",&capture_type,1,&target);
    OK(xr_lir_set_closure_target(m,target,(XrLirClosureTarget){callable,1,false}));
    XrLirType application_parameters[]={callable,contexts?callable:XR_LIR_I64,XR_LIR_I64};
    uint32_t application_count=contexts?3u:higher?2u:1u;
    XrLirBlock application=slice_callable_function(m,"apply",application_parameters,application_count,&apply);
    XrLirType dispatch_type=0;
    if (higher) {
        XrLirCallableParameter parameters[]={{.type=callable,.mode=XR_PARAM_READ},
            {.type=contexts?callable:XR_LIR_I64,.mode=XR_PARAM_READ},{.type=XR_LIR_I64,.mode=XR_PARAM_READ}};
        OK(xr_lir_callable_type(m,&(XrLirCallableType){.parameters=parameters,.parameter_count=application_count,
            .result=XR_LIR_I64},&dispatch_type));
        OK(xr_lir_set_closure_target(m,apply,(XrLirClosureTarget){dispatch_type,0,false}));
    }
    XrLirValue capture={0},callback_parameter={0};
    OK(xr_lir_function_parameter(m,target,0,&capture));
    OK(xr_lir_function_parameter(m,apply,0,&callback_parameter));
    if (array_capture) OK(xr_lir_set_array_shape(m,capture,&(XrLirArrayShape){XR_LIR_I64}));
    XrLirValue one=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=1});
    uint32_t constructor=0;OK(xr_lir_array_operation(m,XR_LIR_I64,XR_LIR_ARRAY_OP_NEW,&constructor));
    XrLirValue array=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
        .intrinsic=constructor,.operands=&one,.operand_count=1});
    XrLirValue cell=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,
        .operands=&array,.operand_count=1});
    slice_callable_effect(body,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&array,.operand_count=1});
    XrLirValue zero=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64});
    XrLirValue range[]={cell,zero,one};
    XrLirValue view={0},actual={0},other_cell={0},first_callback={0};
    if (!array_capture) view=slice_callable_role(m,body,XR_LIR_SLICE_OP_NEW,slice_type,range,3);
    if (array_capture)
        actual=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_ARRAY_BORROW,.type=XR_LIR_ARRAY_REF,
            .operands=&cell,.operand_count=1});
    else if (shape==SLICE_CALLABLE_FORMAL) OK(xr_lir_function_parameter(m,root,0,&actual));
    else actual=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_I64_CELL,
        .operands=&one,.operand_count=1});
    XrLirValue callback=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,.type=callable,
        .callee=target,.operands=&actual,.operand_count=1,.bits=XR_LIR_CLOSURE_FRAME});
    if (contexts) {
        XrLirValue other_array=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
            .type=XR_LIR_ARRAY,.intrinsic=constructor,.operands=&one,.operand_count=1});
        other_cell=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,
            .operands=&other_array,.operand_count=1});
        slice_callable_effect(body,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
            .operands=&other_array,.operand_count=1});
        XrLirValue other=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_ARRAY_BORROW,
            .type=XR_LIR_ARRAY_REF,.operands=&other_cell,.operand_count=1});
        first_callback=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,.type=callable,
            .callee=target,.operands=&other,.operand_count=1,.bits=XR_LIR_CLOSURE_FRAME});
    }
    if (array_capture) view=slice_callable_role(m,body,XR_LIR_SLICE_OP_NEW,slice_type,range,3);
    if (shape==SLICE_CALLABLE_OPAQUE) OK(xr_lir_function_parameter(m,root,0,&callback));
    if (shape==SLICE_CALLABLE_ALIAS) callback=slice_callable_value(body,(XrLirInstruction){
        .op=XR_LIR_FUNCTION_WEAKEN,.type=callable,.operands=&callback,.operand_count=1});
    if (shape==SLICE_CALLABLE_PHI) {
        XrLirBlock joined={0};OK(xr_lir_add_block(m,root,&callable,1,&joined));
        XrLirValue flag=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1});
        OK(xr_lir_set_terminator(m,body,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
            .operands=&flag,.operand_count=1,.edge_count=2,.edges={{joined,&callback,1},{joined,&callback,1}}}));
        body=joined;OK(xr_lir_block_parameter(m,joined,0,&callback));
    }
    XrLirValue arguments[3]={callback,zero,{0}};uint32_t argument_count=higher?application_count:1u;
    if (contexts) {arguments[0]=first_callback;arguments[1]=callback;arguments[2]=zero;}
    if (indirect_argument) {
        XrLirValue dispatch=slice_callable_value(body,(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,
            .type=dispatch_type,.callee=apply,.bits=XR_LIR_CLOSURE_FRAME});
        arguments[0]=dispatch;arguments[1]=callback;arguments[2]=zero;++argument_count;
    }
    bool indirect=shape==SLICE_CALLABLE_INDIRECT || indirect_argument;
    XrLirValue result=slice_callable_value(body,(XrLirInstruction){
        .op=indirect?XR_LIR_CALL_INDIRECT:XR_LIR_CALL,.type=XR_LIR_I64,
        .callee=indirect?(XrLirFunction){0}:apply,.operands=arguments,.operand_count=argument_count});
    XrLirValue length=slice_callable_role(m,body,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,&view,1);
    (void)slice_callable_role(m,body,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,&view,1);
    slice_callable_effect(body,(XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1});
    if (contexts) slice_callable_effect(body,(XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,
        .operands=&other_cell,.operand_count=1});
    XrLirValue sum[]={result,length};result=slice_callable_value(body,(XrLirInstruction){
        .op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=sum,.operand_count=2});
    slice_callable_return(body,result);
    if (array_capture) {
        XrLirValue item=slice_callable_value(worker,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=9});
        array=slice_callable_value(worker,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
            .intrinsic=constructor,.operands=&item,.operand_count=1});
        XrLirValue replace[]={capture,array};slice_callable_effect(worker,(XrLirInstruction){
            .op=XR_LIR_ARRAY_REF_WRITE,.type=XR_LIR_UNIT,.operands=replace,.operand_count=2});
        slice_callable_effect(worker,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&array,.operand_count=1});
        result=item;
    } else {
        result=slice_callable_value(worker,(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_I64,.operands=&capture,.operand_count=1});
        XrLirValue increment=slice_callable_value(worker,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=1});
        XrLirValue incremented[]={result,increment};result=slice_callable_value(worker,(XrLirInstruction){
            .op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=incremented,.operand_count=2});
        XrLirValue operands[]={capture,result};slice_callable_effect(worker,(XrLirInstruction){
            .op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=operands,.operand_count=2});
    }
    slice_callable_return(worker,result);
    result=slice_callable_value(application,(XrLirInstruction){.op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,
        .operands=&callback_parameter,.operand_count=1});
    if (contexts) {
        XrLirValue second={0};OK(xr_lir_function_parameter(m,apply,1,&second));
        second=slice_callable_value(application,(XrLirInstruction){.op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,
            .operands=&second,.operand_count=1});
        XrLirValue joined[]={result,second};result=slice_callable_value(application,(XrLirInstruction){
            .op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=joined,.operand_count=2});
    }
    slice_callable_return(application,result);
    XrLirSliceFunctionFact rows[4]={{0}};
    for (uint32_t f=1;f<=3;++f) rows[f]=(XrLirSliceFunctionFact){.function=f,
        .parameter_count=m->functions[f-1].parameter_count,.result_parameter=UINT32_MAX,.available=true};
    XrLirSliceEffect terms[]={{XR_LIR_SLICE_EFFECT_CAPTURE,0},{XR_LIR_SLICE_EFFECT_CALLBACK,0}};
    rows[target.id].access=rows[target.id].write=(XrLirSliceEffectSpan){0,1};
    if (!higher) rows[apply.id].access=rows[apply.id].write=(XrLirSliceEffectSpan){1,1};
    if (shape==SLICE_CALLABLE_UNAVAILABLE) rows[target.id].available=false;
    OK(xr_lir_internal_slice_effects_set(m,(XrLirSliceFactsView){.functions=rows,.terms=terms,
        .function_count=3,.term_count=2}));
    return m;
}
#endif // LIR_SLICE_CALLABLE_FIXTURE_H
