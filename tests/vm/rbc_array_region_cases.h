/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * rbc_array_region_cases.h - Real windows, scalar cycles and raw CFG boundaries
 */
#ifndef RBC_ARRAY_REGION_CASES_H
#define RBC_ARRAY_REGION_CASES_H
static XrLirBlock rbc_region_block(CArrayFixture *c,XrLirFunction function) {
    const XrLirType types[]={XR_LIR_ARRAY,XR_LIR_I64,XR_LIR_I64,XR_LIR_I64};
    XrLirBlock block=test_block(c->module,function,types,4); XrLirValue array={0};
    OK(xr_lir_block_parameter(c->module,block,0,&array));
    OK(xr_lir_set_array_shape(c->module,array,&(XrLirArrayShape){XR_LIR_I64})); return block;
}
static void rbc_region_parameters(CArrayFixture *c,XrLirBlock block,XrLirValue values[4]) {
    for (uint32_t n=0;n<4;++n) OK(xr_lir_block_parameter(c->module,block,n,&values[n]));
}
static XrLirModule *rbc_region_input(XrLirFunction *root,unsigned trips,unsigned loops,unsigned failure,bool observer) {
    CArrayFixture c={.element=XR_LIR_I64}; OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirFunction helper={0};
    if (observer) {
        helper=test_function(c.module,"observer",NULL,0,XR_LIR_UNIT);
        XrLirBlock block=test_block(c.module,helper,NULL,0); OK(xr_lir_set_entry(c.module,helper,block));
        OK(xr_lir_set_terminator(c.module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT}));
    }
    *root=test_function(c.module,"region",NULL,0,XR_LIR_I64);
    XrLirBlock current=test_block(c.module,*root,NULL,0); OK(xr_lir_set_entry(c.module,*root,current));
    XrLirValue result={0};
    for (unsigned loop=0;loop<loops;++loop) {
        XrLirBlock header=rbc_region_block(&c,*root),body=rbc_region_block(&c,*root),exit=rbc_region_block(&c,*root);
        XrLirValue seed=ca_constant(&c,current,7),values[]={
            ca_intrinsic(&c,current,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,&seed,1),ca_constant(&c,current,0),
            ca_constant(&c,current,11),ca_constant(&c,current,22)};
        OK(xr_lir_set_terminator(c.module,current,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
            .edge_count=1,.edges={{header,values,4}}}));
        rbc_region_parameters(&c,header,values);
        (void)ca_intrinsic(&c,header,XR_LIR_ARRAY_OP_LEN,XR_LIR_I64,values,1);
        XrLirValue limit=ca_constant(&c,header,(int64_t)trips),compare[]={values[1],limit};
        XrLirValue condition={0}; OK(xr_lir_emit_value(c.module,header,&(XrLirInstruction){.op=XR_LIR_ICMP,
            .type=XR_LIR_I1,.predicate=XR_LIR_LT,.operands=compare,.operand_count=2},&condition));
        OK(xr_lir_set_terminator(c.module,header,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
            .operands=&condition,.operand_count=1,.edge_count=2,.edges={{body,values,4},{exit,values,4}}}));
        rbc_region_parameters(&c,body,values);
        XrLirValue zero=ca_constant(&c,body,0),get[]={values[0],failure==1 ? values[1] : zero};
        XrLirValue item=ca_intrinsic(&c,body,XR_LIR_ARRAY_OP_GET,XR_LIR_I64,get,2);
        item=ca_binary(&c,body,XR_LIR_ADD,item,ca_constant(&c,body,1));
        XrLirValue cell=ca_value(&c,body,XR_LIR_CELL_NEW,XR_LIR_ARRAY_CELL,values,1);
        ca_effect(&c,body,XR_LIR_ARRAY_DROP,values[0]);
        if (observer) OK(xr_lir_emit_effect(c.module,body,&(XrLirInstruction){.op=XR_LIR_CALL,
            .type=XR_LIR_UNIT,.callee=helper}));
        XrLirValue set[]={cell,failure==2 ? values[1] : zero,item};
        (void)ca_intrinsic(&c,body,XR_LIR_ARRAY_OP_SET,XR_LIR_UNIT,set,3);
        values[0]=ca_value(&c,body,XR_LIR_CELL_READ,XR_LIR_ARRAY,&cell,1); ca_effect(&c,body,XR_LIR_ARRAY_CELL_DROP,cell);
        values[1]=ca_binary(&c,body,XR_LIR_ADD,values[1],ca_constant(&c,body,1));
        XrLirValue swap[]={values[0],values[1],values[3],values[2]};
        OK(xr_lir_set_terminator(c.module,body,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
            .edge_count=1,.edges={{header,swap,4}}}));
        rbc_region_parameters(&c,exit,values); XrLirValue at[]={values[0],ca_constant(&c,exit,0)};
        result=ca_intrinsic(&c,exit,XR_LIR_ARRAY_OP_GET,XR_LIR_I64,at,2); ca_effect(&c,exit,XR_LIR_ARRAY_DROP,values[0]);
        result=ca_binary(&c,exit,XR_LIR_ADD,result,ca_binary(&c,exit,XR_LIR_MUL,values[2],ca_constant(&c,exit,100)));
        result=ca_binary(&c,exit,XR_LIR_ADD,result,values[3]); current=exit;
    }
    ca_return(&c,current,result); OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}
