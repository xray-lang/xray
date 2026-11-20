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
typedef enum RbcRegionMode {
    RBC_REGION_NORMAL, RBC_REGION_READ_ONLY, RBC_REGION_FIXED, RBC_REGION_FIXED_FALSE, RBC_REGION_FIXED_JOINED
} RbcRegionMode;
static XrLirModule *rbc_region_input(XrLirFunction *root,unsigned trips,unsigned loops,unsigned failure,bool observer,RbcRegionMode mode) {
    CArrayFixture c={.element=XR_LIR_I64}; OK(xr_lir_array_module_create(NULL,&c.module));
    bool readonly=mode==RBC_REGION_READ_ONLY, joined=mode==RBC_REGION_FIXED_JOINED;
    bool fixed=mode==RBC_REGION_FIXED || mode==RBC_REGION_FIXED_FALSE || joined;
    bool reverse=mode==RBC_REGION_FIXED_FALSE;
    XrLirFunction helper={0};
    if (observer) {
        helper=test_function(c.module,"observer",NULL,0,XR_LIR_UNIT);
        XrLirBlock block=test_block(c.module,helper,NULL,0); OK(xr_lir_set_entry(c.module,helper,block));
        OK(xr_lir_set_terminator(c.module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT}));
    }
    *root=test_function(c.module,"region",NULL,0,XR_LIR_I64);
    XrLirBlock current=test_block(c.module,*root,NULL,0); OK(xr_lir_set_entry(c.module,*root,current));
    XrLirBlock joined_header={0};
    XrLirValue result={0}, snapshot_cell={0}, initial_index={0};
    for (unsigned loop=0;loop<loops;++loop) {
        XrLirBlock header=joined_header.id ? joined_header : rbc_region_block(&c,*root);
        XrLirBlock body=rbc_region_block(&c,*root),exit_block=rbc_region_block(&c,*root);
        bool next_is_header=joined && loop+1<loops;
        if (!joined_header.id) {
            XrLirValue seed=ca_constant(&c,current,7),seeds[]={seed,seed,seed,seed};
            CHECK(!fixed || trips<=sizeof(seeds)/sizeof(*seeds));
            XrLirValue initial_array=ca_intrinsic(&c,current,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,seeds,
                fixed ? trips : readonly ? 3 : 1);
            initial_index=ca_constant(&c,current,fixed && failure ? -1 : 0);
            XrLirValue initial[]={initial_array,initial_index,ca_constant(&c,current,11),ca_constant(&c,current,22)};
            /* The separate frame Cell keeps a shared snapshot through the loop.
             * The first update detaches the carried owner before it can borrow. */
            if (fixed) snapshot_cell=ca_value(&c,current,XR_LIR_CELL_NEW,XR_LIR_ARRAY_CELL,initial,1);
            OK(xr_lir_set_terminator(c.module,current,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
                .edge_count=1,.edges={{header,initial,4}}}));
        }
        XrLirValue values[4];
        rbc_region_parameters(&c,header,values);
        XrLirValue length=ca_intrinsic(&c,header,XR_LIR_ARRAY_OP_LEN,XR_LIR_I64,values,1);
        XrLirValue limit=readonly || fixed ? length : ca_constant(&c,header,(int64_t)trips),compare[]={values[1],limit};
        XrLirValue condition={0}; OK(xr_lir_emit_value(c.module,header,&(XrLirInstruction){.op=XR_LIR_ICMP,
            .type=XR_LIR_I1,.predicate=reverse ? XR_LIR_GE : XR_LIR_LT,.operands=compare,.operand_count=2},&condition));
        XrLirValue departure[]={values[0],next_is_header ? initial_index : values[1],values[2],values[3]};
        OK(xr_lir_set_terminator(c.module,header,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
            .operands=&condition,.operand_count=1,.edge_count=2,
            .edges={{reverse ? exit_block : body,reverse ? departure : values,4},
                {reverse ? body : exit_block,reverse ? values : departure,4}}}));
        rbc_region_parameters(&c,body,values);
        XrLirValue zero=ca_constant(&c,body,0),get[]={values[0],failure==1 || readonly || fixed ? values[1] : zero};
        XrLirValue item=ca_intrinsic(&c,body,XR_LIR_ARRAY_OP_GET,XR_LIR_I64,get,2);
        item=ca_binary(&c,body,XR_LIR_ADD,item,ca_constant(&c,body,1));
        if (!readonly) {
        XrLirValue cell=ca_value(&c,body,XR_LIR_CELL_NEW,XR_LIR_ARRAY_CELL,values,1);
        ca_effect(&c,body,XR_LIR_ARRAY_DROP,values[0]);
        if (observer) OK(xr_lir_emit_effect(c.module,body,&(XrLirInstruction){.op=XR_LIR_CALL,
            .type=XR_LIR_UNIT,.callee=helper}));
        XrLirValue set[]={cell,failure==2 || fixed ? values[1] : zero,item};
        (void)ca_intrinsic(&c,body,XR_LIR_ARRAY_OP_SET,XR_LIR_UNIT,set,3);
        values[0]=ca_value(&c,body,XR_LIR_CELL_READ,XR_LIR_ARRAY,&cell,1); ca_effect(&c,body,XR_LIR_ARRAY_CELL_DROP,cell);
        }
        values[1]=ca_binary(&c,body,XR_LIR_ADD,values[1],ca_constant(&c,body,1));
        XrLirValue swap[]={values[0],values[1],values[3],values[2]};
        OK(xr_lir_set_terminator(c.module,body,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
            .edge_count=1,.edges={{header,swap,4}}}));
        if (next_is_header) { joined_header=exit_block; continue; }
        rbc_region_parameters(&c,exit_block,values); XrLirValue at[]={values[0],ca_constant(&c,exit_block,0)};
        result=ca_intrinsic(&c,exit_block,fixed && !trips ? XR_LIR_ARRAY_OP_LEN : XR_LIR_ARRAY_OP_GET,
            XR_LIR_I64,at,fixed && !trips ? 1 : 2);
        ca_effect(&c,exit_block,XR_LIR_ARRAY_DROP,values[0]);
        if (fixed) {
            XrLirValue snapshot=ca_value(&c,exit_block,XR_LIR_CELL_READ,XR_LIR_ARRAY,&snapshot_cell,1);
            ca_effect(&c,exit_block,XR_LIR_ARRAY_CELL_DROP,snapshot_cell);
            at[0]=snapshot;
            XrLirValue original=ca_intrinsic(&c,exit_block,trips ? XR_LIR_ARRAY_OP_GET : XR_LIR_ARRAY_OP_LEN,
                XR_LIR_I64,at,trips ? 2 : 1);
            result=ca_binary(&c,exit_block,XR_LIR_ADD,result,original);
            ca_effect(&c,exit_block,XR_LIR_ARRAY_DROP,snapshot);
        }
        result=ca_binary(&c,exit_block,XR_LIR_ADD,result,ca_binary(&c,exit_block,XR_LIR_MUL,values[2],ca_constant(&c,exit_block,100)));
        result=ca_binary(&c,exit_block,XR_LIR_ADD,result,values[3]); current=exit_block;
    }
    ca_return(&c,current,result); OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}
