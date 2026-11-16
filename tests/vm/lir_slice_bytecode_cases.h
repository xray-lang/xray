/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_bytecode_cases.h - Natural windows, duplicate keeper edges and retirement
 */
typedef struct SliceBytecodeFixture {
    XrLirModule *module;
    XrLirBlock block;
    XrLirType element, slice;
} SliceBytecodeFixture;
static XrLirValue slice_bytecode_role(SliceBytecodeFixture *c,XrLirSliceOperation role,
    XrLirType result,const XrLirValue *arguments,uint32_t count) {
    uint32_t id=0;
    OK(xr_lir_slice_operation(c->module,role,c->element,role==XR_LIR_SLICE_OP_NEW ? XR_LIR_ARRAY_CELL : 0,&id));
    XrLirInstruction instruction={.op=XR_LIR_CALL_INTRINSIC,.type=result,.intrinsic=id,.operands=arguments,.operand_count=count};
    XrLirValue output={0};
    if (result==XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module,c->block,&instruction));
    else OK(xr_lir_emit_value(c->module,c->block,&instruction,&output));
    return output;
}
static XrLirValue slice_bytecode_array(SliceBytecodeFixture *c,XrLirArrayOperation role,
    XrLirType result,const XrLirValue *arguments,uint32_t count) {
    uint32_t id=0; OK(xr_lir_array_operation(c->module,c->element,role,&id));
    XrLirValue output={0};
    OK(xr_lir_emit_value(c->module,c->block,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=result,
        .intrinsic=id,.operands=arguments,.operand_count=count},&output));
    return output;
}
static XrLirModule *slice_bytecode_fixture(XrLirType element,bool edges,bool stateful,XrLirFunction *root) {
    SliceBytecodeFixture c={.element=element};
    OK(xr_lir_module_create(NULL,&c.module)); OK(xr_lir_slice_catalog_install(c.module));
    OK(xr_lir_slice_type(c.module,element,&c.slice));
    XrLirType state_type=XR_LIR_STATE;
    if (stateful) {
        XrLirStateModule group={0};
        XrLirStateSlot slot={.payload=XR_LIR_I64,.mutable=true,.size=8,.alignment=8};
        XrLirStateLayout layout={.modules=&group,.module_count=1,.slots=&slot,.slot_count=1,.payload_size=8,.payload_alignment=8};
        OK(xr_lir_set_state_layout(c.module,&layout));
    }
    XrLirFunctionSpec spec={.name="slice_bytes",.name_length=11,.result=XR_LIR_I64,
        .parameters=stateful ? &state_type : NULL,.parameter_count=stateful ? 1u : 0u};
    OK(xr_lir_declare_function(c.module,&spec,root));
    if (stateful) OK(xr_lir_set_state_entry(c.module,*root));
    OK(xr_lir_define_function(c.module,*root));
    OK(xr_lir_add_block(c.module,*root,NULL,0,&c.block)); OK(xr_lir_set_entry(c.module,*root,c.block));
    XrLirValue state_reference={0};
    if (stateful) {
        XrLirValue state={0}; OK(xr_lir_function_parameter(c.module,*root,0,&state));
        managed_state_effect(c.module,c.block,XR_LIR_MODULE_BEGIN,&state,1,0);
        XrLirValue value=managed_state_value(c.module,c.block,XR_LIR_CONST,XR_LIR_I64,NULL,0,17),args[]={state,value};
        state_reference=managed_state_value(c.module,c.block,XR_LIR_STATE_CONSTRUCT,XR_LIR_STATE_REF,args,2,0);
        managed_state_effect(c.module,c.block,XR_LIR_STATE_PUBLISH,&state_reference,1,0);
        managed_state_effect(c.module,c.block,XR_LIR_MODULE_READY,&state,1,0);
    }
    XrLirValue items[3];
    for (uint32_t n=0;n<3;++n)
        items[n]=managed_state_value(c.module,c.block,XR_LIR_CONST,element,NULL,0,11u*(n+1));
    XrLirValue array=slice_bytecode_array(&c,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,items,3);
    XrLirValue cell=managed_state_value(c.module,c.block,XR_LIR_CELL_NEW,XR_LIR_ARRAY_CELL,&array,1,0);
    managed_state_effect(c.module,c.block,XR_LIR_ARRAY_DROP,&array,1,0);
    XrLirValue zero=managed_state_value(c.module,c.block,XR_LIR_CONST,XR_LIR_I64,NULL,0,0);
    XrLirValue one=managed_state_value(c.module,c.block,XR_LIR_CONST,XR_LIR_I64,NULL,0,1);
    XrLirValue three=managed_state_value(c.module,c.block,XR_LIR_CONST,XR_LIR_I64,NULL,0,3);
    XrLirValue open=managed_state_value(c.module,c.block,XR_LIR_CONST,XR_LIR_I1,NULL,0,0);
    XrLirValue range[]={cell,zero,three};
    XrLirValue original=slice_bytecode_role(&c,XR_LIR_SLICE_OP_NEW,c.slice,range,3);
    XrLirValue window[]={original,one,three,open};
    XrLirValue child=slice_bytecode_role(&c,XR_LIR_SLICE_OP_REBORROW,c.slice,window,4);
    XrLirValue bound=slice_bytecode_role(&c,XR_LIR_SLICE_OP_BIND,c.slice,&child,1),left=bound,right=bound;
    if (edges) {
        XrLirBlock target={0}; XrLirType parameters[]={c.slice,c.slice};
        OK(xr_lir_add_block(c.module,*root,parameters,2,&target));
        XrLirValue copies[]={bound,bound};
        OK(xr_lir_set_terminator(c.module,c.block,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
            .edge_count=1,.edges={{target,copies,2}}}));
        c.block=target;
        OK(xr_lir_block_parameter(c.module,target,0,&left)); OK(xr_lir_block_parameter(c.module,target,1,&right));
    }
    XrLirValue length=slice_bytecode_role(&c,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,&left,1);
    XrLirValue read[]={right,zero};
    XrLirValue first=slice_bytecode_role(&c,XR_LIR_SLICE_OP_GET,element,read,2);
    XrLirValue copied=slice_bytecode_role(&c,XR_LIR_SLICE_OP_COPY,XR_LIR_ARRAY,&left,1);
    // The original edge source remains a live loan after both keeper copies.
    XrLirValue still_live=slice_bytecode_role(&c,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,&bound,1);
    if (edges) {
        (void)slice_bytecode_role(&c,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,&left,1);
        (void)slice_bytecode_role(&c,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,&right,1);
    }
    (void)slice_bytecode_role(&c,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,&bound,1);
    (void)slice_bytecode_role(&c,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,&child,1);
    (void)slice_bytecode_role(&c,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,&original,1);
    managed_state_effect(c.module,c.block,XR_LIR_ARRAY_CELL_DROP,&cell,1,0);
    XrLirValue tail_args[]={copied,one};
    XrLirValue last=slice_bytecode_array(&c,XR_LIR_ARRAY_OP_GET,element,tail_args,2);
    managed_state_effect(c.module,c.block,XR_LIR_ARRAY_DROP,&copied,1,0);
    if (element==XR_LIR_U8) {
        first=managed_state_value(c.module,c.block,XR_LIR_INTCAST,XR_LIR_I64,&first,1,0);
        last=managed_state_value(c.module,c.block,XR_LIR_INTCAST,XR_LIR_I64,&last,1,0);
    }
    XrLirValue sum[]={length,first};
    XrLirValue answer=managed_state_value(c.module,c.block,XR_LIR_ADD,XR_LIR_I64,sum,2,0);
    sum[0]=answer; sum[1]=last; answer=managed_state_value(c.module,c.block,XR_LIR_ADD,XR_LIR_I64,sum,2,0);
    sum[0]=answer; sum[1]=still_live; answer=managed_state_value(c.module,c.block,XR_LIR_ADD,XR_LIR_I64,sum,2,0);
    if (stateful) {
        sum[0]=answer;
        sum[1]=managed_state_value(c.module,c.block,XR_LIR_STATE_READ,XR_LIR_I64,&state_reference,1,0);
        answer=managed_state_value(c.module,c.block,XR_LIR_ADD,XR_LIR_I64,sum,2,0);
    }
    OK(xr_lir_set_terminator(c.module,c.block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&answer,.operand_count=1}));
    OK(xr_lir_module_finish(c.module,NULL));
    return c.module;
}
static void slice_bytecode_retirement(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    for (uint32_t family=0;family<2;++family) for (uint32_t edges=0;edges<2;++edges) {
        bool stateful=family && !edges;
        XrLirFunction root={0}; XrLirModule *input=slice_bytecode_fixture(elements[family],edges!=0,stateful,&root);
        attempts=0; XrRbcProgram *program=compile(input,root); size_t sites=attempts;
        if (!family && edges) {
            xr_rbc_program_destroy(program); program=NULL; CHECK(!compiler_live_allocations);
            for (size_t point=1;point<=sites;++point) {
                attempts=0; failure=point; XrRbcCompileDiagnostic diagnostic={0};
                CHECK(xr_rbc_compile(input,root,&program,&diagnostic)==XR_RBC_COMPILE_OUT_OF_MEMORY &&
                    diagnostic.status==XR_RBC_COMPILE_OUT_OF_MEMORY && !program && !compiler_live_allocations);
            }
            failure=SIZE_MAX; program=compile(input,root);
        }
        check_receiving(program);
        const XrRbcModule *module=xr_rbc_program_view(program);
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
        uint32_t roles=0; size_t borrowed=0;
        for (size_t n=0;n<code->access_count;++n)
            if (code->accesses[n].operation==XR_RBC_VALUE_SLICE) roles|=1u<<code->accesses[n].index;
        for (uint32_t v=1;v<=code->value_count;++v) {
            const XrLirValueHome *home=&code->homes->values[v];
            if (!home->view_workspace) continue;
            CHECK(home->size==sizeof(XrtReadonlyArraySlice) && !home->owner_count && home->first_owner==SIZE_MAX &&
                home->temporary_first_owner==SIZE_MAX && code->locations[v].kind==XR_RBC_VALUE_NOWHERE);
            ++borrowed;
        }
        CHECK(roles==((1u<<XR_LIR_SLICE_OP_COUNT)-2u) && borrowed==(edges ? 5u : 3u));
        // A real lease on copied function descriptors is still an external raw input.
        XrRbcFunction function=module->functions[0]; XrRbcModule raw=*module; raw.functions=&function;
        CHECK(module->function_count==1); XrRbcExecution *invalid=NULL;
        CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&invalid)==XR_RBC_INVALID && !invalid);
        xr_lir_module_destroy(input); run_after_input_retirement(program,stateful ? 76 : 59);
    }
    puts("readonly Slice: I64/U8 windows, copied Arrays, duplicate keeper edges and retired creators passed");
}