typedef struct RbcRegionResult { XrRbcStatus status; uint64_t bits; uint32_t source; XrXirValueStatus failure; } RbcRegionResult;
static RbcRegionResult rbc_region_run(const XrRbcModule *module,XrtLocationOwner *locations) {
    XrRbcExecution *execution=NULL; RbcRegionResult result={0};
    result.status=xr_rbc_module_prepare(module,0,NULL,0,locations,&execution);
    if (result.status!=XR_RBC_OK) return result;
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution); CHECK(outcome);
    result.status=xr_rbc_outcome_status(outcome);
    if (result.status==XR_RBC_OK) CHECK(xr_rbc_outcome_bits(outcome,&result.bits));
    else if (result.status==XR_RBC_VALUE_FAILURE) {
        const XrtLocationOwner *owner=NULL;
        CHECK(xr_rbc_outcome_value_failure(outcome,&result.failure,&result.source,&owner) && owner);
    }
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome); return result;
}
static void rbc_region_case(unsigned trips,unsigned loops,unsigned failure,bool bypass,bool observer) {
    XrLirFunction root={0}; XrLirModule *input=rbc_region_input(&root,trips,loops,failure,observer);
    XrRbcProgram *program=NULL; CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    XrRbcModule module=*xr_rbc_program_view(program); const XrRbcValueCode *code=xr_rbc_module_value_code(&module,0);
    CHECK(code && (observer ? !code->array_regions : code->array_regions && code->array_regions->region_count==loops));
    if (loops==2) CHECK(code->array_regions->regions[1].first_slot && code->array_regions->regions[1].first_move &&
        code->array_regions->regions[1].first_access);
    CHECK(module.code_lease.retain(module.code_lease.owner));
    XrtLocationOwner *locations=program->locations; CHECK(xrt_locations_retain(locations));
    xr_lir_module_destroy(input); xr_rbc_program_destroy(program);
    region_begin_attempts=region_begin_successes=0; region_force_bypass=bypass;
    RbcRegionResult actual=rbc_region_run(&module,locations);
    size_t expected=observer || trips<2 ? 0 : failure ? 1 : loops;
    CHECK(region_begin_attempts==expected && region_begin_successes==(bypass ? 0 : expected));
    XrRbcFunction *functions=xr_malloc(module.function_count*sizeof(*functions)); CHECK(functions);
    memcpy(functions,module.functions,module.function_count*sizeof(*functions));
    XrRbcValueAccess *accesses=xr_malloc(code->access_count*sizeof(*accesses)); CHECK(accesses);
    memcpy(accesses,code->accesses,code->access_count*sizeof(*accesses));
    for (size_t n=0;n<code->access_count;++n) {
        if (accesses[n].operation==XR_RBC_VALUE_BINDING_NEW || accesses[n].operation==XR_RBC_VALUE_BINDING_READ)
            accesses[n].index=0;
        if (accesses[n].operation==XR_RBC_VALUE_ARRAY) accesses[n].other=0;
    }
    XrRbcValueCode raw_code=*code; raw_code.accesses=accesses;
    const XrRbcValueCode **codes=xr_malloc(module.function_count*sizeof(*codes)); CHECK(codes);
    memcpy(codes,module.borrowed_value_codes,module.function_count*sizeof(*codes)); codes[0]=&raw_code;
    XrRbcModule raw=module; raw.functions=functions; raw.borrowed_value_codes=codes;
    CHECK(!xr_rbc_module_code_owner(&raw));
    region_begin_attempts=region_begin_successes=0; region_force_bypass=false;
    RbcRegionResult ordinary=rbc_region_run(&raw,locations);
    CHECK(!region_begin_attempts && actual.status==ordinary.status && actual.bits==ordinary.bits &&
        actual.failure==ordinary.failure && actual.source==ordinary.source);
    if (!failure) CHECK(actual.status==XR_RBC_OK && actual.bits==(trips%2 ? 2218u : 1129u)+trips);
    else CHECK(actual.status==XR_RBC_VALUE_FAILURE && actual.failure==XR_XIR_VALUE_BOUNDS);
    if (!failure && !observer && trips>1) {
        XrRbcFunction *physical=&functions[0]; uint32_t *words=xr_malloc(physical->code_count*sizeof(*words)); CHECK(words);
        memcpy(words,physical->code,physical->code_count*sizeof(*words)); size_t replaced=0;
        for (size_t pc=0;pc<physical->code_count;++pc) if ((words[pc]&255)==XR_RBC_VALUE_ACCESS) {
            XrRbcValueAccess access=code->accesses[words[pc]>>16];
            if (access.operation==XR_RBC_VALUE_ARRAY && access.index==XR_LIR_ARRAY_OP_SET) {
                CHECK(xr_rbc_encode(&words[pc],(XrRbcOperands){.op=XR_RBC_MOV})); ++replaced;
            }
        }
        CHECK(replaced==loops); physical->code=words;
        ordinary=rbc_region_run(&raw,locations);
        CHECK(!region_begin_attempts && ordinary.status==XR_RBC_OK && ordinary.bits==(trips%2 ? 2218u : 1129u));
        xr_free(words);
    }
    xr_free(codes); xr_free(accesses); xr_free(functions);
    xrt_locations_release(locations); module.code_lease.release(module.code_lease.owner);
    CHECK(!tracked_live);
}
static void rbc_region_failures(void) {
    XrLirFunction root={0}; XrLirModule *input=rbc_region_input(&root,4,1,0,false); XrRbcProgram *program=NULL;
    attempts=0; CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK); size_t sites=attempts;
    xr_rbc_program_destroy(program); program=NULL; CHECK(!tracked_live);
    for (size_t point=1;point<=sites;++point) {
        attempts=0; fail_at=point;
        CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OUT_OF_MEMORY && !program && !tracked_live);
    }
    fail_at=SIZE_MAX; CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK); xr_lir_module_destroy(input);
    size_t baseline=tracked_live; attempts=0;
    CHECK(rbc_region_run(xr_rbc_program_view(program),program->locations).status==XR_RBC_OK); sites=attempts;
    for (size_t point=1;point<=sites;++point) {
        attempts=0; fail_at=point; RbcRegionResult result=rbc_region_run(xr_rbc_program_view(program),program->locations);
        CHECK(result.status==XR_RBC_OUT_OF_MEMORY || (result.status==XR_RBC_VALUE_FAILURE && result.failure==XR_XIR_VALUE_OOM));
        CHECK(tracked_live==baseline);
    }
    fail_at=SIZE_MAX; xr_rbc_program_destroy(program); CHECK(!tracked_live);
}
static void rbc_array_region_cases(void) {
    rbc_region_case(0,1,0,false,false); rbc_region_case(1,1,0,false,false);
    rbc_region_case(4,1,0,false,false); rbc_region_case(5,1,0,false,false);
    rbc_region_case(5,2,0,false,false); rbc_region_case(5,1,0,true,false);
    rbc_region_case(4,1,1,false,false); rbc_region_case(4,1,2,false,false);
    rbc_region_case(4,1,0,false,true); rbc_region_failures();
    RbcArrayRegionState state={.cache={(void *)1,(void *)2,3},.phase=RBC_ARRAY_REGION_ACTIVE};
    RbcValueActivation activation={.array_regions=&state,.active_array_region=1,.frame=(void *)4,.payload=(void *)5};
    rbc_value_array_region_forget(&activation,1,RBC_ARRAY_REGION_BYPASS);
    CHECK(!state.cache.owner && !state.cache.elements && !state.cache.length && !activation.active_array_region);
    puts("Array region: complete first latch, single begin, real homes, scalar cycles, raw CFG and retirement checked");
}
#endif // RBC_ARRAY_REGION_CASES_H