typedef struct RbcRegionResult { XrRbcStatus status; uint64_t bits; uint32_t source; XrXirValueStatus failure; } RbcRegionResult;
static void rbc_region_roles(const XrRbcModule *module,const XrRbcValueCode *code,unsigned loops,uint32_t supported) {
    if (!supported) { CHECK(!code->array_region_accesses); return; }
    const XrLirArrayRegionBindings *bindings=code->array_regions;
    CHECK(bindings && bindings->slot_count && code->array_region_accesses);
    uint32_t seen=0;
    for (size_t n=0;n<code->access_count;++n) {
        const XrRbcArrayRegionAccess *role=&code->array_region_accesses[n];
        XrRbcValueAccess access=code->accesses[n];
        if (access.operation==XR_RBC_VALUE_EDGE && access.index!=UINT32_MAX)
            CHECK(role->region==bindings->edges[access.index].region);
        if (!role->region) {
            CHECK(!role->operation.access && !role->operation.move && !role->operation.drop); continue;
        }
        CHECK(role->region<=loops && bindings->regions[role->region-1].slot_count);
        seen|=1u<<(role->region-1);
        if (role->operation.move) {
            CHECK(role->operation.move<=bindings->move_count);
            const XrLirArrayRegionMove *move=&bindings->moves[role->operation.move-1];
            CHECK((access.operation==XR_RBC_VALUE_BINDING_NEW || access.operation==XR_RBC_VALUE_BINDING_READ) &&
                access.source==move->step.source && access.result==move->step.target);
        }
        if (role->operation.drop) {
            CHECK(role->operation.drop<=bindings->move_count);
            CHECK((access.operation==XR_RBC_VALUE_DROP || access.operation==XR_RBC_VALUE_BINDING_DROP) &&
                access.source==bindings->moves[role->operation.drop-1].step.source);
        }
        if (role->operation.access) {
            CHECK(role->operation.access<=bindings->access_count);
            const XrLirArrayRegionAccess *shared=&bindings->accesses[role->operation.access-1];
            CHECK(access.operation==XR_RBC_VALUE_ARRAY && access.index==(uint32_t)shared->operation &&
                access.result==shared->result && code->arguments[access.first]==shared->receiver);
        }
    }
    CHECK(seen==supported);
    const XrRbcFunction *function=&module->functions[0];
    for (size_t pc=0;pc<function->code_count;++pc) if ((function->code[pc]&255)==XR_RBC_VALUE_ACCESS)
        CHECK((function->code[pc]>>16)<code->access_count);
}
static RbcRegionResult rbc_region_run_with_args(const XrRbcModule *module,XrtLocationOwner *locations,
    const uint64_t *arguments,size_t count) {
    XrRbcExecution *execution=NULL; RbcRegionResult result={0};
    result.status=xr_rbc_module_prepare(module,0,arguments,count,locations,&execution);
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
static RbcRegionResult rbc_region_run(const XrRbcModule *module,XrtLocationOwner *locations) {
    return rbc_region_run_with_args(module,locations,NULL,0);
}
static void rbc_fixed_region_case(unsigned trips, unsigned failure, RbcRegionMode mode) {
    XrLirFunction root={0};
    XrLirModule *input=rbc_region_input(&root,trips,1,failure,false,mode);
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(input);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    CHECK(code && !code->array_regions && code->array_region_runtime && code->homes->fixed_array_update_count==1);
    CHECK(code->homes->fixed_array_updates[0].body_edge==(mode==RBC_REGION_FIXED_FALSE ? 2u : 1u));
    uint32_t markers[XR_LIR_ARRAY_REGION_EDGE_EXIT+1]={0};
    for (size_t n=0;n<code->access_count;++n) {
        const XrRbcArrayRegionAccess *role=&code->array_region_accesses[n];
        if (!role->fixed_edge) continue;
        CHECK(role->fixed && role->region==1 && role->fixed_edge<=XR_LIR_ARRAY_REGION_EDGE_EXIT);
        const XrRbcValueAccess *access=&code->accesses[n];
        CHECK(access->operation==XR_RBC_VALUE_EDGE && access->index==UINT32_MAX && !access->count);
        ++markers[role->fixed_edge];
    }
    for (size_t n=1;n<sizeof(markers)/sizeof(*markers);++n) CHECK(markers[n]==1);
    size_t expected=!failure && trips>1;
    region_begin_attempts=region_begin_successes=0; region_force_bypass=false;
    RbcRegionResult actual=rbc_region_run(module,program->locations);
    CHECK(region_begin_attempts==expected && region_begin_successes==expected);
    region_begin_attempts=region_begin_successes=0; region_force_bypass=true;
    RbcRegionResult ordinary=rbc_region_run(module,program->locations);
    CHECK(region_begin_attempts==expected && !region_begin_successes && actual.status==ordinary.status &&
        actual.bits==ordinary.bits && actual.failure==ordinary.failure && actual.source==ordinary.source);
    if (failure) CHECK(actual.status==XR_RBC_VALUE_FAILURE && actual.failure==XR_XIR_VALUE_BOUNDS);
    else CHECK(actual.status==XR_RBC_OK && actual.bits==(trips%2 ? 2211u : 1122u)+(trips ? 15u : 0u));
    XrRbcFunction physical=module->functions[0];
    XrRbcModule raw=*module; raw.functions=&physical;
    CHECK(!xr_rbc_module_code_owner(&raw));
    region_begin_attempts=region_begin_successes=0; region_force_bypass=false;
    CHECK(rbc_region_run(&raw,program->locations).status==XR_RBC_INVALID);
    CHECK(!region_begin_attempts && !region_begin_successes);
    xr_rbc_program_destroy(program);
}
static void rbc_fixed_adjacent_regions(void) {
    XrLirFunction root={0};
    XrLirModule *input=rbc_region_input(&root,4,2,0,false,RBC_REGION_FIXED_JOINED);
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(input);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    CHECK(code && code->array_region_runtime && code->homes->fixed_array_update_count==2);
    const XrLirArraySetRegion *first=&code->homes->fixed_array_updates[0].region;
    const XrLirArraySetRegion *second=&code->homes->fixed_array_updates[1].region;
    CHECK(first->exit_block==second->header && first->header==second->entry_block && first->exit_edge==second->entry_edge);
    size_t adjacent=0;
    for (size_t n=1;n<code->access_count;++n) {
        const XrRbcArrayRegionAccess *from=&code->array_region_accesses[n-1], *to=&code->array_region_accesses[n];
        adjacent+=from->fixed && from->region==1 && from->fixed_edge==XR_LIR_ARRAY_REGION_EDGE_EXIT &&
            to->fixed && to->region==2 && to->fixed_edge==XR_LIR_ARRAY_REGION_EDGE_ENTRY;
    }
    CHECK(adjacent==1);
    region_begin_attempts=region_begin_successes=0; region_force_bypass=false;
    RbcRegionResult actual=rbc_region_run(module,program->locations);
    CHECK(region_begin_attempts==2 && region_begin_successes==2);
    CHECK(actual.status==XR_RBC_OK && actual.bits==1138);
    region_begin_attempts=region_begin_successes=0; region_force_bypass=true;
    RbcRegionResult ordinary=rbc_region_run(module,program->locations);
    CHECK(region_begin_attempts==2 && !region_begin_successes);
    CHECK(ordinary.status==actual.status && ordinary.bits==actual.bits);
    region_force_bypass=false;
    xr_rbc_program_destroy(program);
}
static void rbc_region_case(unsigned trips,unsigned loops,unsigned failure,bool bypass,bool observer) {
    XrLirFunction root={0}; XrLirModule *input=rbc_region_input(&root,trips,loops,failure,observer,RBC_REGION_NORMAL);
    XrRbcProgram *program=NULL; CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    XrRbcModule module=*xr_rbc_program_view(program); const XrRbcValueCode *code=xr_rbc_module_value_code(&module,0);
    CHECK(code && (observer ? !code->array_regions : code->array_regions && code->array_regions->region_count==loops));
    if (loops==2) CHECK(code->array_regions->regions[1].first_slot && code->array_regions->regions[1].first_move &&
        code->array_regions->regions[1].first_access);
    CHECK(module.code_lease.retain(module.code_lease.owner));
    XrtLocationOwner *locations=program->locations; CHECK(xrt_locations_retain(locations));
    xr_lir_module_destroy(input); xr_rbc_program_destroy(program);
    rbc_region_roles(&module,code,loops,observer ? 0 : (1u<<loops)-1);
    region_begin_attempts=region_begin_successes=0; region_force_bypass=bypass;
    RbcRegionResult actual=rbc_region_run(&module,locations);
    size_t expected=observer || trips<2 ? 0 : failure ? 1 : loops;
    CHECK(region_begin_attempts==expected && region_begin_successes==(bypass ? 0 : expected));
    region_begin_attempts=region_begin_successes=0; region_force_bypass=true;
    RbcRegionResult ordinary=rbc_region_run(&module,locations);
    CHECK(region_begin_attempts==expected && !region_begin_successes && actual.status==ordinary.status &&
        actual.bits==ordinary.bits && actual.failure==ordinary.failure && actual.source==ordinary.source);
    if (!failure) CHECK(actual.status==XR_RBC_OK && actual.bits==(trips%2 ? 2218u : 1129u)+trips);
    else CHECK(actual.status==XR_RBC_VALUE_FAILURE && actual.failure==XR_XIR_VALUE_BOUNDS);
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
    /* Owned Frame homes are outside the existing raw admission boundary,
     * even after old private hints are cleared or the new roles are copied. */
    RbcRegionResult rejected=rbc_region_run(&raw,locations);
    CHECK(rejected.status==XR_RBC_INVALID && !region_begin_attempts && !region_begin_successes);
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
        rejected=rbc_region_run(&raw,locations);
        CHECK(rejected.status==XR_RBC_INVALID && !region_begin_attempts && !region_begin_successes);
        xr_free(words);
    }
    xr_free(codes); xr_free(accesses); xr_free(functions);
    xrt_locations_release(locations); module.code_lease.release(module.code_lease.owner);
    CHECK(!tracked_live);
}
static void rbc_region_role_fallbacks(void) {
    XrLirFunction root={0}; XrLirModule *input=rbc_region_input(&root,4,2,0,false,RBC_REGION_NORMAL); XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0); const XrLirValueHomes *homes=code->homes;
    XrRbcCompileDiagnostic diagnostic={0}; RbcLirCompiler collector={.input=input,.diagnostic=&diagnostic};
    RbcHomeVector roots={0}; CHECK(rbc_home_roots(&collector,NULL,&roots,&root.id,1));
    XrtBillingOwner *billing=NULL; CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&billing)==XRT_MANAGED_OK);
    XrLirValueLayouts *layouts=NULL;
    CHECK(xr_lir_value_layouts_create(input,roots.data,roots.count,billing,&layouts)==XR_LIR_VALUE_LAYOUT_OK);
    xr_free(roots.data);
    size_t bytes=((size_t)homes->value_count+1)*sizeof(*homes->values);
    XrLirValueHome *values=xr_malloc(bytes); CHECK(values);
    for (unsigned shape=0;shape<3;++shape) {
        memcpy(values,homes->values,bytes); XrLirValueHomes view=*homes; view.values=values;
        if (!shape) view.frame.payload_alignment=1;
        else values[homes->array_regions[shape-1].canonical_value].parameter=1;
        XrLirArrayRegionBindings *bindings=NULL;
        CHECK(xr_lir_array_region_bindings_create(&view,layouts,&bindings)==XR_LIR_VALUE_LAYOUT_OK);
        uint32_t supported=shape ? 3u&~(1u<<(shape-1)) : 0;
        CHECK(bindings->region_count==2 && (bindings->slot_count!=0)==(supported!=0));
        XrRbcValueCode projected=*code; projected.array_regions=bindings; projected.array_region_accesses=NULL;
        projected.array_region_runtime=rbc_lir_array_region_uses_runtime(bindings);
        CHECK(projected.array_region_runtime==(supported!=0));
        projected.access_count=0; RbcLirCompiler builder={.value_code=&projected,.diagnostic=&diagnostic};
        for (size_t n=0;n<code->access_count;++n) {
            const XrLirArrayRegionOperation *operation=&code->array_region_accesses[n].operation;
            diagnostic.block=diagnostic.instruction=0;
            if (operation->access) {
                const XrLirArrayRegionAccess *access=&code->array_regions->accesses[operation->access-1];
                diagnostic.block=access->block; diagnostic.instruction=access->instruction;
            } else if (operation->move || operation->drop) {
                const XrLirArraySetRegionStep *step=&code->array_regions->moves[
                    (operation->move ? operation->move : operation->drop)-1].step;
                diagnostic.block=step->block; diagnostic.instruction=operation->move ? step->instruction : step->drop;
            }
            CHECK(rbc_lir_array_region_append(&builder,&code->accesses[n])); ++projected.access_count;
        }
        rbc_region_roles(module,&projected,2,supported);
        xr_free((void *)projected.array_region_accesses); xr_lir_array_region_bindings_destroy(bindings);
    }
    xr_free(values); xr_lir_value_layouts_destroy(layouts); xrt_billing_owner_drop(billing);
    xr_lir_module_destroy(input); xr_rbc_program_destroy(program); CHECK(!tracked_live);
}
static void rbc_region_failures(unsigned loops) {
    XrLirFunction root={0}; XrLirModule *input=rbc_region_input(&root,4,loops,0,false,RBC_REGION_NORMAL); XrRbcProgram *program=NULL;
    attempts=0; CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK); size_t sites=attempts;
    const XrRbcValueCode *code=xr_rbc_module_value_code(xr_rbc_program_view(program),0);
    CHECK(code && code->array_region_accesses && (loops==1 || code->access_count>16));
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
static XrLirBlock rbc_read_fold_block(CArrayFixture *c,XrLirFunction function,XrLirValue values[3]) {
    const XrLirType types[]={XR_LIR_ARRAY,XR_LIR_I64,XR_LIR_I64};
    XrLirBlock block=test_block(c->module,function,types,3);
    for (uint32_t n=0;n<3;++n) OK(xr_lir_block_parameter(c->module,block,n,&values[n]));
    OK(xr_lir_set_array_shape(c->module,values[0],&(XrLirArrayShape){XR_LIR_I64}));
    return block;
}

