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
static void rbc_region_roles(const XrRbcModule *module,const XrRbcValueCode *code,unsigned loops) {
    CHECK(loops<=2);
    if (!loops) { CHECK(!code->array_region_accesses); return; }
    CHECK(code->homes->fixed_array_update_count==loops && code->array_region_accesses);
    uint32_t roles[2][XR_LIR_ARRAY_REGION_EDGE_EXIT+1]={0}, writes[2]={0};
    for (size_t n=0;n<code->access_count;++n) {
        const XrRbcArrayRegionAccess *role=&code->array_region_accesses[n];
        XrRbcValueAccess access=code->accesses[n];
        if (access.operation==XR_RBC_VALUE_EDGE) CHECK(!access.other && !access.window);
        if (!role->region) continue;
        CHECK(role->region<=loops);
        uint32_t slot=role->region-1;
        if (role->fixed_edge) {
            CHECK(role->fixed_edge<=XR_LIR_ARRAY_REGION_EDGE_EXIT && access.operation==XR_RBC_VALUE_EDGE &&
                access.index==UINT32_MAX && !access.count);
            ++roles[slot][role->fixed_edge];
        } else if (access.operation==XR_RBC_VALUE_ARRAY && access.index==XR_LIR_ARRAY_OP_SET) {
            const XrLirFixedArrayUpdateHome *row=&code->homes->fixed_array_updates[slot];
            CHECK(code->homes->bindings[code->arguments[access.first]].first_owner==row->owner_slot);
            ++writes[slot];
        } else if (role->binding_identity)
            CHECK(rbc_value_binding_identity(code,access));
    }
    for (unsigned n=0;n<loops;++n) {
        CHECK(writes[n]==1);
        for (size_t role=1;role<sizeof(roles[0])/sizeof(*roles[0]);++role) CHECK(roles[n][role]==1);
    }
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
static void rbc_fixed_identity_words(const XrRbcModule *module, const XrRbcValueCode *code) {
    const XrRbcFunction *function = &module->functions[0];
    uint32_t *words = (uint32_t *)function->code;
    size_t pairs = 0;
    for (size_t pc = 0; pc < function->code_count; ++pc) {
        uint32_t word = words[pc];
        if ((word & 255u) != XR_RBC_VALUE_ACCESS) continue;
        size_t ordinal = word >> 16;
        XrRbcArrayRegionAccess *role = (XrRbcArrayRegionAccess *)&code->array_region_accesses[ordinal];
        if (!role->binding_identity) continue;
        CHECK(xr_rbc_value_instruction_valid(module,function,pc));
        const XrRbcValueAccess *access = &code->accesses[ordinal];
        XrLirHomeEvent *event = (XrLirHomeEvent *)&code->homes->events[access->index - 1];
        CHECK(event->forwarding_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE);
        event->forwarding_mode = XR_LIR_OWNERSHIP_TRANSFER_NONE;
        CHECK(!xr_rbc_value_instruction_valid(module,function,pc));
        event->forwarding_mode = XR_LIR_OWNERSHIP_TRANSFER_MOVE;

        uint32_t drop = words[pc + 1];
        words[pc + 1] = XR_RBC_LOADI;
        CHECK(!xr_rbc_value_instruction_valid(module,function,pc));
        words[pc + 1] = drop;
        uint32_t region=role->region;
        role->region=0;
        CHECK(!xr_rbc_value_instruction_valid(module,function,pc));
        role->region=region;
        CHECK(xr_rbc_value_instruction_valid(module,function,pc));
        ++pairs;
    }
    CHECK(pairs == 2);
}

static void rbc_fixed_region_case(unsigned trips, unsigned failure, RbcRegionMode mode) {
    XrLirFunction root={0};
    XrLirModule *input=rbc_region_input(&root,trips,1,failure,false,mode);
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(input);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    CHECK(code && code->array_region_runtime && code->homes->fixed_array_update_count==1);
    CHECK(code->homes->fixed_array_updates[0].body_edge==(mode==RBC_REGION_FIXED_FALSE ? 2u : 1u));
    uint32_t markers[XR_LIR_ARRAY_REGION_EDGE_EXIT+1]={0};
    for (size_t n=0;n<code->access_count;++n) {
        const XrRbcArrayRegionAccess *role=&code->array_region_accesses[n];
        if (!role->fixed_edge) continue;
        CHECK(role->region==1 && role->fixed_edge<=XR_LIR_ARRAY_REGION_EDGE_EXIT);
        const XrRbcValueAccess *access=&code->accesses[n];
        CHECK(access->operation==XR_RBC_VALUE_EDGE && access->index==UINT32_MAX && !access->count);
        ++markers[role->fixed_edge];
    }
    for (size_t n=1;n<sizeof(markers)/sizeof(*markers);++n) CHECK(markers[n]==1);
    if (trips == 4 && !failure && mode == RBC_REGION_FIXED) rbc_fixed_identity_words(module,code);
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
        adjacent+=from->region==1 && from->fixed_edge==XR_LIR_ARRAY_REGION_EDGE_EXIT &&
            to->region==2 && to->fixed_edge==XR_LIR_ARRAY_REGION_EDGE_ENTRY;
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
static void rbc_fixed_array_leaf_locations(void) {
    enum { ARRAY=1, INDEX, ITEM, RESULT };
    const uint32_t args[]={ARRAY,INDEX,ITEM};
    const XrRbcValueLocation locations[]={
        [INDEX]={XR_RBC_VALUE_SPILL,0}, [ITEM]={XR_RBC_VALUE_REGISTER,0}, [RESULT]={XR_RBC_VALUE_REGISTER,1}
    };
    const XrRbcValueAccess accesses[]={
        {.operation=XR_RBC_VALUE_ARRAY,.result=RESULT,.index=XR_LIR_ARRAY_OP_LEN,.count=1},
        {.operation=XR_RBC_VALUE_ARRAY,.result=RESULT,.index=XR_LIR_ARRAY_OP_GET,.count=2},
        {.operation=XR_RBC_VALUE_ARRAY,.index=XR_LIR_ARRAY_OP_SET,.count=3},
        {.operation=XR_RBC_VALUE_EDGE,.index=UINT32_MAX},
        {.operation=XR_RBC_VALUE_EDGE,.index=UINT32_MAX},
        {.operation=XR_RBC_VALUE_EDGE,.index=UINT32_MAX},
        {.operation=XR_RBC_VALUE_EDGE,.index=UINT32_MAX},
        {.operation=XR_RBC_VALUE_EDGE,.index=UINT32_MAX},
        {.operation=XR_RBC_VALUE_BINDING_READ},
        {.operation=XR_RBC_VALUE_BINDING_NEW},
        {.operation=XR_RBC_VALUE_DROP}
    };
    const XrRbcArrayRegionAccess roles[]={
        {.region=1}, {.region=1}, {.region=1},
        {.region=1,.fixed_edge=XR_LIR_ARRAY_REGION_EDGE_BODY},
        {.region=1,.fixed_edge=XR_LIR_ARRAY_REGION_EDGE_LATCH},
        {.region=1,.fixed_edge=XR_LIR_ARRAY_REGION_EDGE_ENTRY},
        {.region=1,.fixed_edge=XR_LIR_ARRAY_REGION_EDGE_EXIT},
        {.region=2,.fixed_edge=XR_LIR_ARRAY_REGION_EDGE_BODY}, {.region=1},
        {.region=1,.binding_identity=true}, {0}
    };
    const XrRbcValueCode code={.locations=locations,.value_count=RESULT,.arguments=args,.argument_count=3,
        .accesses=accesses,.access_count=sizeof(accesses)/sizeof(accesses[0]),.array_region_accesses=roles};
    XrtManagedHeader owner={0};
    int64_t elements[]={7,11,13};
    const RbcArrayRegionState ready={.phase=RBC_ARRAY_REGION_ACTIVE,
        .cache={.owner=&owner,.elements=(unsigned char *)elements,.length=3}};
    RbcArrayRegionState state={0};
    RbcValueActivation activation={.code=&code,.depth=1,.owned_code=true,.array_regions=&state};
    XrRbcValueRuntime values={.top=&activation};
    XrRbcFrame frame={.pc=19,.base=1,.spill_base=1};
    XrRbcRegister slots[3]={{0},{.bits=23},{.bits=99}}, spills[2]={{0},{.bits=1}};
    XrRbcStack stack={.values=&values,.depth=1,.frames=&frame,.slots=slots,.spills=spills};
    XrtManagedStatus status=XRT_MANAGED_OK;
    CHECK(!rbc_value_try_fixed_array_leaf(&stack,0,&status) && slots[2].bits==99);
    CHECK(!rbc_value_try_fixed_array_leaf(&stack,3,&status) && !rbc_value_try_fixed_array_leaf(&stack,4,&status));
    CHECK(rbc_value_try_fixed_array_leaf(&stack,9,&status)==0);
    state=ready; activation.active_array_region=1; activation.owned_code=false;
    CHECK(!rbc_value_try_fixed_array_leaf(&stack,0,&status) && slots[2].bits==99);
    CHECK(!rbc_value_try_fixed_array_leaf(&stack,3,&status) && !rbc_value_try_fixed_array_leaf(&stack,4,&status));
    CHECK(rbc_value_try_fixed_array_leaf(&stack,9,&status)==0);
    activation.owned_code=true;
    CHECK(rbc_value_try_fixed_array_leaf(&stack,3,&status) && rbc_value_try_fixed_array_leaf(&stack,4,&status));
    CHECK(!rbc_value_try_fixed_array_leaf(&stack,5,&status) && !rbc_value_try_fixed_array_leaf(&stack,6,&status) &&
        !rbc_value_try_fixed_array_leaf(&stack,7,&status) && !rbc_value_try_fixed_array_leaf(&stack,8,&status));
    CHECK(rbc_value_try_fixed_array_leaf(&stack,9,&status)==2);
    CHECK(state.phase==RBC_ARRAY_REGION_ACTIVE && activation.active_array_region==1 &&
        state.cache.owner==&owner && !state.completed_write && frame.pc==19 && status==XRT_MANAGED_OK);
    CHECK(rbc_value_try_fixed_array_leaf(&stack,0,&status) && status==XRT_MANAGED_OK && slots[2].bits==3);
    CHECK(rbc_value_try_fixed_array_leaf(&stack,1,&status) && status==XRT_MANAGED_OK && slots[2].bits==11);
    CHECK(rbc_value_try_fixed_array_leaf(&stack,2,&status) && status==XRT_MANAGED_OK && elements[1]==23);
    CHECK(stack.depth==1 && frame.pc==19 && frame.base==1 && frame.spill_base==1);
    spills[1].bits=UINT64_MAX;
    CHECK(!rbc_value_try_fixed_array_leaf(&stack,1,&status) && status==XRT_MANAGED_OK && slots[2].bits==11);
    CHECK(state.phase==RBC_ARRAY_REGION_BYPASS && !activation.active_array_region && !state.cache.owner);
    state=ready; activation.active_array_region=1; spills[1].bits=3;
    CHECK(!rbc_value_try_fixed_array_leaf(&stack,2,&status) && status==XRT_MANAGED_OK && elements[1]==23);
    CHECK(state.phase==RBC_ARRAY_REGION_BYPASS && !activation.active_array_region && !state.cache.owner);
}

static void rbc_array_read_sum_checkpoint(void) {
    enum { IV=1, ACCUMULATOR, BOUND };
    XrLirFixedArrayUpdateHome row={.region={.primary_kind=XR_LIR_ARRAY_REGION_PRIMARY_GET,
        .range={.kind=XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT,.iv=IV,.length=BOUND},
        .read_sum={.accumulator=ACCUMULATOR,.next_accumulator=ACCUMULATOR,.limit=BOUND}}};
    const XrLirValueHomes homes={.fixed_array_updates=&row,.fixed_array_update_count=1};
    const XrRbcValueLocation locations[]={
        [IV]={XR_RBC_VALUE_SPILL,0}, [ACCUMULATOR]={XR_RBC_VALUE_REGISTER,0}, [BOUND]={XR_RBC_VALUE_REGISTER,1}
    };
    const XrRbcValueAccess latch={.operation=XR_RBC_VALUE_EDGE,.index=UINT32_MAX};
    const XrRbcArrayRegionAccess role={.region=1,.fixed_edge=XR_LIR_ARRAY_REGION_EDGE_LATCH,
        .sum_checkpoint=true};
    const XrRbcValueCode code={.homes=&homes,.locations=locations,.value_count=BOUND,.accesses=&latch,.access_count=1,
        .array_region_accesses=&role};
    int64_t elements[]={42,7,-1,9};
    XrtManagedHeader owner={0};
    const RbcArrayRegionState ready={.phase=RBC_ARRAY_REGION_ACTIVE,
        .cache={.owner=&owner,.elements=(unsigned char *)elements,.length=4}};
    RbcArrayRegionState state=ready;
    RbcValueActivation activation={.code=&code,.depth=1,.owned_code=true,.array_regions=&state,.active_array_region=1};
    XrRbcValueRuntime values={.top=&activation};
    XrRbcFrame frame={.pc=19,.base=1,.spill_base=1};
    XrRbcRegister slots[3]={{0},{.bits=UINT64_MAX},{.bits=3}}, spills[2]={{0},{.bits=1}};
    XrRbcStack stack={.values=&values,.depth=1,.frames=&frame,.slots=slots,.spills=spills};
    XrtManagedStatus status=XRT_MANAGED_OK;
    CHECK(rbc_value_try_fixed_array_leaf(&stack,0,&status)==1);
    CHECK(state.sum_attempted && status==XRT_MANAGED_OK && frame.pc==19 && spills[1].bits==3 && slots[1].bits==5);
    state=ready; slots[1].bits=123; slots[2].bits=5; spills[1].bits=1;
    CHECK(rbc_value_try_fixed_array_leaf(&stack,0,&status)==1);
    CHECK(state.sum_attempted && status==XRT_MANAGED_OK && spills[1].bits==1 && slots[1].bits==123);
}

static void rbc_region_case(unsigned trips,unsigned loops,unsigned failure,bool bypass,bool observer) {
    XrLirFunction root={0}; XrLirModule *input=rbc_region_input(&root,trips,loops,failure,observer,RBC_REGION_NORMAL);
    XrRbcProgram *program=NULL; CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    XrRbcModule module=*xr_rbc_program_view(program); const XrRbcValueCode *code=xr_rbc_module_value_code(&module,0);
    CHECK(code && code->homes->fixed_array_update_count==(observer ? 0u : loops));
    if (loops==2) CHECK(code->homes->fixed_array_updates[1].region.first_step);
    CHECK(module.code_lease.retain(module.code_lease.owner));
    XrtLocationOwner *locations=program->locations; CHECK(xrt_locations_retain(locations));
    xr_lir_module_destroy(input); xr_rbc_program_destroy(program);
    rbc_region_roles(&module,code,observer ? 0 : loops);
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
    RBC_READ_FOLD_FORMAL, RBC_READ_FOLD_IMMEDIATE, RBC_READ_FOLD_LARGE_LITERAL, RBC_READ_FOLD_PAST_END_LITERAL,
    RBC_READ_FOLD_SAME_LEN, RBC_READ_FOLD_EMPTY_LEN, RBC_READ_FOLD_SHARED_FORMAL
} RbcReadFoldBound;

static bool rbc_read_large_bound(RbcReadFoldBound mode) {
    return mode==RBC_READ_FOLD_LARGE_LITERAL || mode==RBC_READ_FOLD_PAST_END_LITERAL;
}

static XrLirModule *rbc_read_fold_input(XrLirFunction *root,bool additive,RbcReadFoldBound mode,bool reverse) {
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
    bool large=rbc_read_large_bound(mode);
    XrLirValue array;
    if (large) {
        XrLirValue items[]={ca_constant(&c,entry,mode==RBC_READ_FOLD_LARGE_LITERAL ? 32768 : 32767),
            ca_constant(&c,entry,7)};
        array=ca_intrinsic(&c,entry,XR_LIR_ARRAY_OP_REPEAT,XR_LIR_ARRAY,items,2);
    } else {
        XrLirValue items[]={largest,ca_constant(&c,entry,-1),ca_constant(&c,entry,7)};
        uint32_t count=mode==RBC_READ_FOLD_EMPTY_LEN ? 0 : 3;
        array=ca_intrinsic(&c,entry,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,count ? items : NULL,count);
    }
    XrLirValue initial[]={array,ca_constant(&c,entry,large ? 32765 : 0),largest};
    XrLirValue snapshot_cell={0};
    if (mode==RBC_READ_FOLD_SHARED_FORMAL)
        snapshot_cell=ca_value(&c,entry,XR_LIR_CELL_NEW,XR_LIR_ARRAY_CELL,initial,1);
    if (mode==RBC_READ_FOLD_IMMEDIATE) bound=ca_constant(&c,entry,3);
    OK(xr_lir_set_terminator(c.module,entry,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{header,initial,3}}}));
    if (large) bound=ca_constant(&c,header,32768);
    else if (mode==RBC_READ_FOLD_SAME_LEN || mode==RBC_READ_FOLD_EMPTY_LEN)
        bound=ca_intrinsic(&c,header,XR_LIR_ARRAY_OP_LEN,XR_LIR_I64,head,1);
    XrLirValue compare[]={head[1],bound},condition={0};
    OK(xr_lir_emit_value(c.module,header,&(XrLirInstruction){.op=XR_LIR_ICMP,
        .type=XR_LIR_I1,.predicate=reverse ? XR_LIR_GE : XR_LIR_LT,.operands=compare,.operand_count=2},&condition));
    OK(xr_lir_set_terminator(c.module,header,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&condition,.operand_count=1,.edge_count=2,
        .edges={{reverse ? exit_block : body,head,3},{reverse ? body : exit_block,head,3}}}));
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
    if (snapshot_cell.id) {
        XrLirValue snapshot=ca_value(&c,exit_block,XR_LIR_CELL_READ,XR_LIR_ARRAY,&snapshot_cell,1);
        ca_effect(&c,exit_block,XR_LIR_ARRAY_CELL_DROP,snapshot_cell);
        XrLirValue last[]={snapshot,ca_constant(&c,exit_block,2)};
        XrLirValue value=ca_intrinsic(&c,exit_block,XR_LIR_ARRAY_OP_GET,XR_LIR_I64,last,2);
        ca_effect(&c,exit_block,XR_LIR_ARRAY_DROP,snapshot);
        done[2]=ca_binary(&c,exit_block,XR_LIR_ADD,done[2],value);
    }
    ca_return(&c,exit_block,done[2]);
    OK(xr_lir_module_finish(c.module,NULL));
    return c.module;
}

