/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * rbc_map_literal_cases.h - Owned provenance, receiving rejection and failure lifetime
 */
#ifndef RBC_MAP_LITERAL_CASES_H
#define RBC_MAP_LITERAL_CASES_H
static XrLirModule *receiving_map_literal(XrLirFunction *root, bool duplicate) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL,&module));
    OK(xr_lir_map_catalog_install(module,false,false,false,false,false));
    OK(xr_lir_declare_function(module,&(XrLirFunctionSpec){.name="literal",.name_length=7,.result=XR_LIR_I64},root));
    OK(xr_lir_define_function(module,*root));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(module,*root,NULL,0,&block)); OK(xr_lir_set_entry(module,*root,block));
    char path[] = "owned-map.xr";
    XrLirMapLiteralOrigin origins[] = {{0,3,12},{0,5,16}};
    XrLirMapLiteral provenance = {path,(uint32_t)sizeof(path)-1,origins,duplicate ? 2u : 1u};
    uint32_t ordinal = 0;
    OK(xr_lir_map_literal_add(module,&provenance,&ordinal));
    path[0] = 'X'; origins[0].line = 99;
    XrLirLiteral bytes = {0}; OK(xr_lir_add_literal(module,(const uint8_t *)"x",1,&bytes));
    XrLirValue key = managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,bytes.id);
    XrLirValue number = managed_state_value(module,block,XR_LIR_CONST,XR_LIR_I64,NULL,0,42);
    XrLirValue arguments[] = {key,number,key,number}, map = {0};
    OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_MAP_LITERAL_NEW,
        .type=XR_LIR_MAP_STRING_I64,.bits=ordinal,.operands=arguments,.operand_count=duplicate ? 4u : 2u},&map));
    uint32_t operation = 0; XrLirValue answer = {0}, lookup[] = {map,key};
    OK(xr_lir_map_operation(module,XR_LIR_MAP_OP_INDEX,XR_LIR_MAP_STRING_I64,&operation));
    OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=XR_LIR_I64,.intrinsic=operation,.operands=lookup,.operand_count=2},&answer));
    managed_state_effect(module,block,XR_LIR_MAP_DROP,&map,1,0);
    managed_state_effect(module,block,XR_LIR_STRING_DROP,&key,1,0);
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&answer,.operand_count=1}));
    OK(xr_lir_module_finish(module,NULL)); return module;
}
static void receiving_map_refused(const XrRbcModule *module, XrtLocationOwner *locations) {
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(module,0,NULL,0,locations,&execution)==XR_RBC_INVALID && !execution);
}
static void receiving_map_provenance(void) {
    XrLirFunction root = {0}; XrLirModule *module = receiving_map_literal(&root,false);
    attempts = 0; XrRbcProgram *program = compile(module,root); size_t sites = attempts;
    xr_rbc_program_destroy(program); CHECK(sites && !compiler_live_allocations);
    for (size_t site=1; site<=sites; ++site) {
        attempts=0; failure=site; program=NULL;
        CHECK(xr_rbc_compile(module,root,&program,NULL)==XR_RBC_COMPILE_OUT_OF_MEMORY && !program);
        CHECK(!compiler_live_allocations);
    }
    failure=SIZE_MAX; program=compile(module,root); check_receiving(program);
    const XrRbcModule *code=xr_rbc_program_view(program);
    XrtLocationOwner *locations=xr_rbc_program_locations(program);
    CHECK(code->map_literal_count==1 && code->map_literals && code->map_literals[0].items[0].line==3);
    XrRbcModule changed=*code;
    changed.map_literal_count=0; receiving_map_refused(&changed,locations);
    changed=*code; changed.map_literals=code->map_literals+1; receiving_map_refused(&changed,locations);
    const XrRbcValueCode *values=xr_rbc_module_value_code(code,0); CHECK(values);
    XrRbcValueAccess *access=NULL;
    for (size_t n=0; n<values->access_count; ++n)
        if (values->accesses[n].operation==XR_RBC_VALUE_MAP_LITERAL)
            access=(XrRbcValueAccess *)(void *)&values->accesses[n];
    CHECK(access && access->count==2);
    XrRbcValueAccess saved=*access;
    access->index=0; receiving_map_refused(code,locations);
    *access=saved; access->index=2; receiving_map_refused(code,locations);
    *access=saved; access->count=1; receiving_map_refused(code,locations);
    *access=saved;
    uint32_t *argument=(uint32_t *)(void *)&values->arguments[access->first];
    uint32_t key=*argument; *argument=values->arguments[access->first+1];
    receiving_map_refused(code,locations); *argument=key;
    xr_lir_module_destroy(module); run_after_input_retirement(program,42);
    printf("Map provenance: %zu compile faults, forged receiving descriptions and retired inputs PASS\n",sites);
}
static void receiving_map_message_lifetime(void) {
    XrLirFunction root={0}; XrLirModule *module=receiving_map_literal(&root,true);
    XrRbcProgram *program=compile(module,root); xr_lir_module_destroy(module);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_MESSAGE_PANIC);
    xr_rbc_execution_free(&execution);
    uint32_t code=0,source=UINT32_MAX; const char *bytes=NULL; size_t length=0;
    const XrtLocationOwner *locations=NULL;
    static const char expected[]="duplicate key \"x\" in Map literal (entries at owned-map.xr:3:12 and owned-map.xr:5:16)";
    CHECK(xr_rbc_outcome_message_panic(outcome,&code,&bytes,&length,&source,&locations));
    CHECK(code==434 && source==0 && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    CHECK(!xr_rbc_outcome_message_panic(outcome,NULL,&bytes,&length,&source,&locations));
    xr_rbc_outcome_free(&outcome); xr_rbc_outcome_free(&outcome); CHECK(!compiler_live_allocations);
}
#endif // RBC_MAP_LITERAL_CASES_H