typedef enum RbcReadFoldBound {
    RBC_READ_FOLD_FORMAL, RBC_READ_FOLD_IMMEDIATE, RBC_READ_FOLD_LARGE_LITERAL, RBC_READ_FOLD_PAST_END_LITERAL
} RbcReadFoldBound;

static XrLirModule *rbc_read_fold_input(XrLirFunction *root,bool additive,RbcReadFoldBound mode) {
    CArrayFixture c={.element=XR_LIR_I64};
    OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirType argument=XR_LIR_I64;
    *root=test_function(c.module,"read_fold",&argument,1,XR_LIR_I64);
    XrLirValue bound={0},head[3],next[3],done[3];
    OK(xr_lir_function_parameter(c.module,*root,0,&bound));
    XrLirBlock entry=test_block(c.module,*root,NULL,0);
    XrLirBlock header=rbc_read_fold_block(&c,*root,head);
    XrLirBlock body=rbc_read_fold_block(&c,*root,next);
    XrLirBlock exit_block=rbc_read_fold_block(&c,*root,done);
    OK(xr_lir_set_entry(c.module,*root,entry));
    XrLirValue largest=ca_constant(&c,entry,INT64_MAX);
    bool large=mode>=RBC_READ_FOLD_LARGE_LITERAL;
    XrLirValue array;
    if (large) {
        XrLirValue items[]={ca_constant(&c,entry,mode==RBC_READ_FOLD_LARGE_LITERAL ? 32768 : 32767),
            ca_constant(&c,entry,7)};
        array=ca_intrinsic(&c,entry,XR_LIR_ARRAY_OP_REPEAT,XR_LIR_ARRAY,items,2);
    } else {
        XrLirValue items[]={largest,ca_constant(&c,entry,-1),ca_constant(&c,entry,7)};
        array=ca_intrinsic(&c,entry,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,items,3);
    }
    XrLirValue initial[]={array,ca_constant(&c,entry,large ? 32765 : 0),largest};
    if (mode==RBC_READ_FOLD_IMMEDIATE) bound=ca_constant(&c,entry,3);
    OK(xr_lir_set_terminator(c.module,entry,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{header,initial,3}}}));
    if (large) bound=ca_constant(&c,header,32768);
    XrLirValue compare[]={head[1],bound},condition={0};
    OK(xr_lir_emit_value(c.module,header,&(XrLirInstruction){.op=XR_LIR_ICMP,
        .type=XR_LIR_I1,.predicate=XR_LIR_LT,.operands=compare,.operand_count=2},&condition));
    OK(xr_lir_set_terminator(c.module,header,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&condition,.operand_count=1,.edge_count=2,.edges={{body,head,3},{exit_block,head,3}}}));
    XrLirValue item={0};
    uint32_t get=0;
    OK(xr_lir_array_operation(c.module,XR_LIR_I64,XR_LIR_ARRAY_OP_GET,&get));
    OK(xr_lir_emit_value(c.module,body,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=XR_LIR_I64,.intrinsic=get,.operands=next,.operand_count=2,
        .source={.path="window.xr",.path_length=9,.line=7,.column=9,.end_line=7,.end_column=9}},&item));
    /* Commuted addition exercises the semantic matcher. Subtraction is pure
     * and still range-proved, but it must retain the ordinary bytecode. */
    next[2]=ca_binary(&c,body,additive ? XR_LIR_ADD : XR_LIR_SUB,item,next[2]);
    next[1]=ca_binary(&c,body,XR_LIR_ADD,next[1],ca_constant(&c,body,1));
    OK(xr_lir_set_terminator(c.module,body,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{header,next,3}}}));
    ca_effect(&c,exit_block,XR_LIR_ARRAY_DROP,done[0]);
    ca_return(&c,exit_block,done[2]);
    OK(xr_lir_module_finish(c.module,NULL));
    return c.module;
}

