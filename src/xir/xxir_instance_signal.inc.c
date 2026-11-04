/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_signal.inc.c - Root-owned signal registrations and deferred delivery
 */
#include "../os/os_signal.h"
struct XirSignalRegistration {
    XirSignalRegistration *next;
    XrXirInstance *generation;
    XrOsSignalOwner owner;
    XrXirValue function;
    uint64_t epoch;
    int64_t signal;
    bool closing,pending;
};
static XrXirCallStatus instance_signal_status(XrOsIoStatus status) {
    switch(status) {
    case XR_OS_IO_OK:return XR_XIR_CALL_READY;
    case XR_OS_IO_OUT_OF_MEMORY:return XR_XIR_CALL_OOM;
    case XR_OS_IO_OVERFLOW:case XR_OS_IO_MEMORY_LIMIT:return XR_XIR_CALL_LIMIT;
    case XR_OS_IO_CANCELLED:return XR_XIR_CALL_CANCELLED;
    case XR_OS_IO_UNSUPPORTED:return XR_XIR_CALL_UNSUPPORTED;
    default:return XR_XIR_CALL_HOST_ERROR;
    }
}
static void instance_signal_unlink(XrXirInstance *root,XirSignalRegistration *registration) {
    XR_CHECK(!registration->owner.token,"only a discharged signal owner can retire");
    XirSignalRegistration **link=&root->signals;
    while (*link && *link!=registration) link=&(*link)->next;
    XR_CHECK(*link==registration,"signal retirement requires its actual Root owner");
    *link=registration->next;
    xr_xir_value_drop(&registration->function);
    xr_xir_domain_metadata_deallocate(root->domain,registration,sizeof(*registration));
}
static bool instance_signal_close(XrXirInstance *root,XirSignalRegistration *registration) {
    registration->closing=true;registration->pending=false;
    if (xr_os_io_signal_close(&registration->owner)!=XR_OS_IO_OK) return false;
    instance_signal_unlink(root,registration);return true;
}
static bool instance_signals_close(XrXirInstance *root) {
    XR_CHECK(root==instance_root(root) && root->driving,"signal close retains the busy Root owner");
    bool closed=true;
    for (XirSignalRegistration *registration=root->signals;registration;) {
        XirSignalRegistration *next=registration->next;
        if (!instance_signal_close(root,registration)) closed=false;
        registration=next;
    }
    return closed;
}
/* The generic native reader authenticates the generated canonical declaration,
 * including its complete term metadata. Before that reader admits registration,
 * this predicate is false for every native entry and no Source path is opened. */
