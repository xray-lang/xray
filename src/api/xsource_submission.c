/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_submission.c - Exact Source decisions over one cumulative Instance
 */
#include "xsource_submission.h"
#include "../base/xchecks.h"
#include "../program/xr_xir_source_product.h"
#include "../xir/xxir_generic.h"
#include "../xir/xxir_specialization_bindings.h"
#include "../xir/xxir_vm.h"
#include <stdio.h>

typedef struct SubmissionBirth {
    struct SubmissionBirth *previous;
    uint64_t source, runtime;
} SubmissionBirth;
struct XrSourceSubmission {
    XrCompileState *state;
    XrXirSourceGenerations *source;
    XrXirInstance *runtime;
    XrXirTarget target;
    uint64_t source_serial, runtime_serial;
    XrXirInstanceResult observed;
    bool busy, has_observed, taken;
    SubmissionBirth *pending_birth, *published_births;
};
static XrSourceStatus submission_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_SOURCE_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_SOURCE_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_SOURCE_OUT_OF_MEMORY:XR_SOURCE_BAD_ARGUMENT;
}
static XrSourceStatus submission_xir(XrXirStatus status) {
    return status==XR_XIR_OK?XR_SOURCE_OK:status==XR_XIR_BUDGET?XR_SOURCE_BUDGET:
        status==XR_XIR_OUT_OF_MEMORY?XR_SOURCE_OUT_OF_MEMORY:status==XR_XIR_BAD_STAGE?XR_SOURCE_BAD_STATE:XR_SOURCE_REJECTED;
}
static XrSourceStatus submission_call(XrXirCallStatus status) {
    switch(status) {
    case XR_XIR_CALL_READY:case XR_XIR_CALL_CANCEL_REQUESTED:return XR_SOURCE_OK;
    case XR_XIR_CALL_OOM:return XR_SOURCE_OUT_OF_MEMORY;
    case XR_XIR_CALL_LIMIT:return XR_SOURCE_LIMIT;
    case XR_XIR_CALL_BUSY:return XR_SOURCE_BUSY;
    case XR_XIR_CALL_BAD_ARGUMENT:case XR_XIR_CALL_BAD_ABI:return XR_SOURCE_BAD_ARGUMENT;
    case XR_XIR_CALL_BAD_STATE:case XR_XIR_CALL_CONSUMED:return XR_SOURCE_BAD_STATE;
    default:return XR_SOURCE_EXECUTION_FAILED;
    }
}
static XrSourceStatus submission_capture(const XrSourceSubmission *owner,XrSourceStatus status) {
    if(status==XR_SOURCE_BUDGET || status==XR_SOURCE_OUT_OF_MEMORY)
        return submission_resource(xr_compile_state_fail(owner->state,status==XR_SOURCE_BUDGET?
            XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OUT_OF_MEMORY));
    return status;
}
static void submission_clear(XrSourceSubmission *owner) {
    owner->source_serial=owner->runtime_serial=0;
    owner->observed=(XrXirInstanceResult){0};owner->has_observed=owner->taken=false;
}
static XrXirCallStatus submission_abort(XrSourceSubmission *owner) {
    XrXirCallStatus status=XR_XIR_CALL_READY;
    if(owner->runtime_serial)status=xr_xir_instance_generation_abort(owner->runtime,owner->runtime_serial);
    if(status==XR_XIR_CALL_BUSY)return status;
    if(owner->source_serial) {
        XrXirStatus source=xr_xir_compile_source_generation_abort(owner->source,owner->source_serial);
        XR_CHECK(source==XR_XIR_OK,"the coordinator owns the exact pending Source record");
    }
    xr_compile_state_free(owner->pending_birth);owner->pending_birth=NULL;
    submission_clear(owner);return status;
}
XR_FUNC XrSourceStatus xr_source_submission_new(const XrSourceSubmissionConfig *config,XrSourceSubmission **output) {
    if(!config || !config->state || !output || *output || !config->source.context.resources ||
        xr_compile_state_resources(config->state)!=config->source.context.resources ||
        config->target.architecture!=XR_XIR_ARCH_X86_64 || config->target.abi_version!=XR_XIR_VALUE_ABI_VERSION)
        return XR_SOURCE_BAD_ARGUMENT;
    XrSourceSubmission *owner=NULL;
    XrSourceStatus status=submission_resource(xr_compile_state_calloc(config->state,1,sizeof(*owner),(void **)&owner));
    if(status!=XR_SOURCE_OK)return status;
    status=submission_resource(xr_compile_state_retain(config->state));
    if(status!=XR_SOURCE_OK){xr_compile_state_free(owner);return status;}
    owner->state=config->state;owner->target=config->target;
    status=submission_capture(owner,submission_xir(xr_xir_compile_source_generations_new(&config->source,&owner->source)));
    if(status==XR_SOURCE_OK)status=submission_call(xr_xir_instance_generation_new(&config->runtime,&owner->runtime));
    if(status==XR_SOURCE_OK)*output=owner;
    else {xr_xir_compile_source_generations_free(owner->source);xr_compile_state_release(owner->state);xr_compile_state_free(owner);}
    return status;
}
typedef struct SubmissionBindingDraft {
    XrSourceSubmission *owner;
    const XrXirArtifact *checked;
    const XrXirSourceGenerationBinding *names;
    size_t name_count;
    XrXirInstanceBindingSpec *rows;
    uint32_t count;
    bool received;
    const char *failure;
    uint8_t *reader_keys;
    uint32_t readers;
} SubmissionBindingDraft;
#include "xsource_submission_bindings.inc.c"
static XrSourceStatus submission_program(XrSourceSubmission *owner,XrXirProgram **output,uint32_t *entry,
    SubmissionBindingDraft *bindings,
    XrXirSourceDiagnostic *diagnostic) {
    XrXirSourceGenerationView view={0};
    XrXirStatus status=xr_xir_compile_source_generation_view(owner->source,owner->source_serial,&view);
    const char *stage="Source candidate view";
    XrXirArtifact *closed=NULL,*lowered=NULL;
    if(status==XR_XIR_OK) {
        stage="Source candidate lexical selectors";
        bindings->owner=owner;bindings->checked=view.result->checked;
        status=xr_xir_compile_source_generation_candidate_bindings(owner->source,owner->source_serial,
            &bindings->names,&bindings->name_count);
    }
    XrXirSpecializationReceiver receiver={bindings,submission_receive_bindings};
    if(status==XR_XIR_OK) {
        stage="Source specialization";status=xr_xir_compile_specialize_receive(view.result->checked,&receiver,&closed,NULL);
        if(status!=XR_XIR_OK && bindings->received)stage=bindings->failure ? bindings->failure : "Source ordinary declaration mapping";
    }
    if(status==XR_XIR_OK) {stage="Source physical lowering";status=xr_xir_compile_lower(closed,&owner->target,&lowered,NULL);}
    if(status==XR_XIR_OK) {stage="Source physical declaration correspondence";status=submission_lowered_bindings(bindings,closed,lowered);}
    xr_xir_compile_artifact_free(closed);
    if(status==XR_XIR_OK) {stage="Source public Program admission";status=xr_xir_compile_source_lowered_public_admit(lowered,diagnostic);}
    if(status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        const XrXirDeclarations *declarations=module?module->declarations:NULL;
        if(!declarations || declarations->entry_function>=module->function_count ||
            module->functions[declarations->entry_function].parameter_count ||
            module->functions[declarations->entry_function].result!=XR_XIR_I64)status=XR_XIR_BAD_STRUCTURE;
        else *entry=declarations->entry_function;
    }
    if(status==XR_XIR_OK) {stage="Source Program transfer";status=xr_xir_compile_vm_program_take(&lowered,output);}
    if(status==XR_XIR_OK) {
        stage="Source sealed Program correspondence";
        status=xr_xir_compile_source_generation_bind_program(owner->source,owner->source_serial,*output);
    }
    xr_xir_compile_artifact_free(lowered);
    if(diagnostic) {
        diagnostic->status=status;
        if(status!=XR_XIR_OK && !diagnostic->message[0])
            (void)snprintf(diagnostic->message,sizeof(diagnostic->message),"%s failed",stage);
    }
    return submission_capture(owner,submission_xir(status));
}
XR_FUNC XrSourceStatus xr_source_submission_prepare(XrSourceSubmission *owner,const XrXirSourceText *text,
    XrXirSourceDiagnostic *diagnostic) {
    if(!owner || !text)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(owner->source_serial)return XR_SOURCE_BAD_STATE;
    XrSourceStatus status=submission_resource(xr_compile_state_status(owner->state));
    if(status!=XR_SOURCE_OK)return status;
    owner->busy=true;
    status=submission_capture(owner,submission_xir(xr_xir_compile_source_generation_prepare_bindings(owner->source,text,&owner->source_serial,diagnostic)));
    if(status==XR_SOURCE_OK)status=submission_capture(owner,submission_resource(xr_compile_state_calloc(owner->state,
        1,sizeof(*owner->pending_birth),(void **)&owner->pending_birth)));
    XrXirProgram *program=NULL;uint32_t entry=UINT32_MAX;
    SubmissionBindingDraft bindings={0};
    if(status==XR_SOURCE_OK)status=submission_program(owner,&program,&entry,&bindings,diagnostic);
    if(status==XR_SOURCE_OK)status=submission_call(xr_xir_source_generation_prepare_instance(owner->source,
        owner->source_serial,owner->runtime,&owner->runtime_serial));
    xr_xir_compile_program_drop(program);
    if(status==XR_SOURCE_OK && bindings.count)status=submission_call(xr_xir_instance_generation_bind(owner->runtime,
        owner->runtime_serial,bindings.rows,bindings.count));
    xr_compile_state_free(bindings.rows);
    if(status==XR_SOURCE_OK) {
        owner->pending_birth->source=owner->source_serial;owner->pending_birth->runtime=owner->runtime_serial;
    }
    if(status==XR_SOURCE_OK)status=submission_call(xr_xir_instance_start(owner->runtime,entry,NULL,0));
    if(status!=XR_SOURCE_OK) {
        XrXirCallStatus cleanup=submission_abort(owner);
        if(cleanup!=XR_XIR_CALL_READY)status=submission_call(cleanup);
    }
    owner->busy=false;return status;
}
XR_FUNC XrSourceStatus xr_source_submission_poll(XrSourceSubmission *owner,uint64_t quantum,XrXirInstanceResult *output) {
    if(!owner || !output || !quantum)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(!owner->runtime_serial || owner->taken)return XR_SOURCE_BAD_STATE;
    owner->busy=true;XrXirInstanceResult result=xr_xir_instance_poll_bounded(owner->runtime,quantum);owner->busy=false;
    if(result.outcome.status==XR_XIR_CALL_BUSY)return XR_SOURCE_BUSY;
    owner->observed=result;owner->has_observed=true;*output=result;return XR_SOURCE_OK;
}
XR_FUNC XrSourceStatus xr_source_submission_wait(const XrSourceSubmission *owner,uint64_t epoch,uint64_t wake,XrXirWaitRequest *output) {
    if(!owner || !output)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(!owner->runtime_serial)return XR_SOURCE_BAD_STATE;
    return submission_call(xr_xir_instance_wait_request(owner->runtime,epoch,wake,output));
}
XR_FUNC XrSourceStatus xr_source_submission_resume(XrSourceSubmission *owner,uint64_t epoch,uint64_t wake) {
    if(!owner)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(!owner->runtime_serial)return XR_SOURCE_BAD_STATE;
    owner->busy=true;XrSourceStatus status=submission_call(xr_xir_instance_resume(owner->runtime,epoch,wake));owner->busy=false;
    if(status==XR_SOURCE_OK)owner->has_observed=false;return status;
}
XR_FUNC XrSourceStatus xr_source_submission_cancel(XrSourceSubmission *owner) {
    if(!owner)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(!owner->runtime_serial)return XR_SOURCE_BAD_STATE;
    owner->busy=true;XrSourceStatus status=submission_call(xr_xir_instance_cancel_current(owner->runtime));owner->busy=false;
    if(status==XR_SOURCE_OK)owner->has_observed=false;return status;
}
XR_FUNC XrSourceStatus xr_source_submission_take(XrSourceSubmission *owner,XrXirCallResult *output) {
    if(!owner || !output)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(!owner->runtime_serial || !owner->has_observed || owner->taken ||
        owner->observed.outcome.status==XR_XIR_CALL_READY || owner->observed.outcome.status==XR_XIR_CALL_SUSPENDED)return XR_SOURCE_BAD_STATE;
    owner->busy=true;XrXirValueStatus copied=xr_xir_call_result_copy(&owner->observed.outcome,output);owner->busy=false;
    if(copied==XR_XIR_VALUE_OK){owner->taken=true;return XR_SOURCE_OK;}
    return copied==XR_XIR_VALUE_OOM?XR_SOURCE_OUT_OF_MEMORY:copied==XR_XIR_VALUE_LIMIT?XR_SOURCE_LIMIT:XR_SOURCE_BAD_ARGUMENT;
}
XR_FUNC XrSourceStatus xr_source_submission_publish(XrSourceSubmission *owner) {
    if(!owner)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(!owner->source_serial || !owner->has_observed || owner->observed.outcome.status!=XR_XIR_CALL_RETURNED)return XR_SOURCE_BAD_STATE;
    XrXirSourceGenerationView view={0};
    XrXirStatus source=xr_xir_compile_source_generation_view(owner->source,owner->source_serial,&view);
    if(source!=XR_XIR_OK || view.published)return XR_SOURCE_BAD_STATE;
    owner->busy=true;
    XrXirCallStatus published=xr_xir_instance_generation_publish(owner->runtime,owner->runtime_serial);
    if(published==XR_XIR_CALL_READY) {
        source=xr_xir_compile_source_generation_publish(owner->source,owner->source_serial);
        XR_CHECK(source==XR_XIR_OK,"allocation-free commit publishes the private exact Source candidate");
        owner->pending_birth->previous=owner->published_births;owner->published_births=owner->pending_birth;
        owner->pending_birth=NULL;
        submission_clear(owner);
    }
    owner->busy=false;return submission_call(published);
}
XR_FUNC XrSourceStatus xr_source_submission_abort(XrSourceSubmission *owner,XrXirCallStatus *cleanup) {
    if(!owner)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    if(!owner->source_serial)return XR_SOURCE_BAD_STATE;
    owner->busy=true;XrXirCallStatus status=submission_abort(owner);owner->busy=false;
    if(status!=XR_XIR_CALL_BUSY && cleanup)*cleanup=status;return submission_call(status);
}
XR_FUNC XrSourceStatus xr_source_submission_lookup(const XrSourceSubmission *owner,const char *name,XrXirSourceGenerationBinding *output) {
    if(!owner || !name || !output)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    XrSourceStatus available=submission_resource(xr_compile_state_status(owner->state));
    if(available!=XR_SOURCE_OK)return available;
    return submission_capture(owner,submission_xir(xr_xir_compile_source_generation_lookup(owner->source,name,output)));
}
XR_FUNC XrSourceStatus xr_source_submission_lookup_runtime(const XrSourceSubmission *owner,const char *name,
    const XrXirInstanceBinding **output) {
    if(!owner || !name || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy)return XR_SOURCE_BUSY;
    XrSourceStatus status=submission_resource(xr_compile_state_status(owner->state));
    if(status!=XR_SOURCE_OK)return status;
    XrXirSourceGenerationBinding source={0};
    status=submission_capture(owner,submission_xir(xr_xir_compile_source_generation_lookup(owner->source,name,&source)));
    if(status!=XR_SOURCE_OK)return status;
    const SubmissionBirth *birth=owner->published_births;
    while(birth && birth->source!=source.generation) {
        if(xr_compile_state_work(owner->state,1)!=XR_COMPILE_RESOURCE_OK)
            return submission_resource(xr_compile_state_status(owner->state));
        birth=birth->previous;
    }
    if(!birth)return XR_SOURCE_BAD_STATE;
    size_t length=0;
    status=submission_resource(xr_compile_state_string_length(owner->state,name,&length));
    if(status!=XR_SOURCE_OK)return status;
    if(!length || length>UINT32_MAX)return XR_SOURCE_BAD_ARGUMENT;
    const XrXirInstanceBinding *binding=NULL;
    status=submission_call(xr_xir_instance_binding_lookup(owner->runtime,name,(uint32_t)length,&binding));
    if(status!=XR_SOURCE_OK)return status;
    XrXirInstanceBindingView view={0};
    status=submission_call(xr_xir_instance_binding_view(owner->runtime,binding,&view));
    if(status!=XR_SOURCE_OK)return status;
    if(view.generation!=birth->runtime)return XR_SOURCE_BAD_STATE;
    *output=binding;return XR_SOURCE_OK;
}
static XrSourceStatus submission_read_binding(XrSourceSubmission *owner,const char *name,XrXirValue *output) {
    XrXirSourceGenerationBinding selected={0};
    XrSourceStatus status=submission_capture(owner,submission_xir(
        xr_xir_compile_source_generation_lookup(owner->source,name,&selected)));
    if(status!=XR_SOURCE_OK)return status;
    XrXirSourceGenerationDefinition definition={0};
    status=submission_capture(owner,submission_xir(xr_xir_compile_source_generation_definition(owner->source,
        selected.generation,name,&definition)));
    if(status!=XR_SOURCE_OK)return status;
    if(definition.reader==UINT32_MAX)return XR_SOURCE_BAD_STATE;
    const SubmissionBirth *birth=owner->published_births;
    while(birth && birth->source!=selected.generation) {
        if(xr_compile_state_work(owner->state,1)!=XR_COMPILE_RESOURCE_OK)
            return submission_resource(xr_compile_state_status(owner->state));
        birth=birth->previous;
    }
    if(!birth)return XR_SOURCE_BAD_STATE;
    uint8_t key[16];status=submission_xir(submission_reader_key(owner,selected.generation,definition.reader,key));
    if(status!=XR_SOURCE_OK)return status;
    const XrXirInstanceBinding *binding=NULL;
    status=submission_call(xr_xir_instance_binding_lookup(owner->runtime,(const char *)key,sizeof(key),&binding));
    if(status!=XR_SOURCE_OK)return status;
    XrXirInstanceBindingView view={0};
    status=submission_call(xr_xir_instance_binding_view(owner->runtime,binding,&view));
    if(status!=XR_SOURCE_OK)return status;
    if(view.generation!=birth->runtime || view.kind!=XR_XIR_INSTANCE_BINDING_FUNCTION || view.parameter_count)
        return XR_SOURCE_BAD_STATE;
    XrXirCallStatus started=xr_xir_instance_binding_start(owner->runtime,binding,NULL,0);
    if(started!=XR_XIR_CALL_READY)return submission_call(started);
    XrXirCallStatus terminal;
    do {terminal=xr_xir_instance_poll_bounded(owner->runtime,256).outcome.status;} while(terminal==XR_XIR_CALL_READY);
    XR_CHECK(terminal!=XR_XIR_CALL_SUSPENDED,"a verified binding reader cannot suspend");
    if(terminal!=XR_XIR_CALL_RETURNED)return submission_call(terminal);
    XrXirCallStatus taken=xr_xir_instance_take_result(owner->runtime,output);
    return taken==XR_XIR_CALL_RETURNED ? XR_SOURCE_OK : submission_call(taken);
}
XR_FUNC XrSourceStatus xr_source_submission_read(XrSourceSubmission *owner,const char *name,XrXirValue *output) {
    if(!owner || !name || !output || output->type || output->reserved || output->payload)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy || owner->source_serial)return XR_SOURCE_BUSY;
    XrSourceStatus status=submission_resource(xr_compile_state_status(owner->state));
    if(status!=XR_SOURCE_OK)return status;
    owner->busy=true;status=submission_read_binding(owner,name,output);owner->busy=false;return status;
}
XR_FUNC XrSourceStatus xr_source_submission_drop(XrSourceSubmission **output,XrXirCallStatus *cleanup) {
    if(!output)return XR_SOURCE_BAD_ARGUMENT;
    if(!*output){if(cleanup)*cleanup=XR_XIR_CALL_READY;return XR_SOURCE_OK;}
    XrSourceSubmission *owner=*output;if(owner->busy)return XR_SOURCE_BUSY;
    owner->busy=true;XrXirCallStatus status=owner->source_serial?submission_abort(owner):XR_XIR_CALL_READY;
    if(status==XR_XIR_CALL_BUSY){owner->busy=false;return XR_SOURCE_BUSY;}
    XrXirCallStatus retired=xr_xir_instance_free(owner->runtime);
    if(retired==XR_XIR_CALL_BUSY){owner->busy=false;return XR_SOURCE_BUSY;}
    if(status==XR_XIR_CALL_READY)status=retired;
    xr_xir_compile_source_generations_free(owner->source);
    while(owner->published_births) {
        SubmissionBirth *previous=owner->published_births->previous;
        xr_compile_state_free(owner->published_births);owner->published_births=previous;
    }
    XrCompileState *state=owner->state;xr_compile_state_free(owner);xr_compile_state_release(state);*output=NULL;
    if(cleanup)*cleanup=status;return submission_call(status);
}
