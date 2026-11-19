/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_push_window_fixture.h - Exact append routes and counter boundaries
 */
#ifndef LIR_ARRAY_PUSH_WINDOW_FIXTURE_H
#define LIR_ARRAY_PUSH_WINDOW_FIXTURE_H
#include "lir_array_set_region_fixture.h"
enum ArrayPushWindowMode {
    PUSH_WINDOW_CURRENT, PUSH_WINDOW_PARAMETER_END, PUSH_WINDOW_PRECOMPUTED_END, PUSH_WINDOW_COMMUTED_STEP,
    PUSH_WINDOW_NEGATIVE_IV, PUSH_WINDOW_MAX_END,
    PUSH_WINDOW_MIRRORED_END, PUSH_WINDOW_INCLUSIVE_END, PUSH_WINDOW_CHANGING_END,
    PUSH_WINDOW_STEP_TWO, PUSH_WINDOW_NEXT_ITEM, PUSH_WINDOW_TWO_PUSHES, PUSH_WINDOW_OBSERVER,
    PUSH_WINDOW_LENGTH_END, PUSH_WINDOW_EXTRA_STATE, PUSH_WINDOW_LATCH_CURRENT,
    PUSH_WINDOW_EXIT_RETAIN_SOURCE, PUSH_WINDOW_EXIT_RETAIN_TARGET, PUSH_WINDOW_ENTRY_EXIT_RETAINS,
    PUSH_WINDOW_LIVE_SNAPSHOT, PUSH_WINDOW_BODY_RETAIN, PUSH_WINDOW_EXIT_BYPASS,
    PUSH_WINDOW_EXIT_BRIDGE, PUSH_WINDOW_EXIT_BRIDGE_OBSERVER, PUSH_WINDOW_EXIT_BRIDGE_BYPASS,
    PUSH_WINDOW_EXIT_BRIDGE_BRANCH
};
#define ARS_PUSH_FIELDS(p) (p).kind,(p).iv,(p).iv_argument,(p).entry_iv,(p).end,(p).end_block,(p).end_instruction, \
    (p).condition,(p).condition_instruction,(p).increment,(p).increment_block,(p).increment_instruction, \
    (p).one,(p).latch_iv,(p).item,(p).first_active_step,(p).checkpoint_step
