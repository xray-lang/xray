/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_direct_owner_cases.h - Real child REF calls and action recheck
 *
 * KEY CONCEPT:
 *   The literal passes complete construction, specialization and lowering.
 *   Native controls attack metadata or replace actuals; no permission is supplied.
 */
#ifndef XIR_INVOCATION_DIRECT_OWNER_CASES_H
#define XIR_INVOCATION_DIRECT_OWNER_CASES_H
#include "xir/xxir_task.h"
#include "xir/xxir_instance_function_internal.h"
typedef struct DirectRuntimeFixture {
    XrXirTypeNode nodes[3];XrXirTypes types;XrXirCallableParameter ref;
    XrXirInstruction init[4],run[6],worker[5],body[4],apply[2];
    XrXirBlock blocks[7];uint32_t actual,worker_actuals[2];XrXirType cell,apply_parameters[2];
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;XrXirSlot slot;XrXirDeclarations declarations;XrXirModule module;
} DirectRuntimeFixture;
typedef struct DirectRuntimeWitness DirectRuntimeWitness;
typedef struct DirectRuntimeEnvironment { DirectRuntimeWitness *owner;uint32_t source; } DirectRuntimeEnvironment;
struct DirectRuntimeWitness {
    unsigned mode,releases,body_calls;
    uint32_t entries[5],producer_instruction,call_instruction,indirect_instruction;
    XrXirType cell,task,callback;
    XrXirValue module_cell;
    XrXirDomain *domain;
    DirectRuntimeEnvironment environments[16];
};
typedef struct DirectRuntimeFrame { unsigned phase;XrXirValue cell,function,extra,task,arguments[2]; } DirectRuntimeFrame;
static void direct_runtime_fixture(DirectRuntimeFixture *f) {
    memset(f,0,sizeof(*f));f->cell=(XrXirType)256;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->ref=(XrXirCallableParameter){f->cell,XR_PARAM_REF};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .parameters=&f->ref,.parameter_count=1,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64};
    f->types=(XrXirTypes){f->nodes,3,NULL,NULL};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=40};
    f->init[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=f->cell};
    f->init[2]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={1,0}};
    f->init[3]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->run[0]=(XrXirInstruction){.op=XR_XIR_GO,.type=(XrXirType)258,.immediate=2};
    f->run[1]=(XrXirInstruction){.op=XR_XIR_TASK_AWAIT,.targets={1,2}};
    f->run[2]=(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64,.immediate=1};
    f->run[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->run[4]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=1};
    f->run[5]=(XrXirInstruction){.op=XR_XIR_THROW,.args={4,0}};
    f->worker[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=40};
    f->worker[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=f->cell};
    f->worker[2]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.immediate=3};
    f->worker[3]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=4,.args={0,2}};
    f->worker[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};f->actual=1;
    f->body[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64};
    f->body[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->body[2]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={0,2}};
    f->body[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->blocks[0]=(XrXirBlock){.count=4};f->blocks[1]=(XrXirBlock){.count=2};
    f->blocks[2]=(XrXirBlock){.first=2,.count=2};f->blocks[3]=(XrXirBlock){.first=4,.count=2};
    f->blocks[4]=(XrXirBlock){.count=5};f->blocks[5]=(XrXirBlock){.count=4};
    const char *names[4]={"init","run","worker","body"};
    XrXirInstruction *ops[4]={f->init,f->run,f->worker,f->body};uint32_t counts[4]={4,6,5,4};
    for (uint32_t i=0;i<4;++i)
        f->functions[i]=(XrXirFunction){.name=names[i],.name_length=(uint32_t)strlen(names[i]),
            .result=i?XR_XIR_I64:XR_XIR_UNIT,.blocks=&f->blocks[i<2?i:i+2],.block_count=i==1?3:1,
            .instructions=ops[i],.instruction_count=counts[i]};
    f->worker_actuals[0]=2;f->worker_actuals[1]=1;
    f->functions[2].operands=f->worker_actuals;f->functions[2].operand_count=2;
    f->apply_parameters[0]=(XrXirType)257;f->apply_parameters[1]=f->cell;f->actual=1;
    f->apply[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.args={0,1}};
    f->apply[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};f->blocks[6]=(XrXirBlock){.count=2};
    f->functions[4]=(XrXirFunction){.name="apply",.name_length=5,.parameters=f->apply_parameters,.parameter_count=2,
        .result=XR_XIR_I64,.blocks=&f->blocks[6],.block_count=1,.instructions=f->apply,.instruction_count=2,
        .operands=&f->actual,.operand_count=1};
    f->functions[3].parameters=&f->cell;f->functions[3].parameter_count=1;
    f->identities[1].exported=1;f->source=(XrXirSourceModule){"runtime-edge",12,NULL,0,0};
    f->slot=(XrXirSlot){0,f->cell,1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .slots=&f->slot,.slot_count=1,.entry_function=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=5,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}
static bool direct_runtime_resource(XrXirCallStatus status) {
    return status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT;
}
static XrXirAction direct_runtime_fault(XrXirCallStatus status) {
    return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
}
static void direct_runtime_release(XrXirCallView *view,XrXirCallStatus reason) {
    (void)reason;DirectRuntimeFrame *f=view->state;
    xr_xir_value_drop(&f->task);xr_xir_value_drop(&f->extra);
    xr_xir_value_drop(&f->function);xr_xir_value_drop(&f->cell);
}
static void direct_runtime_code_drop(void *owner) { ++((DirectRuntimeWitness *)owner)->releases; }
static XrXirCallStatus direct_runtime_select(XrXirCallView *view,DirectRuntimeWitness *w,
    DirectRuntimeFrame *frame,const XrXirValue *function,const XrXirValue *cell,uint32_t *entry) {
    frame->arguments[0]=*function;frame->arguments[1]=*cell;
    XrXirCallStatus status=xr_xir_instance_call_at(view,w->call_instruction,w->entries[4],frame->arguments,2);
    if (status==XR_XIR_CALL_READY) *entry=w->entries[4];
    return status;
}
static XrXirCallStatus direct_runtime_probe(XrXirCallView *view,DirectRuntimeWitness *w,
    DirectRuntimeFrame *frame,uint32_t *entry) {
    const XrXirValue *function=&frame->function,*cell=&frame->cell;
    const XrXirFunctionBinding *binding=xr_xir_function_binding(function);
    CHECK(binding && !binding->capture_count);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    if (w->mode==1)
        status=xr_xir_instance_function(view,w->callback,binding->entry,NULL,0,&frame->extra);
    else if (w->mode==2)
        status=xr_xir_instance_weaken_function(view,w->callback,function,&frame->extra);
    if (status!=XR_XIR_CALL_READY) return status;
    if (w->mode==1 || w->mode==2) { CHECK(!xr_xir_function_producer(&frame->extra));function=&frame->extra; }
    if (w->mode==4) cell=&w->module_cell;
    if (w->mode==7) {
        XrXirCallView wrong=*view;uint32_t masked=UINT32_MAX;
        CHECK(direct_runtime_select(&wrong,w,frame,function,cell,&masked)==
            XR_XIR_CALL_BAD_STATE && masked==UINT32_MAX);
        uint32_t call=w->call_instruction;w->call_instruction=w->producer_instruction;
        status=direct_runtime_select(view,w,frame,function,cell,&masked);w->call_instruction=call;
        if (direct_runtime_resource(status)) return status;
        CHECK(status==XR_XIR_CALL_BAD_STATE && masked==UINT32_MAX);
    }
    XirFunctionProducer *producer=(XirFunctionProducer *)xr_xir_function_producer(function);
    XirFunctionProducer saved={0};
    if (w->mode==3) { CHECK(producer);saved=*producer;producer->site=UINT32_MAX; }
    status=direct_runtime_select(view,w,frame,function,cell,entry);
    if (w->mode==3) *producer=saved;
    if (w->mode==6 && status==XR_XIR_CALL_READY) {
        uint32_t masked=UINT32_MAX;
        XrXirCallStatus repeated=direct_runtime_select(view,w,frame,function,cell,&masked);
        if (direct_runtime_resource(repeated)) return repeated;
        CHECK(repeated==XR_XIR_CALL_BAD_STATE && masked==UINT32_MAX);
    }
    return status;
}
static XrXirAction direct_runtime_resume(XrXirCallView *view) {
    DirectRuntimeEnvironment *environment=view->environment;DirectRuntimeWitness *w=environment->owner;
    DirectRuntimeFrame *frame=view->state;XrXirCallStatus status=XR_XIR_CALL_READY;
    if (!environment->source) {
        if (!w->domain) {
            w->domain=xr_xir_call_admission(view)->domain;
            if (!xr_xir_domain_retain(w->domain)) { w->domain=NULL;return direct_runtime_fault(XR_XIR_CALL_LIMIT); }
        }
        const XrXirValue initial={XR_XIR_I64,0,40};
        status=xr_xir_instance_cell(view,w->cell,&initial,&frame->cell);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_slot_write(view,0,&frame->cell,true);
        if (status==XR_XIR_CALL_READY && xr_xir_value_copy(&frame->cell,&w->module_cell)!=XR_XIR_VALUE_OK)
            status=XR_XIR_CALL_LIMIT;
        return status==XR_XIR_CALL_READY?(XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{0},{0},0}:direct_runtime_fault(status);
    }
    if (environment->source==1) {
        if (!frame->phase++) {
            status=xr_xir_task_go(view,w->task,w->entries[2],NULL,0,&frame->task);
            if (status!=XR_XIR_CALL_READY) return direct_runtime_fault(status);
            return (XrXirAction){XR_XIR_ACTION_AWAIT_TASK,0,NULL,0,frame->task,{0},0};
        }
        if (view->inbox.status!=XR_XIR_CALL_RETURNED) return direct_runtime_fault(view->inbox.status);
        return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,view->inbox.value,{0},0};
    }
    if (environment->source==3) {
        ++w->body_calls;
        const XirEffectInvocationSelection *actual=xr_xir_call_invocation(view);
        CHECK(actual && actual->target==w->entries[3] && actual->instruction==w->indirect_instruction);
        XrXirValue read={0},next={XR_XIR_I64,0,42};
        status=xr_xir_instance_cell_read(view,&view->arguments[0],&read);
        if (status==XR_XIR_CALL_READY) { CHECK(read.payload==40);status=xr_xir_instance_cell_write(view,&view->arguments[0],&next); }
        xr_xir_value_drop(&read);
        return status==XR_XIR_CALL_READY?(XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,next,{0},0}:direct_runtime_fault(status);
    }
    if (environment->source==4) {
        const XirEffectInvocationSelection *actual=xr_xir_call_invocation(view);
        CHECK(actual && actual->target==w->entries[4] && actual->instruction==w->call_instruction);
        if (frame->phase++) return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,view->inbox.value,{0},0};
        uint32_t entry=UINT32_MAX;
        status=xr_xir_instance_resolve_function_at(view,w->indirect_instruction,&view->arguments[0],
            &view->arguments[1],1,&entry);
        if (status!=XR_XIR_CALL_READY) return direct_runtime_fault(status);
        return (XrXirAction){XR_XIR_ACTION_CALL,entry,&view->arguments[1],1,view->arguments[0],{0},0};
    }
    if (frame->phase++) return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,view->inbox.value,{0},0};
    const XrXirValue initial={XR_XIR_I64,0,40};
    status=xr_xir_instance_cell(view,w->cell,&initial,&frame->cell);
    if (status==XR_XIR_CALL_READY)
        status=xr_xir_instance_function_at(view,w->producer_instruction,NULL,0,&frame->function);
    uint32_t entry=UINT32_MAX;
    if (status==XR_XIR_CALL_READY) status=direct_runtime_probe(view,w,frame,&entry);
    if (status!=XR_XIR_CALL_READY) { CHECK(entry==UINT32_MAX || direct_runtime_resource(status));return direct_runtime_fault(status); }
    if (w->mode==5) {
        xr_xir_value_drop(&frame->cell);
        if (xr_xir_value_copy(&w->module_cell,&frame->cell)!=XR_XIR_VALUE_OK) return direct_runtime_fault(XR_XIR_CALL_LIMIT);
    }
    if (w->mode==5) frame->arguments[1]=frame->cell;
    return (XrXirAction){XR_XIR_ACTION_CALL,entry,frame->arguments,2,{0},{0},0};
}
static XrXirStatus direct_runtime_seal(DirectRuntimeWitness *w,XrXirProgram **output) {
    NativeFixtureOwner owner={0};DirectRuntimeFixture fixture;direct_runtime_fixture(&fixture);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    XrXirStatus status=native_fixture_owner_new(&owner);
    if (status==XR_XIR_OK) status=xir_fixture_check(&owner.context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(checked,&closed,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(closed,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(lowered,NULL);
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        CHECK(module->function_count<=16 && module->provenance && module->provenance->origins);
        XrXirCallEntry entries[16]={0};
        for (uint32_t f=0;f<module->function_count;++f) {
            uint32_t source=module->provenance->origins[f].function;CHECK(source<5);
            w->environments[f]=(DirectRuntimeEnvironment){w,source};
            entries[f]=(XrXirCallEntry){XR_XIR_CALL_ABI_VERSION,module->functions[f].parameters,
                module->functions[f].parameter_count,module->functions[f].result,sizeof(DirectRuntimeFrame),
                direct_runtime_resume,direct_runtime_release,&w->environments[f],0,0};
        }
        w->entries[0]=module->declarations->modules[0].initializer;
        w->entries[1]=module->declarations->entry_function;
        CHECK(w->entries[1]<module->function_count);
        const XrXirFunction *run=&module->functions[w->entries[1]];unsigned go=0,sites=0,calls=0;
        for (uint32_t i=0;i<run->instruction_count;++i) {
            const XrXirInstruction *op=&run->instructions[i];
            if (op->op!=XR_XIR_GO) continue;
            CHECK(op->immediate>=0 && (uint64_t)op->immediate<module->function_count);
            ++go;w->entries[2]=(uint32_t)op->immediate;w->task=op->type;
        }
        CHECK(go==1);const XrXirFunction *worker=&module->functions[w->entries[2]];
        for (uint32_t i=0;i<worker->instruction_count;++i) {
            const XrXirInstruction *op=&worker->instructions[i];
            if (op->op==XR_XIR_FUNCTION_REF) {
                CHECK(op->immediate>=0 && (uint64_t)op->immediate<module->function_count);
                ++sites;w->producer_instruction=i;w->entries[3]=(uint32_t)op->immediate;w->callback=op->type;
            }
            if (op->op==XR_XIR_CALL) {
                CHECK(op->immediate>=0 && (uint64_t)op->immediate<module->function_count);
                ++calls;w->call_instruction=i;w->entries[4]=(uint32_t)op->immediate;
            }
        }
        CHECK(sites==1 && calls==1);const XrXirFunction *apply=&module->functions[w->entries[4]];
        unsigned indirect=0;
        for (uint32_t i=0;i<apply->instruction_count;++i)
            if (apply->instructions[i].op==XR_XIR_CALL_INDIRECT) { ++indirect;w->indirect_instruction=i; }
        CHECK(indirect==1);
        CHECK(module->functions[w->entries[3]].parameter_count==1);
        w->cell=module->functions[w->entries[3]].parameters[0];
        for (uint32_t p=0;p<5;++p) CHECK(module->provenance->origins[w->entries[p]].function==p);
        XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,entries,module->function_count,
            module->declarations,{w,direct_runtime_code_drop},module->types,xr_xir_compile_program_proof(lowered)};
        status=xr_xir_compile_program_seal(&owner.context,&spec,output);
    }
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    memset(&fixture,0,sizeof(fixture));native_fixture_owner_free(&owner);return status;
}
static bool invocation_direct_owner_run(unsigned mode,const XrXirInstanceConfig *limits,
    XrXirDomainBudgetStats *cost) {
    DirectRuntimeWitness witness={0};witness.mode=mode;XrXirProgram *program=NULL;
    XrXirStatus sealed=direct_runtime_seal(&witness,&program);
    if (sealed!=XR_XIR_OK) { CHECK(sealed==XR_XIR_OUT_OF_MEMORY && !program);return false; }
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    if (limits) config=*limits;
    XrXirInstance *instance=NULL;XrXirValue result={0};bool complete=false;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    xr_xir_compile_program_drop(program);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,witness.entries[1],NULL,0);
    bool polled=status==XR_XIR_CALL_READY;
    if (polled) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (!direct_runtime_resource(status)) {
        XrXirCallStatus expected=mode>=1 && mode<=5?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_RETURNED;
        CHECK(status==expected);
        if (status==XR_XIR_CALL_RETURNED) {
            CHECK(witness.body_calls==1 && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
            CHECK(result.type==XR_XIR_I64 && result.payload==42);
        } else CHECK(!witness.body_calls);
        complete=true;
    }
    XrXirCallStatus released=xr_xir_instance_free(instance);
    if (polled && status!=XR_XIR_CALL_RETURNED) CHECK(released==status);
    else if (polled) {
        CHECK(released==XR_XIR_CALL_READY || (limits && released==XR_XIR_CALL_LIMIT));
        if (released!=XR_XIR_CALL_READY) complete=false;
    } else {
        /* A rejected preparation has no accepted execution fault. A genuine
         * exhausted budget remains observable during physical shutdown. */
        CHECK(released==XR_XIR_CALL_READY || (status==XR_XIR_CALL_LIMIT && released==XR_XIR_CALL_LIMIT));
    }
    xr_xir_value_drop(&result);xr_xir_value_drop(&witness.module_cell);
    if (witness.domain) {
        if (cost) *cost=xr_xir_domain_budget_stats(witness.domain);
        CHECK(!xr_xir_domain_stats(witness.domain).live_bytes);
        xr_xir_domain_drop(witness.domain);
    }
    CHECK(witness.releases==1);return complete;
}
static void invocation_direct_owner_axes(void) {
    XrXirDomainBudgetStats cost={0};CHECK(invocation_direct_owner_run(0,NULL,&cost));
    const uint64_t exact[5]={cost.requested_bytes,cost.requested_call_bytes,cost.work,cost.metadata_peak,cost.call_peak};
    for (unsigned axis=0;axis<5;++axis) for (unsigned minus=0;minus<2;++minus) {
        CHECK(exact[axis]);XrXirInstanceConfig limits;
        CHECK(xr_xir_instance_config_init(&limits,sizeof(limits))==XR_XIR_CALL_READY);
        if (!axis) limits.requested_value_limit=exact[axis]-minus;
        else if (axis==1) limits.requested_call_limit=exact[axis]-minus;
        else if (axis==2) limits.work_limit=exact[axis]-minus;
        else if (axis==3) limits.metadata_limit=exact[axis]-minus;
        else limits.call_limit=exact[axis]-minus;
        CHECK(invocation_direct_owner_run(0,&limits,NULL)==!minus);
    }
}
#endif // XIR_INVOCATION_DIRECT_OWNER_CASES_H
