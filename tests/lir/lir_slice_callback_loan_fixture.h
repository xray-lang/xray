/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LIR_SLICE_CALLBACK_LOAN_FIXTURE_H
#define LIR_SLICE_CALLBACK_LOAN_FIXTURE_H
#include "lir/xlir_internal.h"
#include <string.h>

static XrLirValue callback_loan_value(XrLirBlock block,XrLirInstruction instruction) {
    XrLirValue value={0};OK(xr_lir_emit_value((XrLirModule *)block.module,block,&instruction,&value));return value;
}
static void callback_loan_effect(XrLirBlock block,XrLirInstruction instruction) {
    OK(xr_lir_emit_effect((XrLirModule *)block.module,block,&instruction));
}
static void callback_loan_return(XrLirBlock block,XrLirValue value) {
    OK(xr_lir_set_terminator((XrLirModule *)block.module,block,&(XrLirInstruction){
        .op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1}));
}
static XrLirBlock callback_loan_function(XrLirModule *module,const char *name,
    const XrLirType *parameters,uint32_t count,XrLirFunction *function) {
    XrLirFunctionSpec spec={.name=name,.name_length=strlen(name),.parameters=parameters,
        .parameter_count=count,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(module,&spec,function));OK(xr_lir_define_function(module,*function));
    XrLirBlock block={0};OK(xr_lir_add_block(module,*function,NULL,0,&block));
    OK(xr_lir_set_entry(module,*function,block));return block;
}

typedef enum SliceCallbackLoanCase {
    SLICE_CALLBACK_LOAN_DIRECT,SLICE_CALLBACK_LOAN_INDIRECT,
    SLICE_CALLBACK_LOAN_ALIAS,SLICE_CALLBACK_LOAN_INDIRECT_ALIAS,
    SLICE_CALLBACK_LOAN_INCOMPLETE,SLICE_CALLBACK_LOAN_MISSING_INVOKE,
    SLICE_CALLBACK_LOAN_MISSING_RELAY,SLICE_CALLBACK_LOAN_LOCAL,
    SLICE_CALLBACK_LOAN_PHI,SLICE_CALLBACK_LOAN_DERIVED,SLICE_CALLBACK_LOAN_INDIRECT_SLICE,
    SLICE_CALLBACK_LOAN_CASE_COUNT
} SliceCallbackLoanCase;
typedef struct SliceCallbackLoanFixture { XrLirModule *module;XrLirFunction root,invoke,relay; } SliceCallbackLoanFixture;