static void rbc_fixed_read_cases(bool additive,RbcReadFoldBound mode,bool reverse) {
    XrLirFunction root={0}; XrLirModule *input=rbc_read_fold_input(&root,additive,mode,reverse);
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(input);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    CHECK(code && code->array_region_runtime &&
        code->homes->fixed_array_update_count==1);
    const XrLirFixedArrayUpdateHome *row=&code->homes->fixed_array_updates[0];
    CHECK(row->region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET && row->body_edge==(reverse ? 2u : 1u) &&
        row->workspace_slot==SIZE_MAX);
    bool same_length=mode==RBC_READ_FOLD_SAME_LEN || mode==RBC_READ_FOLD_EMPTY_LEN;
    CHECK(row->region.range.kind==(uint32_t)(same_length ? XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN :
        XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT));
    const XrLirArrayReadSum *sum_plan=&row->region.read_sum;
    CHECK((sum_plan->accumulator!=0)==additive);
    if (mode==RBC_READ_FOLD_IMMEDIATE) {
        /* The compare may use an immediate without creating a live scalar location. */
        CHECK(!sum_plan->limit && sum_plan->limit_bits==3);
        const XrRbcFunction *function=&module->functions[0];
        size_t immediate_compares=0;
        for (size_t pc=0;pc<function->code_count;++pc) {
            uint32_t word=function->code[pc];
            if ((word&255)==XR_RBC_LTI && (word>>16)==3) ++immediate_compares;
            if (!reverse) CHECK((word&255)!=XR_RBC_LOADI || (word>>16)!=3);
        }
        if (!reverse) CHECK(immediate_compares==1);
    }
    if (rbc_read_large_bound(mode)) CHECK(!sum_plan->limit && sum_plan->limit_bits==32768);
    size_t checkpoints=0;
    for (size_t n=0;n<code->access_count;++n) {
        const XrRbcArrayRegionAccess *role=&code->array_region_accesses[n];
        if (role->sum_checkpoint) {
            CHECK(role->fixed_edge==XR_LIR_ARRAY_REGION_EDGE_LATCH);
            ++checkpoints;
        }
    }
    CHECK(checkpoints==(size_t)additive);

    const uint64_t bounds[]={UINT64_MAX,0,1,2,3,4};
    const uint64_t sum[]={INT64_MAX,INT64_MAX,UINT64_MAX-1,UINT64_MAX-2,4};
    const uint64_t difference[]={INT64_MAX,INT64_MAX,0,UINT64_MAX,8};
    bool formal=mode==RBC_READ_FOLD_FORMAL;
    size_t count=formal ? sizeof(bounds)/sizeof(*bounds) : 1;
    for (size_t n=0;n<count;++n) {
        uint64_t bound=formal ? bounds[n] : 3;
        bool empty=mode==RBC_READ_FOLD_EMPTY_LEN || (formal && n<2);
        bool shared=mode==RBC_READ_FOLD_SHARED_FORMAL;
        bool fails=mode==RBC_READ_FOLD_PAST_END_LITERAL || (formal && bound==4);
        region_begin_attempts=region_begin_successes=0; region_force_bypass=false;
        RbcRegionResult actual=rbc_region_run_with_args(module,program->locations,&bound,1);
        CHECK(region_begin_attempts<=1 && region_begin_successes<=region_begin_attempts);
        if (empty || shared) CHECK(!region_begin_successes);
        else if (!fails && (!formal || bound>=2)) CHECK(region_begin_successes==1);
        if (empty) CHECK(!region_begin_attempts);
        region_begin_attempts=region_begin_successes=0; region_force_bypass=true;
        RbcRegionResult ordinary=rbc_region_run_with_args(module,program->locations,&bound,1);
        CHECK(region_begin_attempts<=1 && !region_begin_successes && actual.status==ordinary.status &&
            actual.bits==ordinary.bits && actual.failure==ordinary.failure && actual.source==ordinary.source);
        if (fails) {
            CHECK(actual.status==XR_RBC_VALUE_FAILURE && actual.failure==XR_XIR_VALUE_BOUNDS && actual.source);
            continue;
        }
        uint64_t expected=4;
        if (formal) expected=additive ? sum[n] : difference[n];
        else if (shared) expected=11;
        else if (empty) expected=INT64_MAX;
        else if (rbc_read_large_bound(mode)) expected=(uint64_t)INT64_MAX+21;
        CHECK(actual.status==XR_RBC_OK && actual.bits==expected);
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
    CHECK(code && (code->array_counted_push!=NULL)==eligible);
    CHECK((code->homes->fixed_array_update_count==1)==eligible);
    if (eligible) {
        const XrLirFixedArrayUpdateHome *row=&code->homes->fixed_array_updates[0];
        CHECK(row->region.push_counted && code->array_counted_push[0].iv==row->region.range.iv);
        if (mode==PUSH_WINDOW_LARGE_END)
            CHECK(!code->array_counted_push[0].limit && code->array_counted_push[0].bits==1000000);
        size_t latch=0;
        for (size_t n=0;n<code->access_count;++n) {
            XrRbcValueAccess access=code->accesses[n];
            if (access.operation==XR_RBC_VALUE_EDGE) CHECK(!access.window);
            const XrRbcArrayRegionAccess *role=&code->array_region_accesses[n];
            if (role->fixed_edge!=XR_LIR_ARRAY_REGION_EDGE_LATCH) continue;
            CHECK(access.operation==XR_RBC_VALUE_EDGE && access.index==UINT32_MAX && !access.count);
            ++latch;
        }
        CHECK(latch==1);
        XrRbcFunction physical=module->functions[0];
        XrRbcModule raw=*module; raw.functions=&physical;
        CHECK(!xr_rbc_module_code_owner(&raw));
        region_begin_attempts=region_begin_successes=0; region_force_bypass=false;
        CHECK(rbc_region_run_with_args(&raw,program->locations,&bound,1).status==XR_RBC_INVALID);
        CHECK(!region_begin_attempts && !region_begin_successes);
    }
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

static XrLirModule *rbc_affine_push_input(XrLirFunction *root, bool dependent) {
    CArrayFixture c={.element=XR_LIR_I64};
    OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirType scalar=XR_LIR_I64;
    *root=test_function(c.module,"affine_push",&scalar,1,XR_LIR_I64);
    XrLirValue bound={0};
    OK(xr_lir_function_parameter(c.module,*root,0,&bound));
    XrLirBlock entry=test_block(c.module,*root,NULL,0),header=rbc_region_block(&c,*root);
    XrLirBlock body=rbc_region_block(&c,*root),exit_block=rbc_region_block(&c,*root);
    OK(xr_lir_set_entry(c.module,*root,entry));
    XrLirValue seed=ca_constant(&c,entry,42);
    XrLirValue initial[]={ca_intrinsic(&c,entry,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,&seed,1),
        ca_constant(&c,entry,0),ca_constant(&c,entry,INT64_MAX),ca_constant(&c,entry,0)};
    OK(xr_lir_set_terminator(c.module,entry,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{header,initial,4}}}));
    XrLirValue head[4],next[4],done[4],condition={0};
    rbc_region_parameters(&c,header,head);
    XrLirValue compare[]={head[1],bound};
    OK(xr_lir_emit_value(c.module,header,&(XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,
        .predicate=XR_LIR_LT,.operands=compare,.operand_count=2},&condition));
    OK(xr_lir_set_terminator(c.module,header,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&condition,.operand_count=1,.edge_count=2,.edges={{body,head,4},{exit_block,head,4}}}));
    rbc_region_parameters(&c,body,next);
    XrLirValue scaled=ca_binary(&c,body,XR_LIR_MUL,next[1],ca_constant(&c,body,-3));
    XrLirValue payload=ca_binary(&c,body,XR_LIR_SUB,scaled,ca_constant(&c,body,7));
    XrLirValue cell=ca_value(&c,body,XR_LIR_CELL_NEW,XR_LIR_ARRAY_CELL,next,1);
    ca_effect(&c,body,XR_LIR_ARRAY_DROP,next[0]);
    XrLirValue append[]={cell,payload};
    (void)ca_intrinsic(&c,body,XR_LIR_ARRAY_OP_PUSH,XR_LIR_UNIT,append,2);
    next[0]=ca_value(&c,body,XR_LIR_CELL_READ,XR_LIR_ARRAY,&cell,1);
    ca_effect(&c,body,XR_LIR_ARRAY_CELL_DROP,cell);
    next[2]=ca_binary(&c,body,XR_LIR_ADD,next[2],dependent ? next[1] : ca_constant(&c,body,5));
    next[3]=ca_binary(&c,body,XR_LIR_SUB,next[3],ca_constant(&c,body,3));
    next[1]=ca_binary(&c,body,XR_LIR_ADD,next[1],ca_constant(&c,body,1));
    OK(xr_lir_set_terminator(c.module,body,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{header,next,4}}}));
    rbc_region_parameters(&c,exit_block,done);
    XrLirValue last[]={done[0],done[1]};
    XrLirValue result=ca_intrinsic(&c,exit_block,XR_LIR_ARRAY_OP_GET,XR_LIR_I64,last,2);
    ca_effect(&c,exit_block,XR_LIR_ARRAY_DROP,done[0]);
    result=ca_binary(&c,exit_block,XR_LIR_ADD,result,done[2]);
    ca_return(&c,exit_block,ca_binary(&c,exit_block,XR_LIR_ADD,result,done[3]));
    OK(xr_lir_module_finish(c.module,NULL));
    return c.module;
}

