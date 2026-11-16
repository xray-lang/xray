/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LIR_MAP_BORROW_FIXTURE_H
#define LIR_MAP_BORROW_FIXTURE_H
static void mb_drop(CArrayFixture *c,XrLirBlock block,XrLirOp op,XrLirValue value) {
    XrLirInstruction instruction={.op=op,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
    OK(xr_lir_emit_effect(c->module,block,&instruction));
}
static XrLirValue mb_operation(CArrayFixture *c,XrLirBlock block,XrLirMapOperation role,
    XrLirType type,const XrLirValue *arguments,uint32_t count) {
    uint32_t id=0; OK(xr_lir_map_operation(c->module,role,XR_LIR_MAP_STRING_I64,&id));
    XrLirInstruction instruction={.op=XR_LIR_CALL_INTRINSIC,.type=type,.intrinsic=id,
        .operands=arguments,.operand_count=count};
    XrLirValue output={0}; OK(xr_lir_emit_value(c->module,block,&instruction,&output)); return output;
}
static XrLirValue map_binding_empty(CArrayFixture *c,XrLirBlock block) {
    XrLirMapLiteral literal={"map-binding.xr",14,NULL,0}; uint32_t ordinal=0;
    OK(xr_lir_map_literal_add(c->module,&literal,&ordinal));
    XrLirInstruction op={.op=XR_LIR_MAP_LITERAL_NEW,.type=XR_LIR_MAP_STRING_I64,.bits=ordinal};
    XrLirValue value={0}; OK(xr_lir_emit_value(c->module,block,&op,&value)); return value;
}
static XrLirModule *map_binding_formal_fixture(unsigned operation,bool retained,bool closure_target,XrLirFunction *root,
    XrLirFunction *helper) {
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    OK(xr_lir_map_catalog_install(c.module,false,false,false,false,false));
    XrLirStateModule owner={0};
    XrLirStateSlot slot={.payload=XR_LIR_MAP_STRING_I64,.mutable=true,.size=8,.alignment=8};
    XrLirStateLayout layout={.modules=&owner,.module_count=1,.slots=&slot,.slot_count=1,.payload_size=8,.payload_alignment=8};
    OK(xr_lir_set_state_layout(c.module,&layout));
    XrLirType parameters[]={XR_LIR_MAP_STRING_I64,XR_LIR_STATE};
    XrLirFunctionSpec spec={.name="borrowed_write",.name_length=14,.parameters=parameters,.parameter_count=2,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,helper));
    if (closure_target) {
        XrLirType callable=0;
        OK(xr_lir_callable_type(c.module,&(XrLirCallableType){.result=XR_LIR_I64},&callable));
        OK(xr_lir_set_closure_target(c.module,*helper,(XrLirClosureTarget){callable,1,true}));
    }
    OK(xr_lir_define_function(c.module,*helper));
    XrLirBlock block={0}; OK(xr_lir_add_block(c.module,*helper,NULL,0,&block)); OK(xr_lir_set_entry(c.module,*helper,block));
    XrLirValue map={0},state={0}; OK(xr_lir_function_parameter(c.module,*helper,0,&map));
    OK(xr_lir_function_parameter(c.module,*helper,1,&state));
    XrLirValue rhs=retained ? ca_value(&c,block,XR_LIR_MAP_RETAIN,XR_LIR_MAP_STRING_I64,&map,1) : map;
    XrLirValue read={0};
    if (operation>=3) {
        XrLirValue ref={0},arguments[]={state,rhs};
        if (operation==4) {
            ref=ca_value(&c,block,XR_LIR_STATE_CONSTRUCT,XR_LIR_STATE_REF,arguments,2);
            mb_drop(&c,block,XR_LIR_STATE_PUBLISH,ref);
        } else {
            ref=ca_value(&c,block,XR_LIR_STATE_REFERENCE,XR_LIR_STATE_REF,&state,1);
            arguments[0]=operation==3 ? ref : state;
            XrLirInstruction write={.op=operation==3 ? XR_LIR_STATE_WRITE : XR_LIR_STATE_INITIALIZE,
                .type=XR_LIR_UNIT,.operands=arguments,.operand_count=2};
            OK(xr_lir_emit_effect(c.module,block,&write));
        }
        if (operation>=4) mb_drop(&c,block,XR_LIR_MODULE_READY,state);
        read=ca_value(&c,block,XR_LIR_STATE_READ,XR_LIR_MAP_STRING_I64,&ref,1);
    } else {
        XrLirValue initial=operation ? map_binding_empty(&c,block) : rhs;
        XrLirValue cell=ca_value(&c,block,XR_LIR_CELL_NEW,XR_LIR_MAP_STRING_I64_CELL,&initial,1);
        if (operation) {
            mb_drop(&c,block,XR_LIR_MAP_DROP,initial);
            XrLirValue binding=operation==2 ? ca_value(&c,block,XR_LIR_MAP_BORROW,XR_LIR_MAP_STRING_I64_REF,&cell,1) : cell;
            XrLirValue arguments[]={binding,rhs};
            XrLirInstruction write={.op=operation==2 ? XR_LIR_MAP_REF_WRITE : XR_LIR_CELL_WRITE,
                .type=XR_LIR_UNIT,.operands=arguments,.operand_count=2};
            OK(xr_lir_emit_effect(c.module,block,&write));
        }
        read=ca_value(&c,block,XR_LIR_CELL_READ,XR_LIR_MAP_STRING_I64,&cell,1);
        mb_drop(&c,block,XR_LIR_MAP_CELL_DROP,cell);
    }
    XrLirValue length=mb_operation(&c,block,XR_LIR_MAP_OP_LEN,XR_LIR_I64,&read,1);
    mb_drop(&c,block,XR_LIR_MAP_DROP,read);
    if (retained) mb_drop(&c,block,XR_LIR_MAP_DROP,rhs);
    ca_return(&c,block,length);
    spec=(XrLirFunctionSpec){.name="binding_root",.name_length=12,.parameters=&parameters[1],.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,root)); OK(xr_lir_set_state_entry(c.module,*root));
    OK(xr_lir_define_function(c.module,*root)); OK(xr_lir_add_block(c.module,*root,NULL,0,&block));
    OK(xr_lir_set_entry(c.module,*root,block)); OK(xr_lir_function_parameter(c.module,*root,0,&state));
    mb_drop(&c,block,XR_LIR_MODULE_BEGIN,state);
    XrLirLiteral literal={0}; OK(xr_lir_add_literal(c.module,"x",1,&literal));
    XrLirInstruction op={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    XrLirValue key={0}; OK(xr_lir_emit_value(c.module,block,&op,&key));
    XrLirMapLiteralOrigin origin={0,1,1}; XrLirMapLiteral description={"map-binding.xr",14,&origin,1};
    uint32_t ordinal=0; OK(xr_lir_map_literal_add(c.module,&description,&ordinal));
    XrLirValue arguments[]={key,ca_constant(&c,block,1)};
    op=(XrLirInstruction){.op=XR_LIR_MAP_LITERAL_NEW,.type=XR_LIR_MAP_STRING_I64,.bits=ordinal,.operands=arguments,.operand_count=2};
    OK(xr_lir_emit_value(c.module,block,&op,&map));
    if (operation<4) {
        XrLirValue empty=map_binding_empty(&c,block),construct[]={state,empty};
        XrLirValue ref=ca_value(&c,block,XR_LIR_STATE_CONSTRUCT,XR_LIR_STATE_REF,construct,2);
        mb_drop(&c,block,XR_LIR_MAP_DROP,empty);
        mb_drop(&c,block,XR_LIR_STATE_PUBLISH,ref); mb_drop(&c,block,XR_LIR_MODULE_READY,state);
    }
    arguments[0]=map; arguments[1]=state;
    op=(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=*helper,.operands=arguments,.operand_count=2};
    XrLirValue result={0}; OK(xr_lir_emit_value(c.module,block,&op,&result));
    mb_drop(&c,block,XR_LIR_MAP_DROP,map); mb_drop(&c,block,XR_LIR_STRING_DROP,key);
    ca_return(&c,block,result); OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}
#endif // LIR_MAP_BORROW_FIXTURE_H