static bool instance_signal_native_target(const XrXirProgram *program,uint32_t target) {
    if (!program || target>=program->entry_count || !program->declarations ||
        !program->declarations->functions || !xir_sys_native_scope(program->declarations,target,program->entry_count))
        return false;
    const XrXirProgramPermissions *permissions=program->permissions;
    if (!permissions || !permissions->input || !permissions->effects || !permissions->entries ||
        permissions->function_count!=program->entry_count || !permissions->entries[target].requires_root ||
        !(program->declarations->functions[target].promises&XR_XIR_FUNCTION_NO_SUSPEND)) return false;
    const XrXirCallEntry *entry=&program->entries[target];
    uint32_t id=program->declarations->functions[target].native_callable;
    if (id!=XR_NATIVE_CALLABLE_SYS_ON_SIGNAL || !xir_sys_native_id(id) || entry->resume!=xr_xir_instance_native_callable ||
        entry->release!=xr_xir_instance_native_callable_release || entry->parameter_count!=2 ||
        !entry->parameters || entry->parameters[0]!=XR_XIR_I64 || entry->result!=XR_XIR_BOOL ||
        entry->state_bytes!=sizeof(XrXirValue) || entry->environment || entry->flags || entry->cleanup_owner ||
        !xir_native_callable_types(id,program->types,entry->parameters,2,XR_XIR_BOOL,XR_XIR_NEVER)) return false;
    const XrXirTypeNode *handler=xr_xir_callable_signature(program->types,entry->parameters[1]);
    return handler && !handler->parameter_count && handler->result==XR_XIR_UNIT &&
        handler->element==XR_XIR_NEVER && handler->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED;
}
static XrXirCallStatus instance_signal_site(XrXirCallView *view,XrXirInstance *instance,
    const XirCallSignalOrigin *origin,const XrXirValue *arguments,uint32_t count) {
    XrXirInstance *root=instance_root(instance),*active=instance_current(root);
    const XrXirProgram *program=instance->program;
    const XrXirProgramPermissions *permissions=program->permissions;
    if (root->stopping || instance->stopping || root->prepared_generation || active!=instance ||
        (view->activation!=active->call && !xir_instance_callback_owns(root,active->executor,view->activation)) ||
        view_is_child(instance,view) ||
        view->phase!=XR_XIR_CALL_NORMAL || view->scope_exit ||
        !permissions || !permissions->input || !permissions->input->artifact ||
        !permissions->effects || !instance_signal_native_target(program,origin->target) || count!=2 || !arguments)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirArtifact *artifact=permissions->input->artifact;
    const XrXirModule *module=&artifact->module;
    if (module->stage!=XR_XIR_LOWERED || module->function_count!=program->entry_count ||
        !module->functions || artifact->program_final!=permissions->effects ||
        origin->caller>=module->function_count || origin->instruction>=module->functions[origin->caller].instruction_count)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirInstruction *op=&module->functions[origin->caller].instructions[origin->instruction];
    if (op->op!=XR_XIR_CALL || op->immediate!=(int64_t)origin->target || op->args[1]!=2)
        return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || admission->context!=instance || admission->arena!=program->arena ||
        admission->domain!=instance->domain) return XR_XIR_CALL_BAD_STATE;
    if (admission->work<64 || !xr_xir_domain_work(instance->domain,64)) return XR_XIR_CALL_LIMIT;
    admission->work-=64;
    for (uint32_t p=0;p<2;++p) {
        XrXirType parameter=program->entries[origin->target].parameters[p];
        /* Staging precedes the ordinary Call's owned parameter conversion. */
        XrXirCallStatus status=arguments[p].type!=(uint32_t)parameter &&
            xr_xir_callable_signature(program->types,(XrXirType)arguments[p].type) &&
            xr_xir_callable_signature(program->types,parameter)?
            xr_xir_instance_argument_weakening_admit(view,parameter,arguments+p):
            admit_instance_value(admission,arguments+p,parameter);
        if (status!=XR_XIR_CALL_READY) return status;
    }
    XrXirInstance *birth=NULL;
    XrXirCallStatus status=instance_function_state(root,arguments+1,&birth);
    if (status!=XR_XIR_CALL_READY) return status;
    if (birth!=instance) return XR_XIR_CALL_BAD_STATE;
    XirEffectProducerView producer={0};
    status=instance_function_producer_value(instance,admission,arguments+1,&producer);
    if (status!=XR_XIR_CALL_READY) return status;
    uint32_t target=0;
    status=resolve_function(instance,arguments+1,&target,admission);
    if (status!=XR_XIR_CALL_READY) return status;
    const XrXirTypeNode *handler=xr_xir_callable_signature(program->types,(XrXirType)arguments[1].type);
    if (!handler || handler->parameter_count || handler->result!=XR_XIR_UNIT || handler->element!=XR_XIR_NEVER ||
        (handler->flags&XR_XIR_CALLABLE_REST) || target!=producer.target ||
        !function_bound_admitted(program,target,handler)) return XR_XIR_CALL_BAD_ARGUMENT;
    return xr_xir_call_execution_status(view);
}
static XrXirCallStatus instance_signal_stage(XrXirCallView *view,uint32_t instruction,
    uint32_t target,const XrXirValue *arguments,uint32_t count) {
    XrXirInstance *instance=view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    XirCallSignalOrigin origin={xr_xir_call_current_entry(view->activation),instruction,target};
    XrXirCallStatus status=instance_signal_site(view,instance,&origin,arguments,count);
    return status==XR_XIR_CALL_READY?xir_call_signal_stage(view,instruction,target):status;
}
XR_FUNC XrXirCallStatus xir_instance_signal_register(XrXirCallView *view,bool *output) {
    if (!view || !output || !instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    XirCallSignalOrigin origin={0};
    XrXirCallStatus status=xir_call_signal_origin(view,&origin);
    if (status!=XR_XIR_CALL_READY) return status;
    if (origin.target!=xr_xir_call_current_entry(view->activation)) return XR_XIR_CALL_BAD_STATE;
    status=instance_signal_site(view,instance,&origin,view->arguments,view->argument_count);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirInstance *root=instance_root(instance);
    XirSignalRegistration *registration=root->signals;
    int64_t signal=view->arguments[0].payload;
    for (;registration;registration=registration->next) {
        if (!xr_xir_domain_work(root->domain,1)) return XR_XIR_CALL_LIMIT;
        if (registration->signal==signal) break;
    }
    if (registration && (registration->closing || registration->generation!=instance ||
        registration->epoch!=instance->epoch)) return XR_XIR_CALL_BAD_STATE;
    XrXirValue function={0};
    status=value_call_status(xr_xir_value_copy(view->arguments+1,&function));
    if (status!=XR_XIR_CALL_READY) return status;
    bool fresh=registration==NULL;
    if (fresh) {
        if (!xr_xir_domain_work(root->domain,sizeof(*registration))) status=XR_XIR_CALL_LIMIT;
        else registration=instance_metadata_allocate(root->domain,sizeof(*registration),&status);
        if (status==XR_XIR_CALL_READY) {
            registration->generation=instance;registration->epoch=instance->epoch;registration->signal=signal;
            registration->next=root->signals;root->signals=registration;
        }
    }
    bool installed=false;
    if (status==XR_XIR_CALL_READY) status=xr_xir_call_execution_status(view);
    if (status==XR_XIR_CALL_READY)
        status=instance_signal_status(xr_os_io_signal_open(signal,&registration->owner,&installed));
    XrXirCallStatus current=xr_xir_call_execution_status(view);
    if (current!=XR_XIR_CALL_READY) status=current;
    if (status==XR_XIR_CALL_READY && installed) {
        xr_xir_value_drop(&registration->function);registration->function=function;function=(XrXirValue){0};
        registration->pending=false;*output=true;
    } else {
        if (registration && (fresh || installed)) (void)instance_signal_close(root,registration);
        if (status==XR_XIR_CALL_READY) *output=false;
    }
    xr_xir_value_drop(&function);return status;
}
static XrXirCallStatus instance_signals_poll(XrXirInstance *instance) {
    XrXirInstance *root=instance_root(instance);
    if (!root->signals) return XR_XIR_CALL_READY;
    if (root->stopping || instance->stopping || root->prepared_generation || root->callback ||
        (instance->state!=XR_XIR_INSTANCE_READY && instance->state!=XR_XIR_INSTANCE_INITIALIZING))
        return XR_XIR_CALL_READY;
    bool eligible=false;
    XrXirCallStatus status=xir_call_signal_frontier(instance->call,&eligible);
    if (status!=XR_XIR_CALL_READY || !eligible) return status;
    for (XirSignalRegistration *registration=root->signals;registration;) {
        XirSignalRegistration *next=registration->next;
        if (registration->closing || registration->generation!=instance || registration->epoch!=instance->epoch) {
            instance_driving(instance,true);
            bool closed=instance_signal_close(root,registration);
            instance_driving(instance,false);
            if (!closed) return XR_XIR_CALL_HOST_ERROR;
            registration=next;continue;
        }
        if (!xr_xir_domain_work(instance->domain,1)) return XR_XIR_CALL_LIMIT;
        bool pending=false;
        XrOsIoStatus observed=xr_os_io_signal_take(&registration->owner,&pending);
        if (observed!=XR_OS_IO_OK) return instance_signal_status(observed);
        registration->pending|=pending;
        if (registration->pending) {
            /* Initialization may observe events, but its ordinary partial-slot
             * and requested-entry gates are not replaced by a callback role. */
            if (instance->state!=XR_XIR_INSTANCE_READY) return XR_XIR_CALL_READY;
            status=xir_instance_callback_begin(root,&registration->function);
            if (status==XR_XIR_CALL_READY) registration->pending=false;
            return status;
        }
        registration=next;
    }
    return XR_XIR_CALL_READY;
}