static void rbc_affine_push_case(bool dependent) {
    XrLirFunction root={0};
    XrLirModule *input=rbc_affine_push_input(&root,dependent);
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(input);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    CHECK(code && code->homes->fixed_array_update_count==1);
    const XrLirArrayPushSpan *span=&code->homes->fixed_array_updates[0].region.push_span;
    CHECK(span->available==!dependent && (code->array_counted_push!=NULL)==!dependent);
    if (!dependent) {
        CHECK(span->factor==UINT64_MAX-2 && span->bias==UINT64_MAX-6 && span->update_count==2);
        const XrLirArrayPushUpdate *updates=code->homes->array_push_updates+span->first_update;
        CHECK(updates[0].delta==5 && updates[1].delta==UINT64_MAX-2 && updates[0].value!=updates[1].value);
    }
    const uint64_t limits[]={0,9};
    const uint64_t results[]={(uint64_t)INT64_MAX+42,(uint64_t)INT64_MAX-(dependent ? 22u : 13u)};
    for (size_t n=0;n<sizeof(limits)/sizeof(*limits);++n) {
        array_pushes=array_push_transactions=0;
        region_force_bypass=false;
        RbcRegionResult actual=rbc_region_run_with_args(module,program->locations,&limits[n],1);
        CHECK(actual.status==XR_RBC_OK && actual.bits==results[n]);
        CHECK(array_pushes==(n ? 3u : 0u));
        array_pushes=array_push_transactions=0;
        region_force_bypass=true;
        RbcRegionResult ordinary=rbc_region_run_with_args(module,program->locations,&limits[n],1);
        CHECK(ordinary.status==actual.status && ordinary.bits==actual.bits && array_pushes==limits[n]);
    }
    region_force_bypass=false;
    xr_rbc_program_destroy(program);
    CHECK(!tracked_live);
}