static void rbc_read_fold_cases(bool additive,RbcReadFoldBound mode) {
    XrLirFunction root={0}; XrLirModule *input=rbc_read_fold_input(&root,additive,mode);
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(input);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    CHECK(code && code->array_regions && code->array_regions->region_count==1);
    CHECK((code->array_read_folds!=NULL)==additive);
    size_t marked=0;
    for (size_t n=0;n<code->access_count;++n)
        marked+=code->accesses[n].operation==XR_RBC_VALUE_EDGE && code->accesses[n].other!=0;
    CHECK(marked==(additive ? 2u : 0u));
    if (mode==RBC_READ_FOLD_IMMEDIATE) {
        /* A preheader constant used solely by the compare never writes its
         * nominal register. The fold must use the same exact immediate bits. */
        CHECK(code->array_read_folds[0].constant && !code->array_read_folds[0].limit &&
            code->array_read_folds[0].limit_bits==3);
        const XrRbcFunction *function=&module->functions[0];
        size_t immediate_compares=0;
        for (size_t pc=0;pc<function->code_count;++pc) {
            uint32_t word=function->code[pc];
            if ((word&255)==XR_RBC_LTI && (word>>16)==3) ++immediate_compares;
            CHECK((word&255)!=XR_RBC_LOADI || (word>>16)!=3);
        }
        CHECK(immediate_compares==1);
    }
    bool large=mode>=RBC_READ_FOLD_LARGE_LITERAL;
    if (large) CHECK(code->array_read_folds[0].constant && !code->array_read_folds[0].limit &&
        code->array_read_folds[0].limit_bits==32768);
    const uint64_t bounds[]={UINT64_MAX,0,1,2,3,4};
    size_t count=mode==RBC_READ_FOLD_FORMAL ? sizeof(bounds)/sizeof(bounds[0]) : 1u;
    for (size_t n=0;n<count;++n) {
        uint64_t bound=large ? 32768u : mode==RBC_READ_FOLD_IMMEDIATE ? 3u : bounds[n];
        size_t expects=additive && bound>1 && bound<=INT64_MAX ? 1u : 0u;
        region_begin_attempts=region_begin_successes=0; region_force_bypass=false;
        RbcRegionResult actual=rbc_region_run_with_args(module,program->locations,&bound,1);
        CHECK(region_begin_attempts==expects && region_begin_successes==expects);
        region_begin_attempts=region_begin_successes=0; region_force_bypass=true;
        RbcRegionResult ordinary=rbc_region_run_with_args(module,program->locations,&bound,1);
        CHECK(region_begin_attempts==expects && !region_begin_successes);
        CHECK(actual.status==ordinary.status && actual.bits==ordinary.bits &&
            actual.failure==ordinary.failure && actual.source==ordinary.source);
        if (bound==4 || mode==RBC_READ_FOLD_PAST_END_LITERAL)
            CHECK(actual.status==XR_RBC_VALUE_FAILURE && actual.failure==XR_XIR_VALUE_BOUNDS && actual.source);
        else CHECK(actual.status==XR_RBC_OK);
        if (bound==3) CHECK(actual.bits==(additive ? 4u : 8u));
        if (mode==RBC_READ_FOLD_LARGE_LITERAL) CHECK(actual.bits==(uint64_t)INT64_MAX+21);
    }
    region_force_bypass=false;
    xr_rbc_program_destroy(program);
    CHECK(!tracked_live);
}

