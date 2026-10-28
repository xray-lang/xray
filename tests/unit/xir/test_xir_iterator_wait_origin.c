/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_iterator_wait_origin.c - Genuine parked consumer instruction facts
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_call_internal.h"
#include "xir/xxir_task_internal.h"
#include "xir/xxir_generator_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec generator_consumption_program;
XR_DATA const uint32_t generator_consumption_entry;
static const XrXirCallEntry *wait_selected[64];
static const XrXirModule *wait_module;
static XrXirCall *wait_call,*wait_previous_call;
static XrXirTaskWaitToken wait_previous_token;
static XrXirValue wait_value;
static uint32_t wait_caller,wait_instruction,wait_steps[2],wait_reads,wait_busy;
static bool wait_native[64],wait_attack;
static XrXirValue wait_attack_value;
static uint32_t wait_attack_producer,wait_attack_count,wait_generator_steps;
static const XrXirProgram *wait_program;
static uint32_t wait_context_reads;
static void wait_producer_context(XrXirCallView *view,uint32_t entry) {
    CHECK(wait_value.type && wait_program && wait_program->permissions);
    const XirGenerator *generator=(const XirGenerator *)(uintptr_t)wait_value.payload;
    CHECK(generator->call==view->activation);
    const XirEffectInvocationSelection *context=xr_xir_call_invocation(view);
    CHECK(context && context->owner==wait_program->permissions->effects && context->target==entry &&
        context->producer==generator->producer && context->caller==wait_caller &&
        context->instruction==wait_instruction && context->node!=UINT32_MAX && context->edge!=UINT32_MAX);
    const XrXirProgramPermissions *permissions=wait_program->permissions;uint32_t creator=UINT32_MAX;
    for(uint32_t f=0;f<permissions->function_count;++f) {
        const XrXirProgramPermission *owner=&permissions->entries[f];
        for(uint32_t i=0;i<owner->generator_count;++i)
            if(permissions->generators[owner->generator_begin+i].site==generator->producer) {
                CHECK(creator==UINT32_MAX);creator=f;
            }
    }
    CHECK(creator!=UINT32_MAX && creator!=context->caller);++wait_context_reads;
}
static XrXirAction wait_resume(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);CHECK(entry<64 && wait_selected[entry]);
    ++wait_steps[wait_native[entry]?1:0];
    if(wait_selected[entry]->flags&XR_XIR_ENTRY_GENERATOR) {
        ++wait_generator_steps;wait_producer_context(view,entry);
    }
    if(view->activation==wait_previous_call) {
        XirIteratorWaitContext output;memset(&output,0xa5,sizeof(output));XirIteratorWaitContext before=output;
        CHECK(xr_xir_call_iterator_context(view->activation,&wait_previous_token,&output)==XR_XIR_CALL_BUSY);
        CHECK(!memcmp(&output,&before,sizeof(output)));++wait_busy;
    }
    XrXirAction action=wait_selected[entry]->resume(view);
    if(action.kind==XR_XIR_ACTION_AWAIT_ITERATOR && action.callee!=XR_XIR_WAIT_ITERATOR_CLOSE) {
        CHECK(!wait_call && xr_xir_call_invocation_pending(view,&wait_instruction));
        CHECK(entry<wait_module->function_count && wait_instruction<wait_module->functions[entry].instruction_count);
        uint32_t op=wait_module->functions[entry].instructions[wait_instruction].op;
        CHECK(op==(uint32_t)(action.callee==XR_XIR_WAIT_ITERATOR_NEXT?XR_XIR_ITERATOR_NEXT:
            action.callee==XR_XIR_WAIT_ITERATOR_HAS_NEXT?XR_XIR_ITERATOR_HAS_NEXT:XR_XIR_ITERATOR_NTH));
        if(wait_attack) {
            CHECK(!wait_attack_count && xr_xir_value_copy(&action.value,&wait_attack_value)==XR_XIR_VALUE_OK);
            XirGenerator *generator=(XirGenerator *)(uintptr_t)wait_attack_value.payload;
            wait_attack_producer=generator->producer;generator->producer=UINT32_MAX;++wait_attack_count;
        } else {wait_call=view->activation;wait_caller=entry;wait_value=action.value;}
    }
    return action;
}
static void wait_inspect(XrXirInstance *instance) {
    if(!wait_call)return;
    XrXirTaskWaitToken token={0};CHECK(xr_xir_call_iterator_wait_token(wait_call,&token));
    CHECK(((const XirTaskActivation *)token.binding.activation)->task);
    XirIteratorWaitContext facts={0};CHECK(xr_xir_call_iterator_context(wait_call,&token,&facts)==XR_XIR_CALL_READY);
    CHECK(facts.instance==instance && facts.admission && facts.admission->domain && facts.iterator &&
        facts.iterator->type==wait_value.type && facts.iterator->payload==wait_value.payload &&
        facts.caller==wait_caller && facts.instruction==wait_instruction);
    if(facts.invocation)CHECK(facts.invocation->owner && facts.invocation->target==wait_caller);
    const XrXirTaskExecutor *executor=token.binding.owner;
    CHECK(xr_xir_task_executor_pending_member(executor,wait_call,&token,facts.admission)==XR_XIR_CALL_READY);
    XirTaskActivation *activation=token.binding.activation;uint64_t ticket=activation->ticket;
    ++activation->ticket;
    CHECK(xr_xir_task_executor_pending_member(executor,wait_call,&token,facts.admission)==XR_XIR_CALL_BAD_STATE);
    activation->ticket=ticket;
    CHECK(xr_xir_task_executor_pending_member(executor,wait_call,&token,facts.admission)==XR_XIR_CALL_READY);
    XrXirCall *owned_call=activation->call;activation->call=NULL;
    CHECK(xr_xir_task_executor_pending_member(executor,wait_call,&token,facts.admission)==XR_XIR_CALL_BAD_STATE);
    activation->call=owned_call;
    CHECK(xr_xir_task_executor_pending_member(executor,wait_call,&token,facts.admission)==XR_XIR_CALL_READY);
    puts("ITERATOR_MEMBER actual owned activation ticket/call attacks and restored positives, no wait shim PASS");
    for(uint32_t attack=0;attack<11;++attack) {
        XrXirTaskWaitToken changed=token;
        switch(attack) {
        case 0:++changed.wake;break;
        case 1:changed.request.kind=changed.request.kind==XR_XIR_WAIT_ITERATOR_NEXT?XR_XIR_WAIT_ITERATOR_NTH:XR_XIR_WAIT_ITERATOR_NEXT;break;
        case 2:changed.request.reserved=1;break;
        case 3:++changed.request.after_ms;break;
        case 4:changed.request.subject=NULL;break;
        case 5:++changed.request.generation;break;
        case 6:++changed.request.ticket;break;
        case 7:changed.binding.owner=NULL;break;
        case 8:changed.binding.activation=NULL;break;
        case 9:++changed.binding.generation;break;
        default:++changed.binding.ticket;break;
        }
        XirIteratorWaitContext output;memset(&output,0xa5,sizeof(output));XirIteratorWaitContext before=output;
        CHECK(xr_xir_call_iterator_context(wait_call,&changed,&output)==XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(&output,&before,sizeof(output)));
    }
    XirIteratorWaitContext restored={0};CHECK(xr_xir_call_iterator_context(wait_call,&token,&restored)==XR_XIR_CALL_READY);
    CHECK(restored.caller==facts.caller && restored.instruction==facts.instruction && restored.iterator==facts.iterator);
    wait_previous_call=wait_call;wait_previous_token=token;wait_call=NULL;++wait_reads;
}
static void wait_execute(XrXirProgram *program,bool attack) {
    wait_call=NULL;wait_previous_call=NULL;wait_previous_token=(XrXirTaskWaitToken){0};
    wait_program=program;wait_context_reads=0;
    wait_steps[0]=wait_steps[1]=wait_reads=wait_busy=wait_generator_steps=0;
    wait_attack=attack;wait_attack_count=0;wait_attack_value=(XrXirValue){0};
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,generator_consumption_entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(uint32_t tick=0;tick<1000000 && status!=XR_XIR_CALL_RETURNED;++tick) {
        status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
        CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_SUSPENDED || status==XR_XIR_CALL_RETURNED || (attack && status==XR_XIR_CALL_BAD_STATE));
        if(attack && status==XR_XIR_CALL_BAD_STATE)break;wait_inspect(instance);
    }
    if(attack) {
        CHECK(status==XR_XIR_CALL_BAD_STATE && wait_attack_count==1 && !wait_generator_steps && wait_attack_value.type);
        XirGenerator *generator=(XirGenerator *)(uintptr_t)wait_attack_value.payload;
        generator->producer=wait_attack_producer;xr_xir_value_drop(&wait_attack_value);
        /* Shutdown preserves this authenticated first driver failure. */
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_BAD_STATE && !wait_generator_steps);return;
    }
    CHECK(status==XR_XIR_CALL_RETURNED && wait_reads && wait_busy && !wait_call &&
        wait_context_reads && wait_context_reads==wait_generator_steps);
    printf("ITERATOR_EXECUTION_CONTEXT: %u actual producer callbacks use the real consumer edge, distinct creation caller PASS\n",wait_context_reads);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==17);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
