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