static void rbc_fixed_push_cases(void) {
    ArraySetRegionFixture fixture=array_push_window_build(PUSH_WINDOW_NEXT_ITEM);
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(fixture.module,fixture.function,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(fixture.module);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    CHECK(code && code->array_region_runtime && code->array_counted_push && code->homes->fixed_array_update_count==1);
    const XrLirFixedArrayUpdateHome *row=&code->homes->fixed_array_updates[0];
    CHECK(row->region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_PUSH && row->workspace_slot!=SIZE_MAX);
    CHECK(row->region.push_span.available && row->region.push_span.factor==1 && row->region.push_span.bias==1);

    const uint64_t ignored=0;
    region_begin_attempts=region_begin_successes=array_pushes=array_push_transactions=0;
    region_force_bypass=false;
    RbcRegionResult actual=rbc_region_run_with_args(module,program->locations,&ignored,1);
    CHECK(actual.status==XR_RBC_OK && actual.bits==10 && array_pushes==3 && array_push_transactions==3 &&
        region_begin_attempts==3 && region_begin_successes==3);
    region_begin_attempts=region_begin_successes=array_pushes=array_push_transactions=0;
    region_force_bypass=true;
    RbcRegionResult ordinary=rbc_region_run_with_args(module,program->locations,&ignored,1);
    CHECK(ordinary.status==actual.status && ordinary.bits==actual.bits && array_pushes==9 &&
        region_begin_attempts==1 && !region_begin_successes);

    /* The third grow fails at capacity 8. The original prefix includes RHS
     * values computed before each PUSH, including writes made in the window. */
    array_push_item_bias=1;
    RbcRegionResult failure[2];
    for (unsigned bypass=0;bypass<2;++bypass) {
        array_push_fault=3;
        region_begin_attempts=region_begin_successes=array_pushes=array_push_transactions=failed_array_length=0;
        region_force_bypass=bypass!=0;
        size_t live=tracked_live;
        failure[bypass]=rbc_region_run_with_args(module,program->locations,&ignored,1);
        CHECK(tracked_live==live && failed_array_length==8 && array_push_transactions==3 &&
            array_pushes==(bypass ? 8u : 3u) && region_begin_successes==(bypass ? 0u : 2u));
        CHECK(failure[bypass].status==XR_RBC_VALUE_FAILURE && failure[bypass].failure==XR_XIR_VALUE_OOM);
    }
    CHECK(failure[0].source && failure[0].source==failure[1].source);
    array_push_item_bias=0; array_push_fault=0; region_force_bypass=false;
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
    rbc_push_region_case(PUSH_WINDOW_OBSERVER,0,9,9,0,false);
    rbc_fixed_push_cases();
    rbc_affine_push_case(false);
    rbc_affine_push_case(true);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_FORMAL,false);
    rbc_fixed_read_cases(false,RBC_READ_FOLD_FORMAL,false);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_IMMEDIATE,false);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_LARGE_LITERAL,false);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_PAST_END_LITERAL,false);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_FORMAL,true);
    rbc_fixed_read_cases(false,RBC_READ_FOLD_FORMAL,true);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_IMMEDIATE,true);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_LARGE_LITERAL,true);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_PAST_END_LITERAL,true);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_SAME_LEN,true);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_EMPTY_LEN,true);
    rbc_fixed_read_cases(true,RBC_READ_FOLD_SHARED_FORMAL,true);
    {
        XrLirFunction root={0}; XrLirModule *input=rbc_region_input(&root,0,1,0,false,RBC_REGION_READ_ONLY);
        XrRbcProgram *program=NULL; CHECK(xr_rbc_compile(input,root,&program,NULL)==XR_RBC_COMPILE_OK);
        const XrRbcModule *module=xr_rbc_program_view(program); const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
        CHECK(code && code->homes->fixed_array_update_count==1 &&
            code->homes->fixed_array_updates[0].region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET &&
            code->array_region_runtime && code->array_region_accesses);
        xr_lir_module_destroy(input);
        region_begin_attempts=region_begin_successes=0; region_force_bypass=false;
        RbcRegionResult result=rbc_region_run(module,program->locations);
        CHECK(result.status==XR_RBC_OK && result.bits==2218 && region_begin_attempts==1 && region_begin_successes==1);
        xr_rbc_program_destroy(program);
    }
    rbc_region_case(0,1,0,false,false); rbc_region_case(1,1,0,false,false);
    rbc_region_case(4,1,0,false,false); rbc_region_case(5,1,0,false,false);
    rbc_region_case(5,2,0,false,false); rbc_region_case(5,1,0,true,false);
    rbc_region_case(4,1,1,false,false); rbc_region_case(4,1,2,false,false);
    rbc_region_case(4,1,0,false,true);
    rbc_region_failures(1); rbc_region_failures(2);
    rbc_fixed_region_case(0,0,RBC_REGION_FIXED);
    rbc_fixed_region_case(1,0,RBC_REGION_FIXED);
    rbc_fixed_region_case(4,0,RBC_REGION_FIXED);
    rbc_fixed_region_case(4,0,RBC_REGION_FIXED_FALSE);
    rbc_fixed_region_case(4,1,RBC_REGION_FIXED_FALSE);
    rbc_fixed_adjacent_regions();
    rbc_fixed_array_leaf_locations();
    rbc_array_read_sum_checkpoint();
    RbcArrayRegionState state={.cache={(void *)1,(void *)2,3},.phase=RBC_ARRAY_REGION_ACTIVE};
    RbcValueActivation activation={.array_regions=&state,.active_array_region=1,.frame=(void *)4,.payload=(void *)5};
    rbc_value_array_region_forget(&activation,1,RBC_ARRAY_REGION_BYPASS);
    CHECK(!state.cache.owner && !state.cache.elements && !state.cache.length && !activation.active_array_region);
    puts("Array region: complete first latch, single begin, real homes, scalar cycles, raw CFG and retirement checked");
}
#endif // RBC_ARRAY_REGION_CASES_H
