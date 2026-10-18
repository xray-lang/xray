/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_view_root_fixture.h - Literal roots and real physical call bindings
 */
#ifndef XIR_VIEW_ROOT_FIXTURE_H
#define XIR_VIEW_ROOT_FIXTURE_H
#define ROOT_ARRAY ((XrXirType)256)
#define ROOT_CELL ((XrXirType)257)
#define ROOT_CLOSURE ((XrXirType)258)
#define ROOT_SHARED ((XrXirType)259)
#define ROOT_MUT ((XrXirType)260)
#define ROOT_NESTED ((XrXirType)261)
#define ROOT_NESTED_CELL ((XrXirType)262)
#define ROOT_MAP ((XrXirType)263)
#define ROOT_MAP_CELL ((XrXirType)264)
#define ROOT_OBJECT ((XrXirType)265)
#define ROOT_OPTIONAL ((XrXirType)266)
#define ROOT_TUPLE ((XrXirType)267)
#define ROOT_REF_CALLABLE ((XrXirType)268)
#define ROOT_ECHO_CALLABLE ((XrXirType)269)
#define ROOT_FACTORY_CALLABLE ((XrXirType)270)
#define ROOT_BORROW_CALLABLE ((XrXirType)271)
typedef struct RootFixture {
    XrXirTypeNode nodes[16];XrXirCallableParameter fields[3],ref,echo;
    XrXirNominalField field;XrXirNominalDeclaration nominal;XrXirNominalTable nominal_table;
    XrXirType array,parameters[4][8];XrXirTypes types;
    XrXirInstruction ops[4][40];XrXirBlock blocks[4][4];uint32_t operands[4][80];
    XrXirFunction functions[4];XrXirSourceModule source;XrXirFunctionIdentity identities[4];
    XrXirSlot slot;XrXirDeclarations declarations;XrXirModule module;
} RootFixture;
static uint32_t root_op(RootFixture *f,uint32_t function,XrXirInstruction op) {
    XrXirFunction *body=&f->functions[function];CHECK(body->instruction_count<40);
    uint32_t i=body->instruction_count++;f->ops[function][i]=op;f->blocks[function][0].count=body->instruction_count;
    return body->parameter_count+i;
}
static uint32_t root_group(RootFixture *f,uint32_t function,const uint32_t *values,uint32_t count) {
    XrXirFunction *body=&f->functions[function];uint32_t start=body->operand_count;CHECK(start+count<=80);
    memcpy(f->operands[function]+start,values,count*sizeof(*values));body->operand_count+=count;return start;
}
static void root_bounds(RootFixture *f,uint32_t function) {
    root_op(f,function,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    root_op(f,function,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1});
}
static uint32_t root_view(RootFixture *f,uint32_t function,uint32_t value,bool reborrow) {
    uint32_t p=f->functions[function].parameter_count,args[]={value,p,p+1u};
    uint32_t start=root_group(f,function,args,3);
    return root_op(f,function,(XrXirInstruction){.op=reborrow?XR_XIR_VIEW_REBORROW:XR_XIR_VIEW_NEW,
        .type=ROOT_SHARED,.args={start,3}});
}
static void root_capture(RootFixture *f,uint32_t value) {
    uint32_t start=root_group(f,0,&value,1);
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=ROOT_CLOSURE,.args={start,1},.immediate=2});
}
static void root_fixture(RootFixture *f) {
    *f=(RootFixture){0};f->array=ROOT_ARRAY;f->ref=(XrXirCallableParameter){ROOT_CELL,XR_PARAM_REF};
    f->echo=(XrXirCallableParameter){ROOT_SHARED,XR_PARAM_READ};
    f->fields[0]=(XrXirCallableParameter){XR_XIR_UNIT,XR_PARAM_READ};f->fields[1]=f->echo;
    f->fields[2]=(XrXirCallableParameter){ROOT_OPTIONAL,XR_PARAM_READ};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=ROOT_ARRAY};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=ROOT_ARRAY};
    f->nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=ROOT_NESTED};
    f->nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=ROOT_ARRAY};
    f->nodes[8]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=ROOT_MAP};
    f->field=(XrXirNominalField){{"items",5},ROOT_ARRAY,XR_XIR_FIELD_MUTABLE};
    f->nominal=(XrXirNominalDeclaration){.module={"roots",5},.name={"Holder",6},.exported=1,
        .kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL,.fields=&f->field,.field_count=1};
    f->nominal_table=(XrXirNominalTable){&f->nominal,1,NULL};
    f->nodes[9]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&f->array,1}};
    f->nodes[10]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=ROOT_SHARED};
    f->nodes[11]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields,.parameter_count=3};
    f->nodes[12]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->ref,.parameter_count=1,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[13]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->echo,.parameter_count=1,
        .result=ROOT_SHARED,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[14]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->ref,.parameter_count=1,
        .result=ROOT_SHARED,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[15]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->echo,.parameter_count=1,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){f->nodes,16,&f->nominal_table,NULL};
    const XrXirType parameters[]={XR_XIR_BOOL,ROOT_CELL,ROOT_CELL,ROOT_OBJECT,ROOT_MAP_CELL,ROOT_NESTED_CELL,ROOT_ARRAY};
    memcpy(f->parameters[0],parameters,sizeof(parameters));f->parameters[1][0]=f->parameters[2][0]=ROOT_CELL;
    for (uint32_t i=0;i<4;++i) f->functions[i]=(XrXirFunction){.name="body",.name_length=4,
        .parameters=f->parameters[i],.parameter_count=i==0?7u:i==3?0u:1u,.instructions=f->ops[i],
        .blocks=f->blocks[i],.block_count=1,.operands=f->operands[i]};
    f->source=(XrXirSourceModule){.name="roots",.name_length=5,.initializer=3};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
}
static void root_phi_case(RootFixture *f,bool captured,bool use_phi) {
    root_bounds(f,0);if (captured) root_capture(f,1);
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
    uint32_t a=f->functions[0].instruction_count;
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    const uint32_t pairs[]={1,1,2,2};uint32_t start=root_group(f,0,pairs,4);
    uint32_t phi=root_op(f,0,(XrXirInstruction){.op=XR_XIR_PHI,.type=ROOT_CELL,.args={start,4}});
    root_view(f,0,use_phi?phi:2,false);f->functions[0].block_count=4;
    f->blocks[0][0]=(XrXirBlock){.count=a};f->blocks[0][1]=(XrXirBlock){.first=a,.count=1};
    f->blocks[0][2]=(XrXirBlock){.first=a+1u,.count=1};f->blocks[0][3]=(XrXirBlock){.first=a+2u,.count=2};
}
static void root_call_case(RootFixture *f,uint32_t kind) {
    root_bounds(f,0);root_capture(f,kind==7?1u:2u);
    uint32_t argument=1;int64_t target=1;XrXirOp call=XR_XIR_CALL;
    if (kind==8 || kind==9) {
        f->functions[1].parameter_count=2;f->parameters[1][1]=ROOT_SHARED;argument=root_view(f,0,1,false);
        uint32_t capture=kind==9?1u:2u,offset=root_group(f,0,&capture,1);
        target=root_op(f,0,(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=ROOT_BORROW_CALLABLE,.args={offset,1},.immediate=1});
        call=XR_XIR_CALL_INDIRECT;
    }
    uint32_t start=root_group(f,0,&argument,1);
    root_op(f,0,(XrXirInstruction){.op=call,.args={start,1},.immediate=target});
    root_bounds(f,1);root_view(f,1,kind==8 || kind==9?1u:0u,kind==8 || kind==9);
}
static void root_project_case(RootFixture *f,uint32_t kind) {
    root_bounds(f,0);uint32_t source=kind==10?5u:kind==11?4u:3u;
    XrXirType type=kind==10?ROOT_NESTED:kind==11?ROOT_MAP:ROOT_OBJECT;
    uint32_t base=root_op(f,0,(XrXirInstruction){.op=kind==12?XR_XIR_OBJECT_PLACE:XR_XIR_CELL_PLACE,.type=type,.args={source}});
    uint32_t place=root_op(f,0,(XrXirInstruction){.op=kind==12?XR_XIR_FIELD_PLACE:XR_XIR_INDEX_PLACE,
        .type=ROOT_ARRAY,.args={base,kind==12?0u:7u}});
    uint32_t cell=root_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=ROOT_CELL,.args={place}});
    root_view(f,0,cell,false);
}
static void root_wrapper_case(RootFixture *f) {
    root_bounds(f,0);uint32_t a=root_view(f,0,1,false),b=root_view(f,0,2,false);
    uint32_t some=root_op(f,0,(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=ROOT_OPTIONAL,.args={b}});
    uint32_t start=root_group(f,0,(uint32_t[]){a,some},2);
    uint32_t tuple=root_op(f,0,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=ROOT_TUPLE,.args={start,2}});
    uint32_t field=root_op(f,0,(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=ROOT_OPTIONAL,.args={tuple},.immediate=2});
    uint32_t value=root_op(f,0,(XrXirInstruction){.op=XR_XIR_NULLABLE_UNWRAP,.type=ROOT_SHARED,.args={field}});
    root_view(f,0,value,true);
}
static void root_return_case(RootFixture *f,uint32_t kind) {
    if (kind==21) {f->parameters[0][7]=ROOT_FACTORY_CALLABLE;f->functions[0].parameter_count=8;}
    root_bounds(f,0);uint32_t arg=1;
    if (kind==14 || kind==15) {
        arg=root_view(f,0,1,false);f->parameters[1][0]=ROOT_SHARED;
        f->functions[1].result=ROOT_SHARED;
        root_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}});
    } else {
        f->functions[1].result=ROOT_SHARED;root_bounds(f,1);
        uint32_t view=root_view(f,1,0,false);root_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN,.args={view}});
    }
    uint32_t start=root_group(f,0,&arg,1);XrXirOp call=kind==15?XR_XIR_INVOKE:XR_XIR_CALL;int64_t target=1;
    if (kind==21) {call=XR_XIR_CALL_INDIRECT;target=7;}
    uint32_t result=root_op(f,0,(XrXirInstruction){.op=call,.type=ROOT_SHARED,.args={start,1},
        .targets={kind==15?1u:0u,kind==15?2u:0u},.immediate=target});
    if (kind==15) {
        uint32_t index=f->functions[0].instruction_count-1u;
        result=root_op(f,0,(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=ROOT_SHARED,.immediate=index});
        root_view(f,0,result,true);root_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});
        uint32_t error=f->functions[0].instruction_count;
        root_op(f,0,(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=index});
        f->functions[0].block_count=3;f->blocks[0][0]=(XrXirBlock){.count=index+1u};
        f->blocks[0][1]=(XrXirBlock){.first=index+1u,.count=3};f->blocks[0][2]=(XrXirBlock){.first=error,.count=1};
    } else root_view(f,0,result,true);
}
static void root_module_case(RootFixture *f,bool captured) {
    f->slot=(XrXirSlot){0,ROOT_CELL,1};f->declarations.slots=&f->slot;f->declarations.slot_count=1;
    root_op(f,3,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=ROOT_ARRAY});
    root_op(f,3,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=ROOT_CELL,.args={0}});
    root_op(f,3,(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={1}});
    if (captured) {uint32_t cell=root_op(f,0,(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=ROOT_CELL});root_capture(f,cell);}
    uint32_t function=captured?1u:0u;root_bounds(f,function);
    uint32_t cell=root_op(f,function,(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=ROOT_CELL});root_view(f,function,cell,false);
}
static void root_loop_case(RootFixture *f) {
    root_bounds(f,0);root_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
    uint32_t start=root_group(f,0,(uint32_t[]){0,1,1,10},4);
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_PHI,.type=ROOT_CELL,.args={start,4}});
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});root_view(f,0,10,false);
    f->functions[0].block_count=3;f->blocks[0][0]=(XrXirBlock){.count=3};
    f->blocks[0][1]=(XrXirBlock){.first=3,.count=2};f->blocks[0][2]=(XrXirBlock){.first=5,.count=1};
}
static inline void root_prepare(RootFixture *f,uint32_t kind) {
    root_fixture(f);
    if (kind==0) {
        root_bounds(f,0);uint32_t cell=root_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=ROOT_CELL,.args={6}});
        uint32_t copy=root_op(f,0,(XrXirInstruction){.op=XR_XIR_COPY,.type=ROOT_CELL,.args={cell}});
        uint32_t view=root_view(f,0,copy,false);root_view(f,0,view,true);
    } else if (kind==1 || kind==2 || kind==3) {
        root_bounds(f,0);if (kind!=1) root_capture(f,1);
        if (kind==3) {root_bounds(f,2);root_view(f,2,0,false);}else root_view(f,0,1,false);
    } else if (kind<=5) root_phi_case(f,true,kind==5);
    else if (kind<=9) root_call_case(f,kind);
    else if (kind<=12) root_project_case(f,kind);
    else if (kind==13) root_wrapper_case(f);
    else if (kind==14 || kind==15 || kind==20 || kind==21) root_return_case(f,kind);
    else if (kind==16 || kind==17) root_module_case(f,kind==17);
    else if (kind==18) root_loop_case(f);
    else if (kind==19) {
        f->parameters[1][0]=ROOT_TUPLE;root_bounds(f,1);
        uint32_t field=root_op(f,1,(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=ROOT_OPTIONAL,.args={0},.immediate=2});
        uint32_t value=root_op(f,1,(XrXirInstruction){.op=XR_XIR_NULLABLE_UNWRAP,.type=ROOT_SHARED,.args={field}});
        root_view(f,1,value,true);
    } else CHECK(false);
    for (uint32_t i=0;i<4;++i) {
        XrXirFunction *body=&f->functions[i];uint32_t last=body->instruction_count;
        if (last && f->ops[i][last-1u].op==XR_XIR_RETURN) continue;
        root_op(f,i,(XrXirInstruction){.op=XR_XIR_RETURN});
        if (body->block_count>1) {f->blocks[i][0].count=i?f->blocks[i][0].count:f->blocks[i][1].first;
            ++f->blocks[i][body->block_count-1u].count;}
    }
    for (uint32_t i=0;i<4;++i) {
        if (!f->functions[i].parameter_count) f->functions[i].parameters=NULL;
        if (!f->functions[i].operand_count) f->functions[i].operands=NULL;
    }
}
#endif // XIR_VIEW_ROOT_FIXTURE_H