static void rbc_push_region_case(enum ArrayPushWindowMode mode,uint64_t bound,size_t trips,size_t normal_pushes,
    size_t attempts_if_fast,bool eligible) {
    ArraySetRegionFixture fixture=array_push_window_build(mode);
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(fixture.module,fixture.function,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(fixture.module);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    CHECK(code && (code->array_push_limits!=NULL)==eligible);
    if (mode==PUSH_WINDOW_LARGE_END)
        CHECK(!code->array_push_limits[0].value && code->array_push_limits[0].bits==1000000);
    size_t tags=0;
    for (size_t n=0;n<code->access_count;++n) {
        XrRbcValueAccess access=code->accesses[n];
        if (access.operation!=XR_RBC_VALUE_EDGE || !access.window) continue;
        ++tags;
        CHECK(access.window==1 && !access.other);
        XrRbcModule raw=*module;
        memset(&raw.code_lease,0,sizeof(raw.code_lease));
        CHECK(!rbc_value_array_push_edge_valid(&raw,code,&module->functions[0],access));
    }
    CHECK(tags==(eligible ? 2u : 0u));
    region_begin_attempts=region_begin_successes=array_pushes=array_push_transactions=0;
    region_force_bypass=false;
    RbcRegionResult actual=rbc_region_run_with_args(module,program->locations,&bound,1);
    CHECK(actual.status==XR_RBC_OK && actual.bits==trips+1 && array_pushes==normal_pushes &&
        region_begin_attempts==attempts_if_fast && region_begin_successes==attempts_if_fast);
    region_begin_attempts=region_begin_successes=array_pushes=array_push_transactions=0;
    region_force_bypass=true;
    RbcRegionResult ordinary=rbc_region_run_with_args(module,program->locations,&bound,1);
    CHECK(ordinary.status==actual.status && ordinary.bits==actual.bits && array_pushes==trips &&
        region_begin_attempts==(attempts_if_fast ? 1u : 0u) && !region_begin_successes);
    if (mode==PUSH_WINDOW_PARAMETER_END && bound==9) {
        RbcRegionResult failure[2];
        for (unsigned bypass=0;bypass<2;++bypass) {
            array_push_fault=3;
            region_begin_attempts=region_begin_successes=array_pushes=array_push_transactions=failed_array_length=0;
            region_force_bypass=bypass!=0;
            size_t live=tracked_live;
            failure[bypass]=rbc_region_run_with_args(module,program->locations,&bound,1);
            CHECK(tracked_live==live && failed_array_length==8 && array_push_transactions==3 &&
                array_pushes==(bypass ? 8u : 3u));
            CHECK(failure[bypass].status==XR_RBC_VALUE_FAILURE && failure[bypass].failure==XR_XIR_VALUE_OOM);
        }
        CHECK(failure[0].source && failure[0].source==failure[1].source);
        array_push_fault=0;
    }
    region_force_bypass=false;
    xr_rbc_program_destroy(program);
    CHECK(!tracked_live);
}

static void rbc_array_region_cases(void) {
    rbc_push_region_case(PUSH_WINDOW_PARAMETER_END,0,0,0,0,true);
    rbc_push_region_case(PUSH_WINDOW_PARAMETER_END,1,1,1,0,true);
    rbc_push_region_case(PUSH_WINDOW_PARAMETER_END,3,3,1,1,true);
    rbc_push_region_case(PUSH_WINDOW_PARAMETER_END,7,7,2,2,true);
    rbc_push_region_case(PUSH_WINDOW_PARAMETER_END,9,9,3,3,true);
    rbc_push_region_case(PUSH_WINDOW_CURRENT,0,9,3,3,true);
    rbc_push_region_case(PUSH_WINDOW_LARGE_END,0,9,3,3,true);
    rbc_push_region_case(PUSH_WINDOW_PRECOMPUTED_END,7,9,3,3,true);
    rbc_push_region_case(PUSH_WINDOW_LIVE_SNAPSHOT,0,9,9,0,false);
    rbc_push_region_case(PUSH_WINDOW_NEXT_ITEM,0,9,9,0,false);
    rbc_push_region_case(PUSH_WINDOW_OBSERVER,0,9,9,0,false);
    rbc_read_fold_cases(true,RBC_READ_FOLD_FORMAL);
    rbc_read_fold_cases(false,RBC_READ_FOLD_FORMAL);
    rbc_read_fold_cases(true,RBC_READ_FOLD_IMMEDIATE);
    rbc_read_fold_cases(true,RBC_READ_FOLD_LARGE_LITERAL);
    rbc_read_fold_cases(true,RBC_READ_FOLD_PAST_END_LITERAL);
    {
        XrLirFunction root={0}; XrLirModule *input=rbc_region_input(&root,0,1,0,false,RBC_REGION_READ_ONLY);
        XrRbcProgram *program=NULL; CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
        const XrRbcModule *module=xr_rbc_program_view(program); const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
        CHECK(code && code->array_regions && code->array_regions->region_count==1 &&
            code->array_regions->regions[0].region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET &&
            !code->array_region_runtime && !code->array_region_accesses);
        xr_lir_module_destroy(input);
        region_begin_attempts=region_begin_successes=0; region_force_bypass=false;
        RbcRegionResult result=rbc_region_run(module,program->locations);
        CHECK(result.status==XR_RBC_OK && result.bits==2218 && !region_begin_attempts && !region_begin_successes);
        xr_rbc_program_destroy(program);
    }
    rbc_region_case(0,1,0,false,false); rbc_region_case(1,1,0,false,false);
    rbc_region_case(4,1,0,false,false); rbc_region_case(5,1,0,false,false);
    rbc_region_case(5,2,0,false,false); rbc_region_case(5,1,0,true,false);
    rbc_region_case(4,1,1,false,false); rbc_region_case(4,1,2,false,false);
    rbc_region_case(4,1,0,false,true); rbc_region_role_fallbacks();
    rbc_region_failures(1); rbc_region_failures(2);
    rbc_fixed_region_case(0,0,RBC_REGION_FIXED);
    rbc_fixed_region_case(1,0,RBC_REGION_FIXED);
    rbc_fixed_region_case(4,0,RBC_REGION_FIXED);
    rbc_fixed_region_case(4,0,RBC_REGION_FIXED_FALSE);
    rbc_fixed_region_case(4,1,RBC_REGION_FIXED_FALSE);
    rbc_fixed_adjacent_regions();
    RbcArrayRegionState state={.cache={(void *)1,(void *)2,3},.phase=RBC_ARRAY_REGION_ACTIVE};
    RbcValueActivation activation={.array_regions=&state,.active_array_region=1,.frame=(void *)4,.payload=(void *)5};
    rbc_value_array_region_forget(&activation,1,RBC_ARRAY_REGION_BYPASS);
    CHECK(!state.cache.owner && !state.cache.elements && !state.cache.length && !activation.active_array_region);
    puts("Array region: complete first latch, single begin, real homes, scalar cycles, raw CFG and retirement checked");
}
#endif // RBC_ARRAY_REGION_CASES_H
