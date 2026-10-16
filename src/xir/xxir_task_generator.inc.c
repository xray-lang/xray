/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_generator.inc.c - Parked producer ownership on the shared FIFO executor
 *
 * KEY CONCEPT:
 *   Only the owning executor holds code authority for a live producer.
 */
static XrXirValue generator_core_value(XirGenerator *generator) {
    return (XrXirValue){(uint32_t)generator->object.type,0,(int64_t)(uintptr_t)generator};
}
XR_FUNC bool xr_xir_generator_storage_valid(const XirGenerator *generator) {
    if(!generator || generator->object.kind!=XR_XIR_TYPE_ITERATOR || !generator->object.domain ||
        !xr_xir_domain_current(generator->object.domain)) return false;
    const XrXirTypeNode *node=xr_xir_type_node(
        xr_xir_compile_type_arena_types(generator->object.arena),generator->object.type);
    if(!node || node->kind!=XR_XIR_TYPE_ITERATOR || node->parameter_span) return false;
    XirGeneratorState state=atomic_load_explicit(&generator->state,memory_order_acquire);
    if(state==XIR_GENERATOR_PREPARING)
        return !generator->generation && !generator->identity && !generator->executor && !generator->call && !generator->consumer &&
            xr_xir_call_result_empty(&generator->outcome);
    if(state==XIR_GENERATOR_PARKED || state==XIR_GENERATOR_RUNNING)
        return generator->generation && generator->identity;
    return state==XIR_GENERATOR_TERMINAL && !generator->executor && !generator->call &&
        generator->generation && generator->identity &&
        generator->outcome.status!=XR_XIR_CALL_READY && generator->outcome.status!=XR_XIR_CALL_SUSPENDED &&
        generator->outcome.status!=XR_XIR_CALL_YIELDED && generator->outcome.status!=XR_XIR_CALL_CONSUMED &&
        (generator->outcome.status!=XR_XIR_CALL_RETURNED || !generator->outcome.value.type) &&
        xr_xir_call_result_valid(&generator->outcome);
}
static XirGenerator *generator_core_activation(const XrXirTaskExecutor *executor,
    const XirTaskActivation *activation) {
    if(!activation || activation->task || activation==&executor->root) return NULL;
    XirGenerator *generator=(XirGenerator *)((unsigned char *)activation- offsetof(XirGenerator,activation));
    return generator->executor==executor && generator->call==activation->call &&
        generator->generation==executor->generation && generator->identity==activation->ticket ? generator : NULL;
}
static XrXirValueStatus generator_core_prepare(XrXirTaskExecutor *executor,XrXirType type,
    XirGenerator **output) {
    XrXirDomain *domain=executor->config.domain;XrXirTypeArena *arena=executor->config.program->arena;
    if(!xr_xir_domain_retain(domain)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    if(!xr_xir_compile_type_arena_retain(arena)) {xr_xir_domain_drop(domain);return XR_XIR_VALUE_REFCOUNT_LIMIT;}
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirGenerator *generator=xr_xir_domain_allocate(domain,sizeof(*generator),&status);
    if(!generator) {xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);return status;}
    memset(generator,0,sizeof(*generator));atomic_init(&generator->object.references,1);
    atomic_init(&generator->state,XIR_GENERATOR_PREPARING);atomic_init(&generator->orphaned,false);
    generator->object.domain=domain;generator->object.arena=arena;generator->object.type=type;
    generator->object.kind=XR_XIR_TYPE_ITERATOR;
    xr_xir_value_object_publish(&generator->object);*output=generator;return XR_XIR_VALUE_OK;
}
static XrXirCallStatus generator_core_new(XrXirTaskExecutor *executor,XrXirCallView *view,
    XrXirType type,const XrXirCallRequest *request,XrXirValue *output,uint32_t instruction) {
    bool child=false;
    if(!executor || !view || !request || !task_core_empty_value(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    if(task_executor_observing(executor) || !xr_xir_task_executor_view_member(executor,view,&child)) return XR_XIR_CALL_BUSY;
    if(executor->stopping || executor->budget->exhausted || executor->next_identity==UINT64_MAX) return XR_XIR_CALL_BAD_STATE;
    const XrXirProgram *program=executor->config.program;
    const XrXirTypeNode *node=xr_xir_type_node(program->types,type);
    if(!node || node->kind!=XR_XIR_TYPE_ITERATOR || node->parameter_span || request->entry>=program->entry_count ||
        program->entries[request->entry].result!=type || !(program->entries[request->entry].flags & XR_XIR_ENTRY_GENERATOR) ||
        program->entries[request->entry].cleanup_owner) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramPermissions *permissions=program->permissions;
    if(!permissions || permissions->function_count!=program->entry_count) return XR_XIR_CALL_BAD_ARGUMENT;
    XirGenerator *generator=NULL;XrXirValueStatus prepared=generator_core_prepare(executor,type,&generator);
    if(prepared!=XR_XIR_VALUE_OK) return task_core_value_status(prepared);
    XrXirCallConfig config;XR_CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY,
        "generator configuration uses the canonical Call ABI");
    config.entries=executor->config.entries ? executor->config.entries : program->entries;
    config.entry_count=executor->config.entries ? executor->config.entry_count : program->entry_count;
    config.instance=executor->config.instance;config.output=executor->config.output;
    config.byte_limit=executor->config.call_limit;config.poll_limit=executor->config.poll_limit;
    config.depth_limit=executor->config.depth_limit;config.accounting=&generator->accounting;
    config.admission=executor->config.admission;
    if(!config.admission.arena) config.admission=(XrXirValueAdmission){program->arena,executor->config.domain,NULL,NULL,
        executor->config.poll_limit,executor->config.call_limit};
    XrXirCallStatus status=xr_xir_call_generator_new_budgeted(&config,request,executor->budget,&generator->call);
    /* Cold creation does not execute the producer. An ineligible worker body
     * remains parked until an actual demand is rejected before scheduling. */
    if(status==XR_XIR_CALL_READY && child && permissions->entries[request->entry].worker==XR_XIR_OK) {
        XirEffectInvocationSelection context={0};
        status=xr_xir_instance_generator_context(view,instruction,request,&context);
        if(status==XR_XIR_CALL_READY) status=xr_xir_call_generator_bind_context(generator->call,&context);
    }
    if(status==XR_XIR_CALL_READY) status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY && (executor->stopping || executor->budget->exhausted))
        status=executor->budget->exhausted ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED;
    if(status!=XR_XIR_CALL_READY) {
        XrXirCallStatus freed=xr_xir_call_free(generator->call);generator->call=NULL;
        XrXirValue unpublished=generator_core_value(generator);xr_xir_value_drop(&unpublished);
        return freed==XR_XIR_CALL_READY ? status : freed;
    }
    generator->generation=executor->generation;generator->identity=++executor->next_identity;
    generator->activation=(XirTaskActivation){.call=generator->call,.ticket=generator->identity,.entry=request->entry};
    const XrXirExecutorBinding binding={executor,&generator->activation,executor->generation,generator->identity};
    XR_CHECK(xr_xir_call_bind_executor(generator->call,&binding)==XR_XIR_CALL_READY,
        "a generator activation receives one exact executor identity before publication");
    generator->executor=executor;generator->child=child;
    generator->active_next=executor->generators;
    if(executor->generators) executor->generators->active_previous=generator;
    executor->generators=generator;
    atomic_store_explicit(&generator->object.references,2,memory_order_relaxed);
    atomic_store_explicit(&generator->state,XIR_GENERATOR_PARKED,memory_order_release);
    *output=generator_core_value(generator);return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_task_executor_generator(XrXirTaskExecutor *executor,
    XrXirCallView *view,XrXirType type,const XrXirCallRequest *request,XrXirValue *output,uint32_t instruction) {
    if(!task_executor_owner_current(executor)) return XR_XIR_CALL_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirCallStatus status=generator_core_new(executor,view,type,request,output,instruction);
    xr_xir_value_graph_end();return status;
}
static void generator_core_complete_consumer(XrXirTaskExecutor *executor,XirGenerator *generator);
static void generator_core_finish(XrXirTaskExecutor *executor,XirGenerator *generator) {
    xr_xir_value_graph_begin();XrXirCallResult outcome={0};
    XrXirCallStatus status=xr_xir_call_take_outcome(generator->call,&outcome);
    XR_CHECK(status!=XR_XIR_CALL_READY && status!=XR_XIR_CALL_SUSPENDED && status!=XR_XIR_CALL_YIELDED,
        "terminal generator publication consumes the complete producer outcome");
    if(xr_xir_call_cleanup_incomplete(generator->call)) executor->cleanup_incomplete=true;
    XrXirCallStatus freed=xr_xir_call_free(generator->call);
    if(freed!=XR_XIR_CALL_READY) {executor->cleanup_incomplete=true;task_core_record_status(executor,freed);}
    if(executor->shutdown_status==XR_XIR_CALL_READY && freed!=XR_XIR_CALL_READY) executor->shutdown_status=freed;
    if(executor->shutdown_status==XR_XIR_CALL_READY && status!=XR_XIR_CALL_RETURNED && status!=XR_XIR_CALL_CANCELLED &&
        (executor->stopping || (status!=XR_XIR_CALL_THROWN && !xr_xir_call_panic_status(status))))
        executor->shutdown_status=status;
    generator->call=NULL;generator->activation.call=NULL;generator->activation.entry=UINT32_MAX;
    if(executor->external==&generator->activation) {executor->external=NULL;executor->external_call_wake=0;}
    if(generator->active_previous) generator->active_previous->active_next=generator->active_next;
    else {XR_CHECK(executor->generators==generator,"generator retirement keeps its original registry membership");
        executor->generators=generator->active_next;}
    if(generator->active_next) generator->active_next->active_previous=generator->active_previous;
    generator->active_next=generator->active_previous=NULL;generator->executor=NULL;
    xr_xir_call_result_move(&outcome,&generator->outcome);
    atomic_store_explicit(&generator->state,XIR_GENERATOR_TERMINAL,memory_order_release);
    generator_core_complete_consumer(executor,generator);
    XrXirValue lease=generator_core_value(generator);xr_xir_value_drop(&lease);xr_xir_value_graph_end();
}
static void generator_core_cancel(XrXirTaskExecutor *executor,XirGenerator *generator) {
    atomic_store_explicit(&generator->orphaned,true,memory_order_release);
    XrXirCallStatus status=xr_xir_call_request_cancel(generator->call);
    XR_CHECK(status==XR_XIR_CALL_CANCEL_REQUESTED || status==XR_XIR_CALL_BAD_STATE || status==XR_XIR_CALL_BUSY,
        "generator cancellation belongs to its exact live Call");
    if(!generator->activation.queued && executor->current!=&generator->activation &&
        xr_xir_call_state(generator->call)!=XR_XIR_CALL_SUSPENDED)
        task_core_enqueue(executor,&generator->activation);
}
static void generator_core_scan(XrXirTaskExecutor *executor) {
    if(executor->stopping) return;
    for(XirGenerator *generator=executor->generators;generator;generator=generator->active_next) {
        if(!xr_xir_domain_work(executor->config.domain,1)) {
            task_core_record_status(executor,XR_XIR_CALL_LIMIT);
            (void)xr_xir_task_executor_stop(executor);return;
        }
        if(atomic_load_explicit(&generator->orphaned,memory_order_acquire)) generator_core_cancel(executor,generator);
    }
}