int main(int argc,char **argv) {
    CHECK(argc==2);bool native=!strcmp(argv[1],"native"),vm_only=!strcmp(argv[1],"vm");
    bool root_native=!strcmp(argv[1],"native-vm");CHECK(native || vm_only || root_native || !strcmp(argv[1],"vm-native"));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);const XrXirProgramSpec *original=&generator_consumption_program;
    CHECK(original->entry_count<=64);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,original->proof.bytes,original->proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_lower(closed,&original->target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    wait_module=xr_xir_compile_artifact_module(lowered);CHECK(wait_module);
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *vm=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&vm)==XR_XIR_OK);XrXirCallEntry entries[64];
    for(uint32_t i=0;i<original->entry_count;++i) {
        wait_native[i]=native || (!vm_only && ((i%2)==(root_native?0u:1u)));
        wait_selected[i]=wait_native[i]?&original->entries[i]:&vm[i];entries[i]=*wait_selected[i];entries[i].resume=wait_resume;
    }
    XrXirProgramSpec spec=*original;spec.entries=entries;XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
    wait_execute(program,false);
    CHECK(native?wait_steps[1] && !wait_steps[0]:vm_only?wait_steps[0] && !wait_steps[1]:wait_steps[0] && wait_steps[1]);
    printf("ITERATOR_WAIT_ORIGIN %s: %u real parked reads,11 token attacks/restores each,%u busy refusals,VM%u/native%u,returned17/physical0 PASS\n",
        argv[1],wait_reads,wait_busy,wait_steps[0],wait_steps[1]);
    wait_execute(program,true);xr_xir_compile_program_drop(program);
    xr_compile_resources_free(vm);xr_compile_resources_free(bindings);xr_xir_compile_artifact_free(lowered);
    source_fixture_owner_free(&owner);CHECK(!runtime_live && !runtime_bytes);
    printf("ITERATOR_JOINT_REJECT %s: genuine object forged producer refused before any generator callback, restored owner and physical0 PASS\n",argv[1]);return 0;
}
