/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_string_append_cases.h - Consuming append through real typed calls and owners
 */
static XrLirValue string_append_call(XrLirModule *module,XrLirBlock block,uint32_t role,
    XrLirValue source,XrLirValue suffix) {
    XrLirValue arguments[]={source,suffix},result={0};
    OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_STRING,
        .intrinsic=role,.operands=arguments,.operand_count=2},&result));
    return result;
}
static XrLirModule *string_append_fixture(bool shared,XrLirFunction *root) {
    XrLirModule *module=NULL; OK(xr_lir_module_create(NULL,&module));
    OK(xr_lir_string_catalog_install(module,false,false,false));
    uint32_t role=0; OK(xr_lir_string_operation(module,XR_LIR_STRING_OP_APPEND_OWNED,&role));
    XrLirType parameter_type=XR_LIR_STRING; XrLirFunction callee={0};
    XrLirFunctionSpec spec={.name="append_suffix",.name_length=13,.parameters=&parameter_type,
        .parameter_count=1,.result=XR_LIR_STRING};
    OK(xr_lir_declare_function(module,&spec,&callee)); OK(xr_lir_define_function(module,callee));
    XrLirBlock block={0}; OK(xr_lir_add_block(module,callee,NULL,0,&block)); OK(xr_lir_set_entry(module,callee,block));
    XrLirValue suffix={0}; OK(xr_lir_function_parameter(module,callee,0,&suffix));
    static const uint8_t bytes[]={'A',0,0xc3,0xa9}; XrLirLiteral literal={0};
    OK(xr_lir_add_literal(module,bytes,sizeof(bytes),&literal));
    XrLirValue left=managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id),old={0};
    if (shared) old=managed_state_value(module,block,XR_LIR_STRING_RETAIN,XR_LIR_STRING,&left,1,0);
    XrLirValue result=string_append_call(module,block,role,left,suffix);
    if (shared) {
        XrLirValue alias=managed_state_value(module,block,XR_LIR_STRING_RETAIN,XR_LIR_STRING,&result,1,0);
        result=string_append_call(module,block,role,result,alias);
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&alias,1,0);
        XrLirValue inputs[]={old,result};
        XrLirValue combined=managed_state_value(module,block,XR_LIR_STRING_CONCAT,XR_LIR_STRING,inputs,2,0);
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&old,1,0);
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&result,1,0); result=combined;
    } else result=string_append_call(module,block,role,result,suffix);
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&result,.operand_count=1}));
    spec=(XrLirFunctionSpec){.name="append_caller",.name_length=13,.result=XR_LIR_STRING};
    OK(xr_lir_declare_function(module,&spec,root)); OK(xr_lir_define_function(module,*root));
    block=(XrLirBlock){0}; OK(xr_lir_add_block(module,*root,NULL,0,&block)); OK(xr_lir_set_entry(module,*root,block));
    literal=(XrLirLiteral){0};
    OK(xr_lir_add_literal(module,(const uint8_t *)"!",1,&literal));
    suffix=managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id);
    result=(XrLirValue){0};
    OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_STRING,.callee=callee,
        .operands=&suffix,.operand_count=1},&result));
    managed_state_effect(module,block,XR_LIR_STRING_DROP,&suffix,1,0);
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&result,.operand_count=1}));
    OK(xr_lir_module_finish(module,NULL)); return module;
}
static void string_append_retirement(void) {
    static const uint8_t unique[]={'A',0,0xc3,0xa9,'!','!'};
    static const uint8_t shared[]={'A',0,0xc3,0xa9,'A',0,0xc3,0xa9,'!','A',0,0xc3,0xa9,'!'};
    for (uint32_t mode=0;mode<2;++mode) {
        XrLirFunction root={0}; XrLirModule *input=string_append_fixture(mode!=0,&root);
        attempts=0; XrRbcProgram *program=compile(input,root); size_t sites=attempts;
        if (mode) {
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
        const XrRbcValueCode *callee=xr_rbc_module_value_code(module,1); size_t appends=0;
        for (size_t n=0;n<callee->access_count;++n) appends+=callee->accesses[n].operation==XR_RBC_VALUE_STRING_APPEND_OWNED;
        CHECK(appends==2 && callee->homes->values[callee->parameters[0]].parameter==1);
        xr_lir_module_destroy(input); XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK); xr_rbc_program_destroy(program);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK); xr_rbc_execution_free(&execution);
        const XrtManagedHeader *owner=NULL; CHECK(xr_rbc_outcome_string(outcome,&owner) && owner);
        const XrtStringBacking *text=(const XrtStringBacking *)owner;
        const uint8_t *expected=mode ? shared : unique; size_t count=mode ? sizeof(shared) : sizeof(unique);
        CHECK(text->length==count && text->runes==(mode ? 11u : 5u) && !memcmp(text->bytes,expected,count) && !text->bytes[count]);
        xr_rbc_outcome_free(&outcome); CHECK(!compiler_live_allocations);
    }
    puts("String append: borrowed call suffix, chained moves, shared aliases and retired creators passed");
}
static XrLirValue string_binding_role(XrLirModule *module,XrLirBlock block,XrLirStringOperation operation,
    XrLirType type,const XrLirValue *arguments,uint32_t count) {
    uint32_t id=0; OK(xr_lir_string_operation(module,operation,&id));
    XrLirInstruction instruction={.op=XR_LIR_CALL_INTRINSIC,.type=type,.intrinsic=id,.operands=arguments,.operand_count=count};
    XrLirValue value={0};
    if (type==XR_LIR_UNIT) OK(xr_lir_emit_effect(module,block,&instruction));
    else OK(xr_lir_emit_value(module,block,&instruction,&value));
    return value;
}
static void string_binding_return(XrLirModule *module,XrLirBlock block,XrLirValue value) {
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1}));
}
static XrLirModule *string_binding_fixture(bool state,XrLirFunction *root) {
    XrLirModule *module=NULL; OK(xr_lir_module_create(NULL,&module));
    OK(xr_lir_string_catalog_install(module,false,false,false));
    XrLirType parameters[]={XR_LIR_STRING_CELL,XR_LIR_STRING_REF,XR_LIR_STRING};
    XrLirFunction callee={0}; XrLirFunctionSpec spec={.name="binding_alias",.name_length=13,
        .parameters=parameters,.parameter_count=3,.result=XR_LIR_STRING};
    XrLirBlock body={0}; XrLirValue inputs[3]={{0}};
    if (!state) {
        OK(xr_lir_declare_function(module,&spec,&callee)); OK(xr_lir_define_function(module,callee));
        OK(xr_lir_add_block(module,callee,NULL,0,&body)); OK(xr_lir_set_entry(module,callee,body));
        for (uint32_t p=0;p<3;++p) OK(xr_lir_function_parameter(module,callee,p,&inputs[p]));
        XrLirValue local=managed_state_value(module,body,XR_LIR_CELL_NEW,XR_LIR_STRING_CELL,&inputs[2],1,0);
        XrLirValue write[]={inputs[0],inputs[2]}; managed_state_effect(module,body,XR_LIR_CELL_WRITE,write,2,0);
        XrLirValue local_ref=string_binding_role(module,body,XR_LIR_STRING_OP_BORROW_CELL,XR_LIR_STRING_REF,&local,1);
        write[0]=local_ref; (void)string_binding_role(module,body,XR_LIR_STRING_OP_WRITE_REF,XR_LIR_UNIT,write,2);
        XrLirValue snapshot=string_binding_role(module,body,XR_LIR_STRING_OP_READ_REF,XR_LIR_STRING,&inputs[1],1);
        write[0]=inputs[1]; (void)string_binding_role(module,body,XR_LIR_STRING_OP_APPEND_REF,XR_LIR_UNIT,write,2);
        write[0]=inputs[0]; (void)string_binding_role(module,body,XR_LIR_STRING_OP_APPEND_CELL,XR_LIR_UNIT,write,2);
        XrLirValue local_copy=managed_state_value(module,body,XR_LIR_CELL_READ,XR_LIR_STRING,&local,1,0),args[]={snapshot,local_copy};
        XrLirValue pair=managed_state_value(module,body,XR_LIR_STRING_CONCAT,XR_LIR_STRING,args,2,0);
        managed_state_effect(module,body,XR_LIR_STRING_DROP,&snapshot,1,0);
        managed_state_effect(module,body,XR_LIR_STRING_DROP,&local_copy,1,0);
        XrLirValue current=managed_state_value(module,body,XR_LIR_CELL_READ,XR_LIR_STRING,&inputs[0],1,0);
        args[0]=pair; args[1]=current;
        XrLirValue result=managed_state_value(module,body,XR_LIR_STRING_CONCAT,XR_LIR_STRING,args,2,0);
        managed_state_effect(module,body,XR_LIR_STRING_DROP,&pair,1,0);
        managed_state_effect(module,body,XR_LIR_STRING_DROP,&current,1,0);
        managed_state_effect(module,body,XR_LIR_STRING_CELL_DROP,&local,1,0); string_binding_return(module,body,result);
    }
    XrLirType state_type=XR_LIR_STATE;
    spec=(XrLirFunctionSpec){.name="binding_root",.name_length=12,.parameters=state ? &state_type : NULL,
        .parameter_count=state ? 1u : 0u,.result=XR_LIR_STRING};
    OK(xr_lir_declare_function(module,&spec,root)); OK(xr_lir_define_function(module,*root));
    if (state) {
        XrLirStateModule owner={0}; XrLirStateSlot slot={.payload=XR_LIR_STRING,.mutable=true,.size=8,.alignment=8};
        XrLirStateLayout layout={.modules=&owner,.module_count=1,.slots=&slot,.slot_count=1,.payload_size=8,.payload_alignment=8};
        OK(xr_lir_set_state_layout(module,&layout)); OK(xr_lir_set_state_entry(module,*root));
    }
    XrLirBlock block={0}; OK(xr_lir_add_block(module,*root,NULL,0,&block)); OK(xr_lir_set_entry(module,*root,block));
    XrLirLiteral literal={0}; OK(xr_lir_add_literal(module,(const uint8_t *)"cell\0\xc3\xa9",7,&literal));
    XrLirValue seed=managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id),result={0};
    if (state) {
        XrLirValue owner={0}; OK(xr_lir_function_parameter(module,*root,0,&owner));
        managed_state_effect(module,block,XR_LIR_MODULE_BEGIN,&owner,1,0); XrLirValue args[]={owner,seed};
        XrLirValue reference=managed_state_value(module,block,XR_LIR_STATE_CONSTRUCT,XR_LIR_STATE_REF,args,2,0);
        managed_state_effect(module,block,XR_LIR_STATE_PUBLISH,&reference,1,0);
        managed_state_effect(module,block,XR_LIR_MODULE_READY,&owner,1,0);
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&seed,1,0);
        XrLirValue snapshot=managed_state_value(module,block,XR_LIR_STATE_READ,XR_LIR_STRING,&reference,1,0);
        args[0]=reference; args[1]=snapshot;
        (void)string_binding_role(module,block,XR_LIR_STRING_OP_APPEND_STATE,XR_LIR_UNIT,args,2);
        XrLirValue borrowed=string_binding_role(module,block,XR_LIR_STRING_OP_BORROW_STATE,XR_LIR_STRING_REF,&reference,1);
        args[0]=borrowed; (void)string_binding_role(module,block,XR_LIR_STRING_OP_APPEND_REF,XR_LIR_UNIT,args,2);
        result=managed_state_value(module,block,XR_LIR_STATE_READ,XR_LIR_STRING,&reference,1,0);
        args[0]=reference; managed_state_effect(module,block,XR_LIR_STATE_WRITE,args,2,0);
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&snapshot,1,0);
    } else {
        XrLirValue cell=managed_state_value(module,block,XR_LIR_CELL_NEW,XR_LIR_STRING_CELL,&seed,1,0);
        XrLirValue old=managed_state_value(module,block,XR_LIR_STRING_RETAIN,XR_LIR_STRING,&seed,1,0);
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&seed,1,0);
        XrLirValue reference=string_binding_role(module,block,XR_LIR_STRING_OP_BORROW_CELL,XR_LIR_STRING_REF,&cell,1);
        literal=(XrLirLiteral){0}; OK(xr_lir_add_literal(module,(const uint8_t *)"!",1,&literal));
        XrLirValue suffix=managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id),args[]={cell,reference,suffix};
        XrLirValue changed={0}; OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_STRING,
            .callee=callee,.operands=args,.operand_count=3},&changed));
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&suffix,1,0);
        args[0]=old; args[1]=changed;
        result=managed_state_value(module,block,XR_LIR_STRING_CONCAT,XR_LIR_STRING,args,2,0);
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&old,1,0);
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&changed,1,0);
        managed_state_effect(module,block,XR_LIR_STRING_CELL_DROP,&cell,1,0);
    }
    string_binding_return(module,block,result); OK(xr_lir_module_finish(module,NULL)); return module;
}
static void string_binding_retirement(void) {
    for (uint32_t state=0;state<2;++state) {
        XrLirFunction root={0}; XrLirModule *input=string_binding_fixture(state!=0,&root);
        attempts=0; XrRbcProgram *program=compile(input,root); size_t sites=attempts;
        xr_rbc_program_destroy(program); program=NULL;
        for (size_t point=1;point<=sites;++point) {
            attempts=0; failure=point; XrRbcCompileDiagnostic diagnostic={0};
            CHECK(xr_rbc_compile(input,root,&program,&diagnostic)==XR_RBC_COMPILE_OUT_OF_MEMORY &&
                diagnostic.status==XR_RBC_COMPILE_OUT_OF_MEMORY && !program && !compiler_live_allocations);
        }
        failure=SIZE_MAX; program=compile(input,root); check_receiving(program);
        const XrRbcModule *module=xr_rbc_program_view(program);
        size_t stages=0,appends=0,self_caps=0;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrRbcValueCode *code=xr_rbc_module_value_code(module,f); CHECK(code && code->string_binding_setup);
            for (size_t n=0;n<code->access_count;++n) {
                XrRbcValueAccess a=code->accesses[n];
                appends+=a.operation==XR_RBC_VALUE_STRING_APPEND_BINDING;
                stages+=(a.operation==XR_RBC_VALUE_BINDING_NEW || a.operation==XR_RBC_VALUE_BINDING_WRITE) && a.window!=UINT32_MAX;
            }
            for (uint32_t v=1;v<=code->value_count;++v) if (code->homes->bindings &&
                code->homes->bindings[v].kind==XR_LIR_HOME_BINDING_STATE_BORROW && code->homes->bindings[v].source==v) {
                CHECK(code->locations[v].kind==XR_RBC_VALUE_REGISTER || code->locations[v].kind==XR_RBC_VALUE_SPILL); ++self_caps;
            }
        }
        CHECK(appends==2 && stages==(state ? 0u : 3u) && self_caps==(state ? 1u : 0u));
        xr_lir_module_destroy(input); XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK); xr_rbc_program_destroy(program);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK); xr_rbc_execution_free(&execution);
        const XrtManagedHeader *owner=NULL; CHECK(xr_rbc_outcome_string(outcome,&owner));
        const XrtStringBacking *text=(const XrtStringBacking *)owner;
        const char *expected=state ? "cell\0\xc3\xa9" "cell\0\xc3\xa9" "cell\0\xc3\xa9" : "cell\0\xc3\xa9!!!!!";
        size_t length=state ? 21u : 12u;
        CHECK(text && text->length==length && text->runes==(state ? 18u : 11u) && !memcmp(text->bytes,expected,length));
        xr_rbc_outcome_free(&outcome); CHECK(!compiler_live_allocations);
    }
    puts("String bindings: borrowed stages, actual aliases, State capabilities and retired creators passed");
}
