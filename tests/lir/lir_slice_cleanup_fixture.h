/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LIR_SLICE_CLEANUP_FIXTURE_H
#define LIR_SLICE_CLEANUP_FIXTURE_H
#include "lir/xlir_internal.h"

typedef enum SliceCleanupCase {
    SLICE_CLEANUP_BRANCH, SLICE_CLEANUP_BEFORE, SLICE_CLEANUP_REACHABLE,
    SLICE_CLEANUP_SAME_BEFORE, SLICE_CLEANUP_SAME_AFTER, SLICE_CLEANUP_LOOP,
    SLICE_CLEANUP_PHI, SLICE_CLEANUP_BIND, SLICE_CLEANUP_REBORROW,
    SLICE_CLEANUP_STRICT_END, SLICE_CLEANUP_CALL_BEFORE, SLICE_CLEANUP_CALL_AFTER,
    SLICE_CLEANUP_CASE_COUNT
} SliceCleanupCase;
typedef struct SliceCleanupFixture {
    XrLirModule *module;
    XrLirFunction root, worker, relay, reader;
    XrLirBlock definition, cleanup;
    XrLirValue target;
    uint32_t cleanup_instruction;
    bool unconstructed;
    int64_t expected;
} SliceCleanupFixture;
static XrLirValue cleanup_value(XrLirBlock block,XrLirInstruction op) {
    XrLirValue value={0};OK(xr_lir_emit_value((XrLirModule *)block.module,block,&op,&value));return value;
}
static XrLirValue cleanup_constant(XrLirBlock block,XrLirType type,uint64_t bits) {
    return cleanup_value(block,(XrLirInstruction){.op=XR_LIR_CONST,.type=type,.bits=bits});
}
static void cleanup_effect(XrLirBlock block,XrLirInstruction op) {
    OK(xr_lir_emit_effect((XrLirModule *)block.module,block,&op));
}
static void cleanup_branch(XrLirBlock block,XrLirBlock target,const XrLirValue *values,uint32_t count) {
    OK(xr_lir_set_terminator((XrLirModule *)block.module,block,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{target,values,count}}}));
}
static XrLirValue cleanup_role(XrLirBlock block,XrLirType element,XrLirSliceOperation role,
    XrLirType type,const XrLirValue *args,uint32_t count) {
    uint32_t id=0;OK(xr_lir_slice_operation((XrLirModule *)block.module,role,element,
        role==XR_LIR_SLICE_OP_NEW?XR_LIR_ARRAY_CELL:0,&id));
    XrLirInstruction op={.op=XR_LIR_CALL_INTRINSIC,.type=type,.intrinsic=id,.operands=args,.operand_count=count};
    if (type!=XR_LIR_UNIT) return cleanup_value(block,op);
    cleanup_effect(block,op);return (XrLirValue){0};
}
static void cleanup_end(XrLirBlock block,XrLirValue target) {
    cleanup_effect(block,(XrLirInstruction){.op=XR_LIR_SLICE_CLEANUP,.type=XR_LIR_UNIT,.bits=target.id});
}
static SliceCleanupFixture slice_cleanup_fixture(XrLirType element,SliceCleanupCase shape,bool take_definition) {
    SliceCleanupFixture c={0};XrLirModule *m=NULL;XrLirType slice=0;
    OK(xr_lir_module_create(NULL,&m));c.module=m;OK(xr_lir_slice_catalog_install(m));
    OK(xr_lir_slice_type(m,element,&slice));
    XrLirType cell_type=XR_LIR_ARRAY_CELL;
    OK(xr_lir_declare_function(m,&(XrLirFunctionSpec){.name="cleanup_root",.name_length=12,.result=XR_LIR_I64},&c.root));
    OK(xr_lir_declare_function(m,&(XrLirFunctionSpec){.name="cleanup_worker",.name_length=14,
        .parameters=&cell_type,.parameter_count=1,.result=XR_LIR_I64},&c.worker));
    bool call=shape==SLICE_CLEANUP_CALL_BEFORE || shape==SLICE_CLEANUP_CALL_AFTER;
    if (call) {
        OK(xr_lir_declare_function(m,&(XrLirFunctionSpec){.name="cleanup_relay",.name_length=13,
            .parameters=&slice,.parameter_count=1,.result=slice},&c.relay));
        OK(xr_lir_declare_function(m,&(XrLirFunctionSpec){.name="cleanup_reader",.name_length=14,
            .parameters=&slice,.parameter_count=1,.result=XR_LIR_I64},&c.reader));
        OK(xr_lir_define_function(m,c.relay));OK(xr_lir_define_function(m,c.reader));
        XrLirBlock relay={0},reader={0};XrLirValue borrowed={0},read_input={0};
        OK(xr_lir_add_block(m,c.relay,NULL,0,&relay));OK(xr_lir_set_entry(m,c.relay,relay));
        OK(xr_lir_add_block(m,c.reader,NULL,0,&reader));OK(xr_lir_set_entry(m,c.reader,reader));
        OK(xr_lir_function_parameter(m,c.relay,0,&borrowed));
        OK(xr_lir_function_parameter(m,c.reader,0,&read_input));
        OK(xr_lir_set_terminator(m,relay,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
            .operands=&borrowed,.operand_count=1}));
        XrLirValue length=cleanup_role(reader,element,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,&read_input,1);
        for (unsigned n=0;n<2;++n) cleanup_effect(reader,(XrLirInstruction){.op=XR_LIR_SLICE_FORMAL_CLEANUP,
            .type=XR_LIR_UNIT,.bits=read_input.id});
        OK(xr_lir_set_terminator(m,reader,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
            .operands=&length,.operand_count=1}));
        /* Install the exact declaration contracts before constructing cleanup
         * of a CALL result; that builder boundary already requires its origin. */
        XrLirSliceFunctionFact rows[5]={{0}};
        for (uint32_t f=1;f<=4;++f) rows[f]=(XrLirSliceFunctionFact){.function=f,
            .parameter_count=m->functions[f-1].parameter_count,.result_parameter=UINT32_MAX,.available=true};
        XrLirSliceEffect origin={XR_LIR_SLICE_EFFECT_PARAMETER,0};
        rows[c.relay.id].result_parameter=0;rows[c.relay.id].result_origin=(XrLirSliceEffectSpan){0,1};
        rows[c.reader.id].access=(XrLirSliceEffectSpan){0,1};
        OK(xr_lir_internal_slice_effects_set(m,(XrLirSliceFactsView){.functions=rows,.function_count=4,
            .terms=&origin,.term_count=1}));
    }
    OK(xr_lir_define_function(m,c.root));OK(xr_lir_define_function(m,c.worker));
    XrLirValue input={0};OK(xr_lir_function_parameter(m,c.worker,0,&input));
    OK(xr_lir_set_array_shape(m,input,&(XrLirArrayShape){element}));
    XrLirBlock root={0},entry={0},other={0},done={0};
    OK(xr_lir_add_block(m,c.root,NULL,0,&root));OK(xr_lir_set_entry(m,c.root,root));
    OK(xr_lir_add_block(m,c.worker,NULL,0,&entry));OK(xr_lir_set_entry(m,c.worker,entry));
    OK(xr_lir_add_block(m,c.worker,NULL,0,&c.definition));
    bool separate=shape==SLICE_CLEANUP_BRANCH || shape==SLICE_CLEANUP_BEFORE ||
        shape==SLICE_CLEANUP_REACHABLE || shape==SLICE_CLEANUP_PHI;
    if (separate) OK(xr_lir_add_block(m,c.worker,shape==SLICE_CLEANUP_PHI?&slice:NULL,
        shape==SLICE_CLEANUP_PHI?1u:0u,&other));
    XrLirType result_type=XR_LIR_I64;
    OK(xr_lir_add_block(m,c.worker,&result_type,1,&done));
    XrLirValue result={0};OK(xr_lir_block_parameter(m,done,0,&result));
    XrLirValue zero=cleanup_constant(entry,XR_LIR_I64,0),three=cleanup_constant(entry,XR_LIR_I64,3);
    XrLirValue condition=cleanup_constant(entry,XR_LIR_I1,take_definition),again=cleanup_constant(entry,XR_LIR_I1,0);
    XrLirValue args[]={input,zero,three};
    XrLirValue original=cleanup_role(c.definition,element,XR_LIR_SLICE_OP_NEW,slice,args,3);
    c.target=original;
    bool derived=shape==SLICE_CLEANUP_BIND || shape==SLICE_CLEANUP_REBORROW;
    if (derived) {
        XrLirValue borrow[]={original,zero,three,again};
        c.target=cleanup_role(c.definition,element,shape==SLICE_CLEANUP_BIND?XR_LIR_SLICE_OP_BIND:XR_LIR_SLICE_OP_REBORROW,
            slice,borrow,shape==SLICE_CLEANUP_BIND?1u:4u);
    }
    if (call) c.target=cleanup_value(c.definition,(XrLirInstruction){.op=XR_LIR_CALL,.type=slice,
        .callee=c.relay,.operands=&original,.operand_count=1});
    XrLirValue length=call ? cleanup_value(c.definition,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,
        .callee=c.reader,.operands=&c.target,.operand_count=1}) :
        cleanup_role(c.definition,element,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,&c.target,1);
    if (shape==SLICE_CLEANUP_PHI) {
        XrLirValue phi={0};OK(xr_lir_block_parameter(m,other,0,&phi));
        c.cleanup=entry;c.target=phi;
        c.cleanup_instruction=m->functions[c.worker.id-1].blocks[entry.id-1].instruction_count+1;
        cleanup_end(entry,phi);
        cleanup_branch(c.definition,other,&original,1);
        length=cleanup_role(other,element,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,&phi,1);
        cleanup_end(other,phi);cleanup_end(other,original);cleanup_branch(other,done,&length,1);
    } else if (shape==SLICE_CLEANUP_REACHABLE) {
        c.cleanup=other;c.cleanup_instruction=1;
        cleanup_branch(c.definition,other,NULL,0);cleanup_end(other,c.target);cleanup_branch(other,done,&length,1);
    } else {
        if (shape==SLICE_CLEANUP_STRICT_END)
            (void)cleanup_role(c.definition,element,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,&c.target,1);
        else {
            c.cleanup=c.definition;
            c.cleanup_instruction=m->functions[c.worker.id-1].blocks[c.definition.id-1].instruction_count+1;
            cleanup_end(c.definition,c.target);
        }
        if (derived || call) cleanup_end(c.definition,original);
        if (shape==SLICE_CLEANUP_LOOP) {
            OK(xr_lir_set_terminator(m,c.definition,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
                .operands=&again,.operand_count=1,.edge_count=2,.edges={{c.definition,NULL,0},{done,&length,1}}}));
        } else cleanup_branch(c.definition,done,&length,1);
        if (shape==SLICE_CLEANUP_BRANCH || shape==SLICE_CLEANUP_BEFORE) {
            c.cleanup=other;c.cleanup_instruction=1;cleanup_end(other,c.target);
            cleanup_branch(other,shape==SLICE_CLEANUP_BEFORE?c.definition:done,
                shape==SLICE_CLEANUP_BEFORE?NULL:&zero,shape==SLICE_CLEANUP_BEFORE?0u:1u);
        }
    }
    if (shape==SLICE_CLEANUP_BRANCH)
        OK(xr_lir_set_terminator(m,entry,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
            .operands=&condition,.operand_count=1,.edge_count=2,.edges={{c.definition,NULL,0},{other,NULL,0}}}));
    else cleanup_branch(entry,shape==SLICE_CLEANUP_BEFORE?other:c.definition,NULL,0);
    if (shape==SLICE_CLEANUP_SAME_BEFORE || shape==SLICE_CLEANUP_LOOP || derived || shape==SLICE_CLEANUP_CALL_BEFORE) {
        uint32_t ordinal=derived || call?1u:0u;
        OK(xr_lir_internal_slice_cleanup_insert(m,c.definition,ordinal,c.target,NULL));
        c.cleanup=c.definition;c.cleanup_instruction=ordinal+1;
    }
    XrLirValue forty=cleanup_constant(done,XR_LIR_I64,40),sum_args[]={result,forty};
    XrLirValue sum=cleanup_value(done,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=sum_args,.operand_count=2});
    OK(xr_lir_set_terminator(m,done,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&sum,.operand_count=1}));
    XrLirValue items[]={cleanup_constant(root,element,10),cleanup_constant(root,element,20),cleanup_constant(root,element,30)};
    uint32_t new_array=0;OK(xr_lir_array_operation(m,element,XR_LIR_ARRAY_OP_NEW,&new_array));
    XrLirValue array=cleanup_value(root,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
        .intrinsic=new_array,.operands=items,.operand_count=3});
    XrLirValue binding=cleanup_value(root,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,.operands=&array,.operand_count=1});
    cleanup_effect(root,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&array,.operand_count=1});
    XrLirValue output=cleanup_value(root,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=c.worker,
        .operands=&binding,.operand_count=1});
    cleanup_effect(root,(XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,.operands=&binding,.operand_count=1});
    OK(xr_lir_set_terminator(m,root,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&output,.operand_count=1}));
    c.unconstructed=shape==SLICE_CLEANUP_BRANCH || shape==SLICE_CLEANUP_BEFORE ||
        shape==SLICE_CLEANUP_SAME_BEFORE || shape==SLICE_CLEANUP_CALL_BEFORE || derived;
    c.expected=shape==SLICE_CLEANUP_BRANCH && !take_definition?40:43;
    return c;
}
#endif
