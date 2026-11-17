/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_forwarding_bytecode_cases.h - Exact binding handoffs retain their real fallbacks
 */
#ifndef LIR_ARRAY_FORWARDING_BYTECODE_CASES_H
#define LIR_ARRAY_FORWARDING_BYTECODE_CASES_H
#include "lir_array_forwarding_fixture.h"

static XrRbcProgram *array_forwarding_compile(ArrayForwardFixture c) {
    XrRbcProgram *program=NULL; XrRbcCompileDiagnostic diagnostic={0};
    XrRbcCompileStatus status=xr_rbc_compile(c.module,c.function,&program,&diagnostic);
    if (status!=XR_RBC_COMPILE_OK) fprintf(stderr,"Array forwarding f%u b%u i%u: %s\n",
        diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.message);
    CHECK(status==XR_RBC_COMPILE_OK && program);
    xr_lir_module_destroy(c.module);
    return program;
}
static void array_forwarding_run_retired(XrRbcProgram *program,const uint64_t *arguments,
    size_t count,uint64_t expected) {
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,arguments,count,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    uint64_t bits=0;
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK &&
        xr_rbc_outcome_bits(outcome,&bits) && bits==expected);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
}
static uint32_t array_forwarding_encoded_sites(const XrRbcModule *module) {
    uint32_t sites=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,f);
        if (!code) continue;
        const XrRbcFunction *function=&module->functions[f];
        for (size_t pc=0;pc<function->code_count;++pc) {
            uint32_t word=function->code[pc];
            if ((word&255)!=XR_RBC_VALUE_ACCESS) continue;
            XrRbcValueAccess a=code->accesses[word>>16];
            if ((a.operation!=XR_RBC_VALUE_BINDING_NEW && a.operation!=XR_RBC_VALUE_BINDING_READ) || !a.index) continue;
            const XrLirHomeEvent *event=&code->homes->events[a.index-1];
            if (!event->forwarding_mode) continue;
            CHECK(event->forwarding_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE &&
                event->view_parameter==UINT32_MAX && pc+1<function->code_count && !a.window);
            word=function->code[pc+1]; CHECK((word&65535u)==XR_RBC_VALUE_ACCESS);
            XrRbcValueAccess drop=code->accesses[word>>16];
            CHECK(drop.operation==(a.operation==XR_RBC_VALUE_BINDING_NEW ? XR_RBC_VALUE_DROP : XR_RBC_VALUE_BINDING_DROP) &&
                drop.source==a.source && !drop.index);
            ++sites;
        }
    }
    return sites;
}
static void array_forwarding_bytecode_execution(void) {
    const uint32_t expected[]={2,1,1,0,0,0,2};
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8,XR_LIR_U64,XR_LIR_STRING};
    for (size_t e=0;e<sizeof(elements)/sizeof(*elements);++e)
        for (unsigned mode=0;mode<sizeof(expected)/sizeof(*expected);++mode) {
            XrRbcProgram *program=array_forwarding_compile(array_forward_fixture(elements[e],(enum ArrayForwardMode)mode));
            const XrRbcModule *module=xr_rbc_program_view(program);
            CHECK(array_forwarding_encoded_sites(module)==(e ? 0u : expected[mode]));
            XrRbcExecution *execution=NULL; XrRbcModule raw=*module; raw.code_lease=(XrRbcCodeLease){0};
            CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
            array_forwarding_run_retired(program,NULL,0,elements[e]==XR_LIR_STRING ? 0u : 1u);
        }
}
static void array_forwarding_reject(const XrRbcProgram *program) {
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(xr_rbc_program_view(program),0,NULL,0,
        xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
}
static void array_forwarding_bytecode_corruptions(void) {
    XrRbcProgram *program=array_forwarding_compile(array_forward_fixture(XR_LIR_I64,ARRAY_FORWARD_LOCAL));
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
    XrRbcValueAccess *accesses=(XrRbcValueAccess *)code->accesses;
    XrLirHomeEvent *events=(XrLirHomeEvent *)code->homes->events;
    uint32_t *words=(uint32_t *)module->functions[0].code;
    uint32_t tested=0;
    for (size_t pc=0;pc<module->functions[0].code_count;++pc) {
        uint32_t word=words[pc]; if ((word&255)!=XR_RBC_VALUE_ACCESS) continue;
        XrRbcValueAccess *access=&accesses[word>>16];
        if ((access->operation!=XR_RBC_VALUE_BINDING_NEW && access->operation!=XR_RBC_VALUE_BINDING_READ) || !access->index) continue;
        XrLirHomeEvent *event=&events[access->index-1]; XrLirHomeEvent original=*event;
        if (!event->forwarding_mode) continue;
        event->forwarding_mode=XR_LIR_OWNERSHIP_TRANSFER_COPY; array_forwarding_reject(program); *event=original;
        event->forwarding_drop=0; array_forwarding_reject(program); *event=original;
        ++event->forwarding_drop; array_forwarding_reject(program); *event=original;
        ++event->block; array_forwarding_reject(program); *event=original;
        ++event->instruction; ++event->forwarding_drop; array_forwarding_reject(program); *event=original;
        event->source=event->target; array_forwarding_reject(program); *event=original;
        event->target=event->source; array_forwarding_reject(program); *event=original;
        event->kind=XR_LIR_OWNERSHIP_BINDING_WRITE; array_forwarding_reject(program); *event=original;
        event->view_cleanup=XR_LIR_SLICE_CLEANUP_UNCONSTRUCTED; array_forwarding_reject(program); *event=original;
        event->view_parameter=0; array_forwarding_reject(program); *event=original;
        XrLirHomeEvent *release=NULL;
        for (size_t n=access->index;n<code->homes->event_count;++n)
            if (events[n].block==event->block && events[n].instruction==event->forwarding_drop &&
                events[n].source==access->source && events[n].kind==(uint32_t)(access->operation==XR_RBC_VALUE_BINDING_NEW ?
                    XR_LIR_OWNERSHIP_RELEASE : XR_LIR_OWNERSHIP_BINDING_RELEASE)) { release=&events[n]; break; }
        CHECK(release && release->view_parameter==UINT32_MAX);
        release->view_parameter=0; array_forwarding_reject(program); release->view_parameter=UINT32_MAX;
        XrRbcValueAccess saved=*access;
        access->index=(uint32_t)code->homes->event_count+1; array_forwarding_reject(program); *access=saved;
        access->window=1; array_forwarding_reject(program); *access=saved;
        access->other=access->source; array_forwarding_reject(program); *access=saved;
        uint32_t next=words[pc+1];
        words[pc+1]=XR_RBC_LOADI; array_forwarding_reject(program); words[pc+1]=next;
        words[pc+1]=XR_RBC_VALUE_ACCESS | (UINT32_C(65535)<<16); array_forwarding_reject(program); words[pc+1]=next;
        XrRbcValueAccess *drop=&accesses[next>>16], old_drop=*drop;
        drop->source=access->result; array_forwarding_reject(program); *drop=old_drop;
        drop->operation=XR_RBC_VALUE_BINDING_WRITE; array_forwarding_reject(program); *drop=old_drop;
        drop->index=access->index; array_forwarding_reject(program); *drop=old_drop;
        XrLirValueHome *home=(XrLirValueHome *)&code->homes->values[
            access->operation==XR_RBC_VALUE_BINDING_NEW ? access->source : access->result], old_home=*home;
        XrLirHomeBinding *binding=(XrLirHomeBinding *)&code->homes->bindings[
            access->operation==XR_RBC_VALUE_BINDING_NEW ? access->result : access->source], old_binding=*binding;
        home->offset=SIZE_MAX; array_forwarding_reject(program); *home=old_home;
        home->parameter=1; array_forwarding_reject(program); *home=old_home;
        home->first_owner=binding->first_owner; array_forwarding_reject(program); *home=old_home;
        binding->kind=XR_LIR_HOME_BINDING_PARAMETER; array_forwarding_reject(program); *binding=old_binding;
        ++tested;
    }
    CHECK(tested==2);
    array_forwarding_run_retired(program,NULL,0,1);
}
static void array_forwarding_raw_fields(void) {
    uint32_t words[2]={0};
    CHECK(xr_rbc_encode(&words[0],(XrRbcOperands){.op=XR_RBC_LOADI,.immediate=42}));
    CHECK(xr_rbc_encode(&words[1],(XrRbcOperands){.op=XR_RBC_RET}));
    XrRbcFunction function={.code=words,.code_count=2,.register_count=1};
    XrRbcValueLocation locations[2]={{0},{XR_RBC_VALUE_REGISTER,0}};
    XrLirValueHome values[2]={{0}}; XrLirHomeEvent event={0};
    XrLirValueHomes homes={.value_count=1,.values=values,.events=&event,.event_count=1};
    XrRbcValueCode code={.homes=&homes,.locations=locations,.value_count=1};
    const XrRbcValueCode *codes[]={&code};
    XrRbcModule module={.functions=&function,.function_count=1,.borrowed_value_codes=codes,.borrowed_value_code_count=1};
    XrtLocationOwner *source=NULL; CHECK(xrt_locations_create(NULL,0,&source)==XRT_LOCATION_OK);
    XrRbcExecution *execution=NULL;
    /* An explicit typed directory selects complete bank/CFG admission even
     * when every logical home is scalar and has no image or owning slot. */
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,source,&execution)==XR_RBC_INVALID && !execution);
    const XrRbcRegisterKind kinds[]={XR_RBC_BITS};
    function.register_kinds=kinds; function.register_kind_count=1;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,source,&execution)==XR_RBC_OK);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution); uint64_t bits=0;
    CHECK(outcome && xr_rbc_outcome_bits(outcome,&bits) && bits==42);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    event.forwarding_mode=XR_LIR_OWNERSHIP_TRANSFER_MOVE;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,source,&execution)==XR_RBC_INVALID && !execution);
    event.forwarding_mode=XR_LIR_OWNERSHIP_TRANSFER_NONE; event.forwarding_drop=1;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,source,&execution)==XR_RBC_INVALID && !execution);
    xrt_locations_release(source);
}
static ArrayForwardFixture array_forwarding_branch_fixture(void) {
    ArrayForwardFixture c={0}; OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirType flag_type=XR_LIR_I1;
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="branch_drop",.name_length=11,
        .parameters=&flag_type,.parameter_count=1,.result=XR_LIR_I64},&c.function));
    OK(xr_lir_define_function(c.module,c.function));
    XrLirBlock other={0},cleanup={0};
    OK(xr_lir_add_block(c.module,c.function,NULL,0,&c.block)); OK(xr_lir_set_entry(c.module,c.function,c.block));
    OK(xr_lir_add_block(c.module,c.function,NULL,0,&other)); OK(xr_lir_add_block(c.module,c.function,NULL,0,&cleanup));
    uint32_t make=0; OK(xr_lir_array_operation(c.module,XR_LIR_I64,XR_LIR_ARRAY_OP_NEW,&make));
    c.initial=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,.intrinsic=make});
    c.cell=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,
        .operands=&c.initial,.operand_count=1});
    array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&c.initial,.operand_count=1});
    array_forward_effect(c.module,c.block,(XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,.operands=&c.cell,.operand_count=1});
    XrLirValue next=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,.intrinsic=make});
    XrLirValue cell=array_forward_value(c.module,c.block,(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_ARRAY_CELL,
        .operands=&next,.operand_count=1}), flag={0};
    OK(xr_lir_function_parameter(c.module,c.function,0,&flag));
    OK(xr_lir_set_terminator(c.module,c.block,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&flag,.operand_count=1,.edge_count=2,.edges={{cleanup,NULL,0},{other,NULL,0}}}));
    XrLirBlock exits[]={other,cleanup};
    for (unsigned n=0;n<2;++n) {
        array_forward_effect(c.module,exits[n],(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&next,.operand_count=1});
        array_forward_effect(c.module,exits[n],(XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1});
        XrLirValue answer=array_forward_value(c.module,exits[n],(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=n ? 42u : 17u});
        array_forward_return(c.module,exits[n],answer);
    }
    OK(xr_lir_module_finish(c.module,NULL)); return c;
}
static void array_forwarding_branch_drop(void) {
    for (uint64_t flag=0;flag<2;++flag) {
        XrRbcProgram *program=array_forwarding_compile(array_forwarding_branch_fixture());
        const XrRbcModule *module=xr_rbc_program_view(program); const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
        const XrRbcFunction *function=&module->functions[0]; size_t direct=0;
        CHECK(array_forwarding_encoded_sites(module)==1);
        for (size_t pc=0;pc<function->code_count;++pc) if ((function->code[pc]&255)==XR_RBC_JMP) {
            uint32_t bits=function->code[pc]>>8; int32_t offset=bits<=XR_RBC_JUMP_MAX ? (int32_t)bits : (int32_t)bits-16777216;
            size_t target=pc+1+(size_t)offset; CHECK(target<function->code_count); uint32_t word=function->code[target];
            direct+=(word&65535u)==XR_RBC_VALUE_ACCESS && code->accesses[word>>16].operation==XR_RBC_VALUE_DROP;
        }
        CHECK(direct);
        array_forwarding_run_retired(program,&flag,1,flag ? 42u : 17u);
    }
}
static void array_forwarding_bytecode_cases(void) {
    array_forwarding_bytecode_execution();
    array_forwarding_bytecode_corruptions();
    array_forwarding_raw_fields();
    array_forwarding_branch_drop();
}
#if defined(XR_ARRAY_FORWARDING_ALLOC_CASES)
static void array_forwarding_native_failures(XrRbcProgram *program) {
    XrtBillingOwner *control=NULL; CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&control)==XRT_MANAGED_OK);
    XrtBillingStats base=xrt_billing_owner_stats(control); size_t failures=0, retained=live_allocations;
    for (size_t allowance=0;;++allowance) {
        XrRbcExecution *execution=NULL; CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        XrtBillingOwner *billing=NULL; CHECK(xrt_native_values_prepare(&execution->values,&billing)==XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
        xrt_billing_owner_fail_after(billing,allowance); XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        CHECK(outcome && !execution->stack.values && !execution->state && xrt_value_root_empty(&execution->values));
        XrRbcStatus status=xr_rbc_outcome_status(outcome); uint64_t bits=0;
        if (status==XR_RBC_OK) CHECK(xr_rbc_outcome_bits(outcome,&bits) && bits==1);
        else { XrXirValueStatus failure; uint32_t source; const XrtLocationOwner *locations;
            CHECK(status==XR_RBC_VALUE_FAILURE && xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations) && failure==XR_XIR_VALUE_OOM); ++failures; }
        xrt_billing_owner_fail_after(billing,SIZE_MAX); xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
        XrtBillingStats after=xrt_billing_owner_stats(billing); CHECK(after.blocks==base.blocks && after.bytes==base.bytes);
        xrt_billing_owner_drop(billing); CHECK(live_allocations==retained); if (status==XR_RBC_OK) break; CHECK(allowance<SIZE_MAX);
    }
    CHECK(failures); xrt_billing_owner_drop(control);
}
#endif
#if defined(XR_ARRAY_FORWARDING_ALLOC_CASES)
typedef struct ArrayForwardProbe { XrRbcExecution *execution; uint32_t index; size_t pc; unsigned mode; bool read,checked; } ArrayForwardProbe;
static void array_forwarding_probe_root(void *context) {
    ArrayForwardProbe *probe=context; XrRbcExecution *execution=probe->execution;
    const XrRbcModule *module=execution->module; XrRbcStack *stack=&execution->stack;
    CHECK(xr_rbc_values_begin(module,stack,0,0)==XR_RBC_OK);
    for (size_t pc=0;!probe->read && pc<probe->pc;++pc) {
        uint32_t word=module->functions[0].code[pc]; CHECK((word&65535u)==XR_RBC_VALUE_ACCESS);
        CHECK(xr_rbc_values_step(module,stack,word>>16,pc+1,0)==XR_RBC_OK);
    }
    RbcValueActivation *activation=stack->values->top; const XrRbcValueCode *code=activation->code;
    XrRbcValueAccess access=code->accesses[probe->index];
    const XrLirValueHome *home=&code->homes->values[probe->read ? access.result : access.source];
    const XrLirHomeBinding *binding=&code->homes->bindings[probe->read ? access.source : access.result];
    size_t source_slot=probe->read ? binding->first_owner : home->first_owner;
    size_t target_slot=probe->read ? home->first_owner : binding->first_owner;
    const XrtBackingTypes *types=NULL; CHECK(xrt_native_values_backings(xrt_value_root_current(0),&types)==XRT_MANAGED_OK);
    if (probe->read) CHECK(xrt_array_backing_i64_create(activation->frame,source_slot,types,NULL,0,0)==XRT_MANAGED_OK);
    XrtManagedSlotLocation from={0},to={0};
    CHECK(xrt_managed_slot_location(activation->frame,source_slot,&from)==XRT_MANAGED_OK &&
        xrt_managed_slot_location(activation->frame,target_slot,&to)==XRT_MANAGED_OK);
    XrtManagedHeader *incoming=*from.owner; CHECK(incoming && !*to.owner);
    XrtPlace source={.frame=activation->frame,.slot=source_slot,.nulls=XRT_PLACE_NONNULL};
    if (probe->mode==1) CHECK(xrt_place_copy_replace((XrtPlace){.frame=activation->frame,
        .slot=home->temporary_first_owner,.nulls=XRT_PLACE_NONNULL},source,NULL)==XRT_MANAGED_OK);
    if (probe->mode==2) atomic_store(&incoming->references,-1);
    if (probe->mode==3) atomic_store(&incoming->references,INT32_MAX);
    if (probe->mode==4) CHECK(xrt_array_backing_i64_create(activation->frame,target_slot,types,NULL,0,0)==XRT_MANAGED_OK);
    XrtManagedHeader *pending=NULL; size_t pending_slot=probe->mode==5 ? source_slot : target_slot;
    if (probe->mode==5 || probe->mode==6)
        CHECK(xrt_managed_slot_prepare(activation->frame,pending_slot,types->arrays[0],sizeof(XrtArrayBacking),&pending)==XRT_MANAGED_OK);
    if (probe->mode==7) CHECK(xrt_managed_domain_enter(from)==XRT_MANAGED_OK);
    if (probe->mode==8) CHECK(xrt_managed_slot_drop(activation->frame,source_slot)==XRT_MANAGED_OK);
    XrtManagedHeader *old_source=*from.owner,*old_target=*to.owner;
    int32_t references=old_source ? atomic_load(&old_source->references) : 0;
    if (probe->mode<2) {
        CHECK(xr_rbc_values_step(module,stack,probe->index,probe->pc+1,0)==XR_RBC_OK &&
            stack->frames[0].pc==probe->pc+2 && !*from.owner && *to.owner==incoming && atomic_load(&incoming->references)==references);
    } else {
        CHECK(!rbc_value_array_forward(module,activation,probe->index,probe->pc+1) && *from.owner==old_source && *to.owner==old_target);
        if (probe->mode==2 || probe->mode==4) {
            CHECK(xr_rbc_values_step(module,stack,probe->index,probe->pc+1,0)==XR_RBC_OK && stack->frames[0].pc==probe->pc+1 &&
                *from.owner==incoming && *to.owner==incoming && atomic_load(&incoming->references)==references+(references<0 ? -1 : 1));
            uint32_t word=module->functions[0].code[probe->pc+1];
            CHECK(xr_rbc_values_step(module,stack,word>>16,probe->pc+2,0)==XR_RBC_OK && !*from.owner &&
                *to.owner==incoming && atomic_load(&incoming->references)==references);
        } else CHECK(rbc_value_binding_step(module,stack,activation,access)==(probe->mode==3 ? XRT_MANAGED_REFCOUNT : XRT_MANAGED_ARGUMENT) &&
            *from.owner==old_source && *to.owner==old_target && (!old_source || atomic_load(&old_source->references)==references));
    }
    if (probe->mode==1) { XrtManagedSlotLocation saved={0};
        CHECK(xrt_managed_slot_location(activation->frame,home->temporary_first_owner,&saved)==XRT_MANAGED_OK && *saved.owner==incoming); }
    if (probe->mode==3) atomic_store(&incoming->references,1);
    if (probe->mode==5 || probe->mode==6) { CHECK((probe->mode==5 ? from.pending : to.pending)->value==pending);
        CHECK(xrt_managed_slot_abort(activation->frame,pending_slot)==XRT_MANAGED_OK); }
    if (probe->mode==7) xrt_managed_domain_leave(from);
    probe->checked=true;
}
#endif
#if defined(XR_ARRAY_FORWARDING_ALLOC_CASES)
static void array_forwarding_live_fallbacks(void) {
    for (unsigned mode=0;mode<18;++mode) {
        XrRbcProgram *program=array_forwarding_compile(array_forward_fixture(XR_LIR_I64,ARRAY_FORWARD_LOCAL));
        const XrRbcModule *module=xr_rbc_program_view(program); const XrRbcValueCode *code=xr_rbc_module_value_code(module,0);
        ArrayForwardProbe probe={.mode=mode%9,.read=mode>=9,.pc=SIZE_MAX};
        for (size_t pc=0;pc<module->functions[0].code_count;++pc) { uint32_t word=module->functions[0].code[pc];
            if ((word&65535u)==XR_RBC_VALUE_ACCESS && code->accesses[word>>16].operation==
                (probe.read ? XR_RBC_VALUE_BINDING_READ : XR_RBC_VALUE_BINDING_NEW)) {
                probe.pc=pc; probe.index=word>>16; break; } }
        CHECK(probe.pc!=SIZE_MAX && xr_rbc_program_prepare(program,NULL,0,&probe.execution)==XR_RBC_OK);
        xr_rbc_program_destroy(program); XrtExecutionFailure failure={0};
        CHECK(xrt_run_embedded_owned(probe.execution->pending->locations,&failure,array_forwarding_probe_root,&probe,
            &probe.execution->values)==XRT_EXECUTION_COMPLETED && probe.checked && xrt_value_root_empty(&probe.execution->values));
        xr_rbc_execution_free(&probe.execution); CHECK(!live_allocations);
    }
}
#endif
#if defined(XR_ARRAY_FORWARDING_ALLOC_CASES)
static void array_forwarding_allocation_cases(void) {
    array_forwarding_bytecode_cases(); CHECK(!live_allocations);
    array_forwarding_live_fallbacks();
    ArrayForwardFixture c=array_forward_fixture(XR_LIR_I64,ARRAY_FORWARD_SNAPSHOT);
    callable_allocation_failures(c.module,c.function,1,"Array forwarding snapshot");
    XrRbcProgram *program=array_forwarding_compile(array_forward_fixture(XR_LIR_I64,ARRAY_FORWARD_SNAPSHOT));
    CHECK(array_forwarding_encoded_sites(xr_rbc_program_view(program))==2);
    array_forwarding_native_failures(program); array_forwarding_run_retired(program,NULL,0,1); CHECK(!live_allocations);
}
#endif
#endif // LIR_ARRAY_FORWARDING_BYTECODE_CASES_H