static SliceCallbackLoanFixture slice_callback_loan_fixture(XrLirType element,SliceCallbackLoanCase shape) {
    SliceCallbackLoanFixture c={0};XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));c.module=m;
    OK(xr_lir_slice_catalog_install(m));XrLirType slice=0,callback=0;
    OK(xr_lir_slice_type(m,element,&slice));OK(xr_lir_callable_type(m,&(XrLirCallableType){.result=XR_LIR_I64},&callback));
    XrLirStateModule state_module={0};XrLirStateSlot slots[]={
        {.payload=XR_LIR_ARRAY,.mutable=true,.size=8,.alignment=8,.array_element=element},
        {.payload=XR_LIR_ARRAY,.mutable=true,.offset=8,.size=8,.alignment=8,.array_element=element}};
    OK(xr_lir_set_state_layout(m,&(XrLirStateLayout){.modules=&state_module,.module_count=1,
        .slots=slots,.slot_count=2,.payload_size=16,.payload_alignment=8}));
    XrLirType state_type=XR_LIR_STATE;
    XrLirBlock root=callback_loan_function(m,"loan_root",&state_type,1,&c.root);OK(xr_lir_set_state_entry(m,c.root));
    XrLirType invoke_types[]={slice,callback},relay_types[]={callback,slice};
    XrLirBlock invoke=callback_loan_function(m,"loan_invoke",invoke_types,2,&c.invoke);
    XrLirBlock relay=callback_loan_function(m,"loan_relay",relay_types,2,&c.relay);
    XrLirFunction change={0};XrLirBlock writer=callback_loan_function(m,"loan_change",&state_type,1,&change);
    OK(xr_lir_set_closure_target(m,change,(XrLirClosureTarget){callback,0,true}));
    XrLirValue state={0};OK(xr_lir_function_parameter(m,c.root,0,&state));
    callback_loan_effect(root,(XrLirInstruction){.op=XR_LIR_MODULE_BEGIN,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1});
    uint32_t new_array=0;OK(xr_lir_array_operation(m,element,XR_LIR_ARRAY_OP_NEW,&new_array));
    XrLirValue refs[2];
    for (uint32_t slot=0;slot<2;++slot) {
        XrLirValue item=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_CONST,.type=element,.bits=slot?4:1});
        XrLirValue array=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
            .intrinsic=new_array,.operands=&item,.operand_count=1});
        XrLirValue args[]={state,array};refs[slot]=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_STATE_CONSTRUCT,
            .type=XR_LIR_STATE_REF,.bits=slot,.operands=args,.operand_count=2});
        callback_loan_effect(root,(XrLirInstruction){.op=XR_LIR_STATE_PUBLISH,.type=XR_LIR_UNIT,.operands=&refs[slot],.operand_count=1});
        callback_loan_effect(root,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&array,.operand_count=1});
    }
    callback_loan_effect(root,(XrLirInstruction){.op=XR_LIR_MODULE_READY,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1});
    XrLirValue fn=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,.type=callback,.callee=change,
        .operands=&state,.operand_count=1,.bits=XR_LIR_CLOSURE_FRAME});
    bool indirect=shape==SLICE_CALLBACK_LOAN_INDIRECT || shape==SLICE_CALLBACK_LOAN_INDIRECT_ALIAS;
    bool indirect_slice=shape==SLICE_CALLBACK_LOAN_INDIRECT_SLICE;
    XrLirValue dispatch={0};XrLirFunction forwarding={0};XrLirBlock forwarding_body={0};
    if (indirect) {
        /* The indirect carrier transports a real callback, never a Slice.
         * Slice arguments still use the established direct-call boundary. */
        forwarding_body=callback_loan_function(m,"loan_forward",&callback,1,&forwarding);
        XrLirCallableParameter parameter={.type=callback,.mode=XR_PARAM_READ};
        XrLirType type=0;OK(xr_lir_callable_type(m,&(XrLirCallableType){.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64},&type));
        OK(xr_lir_set_closure_target(m,forwarding,(XrLirClosureTarget){type,0,false}));
        dispatch=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,.type=type,
            .callee=forwarding,.bits=XR_LIR_CLOSURE_FRAME});
    }
    if (indirect_slice) {
        XrLirCallableParameter parameters[]={{.type=callback,.mode=XR_PARAM_READ},{.type=slice,.mode=XR_PARAM_READ}};
        XrLirType type=0;OK(xr_lir_callable_type(m,&(XrLirCallableType){.parameters=parameters,.parameter_count=2,.result=XR_LIR_I64},&type));
        OK(xr_lir_set_closure_target(m,c.relay,(XrLirClosureTarget){type,0,false}));
        dispatch=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,.type=type,
            .callee=c.relay,.bits=XR_LIR_CLOSURE_FRAME});
    }
    XrLirValue zero=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64});
    XrLirValue one=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=1});
    bool alias=shape==SLICE_CALLBACK_LOAN_ALIAS || shape==SLICE_CALLBACK_LOAN_INDIRECT_ALIAS;
    XrLirValue range[]={refs[alias?0:1],zero,one};uint32_t read_role=0;
    OK(xr_lir_slice_operation(m,XR_LIR_SLICE_OP_NEW,element,XR_LIR_STATE_REF,&read_role));
    XrLirValue view=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=slice,
        .intrinsic=read_role,.operands=range,.operand_count=3});
    XrLirValue indirect_result={0};
    if (indirect) {
        XrLirValue args[]={dispatch,fn};indirect_result=callback_loan_value(root,(XrLirInstruction){
            .op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,.operands=args,.operand_count=2});
    }
    XrLirValue arguments[]={fn,view};
    XrLirValue result=callback_loan_value(root,(XrLirInstruction){.op=XR_LIR_CALL,
        .type=XR_LIR_I64,.callee=c.relay,.operands=arguments,.operand_count=2});
    if (indirect_slice) {
        XrLirValue args[]={dispatch,fn,view};result=callback_loan_value(root,(XrLirInstruction){
            .op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,.operands=args,.operand_count=3});
    }
    if (indirect) {
        XrLirValue sum[]={indirect_result,result};result=callback_loan_value(root,(XrLirInstruction){
            .op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=sum,.operand_count=2});
    }
    callback_loan_effect(root,(XrLirInstruction){.op=XR_LIR_SLICE_CLEANUP,.type=XR_LIR_UNIT,.bits=view.id});
    callback_loan_return(root,result);
    XrLirValue writer_state={0};OK(xr_lir_function_parameter(m,change,0,&writer_state));
    XrLirValue item=callback_loan_value(writer,(XrLirInstruction){.op=XR_LIR_CONST,.type=element,.bits=9});
    XrLirValue array=callback_loan_value(writer,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
        .intrinsic=new_array,.operands=&item,.operand_count=1});
    XrLirValue reference=callback_loan_value(writer,(XrLirInstruction){.op=XR_LIR_STATE_REFERENCE,.type=XR_LIR_STATE_REF,
        .operands=&writer_state,.operand_count=1});
    XrLirValue replace[]={reference,array};callback_loan_effect(writer,(XrLirInstruction){.op=XR_LIR_STATE_WRITE,
        .type=XR_LIR_UNIT,.operands=replace,.operand_count=2});
    callback_loan_effect(writer,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&array,.operand_count=1});
    result=callback_loan_value(writer,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64});callback_loan_return(writer,result);
    XrLirValue part={0},cb={0};OK(xr_lir_function_parameter(m,c.invoke,0,&part));OK(xr_lir_function_parameter(m,c.invoke,1,&cb));
    XrLirValue live_part=part,derived={0};
    if (shape==SLICE_CALLBACK_LOAN_DERIVED) {
        XrLirValue start=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64}),
            limit=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=1}),
            closed=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1});
        XrLirValue bounds[]={part,start,limit,closed};uint32_t role=0;
        OK(xr_lir_slice_operation(m,XR_LIR_SLICE_OP_REBORROW,element,0,&role));
        live_part=derived=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
            .type=slice,.intrinsic=role,.operands=bounds,.operand_count=4});
        callback_loan_effect(invoke,(XrLirInstruction){.op=XR_LIR_SLICE_FORMAL_CLEANUP,.type=XR_LIR_UNIT,.bits=part.id});
    }
    if (shape==SLICE_CALLBACK_LOAN_PHI) {
        XrLirBlock joined={0};OK(xr_lir_add_block(m,c.invoke,&callback,1,&joined));
        OK(xr_lir_set_terminator(m,invoke,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
            .edge_count=1,.edges={{joined,&cb,1}}}));invoke=joined;OK(xr_lir_block_parameter(m,joined,0,&cb));
    }
    XrLirValue local={0},local_cell={0};
    if (shape==SLICE_CALLBACK_LOAN_LOCAL) {
        XrLirValue entry=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CONST,.type=element,.bits=3});
        array=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
            .intrinsic=new_array,.operands=&entry,.operand_count=1});
        local_cell=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,.operands=&array,.operand_count=1});
        callback_loan_effect(invoke,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&array,.operand_count=1});
        XrLirValue start=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64}),
            limit=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=1});
        XrLirValue bounds[]={local_cell,start,limit};uint32_t role=0;OK(xr_lir_slice_operation(m,XR_LIR_SLICE_OP_NEW,element,XR_LIR_ARRAY_CELL,&role));
        local=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=slice,.intrinsic=role,.operands=bounds,.operand_count=3});
    }
    result=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,.operands=&cb,.operand_count=1});
    uint32_t get=0;OK(xr_lir_slice_operation(m,XR_LIR_SLICE_OP_GET,element,0,&get));
    XrLirValue first=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64});
    XrLirValue read[]={live_part,first};XrLirValue selected=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=element,.intrinsic=get,.operands=read,.operand_count=2});
    if (element==XR_LIR_U8) selected=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_INTCAST,.type=XR_LIR_I64,.operands=&selected,.operand_count=1});
    XrLirValue sum[]={result,selected};result=callback_loan_value(invoke,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=sum,.operand_count=2});
    if (local.id) {
        callback_loan_effect(invoke,(XrLirInstruction){.op=XR_LIR_SLICE_CLEANUP,.type=XR_LIR_UNIT,.bits=local.id});
        callback_loan_effect(invoke,(XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,.operands=&local_cell,.operand_count=1});
    }
    if (derived.id) callback_loan_effect(invoke,(XrLirInstruction){.op=XR_LIR_SLICE_CLEANUP,.type=XR_LIR_UNIT,.bits=derived.id});
    callback_loan_effect(invoke,(XrLirInstruction){.op=XR_LIR_SLICE_FORMAL_CLEANUP,.type=XR_LIR_UNIT,.bits=part.id});callback_loan_return(invoke,result);
    OK(xr_lir_function_parameter(m,c.relay,0,&cb));OK(xr_lir_function_parameter(m,c.relay,1,&part));
    XrLirValue forwarded[]={part,cb};result=callback_loan_value(relay,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,
        .callee=c.invoke,.operands=forwarded,.operand_count=2});
    callback_loan_effect(relay,(XrLirInstruction){.op=XR_LIR_SLICE_FORMAL_CLEANUP,.type=XR_LIR_UNIT,.bits=part.id});callback_loan_return(relay,result);
    if (indirect) {
        XrLirValue actual={0};OK(xr_lir_function_parameter(m,forwarding,0,&actual));
        XrLirValue called=callback_loan_value(forwarding_body,(XrLirInstruction){.op=XR_LIR_CALL_INDIRECT,
            .type=XR_LIR_I64,.operands=&actual,.operand_count=1});callback_loan_return(forwarding_body,called);
    }
    XrLirSliceFunctionFact rows[6]={{0}};
    for (uint32_t f=1;f<=m->function_count;++f) rows[f]=(XrLirSliceFunctionFact){.function=f,.parameter_count=m->functions[f-1].parameter_count,
        .available=true,.result_parameter=UINT32_MAX};
    XrLirSliceEffect terms[]={{XR_LIR_SLICE_EFFECT_SLOT,0},{XR_LIR_SLICE_EFFECT_SLOT,1},
        {XR_LIR_SLICE_EFFECT_PARAMETER,0},{XR_LIR_SLICE_EFFECT_CALLBACK,1},
        {XR_LIR_SLICE_EFFECT_PARAMETER,1},{XR_LIR_SLICE_EFFECT_CALLBACK,0}};
    rows[c.root.id].access=rows[c.root.id].write=(XrLirSliceEffectSpan){0,2};
    rows[change.id].access=rows[change.id].write=(XrLirSliceEffectSpan){0,1};
    rows[c.invoke.id].access=(XrLirSliceEffectSpan){2,1};rows[c.invoke.id].write=(XrLirSliceEffectSpan){3,1};
    rows[c.relay.id].access=(XrLirSliceEffectSpan){4,1};rows[c.relay.id].write=(XrLirSliceEffectSpan){5,1};
    if (indirect) rows[forwarding.id].access=rows[forwarding.id].write=(XrLirSliceEffectSpan){5,1};
    XrLirSliceCallbackLoan pairs[]={{1,XR_LIR_SLICE_EFFECT_PARAMETER,0},{0,XR_LIR_SLICE_EFFECT_PARAMETER,1}};
    rows[c.invoke.id].callback_loans=(XrLirSliceEffectSpan){0,shape==SLICE_CALLBACK_LOAN_MISSING_INVOKE?0u:1u};
    rows[c.relay.id].callback_loans=(XrLirSliceEffectSpan){1,shape==SLICE_CALLBACK_LOAN_MISSING_RELAY?0u:1u};
    XrLirSliceFactsView facts={.functions=rows,.terms=terms,.term_count=6,.function_count=m->function_count,
        .callback_loans=pairs,.callback_loan_count=2,.callback_loans_complete=true};
    if (shape==SLICE_CALLBACK_LOAN_INCOMPLETE) {
        facts.callback_loans_complete=false;
        CHECK(xr_lir_internal_slice_effects_set(m,facts)==XR_LIR_INVALID_ARGUMENT && !m->slice_facts);
        facts.callback_loans=NULL;facts.callback_loan_count=0;
        rows[c.invoke.id].callback_loans=rows[c.relay.id].callback_loans=(XrLirSliceEffectSpan){0};
    }
    OK(xr_lir_internal_slice_effects_set(m,facts));
    return c;
}
static inline XrLirModule *slice_callback_loan_ref_fixture(XrLirType element,unsigned kind) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));OK(xr_lir_slice_catalog_install(m));
    XrLirType slice=0,callback=0;OK(xr_lir_slice_type(m,element,&slice));
    bool array=kind==1;XrLirType owned_cell=0;
    if (kind==2) OK(xr_lir_cell_type(m,(XrLirCellType){XR_LIR_I64,0},&owned_cell));
    XrLirCallableParameter parameter={.type=array?XR_LIR_ARRAY_REF:XR_LIR_I64_CELL,
        .array_element=array?element:0,.mode=XR_PARAM_REF};
    if (owned_cell) parameter=(XrLirCallableParameter){.type=owned_cell,.mode=XR_PARAM_REF};
    OK(xr_lir_callable_type(m,&(XrLirCallableType){.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64},&callback));
    XrLirType types[]={slice,callback,owned_cell?owned_cell:array?XR_LIR_ARRAY_REF:XR_LIR_I64_CELL};XrLirFunction function={0};
    XrLirBlock block=callback_loan_function(m,"loan_reference",types,3,&function);
    XrLirValue part={0},cb={0},reference={0};OK(xr_lir_function_parameter(m,function,0,&part));
    OK(xr_lir_function_parameter(m,function,1,&cb));OK(xr_lir_function_parameter(m,function,2,&reference));
    if (array) OK(xr_lir_set_array_shape(m,reference,&(XrLirArrayShape){element}));
    XrLirValue args[]={cb,reference};XrLirValue called=callback_loan_value(block,(XrLirInstruction){
        .op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,.operands=args,.operand_count=2});
    uint32_t length_role=0;OK(xr_lir_slice_operation(m,XR_LIR_SLICE_OP_LEN,element,0,&length_role));
    XrLirValue length=callback_loan_value(block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_I64,
        .intrinsic=length_role,.operands=&part,.operand_count=1});
    XrLirValue sum[]={called,length};XrLirValue result=callback_loan_value(block,(XrLirInstruction){
        .op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=sum,.operand_count=2});
    callback_loan_effect(block,(XrLirInstruction){.op=XR_LIR_SLICE_FORMAL_CLEANUP,.type=XR_LIR_UNIT,.bits=part.id});
    callback_loan_return(block,result);
    XrLirSliceEffect terms[]={{XR_LIR_SLICE_EFFECT_PARAMETER,0},{XR_LIR_SLICE_EFFECT_CALLBACK,1}};
    XrLirSliceFunctionFact rows[2]={{0},{.function=function.id,.parameter_count=3,.result_parameter=UINT32_MAX,
        .available=true,.access={0,1},.write={1,1},.callback_loans={0,1}}};
    XrLirSliceCallbackLoan pair={1,XR_LIR_SLICE_EFFECT_PARAMETER,0};
    OK(xr_lir_internal_slice_effects_set(m,(XrLirSliceFactsView){.functions=rows,.terms=terms,.term_count=2,.function_count=1,
        .callback_loans=&pair,.callback_loan_count=1,.callback_loans_complete=true}));
    return m;
}
static inline XrLirModule *slice_callback_loan_capture_fixture(XrLirType element) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));OK(xr_lir_slice_catalog_install(m));
    XrLirType slice=0,callback=0,closure=0;OK(xr_lir_slice_type(m,element,&slice));
    OK(xr_lir_callable_type(m,&(XrLirCallableType){.result=XR_LIR_I64},&callback));
    XrLirCallableParameter parameter={.type=callback,.mode=XR_PARAM_READ};
    OK(xr_lir_callable_type(m,&(XrLirCallableType){.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64},&closure));
    XrLirType types[]={slice,callback};XrLirFunction function={0};
    XrLirBlock block=callback_loan_function(m,"loan_capture",types,2,&function);
    OK(xr_lir_set_closure_target(m,function,(XrLirClosureTarget){closure,1,false}));
    XrLirValue part={0},cb={0};OK(xr_lir_function_parameter(m,function,0,&part));OK(xr_lir_function_parameter(m,function,1,&cb));
    XrLirValue called=callback_loan_value(block,(XrLirInstruction){.op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,.operands=&cb,.operand_count=1});
    uint32_t role=0;OK(xr_lir_slice_operation(m,XR_LIR_SLICE_OP_LEN,element,0,&role));
    XrLirValue length=callback_loan_value(block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_I64,
        .intrinsic=role,.operands=&part,.operand_count=1});
    XrLirValue sum[]={called,length};XrLirValue result=callback_loan_value(block,(XrLirInstruction){.op=XR_LIR_ADD,
        .type=XR_LIR_I64,.operands=sum,.operand_count=2});
    callback_loan_effect(block,(XrLirInstruction){.op=XR_LIR_SLICE_FORMAL_CLEANUP,.type=XR_LIR_UNIT,.bits=part.id});callback_loan_return(block,result);
    XrLirSliceEffect terms[]={{XR_LIR_SLICE_EFFECT_CAPTURE,0},{XR_LIR_SLICE_EFFECT_CALLBACK,1}};
    XrLirSliceFunctionFact rows[2]={{0},{.function=function.id,.parameter_count=2,.result_parameter=UINT32_MAX,
        .available=true,.access={0,1},.write={1,1},.callback_loans={0,1}}};
    XrLirSliceCallbackLoan pair={1,XR_LIR_SLICE_EFFECT_CAPTURE,0};
    OK(xr_lir_internal_slice_effects_set(m,(XrLirSliceFactsView){.functions=rows,.terms=terms,.term_count=2,.function_count=1,
        .callback_loans=&pair,.callback_loan_count=1,.callback_loans_complete=true}));
    return m;
}
static inline XrLirModule *slice_callback_loan_multiroot_fixture(XrLirType element) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));OK(xr_lir_slice_catalog_install(m));
    XrLirType slice=0,callback=0;OK(xr_lir_slice_type(m,element,&slice));
    OK(xr_lir_callable_type(m,&(XrLirCallableType){.result=XR_LIR_I64},&callback));
    XrLirType types[]={slice,callback,slice,XR_LIR_I1};XrLirFunction function={0};
    XrLirBlock entry=callback_loan_function(m,"loan_selection",types,4,&function),joined={0};
    OK(xr_lir_add_block(m,function,&slice,1,&joined));XrLirValue first={0},second={0},cb={0},flag={0},selected={0};
    OK(xr_lir_function_parameter(m,function,0,&first));OK(xr_lir_function_parameter(m,function,1,&cb));
    OK(xr_lir_function_parameter(m,function,2,&second));OK(xr_lir_function_parameter(m,function,3,&flag));
    OK(xr_lir_block_parameter(m,joined,0,&selected));
    OK(xr_lir_set_terminator(m,entry,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&flag,.operand_count=1,.edge_count=2,.edges={{joined,&first,1},{joined,&second,1}}}));
    callback_loan_effect(joined,(XrLirInstruction){.op=XR_LIR_SLICE_FORMAL_CLEANUP,.type=XR_LIR_UNIT,.bits=first.id});
    callback_loan_effect(joined,(XrLirInstruction){.op=XR_LIR_SLICE_FORMAL_CLEANUP,.type=XR_LIR_UNIT,.bits=second.id});
    XrLirValue called=callback_loan_value(joined,(XrLirInstruction){.op=XR_LIR_CALL_INDIRECT,
        .type=XR_LIR_I64,.operands=&cb,.operand_count=1});uint32_t role=0;
    OK(xr_lir_slice_operation(m,XR_LIR_SLICE_OP_LEN,element,0,&role));
    XrLirValue length=callback_loan_value(joined,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_I64,
        .intrinsic=role,.operands=&selected,.operand_count=1});
    XrLirValue sum[]={called,length};XrLirValue result=callback_loan_value(joined,(XrLirInstruction){.op=XR_LIR_ADD,
        .type=XR_LIR_I64,.operands=sum,.operand_count=2});
    callback_loan_effect(joined,(XrLirInstruction){.op=XR_LIR_SLICE_CLEANUP,.type=XR_LIR_UNIT,.bits=selected.id});
    callback_loan_return(joined,result);
    XrLirSliceEffect terms[]={{XR_LIR_SLICE_EFFECT_PARAMETER,0},{XR_LIR_SLICE_EFFECT_PARAMETER,2},{XR_LIR_SLICE_EFFECT_CALLBACK,1}};
    XrLirSliceFunctionFact rows[2]={{0},{.function=function.id,.parameter_count=4,.result_parameter=UINT32_MAX,
        .available=true,.access={0,2},.write={2,1},.callback_loans={0,2}}};
    XrLirSliceCallbackLoan pairs[]={{1,XR_LIR_SLICE_EFFECT_PARAMETER,0},{1,XR_LIR_SLICE_EFFECT_PARAMETER,2}};
    OK(xr_lir_internal_slice_effects_set(m,(XrLirSliceFactsView){.functions=rows,.terms=terms,.term_count=3,.function_count=1,
        .callback_loans=pairs,.callback_loan_count=2,.callback_loans_complete=true}));
    return m;
}
#endif
