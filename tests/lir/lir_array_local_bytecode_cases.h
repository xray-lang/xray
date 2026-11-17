/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_local_bytecode_cases.h - Admitted local edits preserve snapshots and fallback work
 */
#ifndef LIR_ARRAY_LOCAL_BYTECODE_CASES_H
#define LIR_ARRAY_LOCAL_BYTECODE_CASES_H

static XrLirValue array_local_const(ArrayForwardFixture c,uint64_t bits) {
    return array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=bits});
}
static XrLirValue array_local_get(ArrayForwardFixture c,uint32_t operation,XrLirValue array,XrLirValue index) {
    XrLirValue args[]={array,index};
    return array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_I64,
        .intrinsic=operation,.operands=args,.operand_count=2});
}
static ArrayForwardFixture array_local_fixture(bool snapshot,bool call) {
    ArrayForwardFixture c={0}; OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirFunction observe={0};
    if (call) {
        OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="observe_local",.name_length=13,
            .result=XR_LIR_UNIT},&observe));
        OK(xr_lir_define_function(c.module,observe)); XrLirBlock block={0};
        OK(xr_lir_add_block(c.module,observe,NULL,0,&block)); OK(xr_lir_set_entry(c.module,observe,block));
        array_forward_return(c.module,block,(XrLirValue){0});
    }
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="local_array",.name_length=11,
        .result=XR_LIR_I64},&c.function));
    OK(xr_lir_define_function(c.module,c.function)); OK(xr_lir_add_block(c.module,c.function,NULL,0,&c.block));
    OK(xr_lir_set_entry(c.module,c.function,c.block));
    uint32_t make=0,set=0,push=0,get=0,len=0;
    OK(xr_lir_array_operation(c.module,XR_LIR_I64,XR_LIR_ARRAY_OP_NEW,&make));
    OK(xr_lir_array_mutator(c.module,XR_LIR_I64,XR_LIR_ARRAY_CELL,XR_LIR_ARRAY_OP_SET,&set));
    OK(xr_lir_array_mutator(c.module,XR_LIR_I64,XR_LIR_ARRAY_CELL,XR_LIR_ARRAY_OP_PUSH,&push));
    OK(xr_lir_array_operation(c.module,XR_LIR_I64,XR_LIR_ARRAY_OP_GET,&get));
    OK(xr_lir_array_operation(c.module,XR_LIR_I64,XR_LIR_ARRAY_OP_LEN,&len));
    XrLirValue initial[]={array_local_const(c,17),array_local_const(c,25)};
    c.initial=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,
        .type=XR_LIR_ARRAY,.intrinsic=make,.operands=initial,.operand_count=2});
    if (snapshot) c.saved=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ARRAY_RETAIN,
        .type=XR_LIR_ARRAY,.operands=&c.initial,.operand_count=1});
    c.cell=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,
        .operands=&c.initial,.operand_count=1});
    array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
        .operands=&c.initial,.operand_count=1});
    XrLirValue zero=array_local_const(c,0),two=array_local_const(c,2);
    XrLirValue args[]={c.cell,zero,array_local_const(c,41)};
    array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
        .intrinsic=set,.operands=args,.operand_count=3});
    if (call) array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=observe});
    XrLirValue append[]={c.cell,array_local_const(c,1)};
    array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
        .intrinsic=push,.operands=append,.operand_count=2});
    append[1]=array_local_const(c,7);
    array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,
        .intrinsic=push,.operands=append,.operand_count=2});
    c.result=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_ARRAY,
        .operands=&c.cell,.operand_count=1});
    array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,
        .operands=&c.cell,.operand_count=1});
    XrLirValue sum[]={array_local_get(c,get,c.result,zero),array_local_get(c,get,c.result,two)};
    XrLirValue answer=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,
        .operands=sum,.operand_count=2});
    XrLirValue last[]={array_local_get(c,get,c.result,array_local_const(c,3)),append[1]};
    XrLirValue difference=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_SUB,.type=XR_LIR_I64,
        .operands=last,.operand_count=2});
    XrLirValue add[]={answer,difference};
    answer=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    XrLirValue length=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_I64,
        .intrinsic=len,.operands=&c.result,.operand_count=1});
    XrLirValue expected_length[]={length,array_local_const(c,4)};
    difference=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_SUB,.type=XR_LIR_I64,
        .operands=expected_length,.operand_count=2});
    add[0]=answer; add[1]=difference;
    answer=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=add,.operand_count=2});
    if (snapshot) {
        XrLirValue check[]={array_local_get(c,get,c.saved,zero),initial[0]};
        XrLirValue difference=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_SUB,
            .type=XR_LIR_I64,.operands=check,.operand_count=2});
        XrLirValue add[]={answer,difference};
        answer=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,
            .operands=add,.operand_count=2});
        array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
            .operands=&c.saved,.operand_count=1});
    }
    array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
        .operands=&c.result,.operand_count=1});
    array_forward_return(c.module,c.block,answer);
    XrLirDiagnostic diagnostic={0}; XrLirStatus status=xr_lir_module_finish(c.module,&diagnostic);
    if (status!=XR_LIR_OK) fprintf(stderr,"local Array finish f%u b%u i%u: %s\n",
        diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.message);
    CHECK(status==XR_LIR_OK); return c;
}
static uint32_t array_local_sites(const XrRbcModule *module) {
    uint32_t count=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,f);
        for (size_t n=0;code && n<code->access_count;++n) {
            XrRbcValueAccess a=code->accesses[n];
            if (a.operation!=XR_RBC_VALUE_ARRAY || !a.other) continue;
            CHECK((a.index==XR_LIR_ARRAY_OP_SET || a.index==XR_LIR_ARRAY_OP_PUSH) &&
                a.other<=code->homes->event_count && code->homes->events[a.other-1].kind==XR_LIR_OWNERSHIP_ARRAY_OPERATION);
            ++count;
        }
    }
    return count;
}
static void array_local_bytecode_cases(void) {
    for (unsigned mode=0;mode<4;++mode) {
        XrRbcProgram *program=array_forwarding_compile(array_local_fixture((mode&1)!=0,(mode&2)!=0));
        CHECK(array_local_sites(xr_rbc_program_view(program))==3);
        array_forwarding_run_retired(program,NULL,0,42);
    }
    const XrLirType other_elements[]={XR_LIR_U8,XR_LIR_U64,XR_LIR_STRING};
    for (size_t e=0;e<sizeof(other_elements)/sizeof(*other_elements);++e) {
        XrRbcProgram *other=array_forwarding_compile(array_forward_fixture(other_elements[e],ARRAY_FORWARD_LOCAL));
        CHECK(!array_local_sites(xr_rbc_program_view(other)));
        array_forwarding_run_retired(other,NULL,0,other_elements[e]==XR_LIR_STRING ? 0u : 1u);
    }
    XrRbcProgram *borrowed=array_forwarding_compile(array_forward_fixture(XR_LIR_I64,ARRAY_FORWARD_REF));
    CHECK(!array_local_sites(xr_rbc_program_view(borrowed))); array_forwarding_run_retired(borrowed,NULL,0,1);
    XrRbcProgram *program=array_forwarding_compile(array_local_fixture(false,false));
    const XrRbcModule *module=xr_rbc_program_view(program); const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    XrRbcValueAccess *accesses=(XrRbcValueAccess *)code->accesses;
    XrLirHomeEvent *events=(XrLirHomeEvent *)code->homes->events;
    uint32_t tested=0;
    for (size_t n=0;n<code->access_count;++n) {
        XrRbcValueAccess *a=&accesses[n]; if (a->operation!=XR_RBC_VALUE_ARRAY || !a->other) continue;
        XrRbcValueAccess original=*a; XrLirHomeEvent *event=&events[a->other-1],saved=*event;
        a->other=(uint32_t)code->homes->event_count+1; array_forwarding_reject(program); *a=original;
        event->operation=XR_LIR_ARRAY_OP_GET; array_forwarding_reject(program); *event=saved;
        event->kind=XR_LIR_OWNERSHIP_BINDING_WRITE; array_forwarding_reject(program); *event=saved;
        ++event->instruction; array_forwarding_reject(program); *event=saved;
        event->source=0; array_forwarding_reject(program); *event=saved;
        event->transfer_mode=XR_LIR_OWNERSHIP_TRANSFER_BORROW; array_forwarding_reject(program); *event=saved;
        event->view_parameter=0; array_forwarding_reject(program); *event=saved;
        XrLirHomeWorkspace *workspace=(XrLirHomeWorkspace *)&code->homes->workspaces[a->window],old=*workspace;
        const XrLirHomeBinding *binding=&code->homes->bindings[code->arguments[a->first]];
        workspace->first_owner=binding->first_owner; workspace->offset=binding->offset;
        array_forwarding_reject(program); *workspace=old;
        workspace->value=code->arguments[a->first]; array_forwarding_reject(program); *workspace=old;
        ++workspace->block; array_forwarding_reject(program); *workspace=old;
        workspace->kind=XR_LIR_HOME_WORKSPACE_VIEW_HOLD; array_forwarding_reject(program); *workspace=old;
        workspace->temporary_offset=SIZE_MAX; array_forwarding_reject(program); *workspace=old;
        a->count=1; array_forwarding_reject(program); *a=original;
        ++tested;
    }
    CHECK(tested==3);
    XrRbcModule raw=*module; raw.code_lease=(XrRbcCodeLease){0}; XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
    /* Clearing the optional projection retains the original admitted kernel. */
    for (size_t n=0;n<code->access_count;++n) if (accesses[n].operation==XR_RBC_VALUE_ARRAY) accesses[n].other=0;
    array_forwarding_run_retired(program,NULL,0,42);
}

