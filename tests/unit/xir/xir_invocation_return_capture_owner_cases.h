/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_return_capture_owner_cases.h - Real returned captures, dead factory frames and action recheck
 *
 * KEY CONCEPT:
 *   The literal passes complete construction, specialization and lowering.
 *   Native controls attack metadata or replace actuals; no permission is supplied.
 */
#ifndef XIR_INVOCATION_RETURN_CAPTURE_OWNER_CASES_H
#define XIR_INVOCATION_RETURN_CAPTURE_OWNER_CASES_H
#include "xir/xxir_task.h"
#include "xir/xxir_instance_function_internal.h"
typedef struct ReturnedCaptureRuntimeFixture {
    XrXirTypeNode nodes[3];XrXirTypes types;XrXirCallableParameter ref;
    XrXirInstruction init[4],run[6],worker[3],body[4],factory[4];
    XrXirBlock blocks[7];uint32_t actual;XrXirType cell,capture_type;
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;XrXirSlot slot;XrXirDeclarations declarations;XrXirModule module;
} ReturnedCaptureRuntimeFixture;
typedef struct ReturnedCaptureRuntimeWitness ReturnedCaptureRuntimeWitness;
typedef struct ReturnedCaptureRuntimeEnvironment { ReturnedCaptureRuntimeWitness *owner;uint32_t source; } ReturnedCaptureRuntimeEnvironment;
struct ReturnedCaptureRuntimeWitness {
    unsigned mode,releases,body_calls,factory_releases;bool scalar;
    uint32_t entries[5],producer_instruction,call_instruction;
    XrXirType cell,task,callback;
    XrXirValue module_cell;
    XrXirDomain *domain;
    ReturnedCaptureRuntimeEnvironment environments[16];
};
typedef struct ReturnedCaptureRuntimeFrame { unsigned phase;XrXirValue cell,function,extra,task; } ReturnedCaptureRuntimeFrame;
static void returned_capture_runtime_fixture(ReturnedCaptureRuntimeFixture *f,bool scalar) {
    memset(f,0,sizeof(*f));f->cell=(XrXirType)256;f->capture_type=scalar?XR_XIR_I64:f->cell;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->ref=(XrXirCallableParameter){f->cell,XR_PARAM_REF};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_NONE};
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
    f->worker[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=(XrXirType)257,.immediate=4};
    f->worker[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64};
    f->worker[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=40};
    f->factory[1]=(XrXirInstruction){.op=scalar?XR_XIR_COPY:XR_XIR_CELL_NEW,.type=f->capture_type};
    f->factory[2]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.immediate=3,.args={0,1}};
    f->factory[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};f->actual=1;
    f->body[0]=(XrXirInstruction){.op=scalar?XR_XIR_COPY:XR_XIR_CELL_READ,.type=XR_XIR_I64};
    f->body[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->body[2]=scalar?(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={2,0}}:
        (XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={0,2}};
    f->body[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->blocks[0]=(XrXirBlock){.count=4};f->blocks[1]=(XrXirBlock){.count=2};
    f->blocks[2]=(XrXirBlock){.first=2,.count=2};f->blocks[3]=(XrXirBlock){.first=4,.count=2};
    f->blocks[4]=(XrXirBlock){.count=3};f->blocks[5]=(XrXirBlock){.count=4};
    f->blocks[6]=(XrXirBlock){.count=4};
    const char *names[5]={"init","run","worker","body","factory"};
    XrXirInstruction *ops[5]={f->init,f->run,f->worker,f->body,f->factory};uint32_t counts[5]={4,6,3,4,4};
    for (uint32_t i=0;i<5;++i)
        f->functions[i]=(XrXirFunction){.name=names[i],.name_length=(uint32_t)strlen(names[i]),
            .result=i==4?(XrXirType)257:i?XR_XIR_I64:XR_XIR_UNIT,.blocks=&f->blocks[i<2?i:i+2],.block_count=i==1?3:1,
            .instructions=ops[i],.instruction_count=counts[i]};
    f->functions[4].operands=&f->actual;f->functions[4].operand_count=1;
    f->functions[3].parameters=&f->capture_type;f->functions[3].parameter_count=1;
    f->identities[1].exported=1;f->source=(XrXirSourceModule){"runtime-edge",12,NULL,0,0};
    f->slot=(XrXirSlot){0,f->cell,1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .slots=&f->slot,.slot_count=1,.entry_function=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=5,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}
static bool returned_capture_runtime_resource(XrXirCallStatus status) {
    return status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT;
}
static XrXirAction returned_capture_runtime_fault(XrXirCallStatus status) {
    return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
}
static void returned_capture_runtime_release(XrXirCallView *view,XrXirCallStatus reason) {
    (void)reason;ReturnedCaptureRuntimeFrame *f=view->state;
    ReturnedCaptureRuntimeEnvironment *environment=view->environment;
    if (environment->source==4) ++environment->owner->factory_releases;
    xr_xir_value_drop(&f->task);xr_xir_value_drop(&f->extra);
    xr_xir_value_drop(&f->function);xr_xir_value_drop(&f->cell);
}
static void returned_capture_runtime_code_drop(void *owner) { ++((ReturnedCaptureRuntimeWitness *)owner)->releases; }
static XrXirCallStatus returned_capture_runtime_replace_capture(ReturnedCaptureRuntimeWitness *w,
    const XrXirValue *function) {
    XrXirFunctionBinding *binding=(XrXirFunctionBinding *)xr_xir_function_binding(function);
    CHECK(binding && binding->capture_count==1);
    XrXirValue replacement={0};
    if (xr_xir_value_copy(&w->module_cell,&replacement)!=XR_XIR_VALUE_OK) return XR_XIR_CALL_LIMIT;
    XrXirValue *capture=(XrXirValue *)binding->captures;
    xr_xir_value_drop(capture);*capture=replacement;return XR_XIR_CALL_READY;
}
static XrXirCallStatus returned_capture_runtime_probe(XrXirCallView *view,ReturnedCaptureRuntimeWitness *w,
    ReturnedCaptureRuntimeFrame *frame,uint32_t *entry) {
    const XrXirValue *function=&frame->function;
    const XrXirFunctionBinding *binding=xr_xir_function_binding(function);
    CHECK(binding && binding->capture_count==1);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    if (w->mode==1)
        status=xr_xir_instance_function(view,w->callback,binding->entry,binding->captures,1,&frame->extra);
    else if (w->mode==2)
        status=xr_xir_instance_weaken_function(view,w->callback,function,&frame->extra);
    if (status!=XR_XIR_CALL_READY) return status;
    if (w->mode==1 || w->mode==2) { CHECK(!xr_xir_function_producer(&frame->extra));function=&frame->extra; }
    if (w->mode==4) {
        status=returned_capture_runtime_replace_capture(w,function);
        if (status!=XR_XIR_CALL_READY) return status;
    }
    if (w->mode==7) {
        XrXirCallView wrong=*view;uint32_t masked=UINT32_MAX;
        CHECK(xr_xir_instance_resolve_function_at(&wrong,w->call_instruction,function,NULL,0,&masked)==
            XR_XIR_CALL_BAD_STATE && masked==UINT32_MAX);
        status=xr_xir_instance_resolve_function_at(view,w->producer_instruction,function,NULL,0,&masked);
        if (returned_capture_runtime_resource(status)) return status;
        CHECK(status==XR_XIR_CALL_BAD_STATE && masked==UINT32_MAX);
    }
    XirFunctionProducer *producer=(XirFunctionProducer *)xr_xir_function_producer(function);
    XirFunctionProducer saved={0};
    if (w->mode==3) { CHECK(producer);saved=*producer;producer->site=UINT32_MAX; }
    status=xr_xir_instance_resolve_function_at(view,w->call_instruction,function,NULL,0,entry);
    if (w->mode==3) *producer=saved;
    if (w->mode==6 && status==XR_XIR_CALL_READY) {
        uint32_t masked=UINT32_MAX;
        XrXirCallStatus repeated=xr_xir_instance_resolve_function_at(view,w->call_instruction,function,NULL,0,&masked);
        if (returned_capture_runtime_resource(repeated)) return repeated;
        CHECK(repeated==XR_XIR_CALL_BAD_STATE && masked==UINT32_MAX);
    }
    return status;
}
static XrXirAction returned_capture_runtime_resume(XrXirCallView *view) {
    ReturnedCaptureRuntimeEnvironment *environment=view->environment;ReturnedCaptureRuntimeWitness *w=environment->owner;
    ReturnedCaptureRuntimeFrame *frame=view->state;XrXirCallStatus status=XR_XIR_CALL_READY;
    if (!environment->source) {
        if (!w->domain) {
            w->domain=xr_xir_call_admission(view)->domain;
            if (!xr_xir_domain_retain(w->domain)) { w->domain=NULL;return returned_capture_runtime_fault(XR_XIR_CALL_LIMIT); }
        }
        const XrXirValue initial={XR_XIR_I64,0,40};
        status=xr_xir_instance_cell(view,w->cell,&initial,&frame->cell);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_slot_write(view,0,&frame->cell,true);
        if (status==XR_XIR_CALL_READY && xr_xir_value_copy(&frame->cell,&w->module_cell)!=XR_XIR_VALUE_OK)
            status=XR_XIR_CALL_LIMIT;
        return status==XR_XIR_CALL_READY?(XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{0},{0},0}:returned_capture_runtime_fault(status);
    }
    if (environment->source==1) {
        if (!frame->phase++) {
            status=xr_xir_task_go(view,w->task,w->entries[2],NULL,0,&frame->task);
            if (status!=XR_XIR_CALL_READY) return returned_capture_runtime_fault(status);
            return (XrXirAction){XR_XIR_ACTION_AWAIT_TASK,0,NULL,0,frame->task,{0},0};
        }
        if (view->inbox.status!=XR_XIR_CALL_RETURNED) return returned_capture_runtime_fault(view->inbox.status);
        return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,view->inbox.value,{0},0};
    }
    if (environment->source==3) {
        CHECK(w->factory_releases==1);++w->body_calls;
        const XirEffectInvocationSelection *actual=xr_xir_call_invocation(view);
        CHECK(actual && actual->target==w->entries[3] && actual->instruction==w->call_instruction);
        XrXirValue read={0},next={XR_XIR_I64,0,42};
        if (w->scalar) CHECK(view->arguments[0].type==XR_XIR_I64 && view->arguments[0].payload==40);
        else {
            status=xr_xir_instance_cell_read(view,&view->arguments[0],&read);
            if (status==XR_XIR_CALL_READY) { CHECK(read.payload==40);status=xr_xir_instance_cell_write(view,&view->arguments[0],&next); }
        }
        xr_xir_value_drop(&read);
        return status==XR_XIR_CALL_READY?(XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,next,{0},0}:returned_capture_runtime_fault(status);
    }
    if (environment->source==4) {
        const XrXirValue initial={XR_XIR_I64,0,40};
        if (w->scalar) frame->cell=initial;
        else status=xr_xir_instance_cell(view,w->cell,&initial,&frame->cell);
        if (status==XR_XIR_CALL_READY)
            status=xr_xir_instance_function_at(view,w->producer_instruction,&frame->cell,1,&frame->function);
        return status==XR_XIR_CALL_READY?
            (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,frame->function,{0},0}:returned_capture_runtime_fault(status);
    }
    if (!frame->phase++) return (XrXirAction){XR_XIR_ACTION_CALL,w->entries[4],NULL,0,{0},{0},0};
    if (view->inbox.status!=XR_XIR_CALL_RETURNED) return returned_capture_runtime_fault(view->inbox.status);
    if (frame->phase>2) return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,view->inbox.value,{0},0};
    CHECK(w->factory_releases==1 && xr_xir_function_binding(&view->inbox.value));
    if (xr_xir_value_copy(&view->inbox.value,&frame->function)!=XR_XIR_VALUE_OK)
        return returned_capture_runtime_fault(XR_XIR_CALL_LIMIT);
    uint32_t entry=UINT32_MAX;
    if (status==XR_XIR_CALL_READY) status=returned_capture_runtime_probe(view,w,frame,&entry);
    if (status!=XR_XIR_CALL_READY) { CHECK(entry==UINT32_MAX || returned_capture_runtime_resource(status));return returned_capture_runtime_fault(status); }
    if (w->mode==5) {
        status=returned_capture_runtime_replace_capture(w,&frame->function);
        if (status!=XR_XIR_CALL_READY) return returned_capture_runtime_fault(status);
    }
    return (XrXirAction){XR_XIR_ACTION_CALL,entry,NULL,0,frame->function,{0},0};
}
static XrXirStatus returned_capture_runtime_seal(ReturnedCaptureRuntimeWitness *w,XrXirProgram **output) {
    NativeFixtureOwner owner={0};ReturnedCaptureRuntimeFixture fixture;returned_capture_runtime_fixture(&fixture,w->scalar);
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
            w->environments[f]=(ReturnedCaptureRuntimeEnvironment){w,source};
            entries[f]=(XrXirCallEntry){XR_XIR_CALL_ABI_VERSION,module->functions[f].parameters,
                module->functions[f].parameter_count,module->functions[f].result,sizeof(ReturnedCaptureRuntimeFrame),
                returned_capture_runtime_resume,returned_capture_runtime_release,&w->environments[f],0,0};
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
        CHECK(go==1);const XrXirFunction *worker=&module->functions[w->entries[2]];unsigned factories=0;
        for (uint32_t i=0;i<worker->instruction_count;++i) {
            const XrXirInstruction *op=&worker->instructions[i];
            if (op->op==XR_XIR_CALL) {
                CHECK(op->immediate>=0 && (uint64_t)op->immediate<module->function_count);
                ++factories;w->entries[4]=(uint32_t)op->immediate;
            }
            if (op->op==XR_XIR_CALL_INDIRECT) { ++calls;w->call_instruction=i; }
        }
        CHECK(factories==1 && calls==1);
        const XrXirFunction *factory=&module->functions[w->entries[4]];
        for (uint32_t i=0;i<factory->instruction_count;++i) {
            const XrXirInstruction *op=&factory->instructions[i];
            if (op->op!=XR_XIR_FUNCTION_REF) continue;
            CHECK(op->immediate>=0 && (uint64_t)op->immediate<module->function_count);
            ++sites;w->producer_instruction=i;w->entries[3]=(uint32_t)op->immediate;w->callback=op->type;
        }
        CHECK(sites==1 && factory->result==w->callback);
        CHECK(module->functions[w->entries[3]].parameter_count==1);
        w->cell=module->declarations->slots[0].type;
        for (uint32_t p=0;p<5;++p) CHECK(module->provenance->origins[w->entries[p]].function==p);
        XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,entries,module->function_count,
            module->declarations,{w,returned_capture_runtime_code_drop},module->types,xr_xir_compile_program_proof(lowered)};
        status=xr_xir_compile_program_seal(&owner.context,&spec,output);
    }
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    memset(&fixture,0,sizeof(fixture));native_fixture_owner_free(&owner);return status;
}
static bool invocation_return_capture_owner_run(unsigned mode,bool scalar,const XrXirInstanceConfig *limits,
    XrXirDomainBudgetStats *cost) {
    ReturnedCaptureRuntimeWitness witness={0};witness.mode=mode;witness.scalar=scalar;XrXirProgram *program=NULL;
    XrXirStatus sealed=returned_capture_runtime_seal(&witness,&program);
    if (sealed!=XR_XIR_OK) { CHECK(sealed==XR_XIR_OUT_OF_MEMORY && !program);return false; }
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    if (limits) config=*limits;
    XrXirInstance *instance=NULL;XrXirValue result={0};bool complete=false;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    xr_xir_compile_program_drop(program);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,witness.entries[1],NULL,0);
    bool polled=status==XR_XIR_CALL_READY;
    if (polled) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (!returned_capture_runtime_resource(status)) {
        XrXirCallStatus expected=mode>=1 && mode<=5?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_RETURNED;
        CHECK(status==expected);
        if (status==XR_XIR_CALL_RETURNED) {
            CHECK(witness.factory_releases==1 && witness.body_calls==1 && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
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
static void invocation_return_capture_owner_axes(bool scalar) {
    XrXirDomainBudgetStats cost={0};CHECK(invocation_return_capture_owner_run(0,scalar,NULL,&cost));
    const uint64_t exact[5]={cost.requested_bytes,cost.requested_call_bytes,cost.work,cost.metadata_peak,cost.call_peak};
    for (unsigned axis=0;axis<5;++axis) for (unsigned minus=0;minus<2;++minus) {
        CHECK(exact[axis]);XrXirInstanceConfig limits;
        CHECK(xr_xir_instance_config_init(&limits,sizeof(limits))==XR_XIR_CALL_READY);
        if (!axis) limits.requested_value_limit=exact[axis]-minus;
        else if (axis==1) limits.requested_call_limit=exact[axis]-minus;
        else if (axis==2) limits.work_limit=exact[axis]-minus;
        else if (axis==3) limits.metadata_limit=exact[axis]-minus;
        else limits.call_limit=exact[axis]-minus;
        CHECK(invocation_return_capture_owner_run(0,scalar,&limits,NULL)==!minus);
    }
}
#endif // XIR_INVOCATION_RETURN_CAPTURE_OWNER_CASES_H
