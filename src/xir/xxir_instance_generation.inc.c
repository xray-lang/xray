/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_generation.inc.c - Same-ledger candidate lifetime transactions
 */
#include "xxir_instance_generation.h"
#include "xxir_instance_generation_internal.h"
XR_FUNC XrXirDomain *xir_instance_generation_domain(XrXirInstance *instance) {
    return instance && !instance->owner && instance_owner_current(instance) ? instance->domain : NULL;
}

XR_FUNC XrXirCallStatus xr_xir_instance_generation_new(const XrXirInstanceConfig *config,
    XrXirInstance **output) {
    if (!output || *output) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status = instance_config_status(config);
    if (status != XR_XIR_CALL_READY) return status;
    if (sizeof(XrXirInstance) > config->metadata_limit) return XR_XIR_CALL_LIMIT;
    XrXirDomainBudgetControls controls = {config->requested_value_limit, config->requested_call_limit,
        config->work_limit, config->metadata_limit, config->call_limit};
    XrXirDomain *domain = NULL;
    status = value_call_status(xr_xir_domain_new_budgeted(config->value_limit, &controls, &domain));
    if (status != XR_XIR_CALL_READY) return status;
    XrXirInstance *instance = instance_metadata_allocate(domain, sizeof(*instance), &status);
    if (!instance) { xr_xir_domain_drop(domain); return status; }
    instance->domain = domain;
    instance->config = *config;
    instance->current_module = UINT32_MAX;
    instance->budget = (XrXirCallBudget){.byte_limit = config->call_limit,
        .requested_limit = config->requested_call_limit, .resume_limit = config->poll_limit, .work_domain = domain};
    instance->metadata_bytes = xr_xir_domain_budget_stats(domain).metadata_live;
    *output = instance;
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus generation_owner(XrXirInstance *instance) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance || instance->owner) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    return instance->stopping ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_READY;
}
static XrXirCallStatus generation_storage_receive(XrXirInstance *candidate,
    XrXirInstance *current,const XirGenerationStorage *storage) {
    const XrXirDeclarations *from=current->program->declarations,*to=candidate->program->declarations;
    if (storage->source!=current->program || storage->target!=candidate->program ||
        storage->module_count!=from->module_count || storage->slot_count!=from->slot_count ||
        !storage->modules || (storage->slot_count && !storage->slots)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status=XR_XIR_CALL_READY;
    candidate->module_origins=instance_metadata_allocate(candidate->domain,
        (uint64_t)to->module_count*sizeof(*candidate->module_origins),&status);
    if (status==XR_XIR_CALL_READY && to->slot_count)
        candidate->slot_origins=instance_metadata_allocate(candidate->domain,
            (uint64_t)to->slot_count*sizeof(*candidate->slot_origins),&status);
    if (status!=XR_XIR_CALL_READY) return status;
    for (uint32_t m=0;m<storage->module_count;++m) {
        if (!xr_xir_domain_work(candidate->domain,1)) return XR_XIR_CALL_LIMIT;
        uint32_t target=storage->modules[m];
        if (target>=to->module_count || target==to->root_module || candidate->module_origins[target].owner)
            return XR_XIR_CALL_BAD_ARGUMENT;
        InstanceStorageRoute birth={current,m};
        if (current->module_origins && current->module_origins[m].owner) birth=current->module_origins[m];
        if (instance_root(birth.owner)!=instance_root(candidate) || birth.owner->domain!=candidate->domain ||
            !birth.owner->program || birth.index>=birth.owner->program->declarations->module_count ||
            (candidate->program->active_modules[target] && !birth.owner->ready[birth.index])) return XR_XIR_CALL_BAD_STATE;
        candidate->module_origins[target]=birth;
        candidate->ready[target]=birth.owner->ready[birth.index];
    }
    for (uint32_t s=0;s<storage->slot_count;++s) {
        if (!xr_xir_domain_work(candidate->domain,1)) return XR_XIR_CALL_LIMIT;
        uint32_t target=storage->slots[s];
        if (target>=to->slot_count || candidate->slot_origins[target].owner ||
            from->slots[s].module>=storage->module_count ||
            to->slots[target].module!=storage->modules[from->slots[s].module] ||
            from->slots[s].mutable!=to->slots[target].mutable) return XR_XIR_CALL_BAD_ARGUMENT;
        InstanceStorageRoute birth={current,s};
        if (current->slot_origins && current->slot_origins[s].owner) birth=current->slot_origins[s];
        InstanceStorageRoute module=candidate->module_origins[to->slots[target].module];
        if (birth.owner!=module.owner || birth.index>=birth.owner->program->declarations->slot_count ||
            birth.owner->program->declarations->slots[birth.index].module!=module.index ||
            (candidate->program->active_modules[to->slots[target].module] && !birth.owner->published[birth.index]))
            return XR_XIR_CALL_BAD_STATE;
        candidate->slot_origins[target]=birth;
    }
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus generation_prepare(XrXirInstance *instance,
    XrXirProgram *program,const XirGenerationStorage *storage,uint64_t *generation) {
    if (!program || !generation || *generation) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status = generation_owner(instance);
    if (status != XR_XIR_CALL_READY) return status;
    if (instance->prepared_generation) return XR_XIR_CALL_BUSY;
    XrXirInstance *current = instance_current(instance);
    if (storage && (!current->program || storage->source!=current->program || storage->target!=program))
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (current->executor && !xr_xir_task_executor_root_idle(current->executor)) return XR_XIR_CALL_BUSY;
    if (current->executor && xr_xir_task_executor_completion_status(current->executor) != XR_XIR_CALL_READY)
        return xr_xir_task_executor_completion_status(current->executor);
    if (current->call && xr_xir_call_cleanup_incomplete(current->call)) return XR_XIR_CALL_BAD_STATE;
    if (instance->published_generation && instance->published_generation->state != XR_XIR_INSTANCE_READY)
        return instance->published_generation->state == XR_XIR_INSTANCE_FAILED ?
            instance->published_generation->failure.status : XR_XIR_CALL_BAD_STATE;
    if (instance->program == program) return XR_XIR_CALL_BAD_ARGUMENT;
    for (XrXirInstance *state = instance->generation_next; state; state = state->generation_next) {
        if (!xr_xir_domain_work(instance->domain, 1)) return XR_XIR_CALL_LIMIT;
        if (state->program == program) return XR_XIR_CALL_BAD_ARGUMENT;
    }
    if (instance->generation_counter == UINT64_MAX) return XR_XIR_CALL_LIMIT;
    instance->driving = true;
    XrXirInstance *candidate = NULL;
    status = instance_new_state(program, &instance->config, instance, &candidate);
    if (status==XR_XIR_CALL_READY && storage) status=generation_storage_receive(candidate,current,storage);
    if (status == XR_XIR_CALL_READY && current->call)
        status = instance_executor_new(current, &candidate->replacement_executor);
    if (status != XR_XIR_CALL_READY && candidate) {
        instance_free_state(candidate);
        candidate = NULL;
    }
    instance->driving = false;
    if (status != XR_XIR_CALL_READY) return status;
    candidate->metadata_bytes=xr_xir_domain_budget_stats(candidate->domain).metadata_live;
    candidate->generation_id = ++instance->generation_counter;
    candidate->previous_generation = current;
    candidate->generation_next = instance->generation_next;
    instance->generation_next = candidate;
    instance->active_generation = instance->prepared_generation = candidate;
    *generation = candidate->generation_id;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_instance_generation_prepare(XrXirInstance *instance,
    XrXirProgram *program,uint64_t *generation) {
    return generation_prepare(instance,program,NULL,generation);
}
XR_FUNC XrXirCallStatus xir_instance_generation_prepare_received(XrXirInstance *instance,
    const XirGenerationStorage *storage,uint64_t *generation) {
    if (!storage) return XR_XIR_CALL_BAD_ARGUMENT;
    return generation_prepare(instance,storage->target,storage,generation);
}
XR_FUNC XrXirCallStatus xr_xir_instance_generation_publish(XrXirInstance *instance,
    uint64_t generation) {
    XrXirCallStatus status = generation_owner(instance);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirInstance *candidate = instance->prepared_generation;
    if (!generation || !candidate || candidate->generation_id != generation) return XR_XIR_CALL_BAD_STATE;
    if (candidate->state != XR_XIR_INSTANCE_READY || !candidate->call ||
        candidate->last_outcome != XR_XIR_CALL_RETURNED ||
        !xr_xir_task_executor_root_idle(candidate->executor) ||
        xr_xir_task_executor_completion_status(candidate->executor) != XR_XIR_CALL_READY ||
        (xr_xir_call_state(candidate->call) != XR_XIR_CALL_RETURNED &&
         xr_xir_call_state(candidate->call) != XR_XIR_CALL_CONSUMED)) return XR_XIR_CALL_BAD_STATE;
    instance->driving = true;
    if (candidate->replacement_executor) {
        instance_retire_call(candidate->previous_generation, candidate->replacement_executor);
        candidate->replacement_executor = NULL;
    }
    instance->published_generation = candidate;
    instance->active_generation = candidate;
    instance->prepared_generation = NULL;
    instance->driving = false;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_instance_generation_abort(XrXirInstance *instance,
    uint64_t generation) {
    XrXirCallStatus status = generation_owner(instance);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirInstance *candidate = instance->prepared_generation;
    if (!generation || !candidate || candidate->generation_id != generation) return XR_XIR_CALL_BAD_STATE;
    XR_CHECK(instance->generation_next == candidate, "the sole candidate is the newest owned generation");
    instance->generation_next = candidate->generation_next;
    instance->prepared_generation = NULL;
    XrXirInstance *previous = candidate->previous_generation;
    instance->driving = true;
    status = instance_free_state(candidate);
    instance->active_generation = previous;
    instance->driving = false;
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_generation_published(const XrXirInstance *instance,
    uint64_t *generation) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance || instance->owner || !generation) return XR_XIR_CALL_BAD_ARGUMENT;
    *generation = instance->published_generation ? instance->published_generation->generation_id : 0;
    return XR_XIR_CALL_READY;
}