#if defined(XR_ARRAY_LOCAL_ALLOC_CASES)
typedef struct ArrayLocalProbe { XrRbcExecution *execution; unsigned mode; bool checked; } ArrayLocalProbe;
static int64_t array_local_at(const XrtManagedHeader *owner,size_t index) {
    const XrtArrayBacking *array=(const XrtArrayBacking *)owner; int64_t value=0;
    CHECK(index<array->length); memcpy(&value,array->elements+index*sizeof(value),sizeof(value)); return value;
}
static void array_local_probe_root(void *context) {
    ArrayLocalProbe *probe=context; XrRbcExecution *execution=probe->execution;
    const XrRbcModule *module=execution->module; XrRbcStack *stack=&execution->stack;
    CHECK(xr_rbc_values_begin(module,stack,0,0)==XR_RBC_OK);
    RbcValueActivation *activation=stack->values->top; const XrRbcValueCode *code=activation->code;
    XrRbcValueAccess make={0},binding_new={0},set={0},push={0};
    for (size_t n=0;n<code->access_count;++n) {
        XrRbcValueAccess a=code->accesses[n];
        if (a.operation==XR_RBC_VALUE_BINDING_NEW) binding_new=a;
        if (a.operation!=XR_RBC_VALUE_ARRAY) continue;
        if (a.index==XR_LIR_ARRAY_OP_NEW) make=a;
        if (a.index==XR_LIR_ARRAY_OP_SET) set=a;
        if (a.index==XR_LIR_ARRAY_OP_PUSH) push=a;
    }
    CHECK(make.count==2 && set.other && push.other && !activation->array_backings_ready &&
        !rbc_value_array_local_update(stack,activation,set));
    rbc_value_register(stack,activation,code->arguments[make.first])->bits=17;
    rbc_value_register(stack,activation,code->arguments[make.first+1])->bits=25;
    CHECK(rbc_value_array_step(module,stack,activation,make,0)==XRT_MANAGED_OK && activation->array_backings_ready);
    CHECK(rbc_value_binding_step(module,stack,activation,binding_new)==XRT_MANAGED_OK);
    CHECK(xrt_managed_slot_drop(activation->frame,code->homes->values[binding_new.source].first_owner)==XRT_MANAGED_OK);
    const uint32_t *args=code->arguments+set.first;
    const XrLirHomeBinding *binding=&code->homes->bindings[args[0]];
    const XrLirHomeWorkspace *workspace=&code->homes->workspaces[set.window];
    rbc_value_register(stack,activation,args[1])->bits=0;
    rbc_value_register(stack,activation,args[2])->bits=41;
    XrtManagedSlotLocation from={0},stage={0};
    CHECK(xrt_managed_slot_location(activation->frame,binding->first_owner,&from)==XRT_MANAGED_OK &&
        xrt_managed_slot_location(activation->frame,workspace->first_owner,&stage)==XRT_MANAGED_OK);
    XrtManagedHeader *incoming=*from.owner; CHECK(incoming && atomic_load(&incoming->references)==1);
    const XrtBackingTypes *types=NULL; CHECK(xrt_native_values_backings(xrt_value_root_current(0),&types)==XRT_MANAGED_OK);
    if (!probe->mode) {
        activation->array_backings_ready=false;
        CHECK(!rbc_value_array_local_update(stack,activation,set) && *from.owner==incoming && array_local_at(incoming,0)==17);
        CHECK(rbc_value_array_step(module,stack,activation,set,0)==XRT_MANAGED_OK && activation->array_backings_ready &&
            *from.owner==incoming && array_local_at(incoming,0)==41);
        rbc_value_register(stack,activation,args[2])->bits=42;
    }
    if (probe->mode==9) {
        CHECK(xrt_managed_slot_drop(activation->frame,binding->first_owner)==XRT_MANAGED_OK && !*from.owner);
        CHECK(!rbc_value_array_local_update(stack,activation,set) && !*from.owner && !*stage.owner);
        CHECK(rbc_value_array_step(module,stack,activation,set,0)==XRT_MANAGED_ARGUMENT);
        probe->checked=true; return;
    }
    if (probe->mode==1) {
        uint32_t saved=0;
        for (size_t n=0;n<code->access_count;++n) if (code->accesses[n].operation==XR_RBC_VALUE_COPY) {
            saved=code->accesses[n].result; break;
        }
        CHECK(saved && xrt_backing_slot_copy(activation->frame,code->homes->values[saved].first_owner,incoming)==XRT_MANAGED_OK);
    }
    if (probe->mode==2) atomic_store(&incoming->references,-1);
    if (probe->mode==3) atomic_store(&incoming->references,INT32_MAX);
    if (probe->mode==4) CHECK(xrt_managed_domain_enter(from)==XRT_MANAGED_OK);
    size_t pending_slot=probe->mode==5 ? binding->first_owner : workspace->first_owner; XrtManagedHeader *pending=NULL;
    if (probe->mode==5 || probe->mode==6)
        CHECK(xrt_managed_slot_prepare(activation->frame,pending_slot,types->arrays[0],sizeof(XrtArrayBacking),&pending)==XRT_MANAGED_OK);
    if (probe->mode==7) CHECK(xrt_array_backing_i64_create(activation->frame,workspace->first_owner,types,NULL,0,0)==XRT_MANAGED_OK);
    if (probe->mode==8) rbc_value_register(stack,activation,args[1])->bits=UINT64_MAX;
    XrtManagedHeader *old_stage=*stage.owner; int32_t references=atomic_load(&incoming->references);
    XrtBillingStats before=xrt_billing_owner_stats(stack->values->billing);
    if (!probe->mode) {
        CHECK(rbc_value_array_local_update(stack,activation,set) && *from.owner==incoming && !*stage.owner &&
            array_local_at(incoming,0)==42 && atomic_load(&incoming->references)==references);
        XrtBillingStats applied=xrt_billing_owner_stats(stack->values->billing);
        CHECK(applied.blocks==before.blocks && applied.bytes==before.bytes);
        const uint32_t *append=code->arguments+push.first;
        rbc_value_register(stack,activation,append[1])->bits=1;
        CHECK(!rbc_value_array_local_update(stack,activation,push) && ((XrtArrayBacking *)incoming)->length==2);
        CHECK(rbc_value_array_step(module,stack,activation,push,0)==XRT_MANAGED_OK);
        XrtManagedHeader *grown=*from.owner; CHECK(grown && ((XrtArrayBacking *)grown)->length==3 && array_local_at(grown,2)==1);
        rbc_value_register(stack,activation,append[1])->bits=7;
        CHECK(rbc_value_array_local_update(stack,activation,push) && *from.owner==grown &&
            ((XrtArrayBacking *)grown)->length==4 && array_local_at(grown,3)==7);
    } else {
        CHECK(!rbc_value_array_local_update(stack,activation,set) && *from.owner==incoming && *stage.owner==old_stage &&
            array_local_at(incoming,0)==17 && atomic_load(&incoming->references)==references);
        XrtBillingStats after=xrt_billing_owner_stats(stack->values->billing);
        CHECK(after.blocks==before.blocks && after.bytes==before.bytes);
        if (probe->mode!=3) {
            XrtManagedStatus status=rbc_value_array_step(module,stack,activation,set,0);
            CHECK(status==(probe->mode<3 ? XRT_MANAGED_OK : probe->mode==8 ? XRT_MANAGED_BOUNDS : XRT_MANAGED_ARGUMENT));
            if (probe->mode==1) CHECK(*from.owner!=incoming && array_local_at(*from.owner,0)==41 && array_local_at(incoming,0)==17);
            if (probe->mode==2) CHECK(*from.owner==incoming && array_local_at(incoming,0)==41);
        }
    }
    if (probe->mode==2 || probe->mode==3) atomic_store(&incoming->references,1);
    if (probe->mode==4) xrt_managed_domain_leave(from);
    if (probe->mode==5 || probe->mode==6) CHECK(xrt_managed_slot_abort(activation->frame,pending_slot)==XRT_MANAGED_OK);
    probe->checked=true;
}
static void array_local_live_cases(void) {
    for (unsigned mode=0;mode<10;++mode) {
        XrRbcProgram *program=array_forwarding_compile(array_local_fixture(true,false)); ArrayLocalProbe probe={.mode=mode};
        CHECK(xr_rbc_program_prepare(program,NULL,0,&probe.execution)==XR_RBC_OK); xr_rbc_program_destroy(program);
        XrtExecutionFailure failure={0};
        CHECK(xrt_run_embedded_owned(probe.execution->pending->locations,&failure,array_local_probe_root,&probe,
            &probe.execution->values)==XRT_EXECUTION_COMPLETED && probe.checked && xrt_value_root_empty(&probe.execution->values));
        xr_rbc_execution_free(&probe.execution); CHECK(!live_allocations);
    }
}
static void array_local_native_failures(XrRbcProgram *program) {
    size_t retained=live_allocations,failures=0;
    for (size_t allowance=0;;++allowance) {
        XrRbcExecution *execution=NULL; CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        XrtBillingOwner *billing=NULL;
        CHECK(xrt_native_values_prepare(&execution->values,&billing)==XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
        xrt_billing_owner_fail_after(billing,allowance);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution); CHECK(outcome);
        CHECK(!execution->stack.values && !execution->state && xrt_value_root_empty(&execution->values));
        XrRbcStatus status=xr_rbc_outcome_status(outcome); uint64_t bits=0;
        if (status==XR_RBC_OK) CHECK(xr_rbc_outcome_bits(outcome,&bits) && bits==42);
        else { XrXirValueStatus failure; uint32_t source; const XrtLocationOwner *locations;
            CHECK(status==XR_RBC_VALUE_FAILURE && xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations) &&
                failure==XR_XIR_VALUE_OOM); ++failures; }
        xrt_billing_owner_fail_after(billing,SIZE_MAX); xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
        XrtBillingStats after=xrt_billing_owner_stats(billing); CHECK(!after.blocks && !after.bytes);
        xrt_billing_owner_drop(billing); CHECK(live_allocations==retained);
        if (status==XR_RBC_OK) break;
        CHECK(allowance<SIZE_MAX);
    }
    CHECK(failures);
}
static void array_local_allocation_cases(void) {
    array_local_bytecode_cases(); CHECK(!live_allocations);
    array_local_live_cases();
    ArrayForwardFixture c=array_local_fixture(true,true);
    callable_allocation_failures(c.module,c.function,42,"local Array snapshot and guest call");
    XrRbcProgram *program=array_forwarding_compile(array_local_fixture(true,true));
    array_local_native_failures(program); array_forwarding_run_retired(program,NULL,0,42); CHECK(!live_allocations);
}
#endif
#endif // LIR_ARRAY_LOCAL_BYTECODE_CASES_H