static inline bool ars_push_same(XrLirArrayPushCapacity a,XrLirArrayPushCapacity b) {
    const uint32_t x[]={ARS_PUSH_FIELDS(a)},y[]={ARS_PUSH_FIELDS(b)};
    for (size_t n=0;n<sizeof(x)/sizeof(*x);++n) if (x[n]!=y[n]) return false;
    return true;
}
static inline bool ars_push_expected(enum ArrayPushWindowMode mode) {
    return mode<=PUSH_WINDOW_MIRRORED_END || (mode>=PUSH_WINDOW_EXIT_RETAIN_SOURCE && mode<=PUSH_WINDOW_ENTRY_EXIT_RETAINS) ||
        mode==PUSH_WINDOW_EXIT_BRIDGE;
}
static ArraySetRegionFixture array_push_window_build(enum ArrayPushWindowMode mode) {
    bool snapshot=mode==PUSH_WINDOW_LIVE_SNAPSHOT;
    ArraySetRegionFixture c={.count=2,.arity=(mode==PUSH_WINDOW_EXTRA_STATE || snapshot) ? 3 : 2};
    OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirType scalar=XR_LIR_I64,types[]={XR_LIR_ARRAY,XR_LIR_I64,snapshot ? XR_LIR_ARRAY : XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="capacity",.name_length=8,
        .parameters=&scalar,.parameter_count=1,.result=XR_LIR_I64},&c.function));
    OK(xr_lir_define_function(c.module,c.function));
    if (mode==PUSH_WINDOW_OBSERVER || mode==PUSH_WINDOW_EXIT_BRIDGE_OBSERVER) {
        OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="observe",.name_length=7,.result=XR_LIR_UNIT},&c.helper));
        OK(xr_lir_define_function(c.module,c.helper)); XrLirBlock body={0};
        OK(xr_lir_add_block(c.module,c.helper,NULL,0,&body)); OK(xr_lir_set_entry(c.module,c.helper,body));
        OK(xr_lir_set_terminator(c.module,body,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT}));
    }
    OK(xr_lir_add_block(c.module,c.function,NULL,0,&c.entry)); c.region_entry=c.entry;
    OK(xr_lir_set_entry(c.module,c.function,c.entry));
    OK(xr_lir_add_block(c.module,c.function,types,c.arity,&c.header));
    for (uint32_t n=0;n<2;++n) OK(xr_lir_add_block(c.module,c.function,types,c.arity,&c.body[1-n]));
    OK(xr_lir_add_block(c.module,c.function,types,c.arity,&c.exit));
    for (uint32_t n=0;n<4;++n) {
        XrLirBlock block=n==0 ? c.header : n==3 ? c.exit : c.body[n-1]; XrLirValue array={0};
        OK(xr_lir_block_parameter(c.module,block,0,&array));
        OK(xr_lir_set_array_shape(c.module,array,&(XrLirArrayShape){XR_LIR_I64}));
        if (snapshot) {
            OK(xr_lir_block_parameter(c.module,block,2,&array));
            OK(xr_lir_set_array_shape(c.module,array,&(XrLirArrayShape){XR_LIR_I64}));
        }
    }
    if (mode>=PUSH_WINDOW_EXIT_BRIDGE) {
        XrLirValue array={0}; OK(xr_lir_add_block(c.module,c.function,types,c.arity,&c.bridge));
        OK(xr_lir_block_parameter(c.module,c.bridge,0,&array));
        OK(xr_lir_set_array_shape(c.module,array,&(XrLirArrayShape){XR_LIR_I64}));
    }
    XrLirValue seed=ars_number(&c,c.entry,7),incoming[]={ars_array_op(&c,c.entry,XR_LIR_ARRAY_OP_NEW,&seed,1),
        ars_number(&c,c.entry,mode==PUSH_WINDOW_NEGATIVE_IV ? -1 : 0),ars_number(&c,c.entry,42)};
    XrLirValue end={0};
    if (mode==PUSH_WINDOW_PRECOMPUTED_END) {
        XrLirValue parameter={0}; OK(xr_lir_function_parameter(c.module,c.function,0,&parameter));
        XrLirValue add[]={parameter,ars_number(&c,c.entry,2)};
        end=ars_value(&c,c.entry,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    }
    if (mode==PUSH_WINDOW_ENTRY_EXIT_RETAINS) incoming[0]=ars_retire_copy(&c,c.entry,incoming[0],true);
    if (snapshot) incoming[2]=ars_retained(&c,c.entry,incoming[0]);
    if (mode==PUSH_WINDOW_EXIT_BYPASS || mode==PUSH_WINDOW_EXIT_BRIDGE_BYPASS) {
        XrLirValue bypass=ars_value(&c,c.entry,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1});
        OK(xr_lir_set_terminator(c.module,c.entry,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
            .operands=&bypass,.operand_count=1,.edge_count=2,
            .edges={{c.header,incoming,c.arity},{mode==PUSH_WINDOW_EXIT_BYPASS ? c.exit : c.bridge,incoming,c.arity}}}));
    } else ars_branch(&c,c.entry,c.header,incoming);
    XrLirValue values[4]={{0}}; ars_parameters(&c,c.header,values); c.header_array=values[0];
    if (mode==PUSH_WINDOW_PARAMETER_END) OK(xr_lir_function_parameter(c.module,c.function,0,&end));
    else if (mode==PUSH_WINDOW_LENGTH_END) end=ars_array_op(&c,c.header,XR_LIR_ARRAY_OP_LEN,values,1);
    else if (mode==PUSH_WINDOW_CHANGING_END) {
        XrLirValue sub[]={ars_number(&c,c.header,9),values[1]};
        end=ars_value(&c,c.header,(XrLirInstruction){.op=XR_LIR_SUB,.type=XR_LIR_I64,.operands=sub,.operand_count=2});
    } else if (!end.id) end=ars_number(&c,c.header,mode==PUSH_WINDOW_MAX_END ? INT64_MAX : 9);
    bool mirrored=mode==PUSH_WINDOW_MIRRORED_END;
    XrLirValue compare[]={mirrored ? end : values[1],mirrored ? values[1] : end};
    XrLirPredicate predicate=mirrored ? XR_LIR_GT : mode==PUSH_WINDOW_INCLUSIVE_END ? XR_LIR_LE : XR_LIR_LT;
    XrLirValue condition=ars_value(&c,c.header,(XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,
        .predicate=predicate,.operands=compare,.operand_count=2});
    OK(xr_lir_set_terminator(c.module,c.header,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&condition,.operand_count=1,.edge_count=2,
        .edges={{c.body[0],values,c.arity},{c.exit,values,c.arity}}}));
    ars_parameters(&c,c.body[0],values); ars_branch(&c,c.body[0],c.body[1],values);
    XrLirBlock block=c.body[1]; ars_parameters(&c,block,values);
    if (mode==PUSH_WINDOW_BODY_RETAIN) values[0]=ars_retire_copy(&c,block,values[0],true);
    XrLirValue next={0},one=ars_number(&c,block,mode==PUSH_WINDOW_STEP_TWO ? 2 : 1),add[]={values[1],one};
    if (mode==PUSH_WINDOW_COMMUTED_STEP) { add[0]=one; add[1]=values[1]; }
    if (mode==PUSH_WINDOW_NEXT_ITEM) next=ars_value(&c,block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    XrLirValue cell=ars_value(&c,block,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,.operands=values,.operand_count=1});
    ars_effect(&c,block,XR_LIR_ARRAY_DROP,values[0]);
    if (mode==PUSH_WINDOW_OBSERVER) OK(xr_lir_emit_effect(c.module,block,&(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=c.helper}));
    uint32_t push=0; OK(xr_lir_array_operation(c.module,XR_LIR_I64,XR_LIR_ARRAY_OP_PUSH,&push));
    XrLirValue args[]={cell,mode==PUSH_WINDOW_NEXT_ITEM ? next : values[1]};
    OK(xr_lir_emit_effect(c.module,block,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
        .intrinsic=push,.operands=args,.operand_count=2}));
    if (mode==PUSH_WINDOW_TWO_PUSHES) OK(xr_lir_emit_effect(c.module,block,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
        .intrinsic=push,.operands=args,.operand_count=2}));
    values[0]=ars_value(&c,block,(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_ARRAY,.operands=&cell,.operand_count=1});
    ars_effect(&c,block,XR_LIR_ARRAY_CELL_DROP,cell);
    if (!next.id) next=ars_value(&c,block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    if (mode!=PUSH_WINDOW_LATCH_CURRENT) values[1]=next;
    ars_branch(&c,block,c.header,values);
    ars_parameters(&c,c.exit,values);
    XrLirBlock done=c.exit;
    if (mode>=PUSH_WINDOW_EXIT_BRIDGE) {
        if (mode==PUSH_WINDOW_EXIT_BRIDGE_OBSERVER)
            OK(xr_lir_emit_effect(c.module,done,&(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=c.helper}));
        if (mode==PUSH_WINDOW_EXIT_BRIDGE_BRANCH) {
            XrLirValue branch=ars_value(&c,done,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1});
            XrLirBlock alternate={0}; XrLirValue forwarded[4]={{0}};
            OK(xr_lir_add_block(c.module,c.function,types,c.arity,&alternate));
            ars_parameters(&c,alternate,forwarded);
            OK(xr_lir_set_array_shape(c.module,forwarded[0],&(XrLirArrayShape){XR_LIR_I64}));
            ars_branch(&c,alternate,c.bridge,forwarded);
            OK(xr_lir_set_terminator(c.module,done,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
                .operands=&branch,.operand_count=1,.edge_count=2,
                .edges={{c.bridge,values,c.arity},{alternate,values,c.arity}}}));
        } else ars_branch(&c,done,c.bridge,values);
        done=c.bridge; ars_parameters(&c,done,values);
    }
    if (mode>=PUSH_WINDOW_EXIT_RETAIN_SOURCE && mode<=PUSH_WINDOW_ENTRY_EXIT_RETAINS)
        values[0]=ars_retire_copy(&c,done,values[0],mode!=PUSH_WINDOW_EXIT_RETAIN_TARGET);
    if (mode==PUSH_WINDOW_EXIT_BYPASS || mode>=PUSH_WINDOW_EXIT_BRIDGE) values[0]=ars_retire_copy(&c,done,values[0],true);
    c.result=ars_array_op(&c,done,XR_LIR_ARRAY_OP_LEN,values,1);
    ars_effect(&c,done,XR_LIR_ARRAY_DROP,values[0]);
    if (snapshot) ars_effect(&c,done,XR_LIR_ARRAY_DROP,values[2]);
    OK(xr_lir_set_terminator(c.module,done,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&c.result,.operand_count=1}));
    XrLirDiagnostic diagnostic={0};
    XrLirStatus status=xr_lir_module_finish(c.module,&diagnostic);
    if (status!=XR_LIR_OK) fprintf(stderr,"PUSH window fixture %u: %s\n",(unsigned)mode,diagnostic.message);
    OK(status); return c;
}
#endif // LIR_ARRAY_PUSH_WINDOW_FIXTURE_H
