/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance.c - One initialization publisher and isolated runtime slots
 *
 * KEY CONCEPT:
 *   Instances retain immutable descriptors until execution and cleanup finish.
 */


#include "xxir_program_internal.h"
#include "../base/xresource_limits.h"
#include "xxir_effects.h"
#include "xxir_instance_debug_internal.h"
#include "xxir_instance_activation.h"
#include "xxir_instance_value.h"
#include "xxir_native_callable.h"
#include "xxir_from_utf8_internal.h"
#include "xxir_call_view_internal.h"
#include "xxir_enum.h"
#include "xxir_instance_binding.h"
#include "xxir_instance_hash.h"
#include "xxir_instance_function_internal.h"
#include "xxir_value_internal.h"
#include "xxir_type_arena_internal.h"
#include "xxir_call_internal.h"
#include "xxir_cell_owner_internal.h"
#include "xxir_class_origin_internal.h"
#include "xxir_cell_provenance_internal.h"
#include "xxir_effect_runtime_internal.h"
#include "xxir_task_internal.h"
#include "xxir_generator_internal.h"
#include "xxir_instance_generator_internal.h"
#include "xxir_task_shapes.h"
#include "xxir_task_shapes.h"
#include "xxir_struct.h"
#include "xxir_output.h"
#include "xxir_panic.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"

typedef struct FunctionGate {
    _Atomic(uint32_t) references;
    XrXirProgram *program;
    XrXirInstance *instance;
    XrXirDomain *domain;
} FunctionGate;
static void function_gate_drop(void *owner) {
    FunctionGate *gate = owner;
    XR_CHECK(gate && xr_xir_domain_current(gate->domain), "function gate release requires its current domain");
    uint32_t before = atomic_fetch_sub_explicit(&gate->references, 1, memory_order_acq_rel);
    XR_CHECK(before, "function gate reference underflow");
    if (before == 1) {
        XrXirDomain *domain = gate->domain;
        xr_xir_compile_program_drop(gate->program);
        xr_xir_domain_metadata_deallocate(domain, gate, sizeof(*gate));
        xr_xir_domain_drop(domain);
    }
}
static bool function_gate_retain(FunctionGate *gate) {
    XR_CHECK(gate && xr_xir_domain_current(gate->domain), "function gate retain requires its current domain");
    uint32_t count = atomic_load_explicit(&gate->references, memory_order_relaxed);
    for (;;) {
        if (!count || count == UINT32_MAX) return false;
        if (atomic_compare_exchange_weak_explicit(&gate->references, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
}

typedef struct InstanceStorageRoute {
    XrXirInstance *owner;
    uint32_t index;
} InstanceStorageRoute;
struct XrXirInstance {
    XrXirProgram *program;
    XrXirInstanceConfig config;
    XrXirInstanceState state;
    XrXirDomain *domain;
    FunctionGate *function_gate;
    XrXirValue *slots;
    InstanceStorageRoute *module_origins, *slot_origins, *function_origins;
    uint8_t *published, *ready;
    uint32_t *publication_order, publication_count, cursor, current_module, requested;
    uint32_t *published_counts;
    XrXirCallEntry *entries;
    XrXirCall *call;
    XrXirTaskExecutor *executor;
    XrXirCallAccounting accounting[2];
    uint32_t accounting_index;
    XrXirCallBudget budget;
    XrXirCallResult failure;
    XrXirValue pending_function;
    uint64_t epoch, metadata_bytes;
    bool driving, stopping, observing, debug_visiting;
    XrXirInstance *owner, *generation_next;
    XrXirInstance *active_generation, *published_generation, *prepared_generation;
    XrXirInstance *previous_generation;
    XrXirTaskExecutor *replacement_executor;
    uint64_t generation_id, generation_counter, call_epoch;
    XrXirCallStatus last_outcome;
    struct XirInstanceBindingTable *symbol_bindings;
    bool bindings_published;
};
static void instance_bindings_free(XrXirInstance *instance);
static bool instance_bindings_ready(const XrXirInstance *instance);
static XrXirInstance *instance_root(XrXirInstance *instance) {
    return instance && instance->owner ? instance->owner : instance;
}
static XrXirInstance *instance_current(XrXirInstance *instance) {
    XrXirInstance *root = instance_root(instance);
    return root && root->active_generation ? root->active_generation : root;
}
static XrXirCallBudget *instance_budget(XrXirInstance *instance) {
    return &instance_root(instance)->budget;
}
/* Notifications use the root budget even when the active state is a child
 * generation. Reject reentry before any function gate or generation retires. */
static bool instance_observing(const XrXirInstance *instance) {
    const XrXirInstance *root = instance && instance->owner ? instance->owner : instance;
    return instance && (instance->observing || root->observing ||
        instance->budget.notifying || root->budget.notifying);
}
static bool instance_busy(const XrXirInstance *instance) {
    const XrXirInstance *root = instance && instance->owner ? instance->owner : instance;
    return instance && (instance->driving || root->driving || instance_observing(instance));
}
static void instance_driving(XrXirInstance *instance, bool driving) {
    instance->driving = driving;
    instance_root(instance)->driving = driving;
}
static XrXirInstance *instance_entry_state(XrXirInstance *instance) {
    XrXirInstance *root = instance_root(instance);
    if (!root) return NULL;
    return root->prepared_generation ? root->prepared_generation :
        root->published_generation ? root->published_generation : root;
}
static XrXirCallStatus instance_function_state(XrXirInstance *instance, const XrXirValue *value,
    XrXirInstance **output) {
    XrXirInstance *root = instance_root(instance);
    const XrXirFunctionBinding *binding = xr_xir_function_binding(value);
    if (!root || !binding || binding->release != function_gate_drop || !binding->owner)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (root->prepared_generation) {
        if (root->prepared_generation->function_gate != binding->owner) return XR_XIR_CALL_BAD_ARGUMENT;
        *output = root->prepared_generation;
        return XR_XIR_CALL_READY;
    }
    if (root->function_gate == binding->owner) { *output = root; return XR_XIR_CALL_READY; }
    for (XrXirInstance *state = root->generation_next; state; state = state->generation_next) {
        if (!xr_xir_domain_work(root->domain, 1)) return XR_XIR_CALL_LIMIT;
        if (state->function_gate == binding->owner) { *output = state; return XR_XIR_CALL_READY; }
    }
    return XR_XIR_CALL_BAD_ARGUMENT;
}
/* This authenticates only the original Domain slot of an otherwise valid
 * Instance. It grants no admission to arbitrary or retired opaque handles. */
static bool instance_owner_current(const XrXirInstance *instance) {
    return !instance || xr_xir_domain_current(instance->domain);
}
static bool instance_view_current(const XrXirCallView *view) {
    return !view || instance_owner_current(view->instance);
}
#include "xxir_instance_cell_roles.inc.c"
static XrXirCallResult instance_result(XrXirCallStatus status) {
    return (XrXirCallResult) {.status=status};
}
static XrXirCallStatus value_call_status(XrXirValueStatus status) {
    if (status == XR_XIR_VALUE_OK) return XR_XIR_CALL_READY;
    if (status == XR_XIR_VALUE_BOUNDS) return XR_XIR_CALL_BOUNDS;
    if (status == XR_XIR_VALUE_OOM) return XR_XIR_CALL_OOM;
    if (status == XR_XIR_VALUE_UNSUPPORTED) return XR_XIR_CALL_UNSUPPORTED;
    if (status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT) return XR_XIR_CALL_LIMIT;
    return XR_XIR_CALL_BAD_STATE;
}
static XrXirCallStatus value_path_call_status(XrXirValueStatus status, const XrXirFaultDetail *fault) {
    if (status == XR_XIR_VALUE_BOUNDS && fault && xr_xir_fault_runtime_valid(*fault))
        return XR_XIR_CALL_RUNTIME_PANIC;
    return value_call_status(status);
}
static bool instance_cleanup_grant(const XrXirInstance *instance) {
    const XrXirInstance *active=instance_current((XrXirInstance *)instance);
    return active->driving && !instance_observing(active) && xr_xir_task_executor_cleanup_active(active->executor);
}
/* The sealed body's facts cannot be replaced by the carrier's advertised bound,
 * a Sendable proof, or eligibility for one particular execution role. */
static bool function_shape_admitted(const XrXirProgram *program,uint32_t entry,
    const XrXirTypeNode *signature) {
    const XrXirProgramPermissions *permissions=program->permissions;
    return signature && permissions && entry<program->entry_count &&
        !(program->entries[entry].flags&XR_XIR_ENTRY_GENERATOR) &&
        permissions->function_count==program->entry_count &&
        permissions->slot_count==program->declarations->slot_count &&
        permissions->input && permissions->input->artifact &&
        entry<permissions->input->artifact->module.function_count &&
        xr_xir_error_bound_accepts(permissions->input->artifact->module.functions[entry].error_type,signature->element) &&
        (!(signature->flags&XR_XIR_CALLABLE_NO_SUSPEND) ||
         (program->declarations->functions[entry].promises&XR_XIR_FUNCTION_NO_SUSPEND));
}
static bool function_bound_admitted(const XrXirProgram *program,uint32_t entry,
    const XrXirTypeNode *signature) {
    if(!function_shape_admitted(program,entry,signature)) return false;
    const XrXirProgramPermission *permission=&program->permissions->entries[entry];
    return xr_xir_callable_root_accepts(permission->requires_root,permission->unresolved,signature->flags);
}
/* Real immutable entry roles authenticate the reference protocol. A Cell
 * descriptor by itself cannot authorize a borrowed call or an owned capture. */
static bool instance_function_parameters(XrXirProgram *program, uint32_t entry,
    const XrXirTypeNode *signature, uint32_t captures) {
    const XrXirCallEntry *target = &program->entries[entry];
    for (uint32_t p = 0; p < signature->parameter_count; ++p) {
        const XrXirCallableParameter *parameter = &signature->parameters[p];
        if (!xr_xir_callable_parameter_storage_valid(program->types,parameter) ||
            target->parameters[p+captures] != parameter->type) return false;
        if (parameter->mode == XR_PARAM_REF &&
            instance_cell_role(program,entry,p+captures) != XR_XIR_CELL_ROLE_SCOPED_REF) return false;
    }
    return true;
}
/* A conversion creates a new carrier in the receiver. Its physical entry
 * comes only from the actual compiler receiving map; ordinary calls always
 * retain the original binding and select its authentic birth frame. */
static XrXirCallStatus instance_received_function_entry(XrXirInstance *instance,
    const XrXirFunctionBinding *binding,uint32_t *output,uint64_t *work) {
    if (!instance || !binding || !output || !work || binding->release!=function_gate_drop || !binding->owner)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const FunctionGate *gate=binding->owner;
    if (!xr_xir_domain_current(gate->domain) || gate->domain!=instance->domain ||
        (gate->instance!=instance_root(instance) &&
         !(instance->stopping && !gate->instance && instance_cleanup_grant(instance))) ||
        !gate->program || binding->entry>=gate->program->entry_count) return XR_XIR_CALL_BAD_ARGUMENT;
    if (gate==instance->function_gate && gate->program==instance->program) {
        *output=binding->entry;return XR_XIR_CALL_READY;
    }
    if (!instance->function_origins) return XR_XIR_CALL_BAD_ARGUMENT;
    for (uint32_t f=0;f<instance->program->entry_count;++f) {
        InstanceStorageRoute route=instance->function_origins[f];
        uint64_t generation=instance->generation_id;
        for (;;) {
            if (!*work || !xr_xir_domain_work(instance->domain,1)) return XR_XIR_CALL_LIMIT;
            --*work;
            if (!route.owner) break;
            if (instance_root(route.owner)!=instance_root(instance) || route.owner->domain!=instance->domain ||
                !route.owner->program || route.index>=route.owner->program->entry_count ||
                !route.owner->generation_id || route.owner->generation_id>=generation)
                return XR_XIR_CALL_BAD_STATE;
            if (route.owner->function_gate==gate && route.owner->program==gate->program && route.index==binding->entry) {
                if (!instance->program->active_modules[instance->program->declarations->functions[f].module])
                    return XR_XIR_CALL_BAD_ARGUMENT;
                *output=f;return XR_XIR_CALL_READY;
            }
            generation=route.owner->generation_id;
            route=route.owner->function_origins ? route.owner->function_origins[route.index] : (InstanceStorageRoute){0};
        }
    }
    return XR_XIR_CALL_BAD_ARGUMENT;
}
static XrXirCallStatus instance_function_type(XrXirInstance *instance,const XrXirValue *value,
    XrXirValueAdmission *admission,XrXirType *output) {
    if (!instance || !value || !admission || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    uint64_t cost=xir_type_arena_lookup_work(instance->program->arena);
    if (cost>admission->work || !xr_xir_domain_work(instance->domain,cost)) return XR_XIR_CALL_LIMIT;
    admission->work-=cost;
    return xir_type_arena_local_type(instance->program->arena,xr_xir_value_arena(value),
        (XrXirType)value->type,output) ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT;
}
static XrXirValueStatus instance_function_admission_state(XrXirInstance *receiver,
    const void *owner,uint64_t *work,XrXirInstance **output) {
    if (owner==receiver->function_gate) { *output=receiver;return XR_XIR_VALUE_OK; }
    XrXirInstance *root=instance_root(receiver);
    if (receiver->stopping || root->stopping) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirInstance *states[]={root,root->prepared_generation};
    for (unsigned i=0;i<2;++i) {
        if (!states[i] || states[i]==receiver) continue;
        if (!*work || !xr_xir_domain_work(root->domain,1)) return XR_XIR_VALUE_LIMIT;
        --*work;
        if (states[i]->function_gate==owner && !states[i]->stopping) {
            *output=states[i];return XR_XIR_VALUE_OK;
        }
    }
    for (XrXirInstance *state=root->generation_next;state;state=state->generation_next) {
        if (!*work || !xr_xir_domain_work(root->domain,1)) return XR_XIR_VALUE_LIMIT;
        --*work;
        if (state->function_gate==owner && !state->stopping) {
            *output=state;return XR_XIR_VALUE_OK;
        }
    }
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus admit_function_binding(void *context, const XrXirFunctionBinding *binding,
    XrXirType type, uint64_t *work) {
    XrXirInstance *instance = context;
    if (!instance_owner_current(instance)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!instance || (instance->stopping && !instance_cleanup_grant(instance)) || !binding || binding->release != function_gate_drop ||
        !binding->owner || !work) return XR_XIR_VALUE_BAD_ARGUMENT;
    /* Value admission supplies this exact live field, including the actual
     * metadata arena for the current function callback. No binding-supplied
     * pointer or type number can replace that owner coordinate. */
    const XrXirValueAdmission *admission=(const XrXirValueAdmission *)
        ((const unsigned char *)work - offsetof(XrXirValueAdmission,work));
    if (admission->function!=admit_function_binding || admission->context!=context ||
        admission->domain!=instance->domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirInstance *birth=NULL;
    XrXirValueStatus selected=instance_function_admission_state(instance,binding->owner,work,&birth);
    if (selected!=XR_XIR_VALUE_OK) return selected;
    instance=birth;
    if (admission->arena!=instance->program->arena) return XR_XIR_VALUE_BAD_ARGUMENT;
    const FunctionGate *gate = binding->owner;
    if (!xr_xir_domain_current(gate->domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if ((gate->instance != instance_root(instance) && !(instance->stopping && !gate->instance && instance_cleanup_grant(instance))) ||
        gate->program != instance->program ||
        binding->entry >= instance->program->entry_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!instance->program->active_modules[instance->program->declarations->functions[binding->entry].module])
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *signature = xr_xir_callable_signature(instance->program->types, type);
    const XrXirCallEntry *entry = &instance->program->entries[binding->entry];
    /* Holding an immutable value grants no execution. A produced reference
     * keeps its checked site and type; child execution checks the current key. */
    if (!function_shape_admitted(instance->program, binding->entry, signature) || entry->cleanup_owner ||
        binding->entry == instance->program->declarations->modules[
            instance->program->declarations->functions[binding->entry].module].initializer ||
        binding->capture_count > entry->parameter_count ||
        signature->parameter_count != entry->parameter_count - binding->capture_count ||
        signature->result != entry->result || (binding->capture_count && !binding->captures))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t cost = (uint64_t) entry->parameter_count + 1;
    if (cost > *work || !xr_xir_domain_work(instance->domain,cost)) return XR_XIR_VALUE_LIMIT;
    *work -= cost;
    for (uint32_t i = 0; i < binding->capture_count; ++i) {
        if (!xr_xir_value_argument(&binding->captures[i], instance->program->arena, entry->parameters[i]))
            return XR_XIR_VALUE_BAD_ARGUMENT;
        if (xr_xir_type_is_cell(instance->program->types, entry->parameters[i]) &&
            (instance_cell_role(instance->program, binding->entry, i) != XR_XIR_CELL_ROLE_OWNED_CAPTURE ||
             !xr_xir_cell_unborrowed(&binding->captures[i]) || xr_xir_cell_module_storage(&binding->captures[i])))
            return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    if (!instance_function_parameters(instance->program,binding->entry,signature,binding->capture_count))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    return XR_XIR_VALUE_OK;
}
static XrXirValueAdmission instance_candidate_admission(XrXirInstance *instance) {
    XR_CHECK(instance && instance_owner_current(instance), "instance internal owner requires its current domain");
    const XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(instance->domain);
    XR_CHECK(budget.bound && budget.work <= budget.work_limit, "instance admission uses its live work ledger");
    return (XrXirValueAdmission) {instance->program->arena, instance->domain,
        admit_function_binding, instance, budget.work_limit - budget.work, instance->config.metadata_limit};
}
static XrXirCallStatus admit_instance_value(XrXirValueAdmission *admission,
                                           const XrXirValue *value, XrXirType type) {
    XrXirValueStatus status = xr_xir_value_admit(value, type, admission);
    return status == XR_XIR_VALUE_BAD_ARGUMENT ? XR_XIR_CALL_BAD_ARGUMENT : value_call_status(status);
}
static void instance_trace(XrXirInstance *instance, XrXirLifecycleEvent event, uint32_t index) {
    XR_CHECK(instance && instance_owner_current(instance), "instance internal owner requires its current domain");
    if (!instance->config.trace) return;
    instance->observing = true;
    instance_root(instance)->observing = true;
    instance->config.trace(instance->config.trace_context, event, index);
    instance->observing = false;
    instance_root(instance)->observing = false;
}
static void clear_slots(XrXirInstance *instance) {
    XR_CHECK(instance && instance_owner_current(instance), "instance internal owner requires its current domain");
    while (instance->publication_count) {
        uint32_t slot = instance->publication_order[--instance->publication_count];
        instance->published[slot] = 0;
        --instance->published_counts[instance->program->declarations->slots[slot].module];
        xr_xir_value_drop(&instance->slots[slot]);
        instance_trace(instance, XR_XIR_SLOT_RELEASED, slot);
    }
}
static void fail_initialization(XrXirInstance *instance, XrXirCallResult failure) {
    XrXirCallResult copy = instance_result(XR_XIR_CALL_READY);
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (failure.status==XR_XIR_CALL_CLEANUP_FAILED) {
        XR_CHECK(xr_xir_call_take_outcome(instance->call,&copy)==XR_XIR_CALL_CLEANUP_FAILED,
            "sticky fatal initialization takes its actual complete root owner");
    } else status=xr_xir_call_result_copy(&failure,&copy);
    if (status != XR_XIR_VALUE_OK) copy = instance_result(value_call_status(status));
    xr_xir_call_result_drop(&instance->failure);
    xr_xir_call_result_move(&copy, &instance->failure);
    instance->state = XR_XIR_INSTANCE_FAILED;
    instance->current_module = UINT32_MAX;
    clear_slots(instance);
}
static XrXirAction initialization_resume(XrXirCallView *view) {
    XrXirInstance *instance = instance_current(view->instance);
    XR_CHECK(instance && instance_owner_current(instance), "initializer requires its current domain");
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t *phase = view->state;
    if (*phase) {
        if (view->inbox.status == XR_XIR_CALL_THROWN)
            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, view->inbox.value, {0}, 0};
        if (view->inbox.status != XR_XIR_CALL_RETURNED)
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
        if (*phase == 2)
            return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, view->inbox.value, {0}, 0};
        uint32_t module = instance->current_module;
        if (instance->published_counts[module] != instance->program->module_slots[module])
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};
        instance->ready[module] = 1;
        instance_trace(instance, XR_XIR_MODULE_READY, module);
        ++instance->cursor;
        instance->current_module = UINT32_MAX;
    }
    while (instance->cursor < instance->program->initialization_count &&
        instance->ready[instance->program->order[instance->cursor]]) {
        if (!xr_xir_domain_work(instance->domain,1))
            return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,XR_XIR_CALL_LIMIT},{0},0};
        ++instance->cursor;
    }
    if (instance->cursor < instance->program->initialization_count) {
        instance->current_module = instance->program->order[instance->cursor];
        *phase = 1;
        instance_trace(instance, XR_XIR_MODULE_BEGIN, instance->current_module);
        return (XrXirAction) {XR_XIR_ACTION_CALL, d->modules[instance->current_module].initializer,
            NULL, 0, {0, 0, 0}, {0}, 0};
    }
    instance->state = XR_XIR_INSTANCE_READY;
    *phase = 2;
    return (XrXirAction) {XR_XIR_ACTION_CALL, instance->requested, view->arguments,
        view->argument_count, instance->pending_function, {0}, 0};
}
XR_FUNC bool xr_xir_instance_initialization_publication_missing(
    XrXirCallView *view, const XrXirAction *action) {
    if (!view || !action || action->kind != XR_XIR_ACTION_FAULT || action->callee ||
        action->arguments || action->argument_count || action->flags || action->value.type ||
        action->value.reserved || action->value.payload || !xr_xir_panic_empty(&action->panic)) return false;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    /* Authenticate the concrete owner before interpreting an opaque host context. */
    if (!admission || admission->function != admit_function_binding) return false;
    XrXirInstance *instance = admission->context;
    if (!instance || instance_root(instance) != view->instance || !instance_owner_current(instance) ||
        admission->domain != instance->domain || admission->arena != instance->program->arena ||
        !instance->driving || instance_observing(instance) || instance->stopping ||
        instance->state != XR_XIR_INSTANCE_INITIALIZING || view->activation != instance->call ||
        view->phase != XR_XIR_CALL_NORMAL || view->scope_exit || !view->state) return false;
    bool child = false;
    if (!xr_xir_task_executor_view_member(instance->executor, view, &child) || child) return false;
    const XrXirProgram *program = instance->program;
    if (xr_xir_call_current_entry(view->activation) != program->entry_count) return false;
    const XrXirCallEntry *entry = &instance->entries[program->entry_count];
    if (entry->resume != initialization_resume || entry->release != initialization_release ||
        entry->state_bytes != sizeof(uint32_t) || entry->environment || entry->flags || entry->cleanup_owner ||
        *(const uint32_t *)view->state != 1 || view->inbox.status != XR_XIR_CALL_RETURNED ||
        view->inbox.value.type != XR_XIR_UNIT || view->inbox.value.reserved || view->inbox.value.payload ||
        view->inbox.wake || !xr_xir_panic_empty(&view->inbox.panic)) return false;
    const XrXirDeclarations *declarations = program->declarations;
    if (instance->cursor >= program->initialization_count ||
        instance->current_module >= declarations->module_count ||
        program->order[instance->cursor] != instance->current_module) return false;
    uint32_t module = instance->current_module;
    return instance->published_counts[module] < program->module_slots[module];
}
static XrXirCallStatus instance_config_init(XrXirInstanceConfig *config, size_t size, bool sandbox) {
    if (!config) return XR_XIR_CALL_BAD_ARGUMENT;
    if (size != sizeof(*config)) return XR_XIR_CALL_BAD_ABI;
    uint64_t live_limit = sandbox ? XR_RESOURCE_SANDBOX_RUNTIME_LIVE : XR_RESOURCE_DEFAULT_LIMIT;
    uint64_t requested = sandbox ? XR_RESOURCE_SANDBOX_RUNTIME_REQUESTED : XR_RESOURCE_DEFAULT_LIMIT;
    *config = (XrXirInstanceConfig) {XR_XIR_CALL_ABI_VERSION, sizeof(*config),
        live_limit, live_limit, live_limit,
        sandbox ? XR_RESOURCE_SANDBOX_RUNTIME_POLLS : XR_RESOURCE_DEFAULT_LIMIT,
        sandbox ? XR_RESOURCE_SANDBOX_RUNTIME_DEPTH : XR_RESOURCE_DEFAULT_DEPTH,
        {0}, NULL, NULL, {0}, requested, requested,
        sandbox ? XR_RESOURCE_SANDBOX_RUNTIME_WORK : XR_RESOURCE_DEFAULT_LIMIT};
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_instance_config_init(XrXirInstanceConfig *config, size_t size) {
    return instance_config_init(config, size, false);
}
XR_FUNC XrXirCallStatus xr_xir_instance_config_init_sandbox(XrXirInstanceConfig *config, size_t size) {
    return instance_config_init(config, size, true);
}
static void instance_dispose(XrXirInstance *instance) {
    XR_CHECK(instance && instance_owner_current(instance), "instance internal owner requires its current domain");
    XrXirDomain *domain = instance->domain;
    XrXirProgram *program = instance->program;
    const XrXirDeclarations *d = program ? program->declarations : NULL;
    bool close = !instance->owner;
    (void)xr_xir_task_executor_free(instance->replacement_executor, NULL);
    xr_xir_value_drop(&instance->pending_function);
    XR_CHECK(xr_xir_task_executor_free(instance->executor, NULL) == XR_XIR_CALL_READY,
        "unpublished or already drained instance executor has no incomplete cleanup");
    instance_bindings_free(instance);
    if (instance->entries) xr_xir_domain_metadata_deallocate(domain, instance->entries,
        ((uint64_t)program->entry_count + 1) * sizeof(*instance->entries));
    if (instance->slots) xr_xir_domain_metadata_deallocate(domain, instance->slots,
        (uint64_t)d->slot_count * sizeof(*instance->slots));
    if (instance->published) xr_xir_domain_metadata_deallocate(domain, instance->published, d->slot_count);
    if (instance->ready) xr_xir_domain_metadata_deallocate(domain, instance->ready, d->module_count);
    if (instance->publication_order) xr_xir_domain_metadata_deallocate(domain, instance->publication_order,
        (uint64_t)d->slot_count * sizeof(*instance->publication_order));
    if (instance->published_counts) xr_xir_domain_metadata_deallocate(domain, instance->published_counts,
        (uint64_t)d->module_count * sizeof(*instance->published_counts));
    if (instance->module_origins) xr_xir_domain_metadata_deallocate(domain,instance->module_origins,
        (uint64_t)d->module_count*sizeof(*instance->module_origins));
    if (instance->function_origins) xr_xir_domain_metadata_deallocate(domain,instance->function_origins,
        (uint64_t)program->entry_count*sizeof(*instance->function_origins));
    if (instance->slot_origins) xr_xir_domain_metadata_deallocate(domain,instance->slot_origins,
        (uint64_t)d->slot_count*sizeof(*instance->slot_origins));
    if (instance->function_gate) {
        instance->function_gate->instance = NULL;
        function_gate_drop(instance->function_gate);
    }
    if (instance->owner && instance->owner->active_generation == instance)
        instance->owner->active_generation = instance->owner->published_generation == instance ?
            NULL : instance->owner->published_generation;
    xr_xir_domain_metadata_deallocate(domain, instance, sizeof(*instance));
    /* Internal execution roots are gone; escaped owned values retain their full graph. */
    if (close) xr_xir_domain_close(domain);
    xr_xir_compile_program_drop(program);
    xr_xir_domain_drop(domain);
}
static void *instance_metadata_allocate(XrXirDomain *domain, uint64_t bytes, XrXirCallStatus *status) {
    XrXirValueStatus value_status = XR_XIR_VALUE_OK;
    void *memory = xr_xir_domain_metadata_allocate(domain, bytes, &value_status);
    if (!memory) *status = value_call_status(value_status);
    return memory;
}
static XrXirCallStatus instance_executor_new(XrXirInstance *instance, XrXirTaskExecutor **output) {
    const XrXirInstanceConfig *config = &instance->config;
    XrXirProgram *program = instance->program;
    XrXirTaskExecutorConfig execution = {program, instance->domain, program->arena,
        config->call_limit, config->poll_limit, config->requested_value_limit,
        config->requested_call_limit, config->work_limit, config->depth_limit,
        instance_budget(instance), instance_root(instance), instance->entries, program->entry_count + 1,
        instance_candidate_admission(instance), config->output};
    return xr_xir_task_executor_new(&execution, output);
}
static void instance_retire_call(XrXirInstance *instance, XrXirTaskExecutor *replacement) {
    XR_CHECK(instance && replacement && xr_xir_task_executor_root_idle(instance->executor),
        "a superseded root is terminal before its already prepared driver is installed");
    XR_CHECK(!xr_xir_call_cleanup_incomplete(instance->call),
        "an incomplete cleanup cannot be superseded by generation publication");
    /* Like a successful ordinary start, this supersedes the previous terminal
     * outcome. Failed preparation never enters this consuming transition. */
    (void)xr_xir_task_executor_free(instance->executor, NULL);
    (void)xr_xir_call_free(instance->call);
    instance->call = NULL;
    instance->executor = replacement;
}
static XrXirCallStatus instance_config_status(const XrXirInstanceConfig *config) {
    if (!config) return XR_XIR_CALL_BAD_ARGUMENT;
    if (config->abi_version != XR_XIR_CALL_ABI_VERSION || config->struct_size != sizeof(*config))
        return XR_XIR_CALL_BAD_ABI;
    if (!xr_xir_output_provider_valid(&config->output) || !xr_xir_time_provider_valid(&config->time))
        return XR_XIR_CALL_BAD_ABI;
    if (!config->requested_value_limit || !config->requested_call_limit || !config->work_limit)
        return XR_XIR_CALL_BAD_ARGUMENT;
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_new_state(XrXirProgram *program, const XrXirInstanceConfig *config,
    XrXirInstance *owner, XrXirInstance **output) {
    if (!output || *output || !program) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status = instance_config_status(config);
    if (status != XR_XIR_CALL_READY) return status;
    const XrXirDeclarations *d = program->declarations;
    uint64_t bytes = sizeof(XrXirInstance) + (uint64_t) d->slot_count * (sizeof(XrXirValue) + 5) +
        (uint64_t) d->module_count * 5 + ((uint64_t) program->entry_count + 1) * sizeof(XrXirCallEntry);
    if (bytes > config->metadata_limit || bytes > SIZE_MAX) return XR_XIR_CALL_LIMIT;
    if (!xr_xir_compile_program_retain(program)) return XR_XIR_CALL_LIMIT;
    XrXirDomain *domain = NULL;
    XrXirDomainBudgetControls controls = {config->requested_value_limit, config->requested_call_limit,
        config->work_limit, config->metadata_limit, config->call_limit};
    if (owner) {
        if (!xr_xir_domain_retain(owner->domain)) status = XR_XIR_CALL_LIMIT;
        else domain = owner->domain;
    } else status = value_call_status(xr_xir_domain_new_budgeted(config->value_limit, &controls, &domain));
    if (status != XR_XIR_CALL_READY) { xr_xir_compile_program_drop(program); return status; }
    if (!xr_xir_domain_work(domain, (uint64_t)program->entry_count + d->module_count + d->slot_count)) {
        xr_xir_domain_drop(domain); xr_xir_compile_program_drop(program); return XR_XIR_CALL_LIMIT;
    }
    XrXirInstance *instance = instance_metadata_allocate(domain, sizeof(*instance), &status);
    if (!instance) { xr_xir_domain_drop(domain); xr_xir_compile_program_drop(program); return status; }
    instance->program = program;
    instance->owner = owner;
    instance->domain = domain;
    instance->config = *config;
    instance->metadata_bytes = bytes;
    instance->current_module = UINT32_MAX;
    instance->budget = (XrXirCallBudget){.byte_limit = config->call_limit,
        .requested_limit = config->requested_call_limit, .resume_limit = config->poll_limit, .work_domain = domain};
    if (!owner) {
        instance->active_generation = instance;
        instance->published_generation = instance;
        instance->generation_id = instance->generation_counter = 1;
    }
    instance->entries = instance_metadata_allocate(domain,
        ((uint64_t)program->entry_count + 1) * sizeof(*instance->entries), &status);
    if (status == XR_XIR_CALL_READY) instance->ready = instance_metadata_allocate(domain, d->module_count, &status);
    if (status == XR_XIR_CALL_READY) instance->published_counts = instance_metadata_allocate(domain,
        (uint64_t)d->module_count * sizeof(*instance->published_counts), &status);
    if (d->slot_count && status == XR_XIR_CALL_READY) {
        instance->slots = instance_metadata_allocate(domain, (uint64_t)d->slot_count * sizeof(*instance->slots), &status);
        if (status == XR_XIR_CALL_READY) instance->published = instance_metadata_allocate(domain, d->slot_count, &status);
        if (status == XR_XIR_CALL_READY) instance->publication_order = instance_metadata_allocate(domain,
            (uint64_t)d->slot_count * sizeof(*instance->publication_order), &status);
    }
    if (!instance->entries || !instance->ready || !instance->published_counts ||
        (d->slot_count && (!instance->slots || !instance->published || !instance->publication_order))) {
        instance_dispose(instance);
        return status;
    }
    memcpy(instance->entries, program->entries, (size_t) program->entry_count * sizeof(*program->entries));
    status = instance_executor_new(instance, &instance->executor);
    if (status != XR_XIR_CALL_READY) { instance_dispose(instance); return status; }
    instance->metadata_bytes = xr_xir_domain_budget_stats(domain).metadata_live;
    *output = instance;
    return XR_XIR_CALL_READY;
}
XrXirCallStatus xr_xir_instance_new(XrXirProgram *program, const XrXirInstanceConfig *config,
    XrXirInstance **output) {
    if (!program || !output || *output) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status=instance_config_status(config);
    if (status!=XR_XIR_CALL_READY) return status;
    if (program->requires_birth_storage) return XR_XIR_CALL_BAD_STATE;
    return instance_new_state(program, config, NULL, output);
}
XrXirInstanceState xr_xir_instance_state(const XrXirInstance *instance) {
    if (!instance_owner_current(instance)) return XR_XIR_INSTANCE_DRAINING;
    XrXirInstance *current = instance_current((XrXirInstance *)instance);
    return !current || instance->stopping ? XR_XIR_INSTANCE_DRAINING : current->state;
}
XR_FUNC XrXirCallStatus xir_instance_native_source_bind(XrXirInstance *instance,XrXirProgram *program,
    const XirCallNativeSourceObserver *observer) {
    if (!instance || !program || !observer || !observer->owner || !observer->before || !observer->call || !instance_owner_current(instance))
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->driving || instance_observing(instance)) return XR_XIR_CALL_BUSY;
    if (instance->program!=program || instance->state!=XR_XIR_INSTANCE_NEW || instance->stopping ||
        instance->call || instance->epoch || instance->budget.native_source) return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_domain_work(instance->domain,1)) return XR_XIR_CALL_LIMIT;
    instance->budget.native_source=observer;instance->budget.native_source_instance=instance;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_instance_class_observation_open(XrXirInstance *instance,
    uint32_t capacity, XrXirClassObservation **output) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance || !output || *output || !capacity || capacity > XR_XIR_CLASS_OBSERVATION_CAPACITY)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    if (instance->state != XR_XIR_INSTANCE_NEW || instance->stopping || instance->call)
        return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status = value_call_status(
        xr_xir_domain_class_observation_open(instance->domain, capacity, output));
    if (status == XR_XIR_CALL_READY) instance->metadata_bytes = xr_xir_domain_budget_stats(instance->domain).metadata_live;
    return status;
}
static void drop_arguments(XrXirDomain *domain, XrXirValue *arguments, uint32_t count) {
    if (arguments) for (uint32_t p = 0; p < count; ++p) xr_xir_value_drop(&arguments[p]);
    if (arguments) xr_xir_domain_metadata_deallocate(domain, arguments, (uint64_t)count * sizeof(*arguments));
}
static XrXirCallStatus capture_arguments(XrXirInstance *instance, const XrXirValue *arguments,
                                        uint32_t count, XrXirValue **output) {
    *output = NULL;
    if (!count) return XR_XIR_CALL_READY;
    uint64_t bytes = (uint64_t) count * sizeof(XrXirValue);
    if (bytes > SIZE_MAX || bytes > instance->config.metadata_limit - instance->metadata_bytes)
        return XR_XIR_CALL_LIMIT;
    if (!xr_xir_domain_work(instance->domain, count)) return XR_XIR_CALL_LIMIT;
    XrXirCallStatus allocation_status = XR_XIR_CALL_READY;
    XrXirValue *owned = instance_metadata_allocate(instance->domain, bytes, &allocation_status);
    if (!owned) return allocation_status;
    for (uint32_t p = 0; p < count; ++p) {
        XrXirCallStatus status = value_call_status(xr_xir_value_copy(&arguments[p], &owned[p]));
        if (status != XR_XIR_CALL_READY) { drop_arguments(instance->domain, owned, count); return status; }
    }
    *output = owned;
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_start(XrXirInstance *instance, uint32_t entry,
    const XrXirValue *arguments, uint32_t count, const XrXirValue *function,
    XrXirValueAdmission *admission, bool test_entry) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (admission && admission->domain && !xr_xir_domain_current(admission->domain))
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    if (!instance->program) return XR_XIR_CALL_BAD_STATE;
    if (instance->state == XR_XIR_INSTANCE_FAILED) return instance->failure.status;
    if (instance->stopping) return XR_XIR_CALL_BAD_STATE;
    if (instance->executor && !xr_xir_task_executor_root_idle(instance->executor)) return XR_XIR_CALL_BUSY;
    if (instance->call) {
        XrXirCallStatus state = xr_xir_call_state(instance->call);
        if (state == XR_XIR_CALL_READY || state == XR_XIR_CALL_SUSPENDED) return XR_XIR_CALL_BUSY;
        if (instance_root(instance)->prepared_generation == instance) return XR_XIR_CALL_BAD_STATE;
    }
    const XrXirDeclarations *d = instance->program->declarations;
    const XrXirFunctionBinding *binding = function ? xr_xir_function_binding(function) : NULL;
    if (function && !binding) return XR_XIR_CALL_BAD_ARGUMENT;
    if (entry >= instance->program->entry_count ||
        !instance->program->active_modules[d->functions[entry].module] ||
        (!test_entry && !binding && entry != d->entry_function && !d->functions[entry].exported)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *requested = &instance->program->entries[entry];
    if (requested->flags & XR_XIR_ENTRY_GENERATOR) return XR_XIR_CALL_BAD_ARGUMENT;
    if (test_entry) {
        const XrXirFunctionIdentity *identity = &d->functions[entry];
        if (identity->module != d->root_module || identity->test_role == XR_XIR_TEST_ROLE_NONE ||
            identity->test_role == XR_XIR_TEST_ROLE_SKIP || identity->test_role > XR_XIR_TEST_ROLE_AFTER_EACH ||
            identity->nominal_owner || entry == d->entry_function ||
            entry == d->modules[d->root_module].initializer || requested->parameter_count ||
            requested->result != XR_XIR_UNIT) return XR_XIR_CALL_BAD_ARGUMENT;
    }
    if (requested->cleanup_owner) return XR_XIR_CALL_BAD_ARGUMENT;
    uint32_t captures = binding ? binding->capture_count : 0;
    if (captures > requested->parameter_count || count != requested->parameter_count - captures ||
        (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    for (uint32_t p = 0; p < requested->parameter_count; ++p) {
        const XrXirValue *value = p < captures ? &binding->captures[p] : &arguments[p - captures];
        XrXirCallStatus admitted = admit_instance_value(admission, value, requested->parameters[p]);
        if (admitted != XR_XIR_CALL_READY) return admitted;
    }
    XrXirInstance *root_instance = instance_root(instance);
    XrXirInstance *previous = instance_current(root_instance);
    uint64_t epoch = root_instance->call_epoch > instance->epoch ? root_instance->call_epoch : instance->epoch;
    if (epoch == UINT64_MAX) return XR_XIR_CALL_LIMIT;
    XrXirValue *owned = NULL;
    XrXirCallStatus status = capture_arguments(instance, arguments, count, &owned);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirValue prepared_function = {0};
    if (function) {
        if (!admission->work || !xr_xir_domain_work(instance->domain, 1)) {
            drop_arguments(instance->domain, owned, count); return XR_XIR_CALL_LIMIT;
        }
        --admission->work;
        status = value_call_status(xr_xir_value_copy(function, &prepared_function));
        if (status != XR_XIR_CALL_READY) { drop_arguments(instance->domain, owned, count); return status; }
    }
    uint32_t root = instance->program->entry_count;
    instance->entries[root] = (XrXirCallEntry) {XR_XIR_CALL_ABI_VERSION,
        count ? requested->parameters + captures : NULL,
        count, requested->result, sizeof(uint32_t), initialization_resume, initialization_release, NULL, 0, 0};
    XrXirCallConfig config;
    status = xr_xir_call_config_init(&config, sizeof(config));
    if (status != XR_XIR_CALL_READY) {
        drop_arguments(instance->domain, owned, count); xr_xir_value_drop(&prepared_function); return status;
    }
    config.entries = instance->entries; config.entry_count = root + 1; config.instance = root_instance;
    config.byte_limit = instance->config.call_limit; config.poll_limit = instance->config.poll_limit;
    config.depth_limit = instance->config.depth_limit;
    /* Global wake epochs may skip after a different generation runs or aborts.
     * Alternate this generation's ledgers independently of that sequence. */
    uint32_t accounting_index = 1 - instance->accounting_index;
    config.accounting = &instance->accounting[accounting_index];
    config.output = instance->config.output; config.admission = *admission; config.time = instance->config.time;
    XrXirTaskExecutor *previous_replacement = NULL;
    if (previous != instance && previous->call && !root_instance->prepared_generation) {
        if (!xr_xir_task_executor_root_idle(previous->executor) ||
            xr_xir_task_executor_completion_status(previous->executor) != XR_XIR_CALL_READY ||
            xr_xir_call_cleanup_incomplete(previous->call)) {
            drop_arguments(instance->domain, owned, count);
            xr_xir_value_drop(&prepared_function);
            return XR_XIR_CALL_BAD_STATE;
        }
        status = instance_executor_new(previous, &previous_replacement);
        if (status != XR_XIR_CALL_READY) {
            drop_arguments(instance->domain, owned, count);
            xr_xir_value_drop(&prepared_function);
            return status;
        }
    }
    XrXirCall *replacement = NULL;
    XrXirCallRequest request = {root, owned, count, instance_cell_role, instance->program};
    status = xr_xir_call_new_budgeted(&config, &request, instance_budget(instance), &replacement);
    drop_arguments(instance->domain, owned, count);
    if (status != XR_XIR_CALL_READY) {
        (void)xr_xir_task_executor_free(previous_replacement, NULL);
        xr_xir_value_drop(&prepared_function); return status;
    }
    XrXirTaskExecutor *executor = instance->executor;
    if (!executor) status = instance_executor_new(instance, &executor);
    if (status == XR_XIR_CALL_READY)
        status = xr_xir_task_executor_attach_root(executor, replacement, epoch + 1);
    if (status != XR_XIR_CALL_READY) {
        xr_xir_call_free(replacement);
        if (!instance->executor) (void)xr_xir_task_executor_free(executor, NULL);
        (void)xr_xir_task_executor_free(previous_replacement, NULL);
        xr_xir_value_drop(&prepared_function); return status;
    }
    if (previous_replacement) instance_retire_call(previous, previous_replacement);
    instance->executor = executor;
    xr_xir_call_free(instance->call);
    xr_xir_value_drop(&instance->pending_function);
    instance->pending_function = prepared_function;
    instance->call = replacement;
    instance->accounting_index = accounting_index;
    instance->epoch = epoch + 1;
    root_instance->call_epoch = instance->epoch;
    root_instance->active_generation = instance;
    instance->requested = entry;
    instance->last_outcome = XR_XIR_CALL_READY;
    if (instance->state == XR_XIR_INSTANCE_NEW) instance->state = XR_XIR_INSTANCE_INITIALIZING;
    return status;
}
XR_FUNC XrXirCallStatus xir_instance_visit_vm(XrXirInstance *instance,const XrXirExecutorBinding *binding,uint64_t frame,bool *entered) {
    if (!instance_owner_current(instance) || !instance || !binding || !entered) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    instance=instance_current(instance);
    if (instance->stopping || !instance->call || instance->state==XR_XIR_INSTANCE_FAILED) return XR_XIR_CALL_BAD_STATE;
    instance_driving(instance,true);instance->debug_visiting=true;
    XrXirCallStatus status=xir_task_executor_visit_vm(instance->executor,binding,frame,entered);
    instance->debug_visiting=false;instance_driving(instance,false);return status;
}
XrXirInstanceResult xr_xir_instance_poll_bounded(XrXirInstance *instance, uint64_t quantum) {
    if (!instance_owner_current(instance)) return (XrXirInstanceResult){.outcome = {.status = XR_XIR_CALL_BAD_ARGUMENT}};
    instance = instance_current(instance);
    if (!instance || !quantum) return (XrXirInstanceResult) {instance_result(XR_XIR_CALL_BAD_ARGUMENT), 0};
    if (instance_busy(instance))
        return (XrXirInstanceResult) {instance_result(XR_XIR_CALL_BUSY), instance->epoch};
    if (instance->state == XR_XIR_INSTANCE_FAILED)
        return (XrXirInstanceResult) {instance->failure, instance->epoch};
    if (!instance->call) return (XrXirInstanceResult) {instance_result(XR_XIR_CALL_BAD_STATE), instance->epoch};
    instance_driving(instance, true);
    XrXirCallResult result = xr_xir_task_executor_poll_root(instance->executor, quantum);
    if (instance->state == XR_XIR_INSTANCE_INITIALIZING &&
        result.status != XR_XIR_CALL_READY && result.status != XR_XIR_CALL_SUSPENDED) {
        fail_initialization(instance, result);
        result = instance->failure;
    }
    instance_driving(instance, false);
    if (result.status != XR_XIR_CALL_CONSUMED) instance->last_outcome = result.status;
    return (XrXirInstanceResult) {result, instance->epoch};
}
XrXirCallStatus xr_xir_instance_resume(XrXirInstance *instance, uint64_t epoch, uint64_t wake) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    instance = instance_current(instance);
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    if (instance->stopping || !instance->call || epoch != instance->epoch) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_task_executor_resume_root(instance->executor, epoch, wake);
}
XrXirCallStatus xr_xir_instance_wait_request(const XrXirInstance *instance, uint64_t epoch,
    uint64_t wake, XrXirWaitRequest *output) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy((XrXirInstance *)instance)) return XR_XIR_CALL_BUSY;
    instance = instance_current((XrXirInstance *)instance);
    if (!instance || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    if (instance->stopping || !instance->call || epoch != instance->epoch) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_task_executor_root_wait(instance->executor, epoch, wake, output);
}
XrXirCallStatus xr_xir_instance_take_result(XrXirInstance *instance, XrXirValue *output) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    instance = instance_current(instance);
    if (!instance || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    if (instance->state == XR_XIR_INSTANCE_FAILED) return XR_XIR_CALL_BAD_STATE;
    if (!instance->call) return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_task_executor_root_idle(instance->executor)) return XR_XIR_CALL_BAD_STATE;
    if (!xir_call_empty_destination(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus completed = xr_xir_task_executor_completion_status(instance->executor);
    /* A group failure retains its status and never transfers the root value. */
    if (completed != XR_XIR_CALL_READY) return completed;
    return xr_xir_call_take_result(instance->call, output);
}
XrXirCallStatus xr_xir_instance_copy_failure(XrXirInstance *instance, XrXirCallResult *output) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    instance = instance_current(instance);
    if (!instance || !xr_xir_call_result_empty(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    if (instance->state != XR_XIR_INSTANCE_FAILED) return XR_XIR_CALL_BAD_STATE;
    XrXirCallResult copy = instance_result(XR_XIR_CALL_READY);
    XrXirCallStatus status = value_call_status(xr_xir_call_result_copy(&instance->failure, &copy));
    if (status != XR_XIR_CALL_READY) return status;
    XrXirCallStatus failure_status = copy.status;
    xr_xir_call_result_move(&copy, output);
    return failure_status;
}
XrXirCallStatus xr_xir_instance_cancel_current(XrXirInstance *instance) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    instance = instance_current(instance);
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    if (instance->stopping || !instance->call) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_task_executor_cancel_root(instance->executor);
}
XrXirCallStatus xr_xir_instance_close_activation(XrXirInstance *instance) {
    if (!instance_owner_current(instance) || !instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    XrXirInstance *root = instance_root(instance);
    if (root->prepared_generation) return XR_XIR_CALL_BAD_STATE;
    instance = instance_current(instance);
    if (!instance->executor) return XR_XIR_CALL_READY;
    instance_driving(instance, true);
    XrXirCallStatus status = xr_xir_task_executor_drain(instance->executor,XR_XIR_CALL_READY);
    XR_CHECK(xr_xir_task_executor_root_idle(instance->executor),
        "an exclusively owned current activation drains every runnable task");
    if (instance->call && instance->state == XR_XIR_INSTANCE_INITIALIZING) {
        XrXirCallResult result = xr_xir_task_executor_poll_root(instance->executor, 1);
        XR_CHECK(result.status != XR_XIR_CALL_READY && result.status != XR_XIR_CALL_SUSPENDED,
            "drain leaves the actual initialization outcome terminal");
        fail_initialization(instance, result);
    }
    (void)xr_xir_task_executor_free(instance->executor, NULL);
    instance->executor = NULL;
    XrXirCallStatus released = xr_xir_call_free(instance->call);
    instance->call = NULL;
    xr_xir_value_drop(&instance->pending_function);
    instance->metadata_bytes = xr_xir_domain_budget_stats(instance->domain).metadata_live;
    instance_driving(instance, false);
    if (status == XR_XIR_CALL_READY) status = released;
    if (status != XR_XIR_CALL_READY) (void)xr_xir_instance_stop(root);
    return status;
}
XR_FUNC XrXirCallStatus xir_instance_host_finish(XrXirInstance *instance,XrXirInstanceResult *observed) {
    if (!instance_owner_current(instance) || !instance || !observed) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    XrXirInstance *root=instance_root(instance),*active=instance_current(root);
    if (root->prepared_generation) return XR_XIR_CALL_BAD_STATE;
    if (instance!=root && instance!=active) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!active->executor || !active->call || active->last_outcome==XR_XIR_CALL_READY ||
        active->last_outcome==XR_XIR_CALL_SUSPENDED) return XR_XIR_CALL_BAD_STATE;
    instance_driving(active,true);
    XrXirCallStatus closed=xr_xir_task_executor_drain(active->executor,XR_XIR_CALL_READY);
    if (closed==XR_XIR_CALL_CLEANUP_FAILED && active->state==XR_XIR_INSTANCE_FAILED &&
        (active->failure.status==XR_XIR_CALL_THROWN || xr_xir_call_panic_status(active->failure.status))) {
        XrXirCallResult selected=xr_xir_task_executor_poll_root(active->executor,1);
        XR_CHECK(selected.status==closed,"late initialization cleanup keeps its actual fatal root owner");
        fail_initialization(active,selected);
    }
    instance_driving(active,false);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(active,1);
    XR_CHECK(result.outcome.status!=XR_XIR_CALL_READY && result.outcome.status!=XR_XIR_CALL_SUSPENDED &&
        xr_xir_call_result_valid(&result.outcome),"a finished host group owns its selected complete terminal");
    *observed=result;return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xir_instance_host_failure(XrXirInstance *instance,XrXirInstanceResult *observed) {
    if (!instance_owner_current(instance) || !instance || !observed) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    XrXirInstance *root=instance_root(instance);
    if (root->prepared_generation) return XR_XIR_CALL_BAD_STATE;
    XrXirInstance *active=instance_current(root);
    if ((instance!=root && instance!=active) || root->stopping || active->stopping ||
        active->state==XR_XIR_INSTANCE_FAILED || active->state==XR_XIR_INSTANCE_DRAINING)
        return XR_XIR_CALL_BAD_STATE;
    instance=active;
    if (!instance->executor || !instance->call) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus current=xr_xir_call_state(instance->call);
    if (current!=XR_XIR_CALL_READY && current!=XR_XIR_CALL_SUSPENDED) return XR_XIR_CALL_BAD_STATE;
    instance_driving(instance,true);
    XrXirCallStatus status=xr_xir_task_executor_drain(instance->executor,XR_XIR_CALL_HOST_ERROR);
    instance_driving(instance,false);
    (void)xr_xir_instance_stop(root);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1);
    XR_CHECK(result.outcome.status==status && xr_xir_call_result_valid(&result.outcome),
        "hard host close preserves its existing complete first terminal");
    *observed=result;return XR_XIR_CALL_READY;
}
XrXirCallStatus xr_xir_instance_stop(XrXirInstance *instance) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *root = instance_root(instance);
    if (instance_observing(instance)) return XR_XIR_CALL_BUSY;
    root->stopping = true;
    if (root->function_gate) root->function_gate->instance = NULL;
    for (XrXirInstance *state = root->generation_next; state; state = state->generation_next) {
        state->stopping = true;
        if (state->function_gate) state->function_gate->instance = NULL;
    }
    instance = instance_current(root);
    instance->stopping = true;
    if (instance->function_gate) instance->function_gate->instance = NULL;
    bool driving = instance->driving;
    instance_driving(instance, true);
    if (!instance->executor) { instance_driving(instance, driving); return XR_XIR_CALL_READY; }
    XrXirCallStatus status = xr_xir_task_executor_stop(instance->executor);
    if (!driving && xr_xir_task_executor_root_idle(instance->executor))
        status = xr_xir_task_executor_completion_status(instance->executor);
    else if (instance->call && !driving) {
        do { status = xr_xir_task_executor_poll_root(instance->executor, 256).status; }
        while (status == XR_XIR_CALL_READY);
    }
    if (!driving && instance->call && instance->state == XR_XIR_INSTANCE_INITIALIZING &&
        xr_xir_task_executor_root_idle(instance->executor)) {
        XrXirCallResult result = xr_xir_task_executor_poll_root(instance->executor, 1);
        if (result.status != XR_XIR_CALL_READY && result.status != XR_XIR_CALL_SUSPENDED) {
            /* Drain publishes the same sticky initialization outcome as an ordinary poll. */
            fail_initialization(instance, result);
            status = instance->failure.status;
        }
    }
    instance_driving(instance, driving);
    return status == XR_XIR_CALL_CANCELLED || status == XR_XIR_CALL_CANCEL_REQUESTED ? XR_XIR_CALL_READY : status;
}
static XrXirCallStatus instance_free_state(XrXirInstance *instance) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance) return XR_XIR_CALL_READY;
    /* The caller already excludes mutation of the root. Its driving flag
     * describes this teardown and must not be mistaken for callback reentry. */
    if (instance_observing(instance)) return XR_XIR_CALL_BUSY;
    instance->driving = true;
    instance->stopping = true;
    if (instance->function_gate) instance->function_gate->instance = NULL;
    XrXirCallStatus tasks = xr_xir_task_executor_free(instance->executor, NULL);
    instance->executor = NULL;
    XrXirCallStatus status = xr_xir_call_free(instance->call);
    instance->call = NULL;
    if (status == XR_XIR_CALL_READY) status = tasks;
    clear_slots(instance);
    xr_xir_call_result_drop(&instance->failure);
    instance_dispose(instance);
    return status;
}
XrXirCallStatus xr_xir_instance_free(XrXirInstance *instance) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance) return XR_XIR_CALL_READY;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    XrXirInstance *root = instance_root(instance), *current = instance_current(instance);
    if (instance != root) return XR_XIR_CALL_BAD_ARGUMENT;
    root->driving = root->stopping = true;
    root->published_generation = root->prepared_generation = NULL;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    while (root->generation_next) {
        XrXirInstance *state = root->generation_next;
        root->generation_next = state->generation_next;
        root->active_generation = state;
        bool is_current = state == current;
        XrXirCallStatus disposed = instance_free_state(state);
        if (is_current) status = disposed;
    }
    root->active_generation = root;
    bool exhausted = root->budget.exhausted;
    bool is_original = current == root;
    XrXirCallStatus original = instance_free_state(root);
    if (is_original) status = original;
    return status == XR_XIR_CALL_READY && exhausted ? XR_XIR_CALL_LIMIT : status;
}
#include "xxir_instance_generation.inc.c"
static XrXirCallStatus resolve_function_target(XrXirInstance *instance,const XrXirValue *value,
    uint32_t *entry,XrXirValueAdmission *admission,XrXirInstance **output);
/* Execution membership belongs to the active executor; metadata belongs to the
 * exact live frame generation. A borrowed old Program never creates a Call. */
static bool instance_view_member(XrXirInstance *instance,XrXirCallView *view,bool *child) {
    XrXirInstance *active=instance_current(view->instance);
    return instance && instance_owner_current(instance) && instance_root(instance)==view->instance &&
        active && active->driving && !instance_observing(active) &&
        (!active->stopping || instance_cleanup_grant(active)) &&
        xr_xir_task_executor_view_member(active->executor,view,child);
}
static XrXirInstance *view_instance(XrXirCallView *view) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || !view->instance || admission->function!=admit_function_binding) return NULL;
    XrXirInstance *active=instance_current(view->instance),*instance=admission->context;
    bool known=instance==active;
    if (!known && instance==instance_root(active)) known=true;
    for (XrXirInstance *state=known?NULL:instance_root(active)->generation_next;state;state=state->generation_next) {
        if (!admission->work || !xr_xir_domain_work(active->domain,1)) return NULL;
        --admission->work;
        if (state==instance) {known=true;break;}
    }
    bool child=false;
    if (!known || !instance_view_member(instance,view,&child) || admission->domain!=instance->domain ||
        admission->arena!=instance->program->arena ||
        xr_xir_call_current_entry(view->activation)>=instance->program->entry_count) return NULL;
    return instance;
}
static bool view_is_child(const XrXirInstance *instance,const XrXirCallView *view) {
    bool child=false;
    const XrXirInstance *active=instance_current((XrXirInstance *)instance);
    XR_CHECK(xr_xir_task_executor_view_member(active->executor,view,&child),
        "an admitted instance view keeps its exact activation membership");
    return child;
}
static XrXirCallStatus instance_function_producer_value(XrXirInstance *instance,
    XrXirValueAdmission *admission,const XrXirValue *function,XirEffectProducerView *output);
#include "xxir_instance_invocation.inc.c"

/* The private admission owner distinguishes an Instance from an opaque host
 * context. Conditional Cell-root inspection charges the current Domain before
 * the Call owner prepares any callee loans. */

static XrXirCallStatus instance_call_entry_check(XrXirCallView *view, uint32_t entry,
    const XrXirValue *function, const XrXirValue *arguments, uint32_t count,
    const XirEffectInvocationSelection *prepared) {
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    if (!admission) return XR_XIR_CALL_BAD_STATE;
    if (admission->function != admit_function_binding) return XR_XIR_CALL_READY;
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = admission->context;
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance || instance_root(instance) != view->instance || admission->arena != instance->program->arena ||
        admission->domain != instance->domain) return XR_XIR_CALL_BAD_STATE;
    bool child = false;
    if (!instance_view_member(instance,view,&child)) return XR_XIR_CALL_BAD_STATE;
    XrXirInstance *target=instance;
    if (function && xr_xir_function_binding(function)) {
        uint32_t resolved=0;
        XrXirCallStatus status=resolve_function_target(instance,function,&resolved,admission,&target);
        if (status!=XR_XIR_CALL_READY) return status;
        if (resolved!=entry) return XR_XIR_CALL_BAD_ARGUMENT;
    }
    /* A different sealed equation owner requires an explicit invocation proof. */
    if (child && target!=instance) return XR_XIR_CALL_BAD_STATE;
    const XrXirProgram *program = target->program;
    uint32_t caller = xr_xir_call_current_entry(view->activation);
    if (entry >= program->entry_count || caller > instance->program->entry_count ||
        (child && caller == instance->program->entry_count) || !program->permissions ||
        program->permissions->function_count != program->entry_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d = program->declarations;
    uint32_t module = d->functions[entry].module;
    if (!program->active_modules[module]) return XR_XIR_CALL_BAD_STATE;
    if (entry == d->modules[module].initializer)
        return target==instance && !child && caller == program->entry_count &&
            instance->state == XR_XIR_INSTANCE_INITIALIZING && instance->current_module == module ?
            XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
    if (caller == instance->program->entry_count &&
        (target!=instance || instance->state != XR_XIR_INSTANCE_READY || instance->requested != entry)) return XR_XIR_CALL_BAD_STATE;
    if (!target->ready[module] && target->current_module != module) return XR_XIR_CALL_BAD_STATE;
    if (!child) return XR_XIR_CALL_READY;
    if (!function) return XR_XIR_CALL_BAD_STATE;
    XrXirCallFrontier frontier={0};
    if (xr_xir_call_invocation_pending(view,&frontier)) {
        if (entry!=frontier.target ||
            (function->type!=XR_XIR_UNIT)!=(frontier.producer!=UINT32_MAX)) return XR_XIR_CALL_BAD_STATE;
        XirEffectInvocationSelection selection={0};
        XrXirCallStatus selected=function->type!=XR_XIR_UNIT ?
            instance_invocation_select(view,frontier.instruction,function,arguments,count,&selection) :
            instance_invocation_direct(view,frontier.instruction,entry,arguments,count,&selection);
        if (selected!=XR_XIR_CALL_READY) return selected;
        if (selection.target!=entry || selection.producer!=frontier.producer) return XR_XIR_CALL_BAD_STATE;
        if (prepared) {
            if (selection.owner != prepared->owner || selection.caller != prepared->caller ||
                selection.instruction != prepared->instruction || selection.target != prepared->target ||
                selection.producer != prepared->producer || selection.parent != prepared->parent ||
                selection.node != prepared->node || selection.edge != prepared->edge)
                return XR_XIR_CALL_BAD_STATE;
            return XR_XIR_CALL_READY;
        }
        return xr_xir_call_invocation_accept(view,&selection);
    }
    if (prepared && prepared->owner) return XR_XIR_CALL_BAD_STATE;
    uint32_t actual_root = 0;
    XrXirCallStatus root_status = instance_entry_cell_root(view, program, entry,
        function, arguments, count, &actual_root);
    if (root_status != XR_XIR_CALL_READY) return root_status;
    if (actual_root) return XR_XIR_CALL_BAD_STATE;
    /* The carrier may advertise less precision than the sealed body.
     * Ordinary child calls still require a closed no-root advertisement. */
    if (function->type != XR_XIR_UNIT) {
        const XrXirTypeNode *signature = xr_xir_callable_signature(program->types, (XrXirType)function->type);
        if (!signature || (signature->flags & XR_XIR_CALLABLE_ROOT_MASK) != XR_XIR_CALLABLE_ROOT_NONE)
            return XR_XIR_CALL_BAD_STATE;
    }
    return XR_XIR_CALL_READY;
}

XR_FUNC XrXirCallStatus xr_xir_instance_call_entry_admit(XrXirCallView *view, uint32_t entry,
    const XrXirValue *function, const XrXirValue *arguments, uint32_t count) {
    return instance_call_entry_check(view, entry, function, arguments, count, NULL);
}
/* The Call owner invokes this only after complete actual loan preparation.
 * Rechecking never publishes a second pending selection or manufactures a view. */
XR_FUNC XrXirCallStatus xr_xir_instance_call_entry_recheck(XrXirCallView *view, uint32_t entry,
    const XrXirValue *function, const XrXirValue *arguments, uint32_t count,
    const XirEffectInvocationSelection *prepared) {
    if (!prepared) return XR_XIR_CALL_BAD_ARGUMENT;
    return instance_call_entry_check(view, entry, function, arguments, count, prepared);
}

static bool worker_slot_read(const XrXirInstance *instance, uint32_t slot) {
    const XrXirProgram *program = instance->program;
    const XrXirSlot *entry = &program->declarations->slots[slot];
    return !entry->mutable && instance->ready[entry->module] && program->permissions &&
        program->permissions->entries[(uint64_t)program->entry_count + slot].worker == XR_XIR_OK;
}
static bool value_unit(XrXirValue value) {
    return value.type == XR_XIR_UNIT && !value.reserved && !value.payload;
}
#include "xxir_instance_cell_access.inc.c"
#include "xxir_instance_class_access.inc.c"
#include "xxir_instance_debug_activation.inc.c"
static XrXirCallStatus value_place(XrXirCallView *view, XrXirInstance *instance,
    const XrXirValueReceiver *receiver, bool writable, XrXirValueAdmission *admission,
    XrXirValuePlace *place) {
    if (admission && admission->domain && !xr_xir_domain_current(admission->domain))
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (!receiver || admission != xr_xir_call_admission(view)) return XR_XIR_CALL_BAD_STATE;
    place->type = receiver->type;
    if (receiver->kind == XR_XIR_ROOT_CELL) {
        if (receiver->local_payload || receiver->slot) return XR_XIR_CALL_BAD_STATE;
        XrXirCallStatus gate = instance_cell_access(view, instance, &receiver->value);
        if (gate != XR_XIR_CALL_READY) return gate;
        XrXirValueStatus status = xr_xir_call_cell_place(view, &receiver->value, place);
        if (status != XR_XIR_VALUE_OK) return value_call_status(status);
        return place->type == receiver->type ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
    }
    if (receiver->kind == XR_XIR_ROOT_OBJECT) {
        if (view_is_child(instance,view)) return XR_XIR_CALL_BAD_STATE;
        /* The object handle is only navigated: class steps never publish through this temporary. */
        if (receiver->local_payload || receiver->slot || receiver->value.type != (uint32_t) receiver->type)
            return XR_XIR_CALL_BAD_STATE;
        place->payload = (void *) &receiver->value.payload;
        return XR_XIR_CALL_READY;
    }
    if (!value_unit(receiver->value)) return XR_XIR_CALL_BAD_STATE;
    if (receiver->kind == XR_XIR_ROOT_LOCAL) {
        uint32_t entry = xr_xir_call_current_entry(view->activation);
        uintptr_t address = (uintptr_t) receiver->local_payload, base = (uintptr_t) view->state;
        uint32_t bytes = instance->entries[entry].state_bytes;
        if (receiver->slot || !receiver->local_payload || address < base ||
            bytes < sizeof(int64_t) || address - base > bytes - sizeof(int64_t))
            return XR_XIR_CALL_BAD_STATE;
        place->payload = receiver->local_payload;
        return XR_XIR_CALL_READY;
    }
    const XrXirDeclarations *d = instance->program->declarations;
    if (receiver->kind != XR_XIR_ROOT_SLOT || receiver->local_payload || receiver->slot >= d->slot_count)
        return XR_XIR_CALL_BAD_STATE;
    uint32_t slot = receiver->slot, module = d->slots[slot].module;
    if (writable || d->slots[slot].mutable) return XR_XIR_CALL_BAD_STATE;
    if (view_is_child(instance, view) && (writable || !worker_slot_read(instance, slot)))
        return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus scoped=instance_slot_scope(view,instance,xr_xir_call_current_entry(view->activation),module);
    if (scoped!=XR_XIR_CALL_READY) return scoped;
    if (instance->slot_origins && instance->slot_origins[slot].owner) {
        InstanceStorageRoute route=instance->slot_origins[slot];XrXirInstance *birth=route.owner;
        if (d->slots[slot].type!=receiver->type || !instance->ready[module] ||
            route.index>=birth->program->declarations->slot_count ||
            !birth->published[route.index] || !birth->ready[birth->program->declarations->slots[route.index].module])
            return XR_XIR_CALL_BAD_STATE;
        XrXirCallStatus status=admit_instance_value(admission,&birth->slots[route.index],receiver->type);
        if (status!=XR_XIR_CALL_READY) return status;
        place->payload=&birth->slots[route.index].payload;
        return XR_XIR_CALL_READY;
    }
    if (d->slots[slot].type != receiver->type || !instance->published[slot] ||
        (!instance->ready[module] && instance->current_module != module) ||
        (writable && !d->slots[slot].mutable))
        return XR_XIR_CALL_BAD_STATE;
    place->payload = &instance->slots[slot].payload;
    return XR_XIR_CALL_READY;
}
#include "xxir_instance_path.inc.c"
#include "xxir_instance_class_path.inc.c"
#include "xxir_instance_string_owned.inc.c"
XrXirCallStatus xr_xir_instance_array_new(XrXirCallView *view, XrXirType type,
    const XrXirValue *values, uint32_t count, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    return value_call_status(xr_xir_array_new(type, values, count, admission, output));
}
XrXirCallStatus xr_xir_instance_array_repeat(XrXirCallView *view, XrXirType type,
    int64_t length, const XrXirValue *fill, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance || !fill || !output) return XR_XIR_CALL_BAD_STATE;
    if (length < 0) return XR_XIR_CALL_NUMERIC_RANGE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    return value_call_status(xr_xir_array_repeat(type, length, fill, admission, output));
}
XR_FUNC XrXirCallStatus xr_xir_instance_array_with_capacity(XrXirCallView *view,
    XrXirType type, int64_t capacity, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!output || !value_unit(*output)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (capacity < 0) return XR_XIR_CALL_NUMERIC_RANGE;
    return value_call_status(xr_xir_array_with_capacity(type, capacity, xr_xir_call_admission(view), output));
}
XR_FUNC XrXirCallStatus xr_xir_instance_array_reserve(XrXirCallView *view,
    const XrXirValue *array, int64_t capacity, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!output || !value_unit(*output)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (capacity < 0) return XR_XIR_CALL_NUMERIC_RANGE;
    return value_call_status(xr_xir_array_reserve(array, capacity, xr_xir_call_admission(view), output));
}
XR_FUNC XrXirCallStatus xr_xir_instance_array_capacity(XrXirCallView *view,
    const XrXirValueReceiver *receiver, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (receiver && receiver->kind == XR_XIR_ROOT_CELL) {
        XrXirValuePath path = {0}; XrXirFaultDetail fault = {0};
        return xr_xir_instance_path_capacity(view, receiver, &path, output, &fault);
    }
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver || !output || !value_unit(*output)) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirValue borrowed = {0};
    if (receiver->kind == XR_XIR_ROOT_VALUE) {
        if (receiver->local_payload || receiver->slot || receiver->value.type != (uint32_t)receiver->type)
            return XR_XIR_CALL_BAD_STATE;
        borrowed = receiver->value;
    } else {
        XrXirValuePlace place = {0};
        XrXirCallStatus status = value_place(view, instance, receiver, false, admission, &place);
        if (status != XR_XIR_CALL_READY) return status;
        borrowed.type = (uint32_t)place.type;
        memcpy(&borrowed.payload, place.payload, sizeof(borrowed.payload));
    }
    int64_t capacity = 0;
    XrXirCallStatus status = value_call_status(xr_xir_array_capacity(&borrowed, admission, &capacity));
    if (status == XR_XIR_CALL_READY) *output = (XrXirValue){XR_XIR_I64, 0, capacity};
    return status;
}
XrXirCallStatus xr_xir_instance_array_read(XrXirCallView *view,
    const XrXirValueReceiver *receiver, int64_t index, bool length,
    XrXirValue *output, XrXirFaultDetail *fault) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (receiver && receiver->kind == XR_XIR_ROOT_CELL) {
        XrXirValuePathStep step = {XR_XIR_PATH_INDEX, receiver->type, index};
        XrXirValuePath path = {length ? NULL : &step, !length};
        return length ? xr_xir_instance_path_length(view, receiver, &path, output, fault) :
            xr_xir_instance_path_read(view, receiver, &path, output, fault);
    }
    if (!fault) return XR_XIR_CALL_BAD_ARGUMENT;
    *fault = (XrXirFaultDetail) {0};
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver || !output || !value_unit(*output)) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirValue borrowed = {0}, held = {0};
    if (receiver->kind == XR_XIR_ROOT_VALUE) {
        if (receiver->local_payload || receiver->slot ||
            !xr_xir_value_argument(&receiver->value,instance->program->arena,receiver->type))
            return XR_XIR_CALL_BAD_STATE;
        borrowed = receiver->value;
    } else {
        XrXirValuePlace place = {0};
        XrXirCallStatus status = value_place(view, instance, receiver, false, admission, &place);
        if (status != XR_XIR_CALL_READY) return status;
        borrowed.type = (uint32_t) place.type;
        memcpy(&borrowed.payload, place.payload, sizeof(borrowed.payload));
        if (!xr_xir_value_from_payload(admission->arena,place.type,borrowed.payload,&borrowed))
            return XR_XIR_CALL_BAD_STATE;
        if (!length) {
            XrXirValueStatus copied = xr_xir_value_copy(&borrowed, &held);
            if (copied != XR_XIR_VALUE_OK) return value_call_status(copied);
            borrowed = held;
        }
    }
    XrXirValueStatus status;
    if (length) {
        int64_t size = 0;
        status = xr_xir_array_len(&borrowed, admission, &size);
        if (status == XR_XIR_VALUE_OK) *output = (XrXirValue) {XR_XIR_I64, 0, size};
    } else status = xr_xir_array_get(&borrowed, index, admission, output, fault);
    xr_xir_value_drop(&held);
    return value_call_status(status);
}
XrXirCallStatus xr_xir_instance_array_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver, int64_t index, const XrXirValue *element,
    bool append, XrXirFaultDetail *fault) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (receiver && receiver->kind == XR_XIR_ROOT_CELL) {
        XrXirValuePathStep step = {XR_XIR_PATH_INDEX, receiver->type, index};
        XrXirValuePath path = {append ? NULL : &step, !append};
        return append ? xr_xir_instance_path_push(view, receiver, &path, element, fault) :
            xr_xir_instance_path_write(view, receiver, &path, element, fault);
    }
    if (!fault) return XR_XIR_CALL_BAD_ARGUMENT;
    *fault = (XrXirFaultDetail) {0};
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirValuePlace place = {0};
    XrXirCallStatus status = value_place(view, instance, receiver, true, admission, &place);
    if (status != XR_XIR_CALL_READY) return status;
    return value_call_status(append ? xr_xir_array_push(&place, element, admission) :
        xr_xir_array_set(&place, index, element, admission, fault));
}
XrXirCallStatus xr_xir_instance_struct_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver, uint32_t field, const XrXirValue *value) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (receiver && receiver->kind == XR_XIR_ROOT_CELL) {
        XrXirValuePathStep step = {XR_XIR_PATH_FIELD, receiver->type, field};
        XrXirValuePath path = {&step, 1}; XrXirFaultDetail fault = {0};
        return xr_xir_instance_path_write(view, receiver, &path, value, &fault);
    }
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirValuePlace place = {0};
    XrXirCallStatus status = value_place(view, instance, receiver, true, admission, &place);
    if (status != XR_XIR_CALL_READY) return status;
    return value_call_status(xr_xir_struct_set(&place, field, value, admission));
}
XrXirCallStatus xr_xir_instance_literal(XrXirCallView *view, uint32_t literal, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance || literal >= instance->program->declarations->literal_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirLiteral *bytes = &instance->program->declarations->literals[literal];
    return value_call_status(xr_xir_string_new(instance->domain, bytes->bytes, bytes->length, output));
}
XrXirCallStatus xr_xir_instance_scalar_text(XrXirCallView *view, const XrXirValue *scalar, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    char buffer[XR_XIR_SCALAR_TEXT_BYTES]; const char *bytes = NULL; size_t length = 0;
    if (!instance || !scalar || !output || !xr_xir_scalar_text(scalar, buffer, &bytes, &length))
        return XR_XIR_CALL_BAD_STATE;
    return value_call_status(xr_xir_string_new(instance->domain, bytes, length, output));
}
static XrXirCallStatus time_call_status(XrXirTimeStatus status) {
    return status == XR_XIR_TIME_OK ? XR_XIR_CALL_READY :
        status == XR_XIR_TIME_RANGE ? XR_XIR_CALL_NUMERIC_RANGE : XR_XIR_CALL_HOST_ERROR;
}
XrXirCallStatus xr_xir_instance_clock_ns(XrXirCallView *view, XrXirClockKind clock, int64_t *nanoseconds) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance || !nanoseconds || clock < XR_XIR_CLOCK_REALTIME || clock > XR_XIR_CLOCK_MONOTONIC)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirTimeProvider *provider = &instance->config.time;
    if (!provider->abi_version) return XR_XIR_CALL_HOST_ERROR;
    int64_t reading = 0;
    XrXirTimeStatus status = provider->clock(provider->context, clock, &reading);
    /* Clock readings are nonnegative nanosecond counts; a rejection is not a range fault. */
    if (status == XR_XIR_TIME_OK && reading < 0) status = XR_XIR_TIME_FAILED;
    if (status == XR_XIR_TIME_RANGE) status = XR_XIR_TIME_FAILED;
    if (status == XR_XIR_TIME_OK) *nanoseconds = reading;
    return time_call_status(status);
}
XrXirCallStatus xr_xir_instance_utc_offset(XrXirCallView *view, int64_t seconds, int64_t *minutes) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance || !minutes) return XR_XIR_CALL_BAD_STATE;
    const XrXirTimeProvider *provider = &instance->config.time;
    if (!provider->abi_version) return XR_XIR_CALL_HOST_ERROR;
    int64_t offset = 0;
    XrXirTimeStatus status = provider->utc_offset(provider->context, seconds, &offset);
    if (status == XR_XIR_TIME_OK) *minutes = offset;
    return time_call_status(status);
}
static XrXirCallStatus instance_slot_copy(XrXirCallView *view,XrXirInstance *instance,
    uint32_t function,uint32_t slot,XrXirValue *output) {
    if (!instance || function>=instance->program->entry_count || slot>=instance->program->declarations->slot_count)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t module = d->slots[slot].module;
    if (view_is_child(instance, view) && !worker_slot_read(instance, slot)) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus scoped=instance_slot_scope(view,instance,function,module);
    if (scoped!=XR_XIR_CALL_READY) return scoped;
    if (instance->slot_origins && instance->slot_origins[slot].owner) {
        InstanceStorageRoute route=instance->slot_origins[slot];
        XrXirInstance *birth=route.owner;
        if (!instance->ready[module] ||
            route.index>=birth->program->declarations->slot_count || !birth->published[route.index] ||
            !birth->ready[birth->program->declarations->slots[route.index].module]) return XR_XIR_CALL_BAD_STATE;
        XrXirValueAdmission *admission=xr_xir_call_admission(view);
        XrXirCallStatus status=admit_instance_value(admission,&birth->slots[route.index],d->slots[slot].type);
        if (status!=XR_XIR_CALL_READY) return status;
        if (d->slots[slot].mutable && (!instance_cell_published(birth,route.index,&birth->slots[route.index]) ||
            !xr_xir_cell_unborrowed(&birth->slots[route.index]))) return XR_XIR_CALL_BAD_STATE;
        return value_call_status(xr_xir_value_copy(&birth->slots[route.index],output));
    }
    if (!instance->published[slot] ||
        (!instance->ready[module] && instance->current_module != module)) return XR_XIR_CALL_BAD_STATE;
    if (d->slots[slot].mutable &&
        (!instance_cell_published(instance, slot, &instance->slots[slot]) ||
         !xr_xir_cell_unborrowed(&instance->slots[slot]))) return XR_XIR_CALL_BAD_STATE;
    return value_call_status(xr_xir_value_copy(&instance->slots[slot], output));
}
XR_FUNC XrXirCallStatus xr_xir_instance_slot_read(XrXirCallView *view,uint32_t slot,XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    return instance_slot_copy(view,instance,xr_xir_call_current_entry(view->activation),slot,output);
}
XR_FUNC XrXirCallStatus xir_instance_observe_slot(XrXirCallView *view,uint64_t frame,uint32_t slot,XrXirValue *output) {
    XrXirInstance *instance=NULL;XrXirCallStatus status=instance_class_view(view,&instance);
    if (status!=XR_XIR_CALL_READY) return status;
    if (!output || !value_unit(*output) || !instance->driving || xr_xir_call_cleanup_active(view->activation))
        return XR_XIR_CALL_BAD_STATE;
    uint32_t function=UINT32_MAX;status=xir_call_vm_frame_entry(view,frame,&function);
    if (status!=XR_XIR_CALL_READY) return status;
    return instance_slot_copy(view,instance,function,slot,output);
}
XrXirCallStatus xr_xir_instance_slot_write(XrXirCallView *view, uint32_t slot,
                                          const XrXirValue *value, bool publish) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance || slot >= instance->program->declarations->slot_count || view_is_child(instance, view))
        return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t function = xr_xir_call_current_entry(view->activation), module = d->slots[slot].module;
    if (instance->slot_origins && instance->slot_origins[slot].owner) return XR_XIR_CALL_BAD_STATE;
    if (d->functions[function].module != module || !(d->slots[slot].type == XR_XIR_UNIT ?
            (value && value_unit(*value)) :
            xr_xir_value_argument(value, instance->program->arena, d->slots[slot].type)))
        return XR_XIR_CALL_BAD_STATE;
    if (!publish || instance->published[slot] || instance->current_module != module ||
        d->modules[module].initializer != function || instance->publication_count >= d->slot_count ||
        !value_unit(instance->slots[slot])) return XR_XIR_CALL_BAD_STATE;
    XrXirCellPublication publication = {instance, instance->domain, slot};
    if (d->slots[slot].mutable) {
        if (!instance_cell_work(view, 1)) return XR_XIR_CALL_LIMIT;
        if (xr_xir_cell_publication_prepare(value, &publication) != XR_XIR_VALUE_OK)
            return XR_XIR_CALL_BAD_STATE;
    }
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirCallStatus admitted = d->slots[slot].type == XR_XIR_UNIT ? XR_XIR_CALL_READY :
        admit_instance_value(admission, value, d->slots[slot].type);
    if (admitted != XR_XIR_CALL_READY) return admitted;
    XrXirValue owned = {0};
    XrXirCallStatus status = value_call_status(xr_xir_value_copy(value, &owned));
    if (status != XR_XIR_CALL_READY) return status;
    XrXirClassPublication class_publication = {0};
    status = value_call_status(xr_xir_class_publication_prepare(&owned, 1, admission, &class_publication));
    if (status == XR_XIR_CALL_READY) status = xr_xir_call_execution_status(view);
    if (status != XR_XIR_CALL_READY) {
        xr_xir_class_publication_drop(&class_publication);
        xr_xir_value_drop(&owned); return status;
    }
    xr_xir_class_publication_commit(&class_publication);
    if (d->slots[slot].mutable) xr_xir_cell_publication_commit(&owned, &publication);
    instance->slots[slot] = owned;
    if (publish) {
        instance->published[slot] = 1;
        ++instance->published_counts[module];
        instance->publication_order[instance->publication_count++] = slot;
        instance_trace(instance, XR_XIR_SLOT_PUBLISHED, slot);
    }
    xr_xir_class_publication_drop(&class_publication);
    return XR_XIR_CALL_READY;
}

#include "xxir_instance_slot_group.inc.c"

XrXirCallStatus xr_xir_instance_start(XrXirInstance *instance, uint32_t entry,
    const XrXirValue *arguments, uint32_t count) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    instance = instance_entry_state(instance);
    if (!instance->program) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission admission = instance_candidate_admission(instance);
    return instance_start(instance, entry, arguments, count, NULL, &admission, false);
}
XrXirCallStatus xr_xir_instance_start_test(XrXirInstance *instance, uint32_t entry) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    instance = instance_entry_state(instance);
    if (!instance->program) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission admission = instance_candidate_admission(instance);
    return instance_start(instance, entry, NULL, 0, NULL, &admission, true);
}
static XrXirCallStatus resolve_function_target(XrXirInstance *instance,const XrXirValue *value,
    uint32_t *entry,XrXirValueAdmission *admission,XrXirInstance **output) {
    if (!instance || !value || !entry || !admission || !output || !instance_owner_current(instance))
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirFunctionBinding *binding=xr_xir_function_binding(value);
    if (!binding || binding->release!=function_gate_drop || !binding->owner) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *birth=NULL;
    XrXirValueStatus selected=instance_function_admission_state(instance,binding->owner,&admission->work,&birth);
    if (selected!=XR_XIR_VALUE_OK) return selected==XR_XIR_VALUE_BAD_ARGUMENT?
        XR_XIR_CALL_BAD_ARGUMENT:value_call_status(selected);
    XrXirType received=XR_XIR_UNIT;
    XrXirCallStatus status=instance_received_value(admission,value,&received);
    if (status!=XR_XIR_CALL_READY) return status;
    const FunctionGate *gate=binding->owner;
    if ((!gate->instance || instance_root(instance)->stopping) && !instance_cleanup_grant(instance))
        return XR_XIR_CALL_BAD_STATE;
    if (gate->program!=birth->program || xr_xir_value_arena(value)!=birth->program->arena ||
        binding->entry>=birth->program->entry_count) return XR_XIR_CALL_BAD_ARGUMENT;
    *entry=binding->entry;*output=birth;return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_instance_call_target(XrXirCallView *view,
    const XrXirValue *function,uint32_t entry,XrXirCallMetadata *output) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || !output) return XR_XIR_CALL_BAD_STATE;
    if (admission->function!=admit_function_binding || !xr_xir_function_binding(function))
        return XR_XIR_CALL_READY;
    XrXirInstance *instance=view_instance(view);
    /* The synthetic relay always invokes its already selected generation. */
    if (!instance && admission->context==instance_current(view->instance) &&
        xr_xir_call_current_entry(view->activation)==instance_current(view->instance)->program->entry_count)
        instance=admission->context;
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    XrXirInstance *birth=NULL;uint32_t resolved=0;
    XrXirCallStatus status=resolve_function_target(instance,function,&resolved,admission,&birth);
    if (status!=XR_XIR_CALL_READY) return status;
    if (resolved!=entry || (view_is_child(instance,view) && birth!=instance)) return XR_XIR_CALL_BAD_STATE;
    if (birth!=instance) *output=(XrXirCallMetadata){birth->program->entries,birth->program->entry_count,
        birth->program->arena,birth,instance_cell_role,birth->program};
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus resolve_function(XrXirInstance *instance, const XrXirValue *value,
    uint32_t *entry, XrXirValueAdmission *admission) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (admission && admission->domain && !xr_xir_domain_current(admission->domain))
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance || !entry) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!value || !xr_xir_value_argument(value, instance->program->arena, (XrXirType) value->type))
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirFunctionBinding *binding = xr_xir_function_binding(value);
    if (!binding || binding->release != function_gate_drop || !binding->owner ||
        binding->owner != instance->function_gate) return XR_XIR_CALL_BAD_ARGUMENT;
    FunctionGate *gate = binding->owner;
    if ((!gate->instance || instance->stopping) && !instance_cleanup_grant(instance)) return XR_XIR_CALL_BAD_STATE;
    if ((gate->instance != instance_root(instance) && !(instance->stopping && !gate->instance && instance_cleanup_grant(instance))) ||
        gate->program != instance->program ||
        binding->entry >= instance->program->entry_count) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance->program->active_modules[instance->program->declarations->functions[binding->entry].module])
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus admitted = admit_instance_value(admission, value, (XrXirType) value->type);
    if (admitted != XR_XIR_CALL_READY) return admitted;
    *entry = binding->entry; return XR_XIR_CALL_READY;
}
XrXirCallStatus xr_xir_instance_start_function(XrXirInstance *instance, const XrXirValue *function,
    const XrXirValue *arguments, uint32_t count) {
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance) return XR_XIR_CALL_BAD_ARGUMENT;
    if (instance->stopping) return XR_XIR_CALL_BAD_STATE;
    if (instance_busy(instance)) return XR_XIR_CALL_BUSY;
    XrXirInstance *selected = NULL;
    XrXirCallStatus status = instance_function_state(instance, function, &selected);
    if (status != XR_XIR_CALL_READY) return status;
    instance = selected;
    XrXirValueAdmission admission = instance_candidate_admission(instance);
    uint32_t entry = 0;
    status = resolve_function(instance, function, &entry, &admission);
    return status == XR_XIR_CALL_READY ? instance_start(instance, entry, arguments, count,
        function, &admission, false) : status;
}
XrXirCallStatus xr_xir_instance_resolve_function(XrXirCallView *view, const XrXirValue *function, uint32_t *entry) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!entry) return XR_XIR_CALL_BAD_ARGUMENT;
    uint32_t candidate = 0;XrXirInstance *birth=NULL;
    XrXirCallStatus status=resolve_function_target(instance,function,&candidate,xr_xir_call_admission(view),&birth);
    if (status != XR_XIR_CALL_READY) return status;
    if (view_is_child(instance, view)) {
        if (birth!=instance) return XR_XIR_CALL_BAD_STATE;
        const XrXirTypeNode *signature = xr_xir_callable_signature(instance->program->types, (XrXirType)function->type);
        XrXirEffectFacts facts = xr_xir_effects_read(instance->program->permissions->effects, candidate);
        /* A local ordinary call proves both bounds; worker eligibility and
         * address recovery cannot supply a missing execution permission. */
        if (!signature || facts.status != XR_XIR_OK ||
            (signature->flags & XR_XIR_CALLABLE_ROOT_MASK) != XR_XIR_CALLABLE_ROOT_NONE ||
            facts.root.requires_root || facts.root.unresolved) return XR_XIR_CALL_BAD_STATE;
    }
    *entry = candidate;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_instance_call_at(XrXirCallView *view,uint32_t instruction,
    uint32_t entry,const XrXirValue *arguments,uint32_t count) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!view_is_child(instance,view)) return XR_XIR_CALL_READY;
    XirEffectInvocationSelection selected={0};
    XrXirCallStatus status=instance_invocation_direct(view,instruction,entry,arguments,count,&selected);
    return status==XR_XIR_CALL_READY?xr_xir_call_invocation_stage(view,&selected):status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_resolve_function_at(XrXirCallView *view,
    uint32_t instruction,const XrXirValue *function,const XrXirValue *arguments,
    uint32_t count,uint32_t *entry) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!entry) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!view_is_child(instance,view)) return xr_xir_instance_resolve_function(view,function,entry);
    XirEffectInvocationSelection selected={0};
    XrXirCallStatus status=instance_invocation_select(view,instruction,function,arguments,count,&selected);
    if (status!=XR_XIR_CALL_READY) return status;
    status=xr_xir_call_invocation_stage(view,&selected);
    if (status!=XR_XIR_CALL_READY) return status;
    *entry=selected.target;return XR_XIR_CALL_READY;
}
static XrXirCallStatus prepare_function_gate(XrXirInstance *instance);
XR_FUNC XrXirCallStatus xr_xir_instance_argument_weakening_admit(XrXirCallView *view,
    XrXirType type, const XrXirValue *input) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(!admission || admission->function!=admit_function_binding) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance || !input) return XR_XIR_CALL_BAD_ARGUMENT;
    if(!xr_xir_callable_signature(instance->program->types,type) ||
        !xr_xir_callable_signature(instance->program->types,(XrXirType)input->type)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status = admit_instance_value(admission, input, (XrXirType)input->type);
    if (status != XR_XIR_CALL_READY) return status;
    InstanceInvocationRead read={admission,NULL,NULL,NULL,instance};
    XrXirStatus match = xr_xir_callable_weakening_admit(instance->program->types,
        (XrXirType)input->type,type,&read,instance_invocation_work);
    if (match != XR_XIR_OK) return match == XR_XIR_BUDGET ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirFunctionBinding *binding = xr_xir_function_binding(input);
    if (!binding || !function_bound_admitted(instance->program, binding->entry,
        xr_xir_callable_signature(instance->program->types, type))) return XR_XIR_CALL_BAD_ARGUMENT;
    return XR_XIR_CALL_READY;
}

XR_FUNC XrXirCallStatus xr_xir_instance_weaken_function(XrXirCallView *view,
    XrXirType type, const XrXirValue *input, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance || !input) return XR_XIR_CALL_BAD_STATE;
    if (!output || !value_unit(*output)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirType local=XR_XIR_UNIT;
    XrXirCallStatus status=instance_function_type(instance,input,admission,&local);
    if (status!=XR_XIR_CALL_READY) return status;
    status = admit_instance_value(admission, input, local);
    if (status != XR_XIR_CALL_READY) return status;
    InstanceInvocationRead read={admission,NULL,NULL,NULL,instance};
    XrXirStatus match = xr_xir_callable_weakening_admit(instance->program->types,
        local, type, &read, instance_invocation_work);
    if (match != XR_XIR_OK) return match == XR_XIR_BUDGET ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirFunctionBinding *binding = xr_xir_function_binding(input);
    if (!binding) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *carrier=instance;
    XrXirType carrier_type=type;
    uint32_t entry=0;
    if (local==type) {
        XrXirValueStatus selected=instance_function_admission_state(instance,binding->owner,
            &admission->work,&carrier);
        if (selected!=XR_XIR_VALUE_OK) return value_call_status(selected);
        carrier_type=(XrXirType)input->type;entry=binding->entry;
    } else {
        status=instance_received_function_entry(instance,binding,&entry,&admission->work);
        if (status!=XR_XIR_CALL_READY) return status;
    }
    if (!function_bound_admitted(carrier->program, entry,
        xr_xir_callable_signature(carrier->program->types, carrier_type))) return XR_XIR_CALL_BAD_ARGUMENT;
    const XirFunctionProducer *producer=xr_xir_function_producer(input);
    if (producer) {
        XirEffectProducerView origin={0};
        status=instance_function_producer_value(instance,admission,input,&origin);
        if (status!=XR_XIR_CALL_READY) return status;
    }
    status=prepare_function_gate(carrier);
    if (status!=XR_XIR_CALL_READY) return status;
    if (!function_gate_retain(carrier->function_gate)) return XR_XIR_CALL_LIMIT;
    /* Canonical identity keeps the actual birth arena. Produced functions
     * retain their authentic origin through the ordinary construction owner. */
    XrXirFunctionBinding received={carrier->function_gate,function_gate_drop,entry,
        binding->captures,binding->capture_count};
    XrXirValueAdmission local_admission=*admission;local_admission.arena=carrier->program->arena;
    const XirFunctionConstruction construction={&received,&local_admission,producer};
    status=value_call_status(producer ?
        xr_xir_function_new_produced(instance->domain,carrier->program->arena,carrier_type,&construction,output) :
        xr_xir_function_new(instance->domain,carrier->program->arena,carrier_type,&received,&local_admission,output));
    admission->work=local_admission.work;admission->scratch_bytes=local_admission.scratch_bytes;
    if (status != XR_XIR_CALL_READY) function_gate_drop(carrier->function_gate);
    return status;
}
static XrXirCallStatus prepare_function_gate(XrXirInstance *instance) {
    if (instance->function_gate) return XR_XIR_CALL_READY;
    if (sizeof(FunctionGate) > instance->config.metadata_limit - instance->metadata_bytes)
        return XR_XIR_CALL_LIMIT;
    if (!xr_xir_domain_retain(instance->domain)) return XR_XIR_CALL_LIMIT;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    FunctionGate *gate = instance_metadata_allocate(instance->domain, sizeof(*gate), &status);
    if (!gate) { xr_xir_domain_drop(instance->domain); return status; }
    if (!xr_xir_compile_program_retain(instance->program)) {
        xr_xir_domain_metadata_deallocate(instance->domain, gate, sizeof(*gate));
        xr_xir_domain_drop(instance->domain); return XR_XIR_CALL_LIMIT;
    }
    atomic_init(&gate->references, 1);
    gate->program = instance->program;
    gate->instance = instance->stopping || instance_root(instance)->stopping ? NULL : instance_root(instance);
    gate->domain = instance->domain;
    instance->function_gate = gate; instance->metadata_bytes += sizeof(*gate);
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_private_reference(XrXirInstance *instance,
    uint32_t caller, uint32_t entry, XrXirType type, uint32_t captures) {
    const XrXirProgramPermissions *permissions = instance->program->permissions;
    if (!permissions || caller >= permissions->function_count) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramPermission *owner = &permissions->entries[caller];
    if (!xr_xir_domain_work(instance->domain, owner->reference_count)) return XR_XIR_CALL_LIMIT;
    for (uint32_t i = 0; i < owner->reference_count; ++i) {
        const XrXirProgramFunctionRef *edge = &permissions->references[owner->reference_begin + i];
        if (edge->entry == entry && edge->type == type && edge->captures == captures)
            return XR_XIR_CALL_READY;
    }
    return XR_XIR_CALL_BAD_ARGUMENT;
}
typedef struct InstanceFunctionRequest {
    XrXirType type;uint32_t entry;
    const XrXirValue *captures;uint32_t count;
    const XirFunctionProducer *producer;
} InstanceFunctionRequest;
static XrXirCallStatus instance_function_create(XrXirCallView *view,
    const InstanceFunctionRequest *request,XrXirValue *output) {
    if (!request) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirType type=request->type;uint32_t entry=request->entry,count=request->count;
    const XrXirValue *captures=request->captures;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    const XrXirTypeNode *signature = xr_xir_callable_signature(instance->program->types, type);
    if (!signature || entry >= instance->program->entry_count || (count && !captures)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *target = &instance->program->entries[entry];
    const XrXirDeclarations *d = instance->program->declarations;
    uint32_t caller = xr_xir_call_current_entry(view->activation);
    uint32_t from = d->functions[caller].module, to = d->functions[entry].module;
    if (!instance->program->active_modules[from] || !instance->program->active_modules[to])
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (!(request->producer?function_shape_admitted(instance->program,entry,signature):
        function_bound_admitted(instance->program,entry,signature))) return XR_XIR_CALL_BAD_ARGUMENT;
    if (target->cleanup_owner || entry == d->modules[to].initializer || !xr_xir_module_imports(d, from, to) ||
        count > target->parameter_count || target->parameter_count - count != signature->parameter_count ||
        target->result != signature->result) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_module_same_scope(d, from, to) && !d->functions[entry].exported) {
        XrXirCallStatus visible = instance_private_reference(instance, caller, entry, type, count);
        if (visible != XR_XIR_CALL_READY) return visible;
    }
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    for (uint32_t p = 0; p < count; ++p) {
        XrXirCallStatus admitted = admit_instance_value(admission, &captures[p], target->parameters[p]);
        if (admitted != XR_XIR_CALL_READY) return admitted;
    }
    if (!instance_function_parameters(instance->program,entry,signature,count)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status = prepare_function_gate(instance);
    if (status != XR_XIR_CALL_READY) return status;
    if (!function_gate_retain(instance->function_gate)) return XR_XIR_CALL_LIMIT;
    XrXirFunctionBinding binding = {instance->function_gate, function_gate_drop, entry, captures, count};
    if (request->producer) {
        const XirFunctionConstruction construction={&binding,admission,request->producer};
        status=value_call_status(xr_xir_function_new_produced(instance->domain,instance->program->arena,
            type,&construction,output));
    } else status = value_call_status(xr_xir_function_new(instance->domain, instance->program->arena,
        type, &binding, admission, output));
    if (status != XR_XIR_CALL_READY) function_gate_drop(instance->function_gate);
    return status;
}
XrXirCallStatus xr_xir_instance_function(XrXirCallView *view,XrXirType type,uint32_t entry,
    const XrXirValue *captures,uint32_t count,XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    const InstanceFunctionRequest request={type,entry,captures,count,NULL};
    return instance_function_create(view,&request,output);
}
#include "xxir_instance_function_producer.inc.c"
#include "xxir_instance_generator.inc.c"
XR_FUNC XrXirCallStatus xr_xir_task_go(XrXirCallView *view, XrXirType task_type, uint32_t direct_entry,
    const XrXirValue *arguments, uint32_t count, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!output || !value_unit(*output) || (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (xr_xir_call_cleanup_active(view->activation)) return XR_XIR_CALL_DEFER_ASYNC;
    XrXirCallStatus status = xr_xir_call_execution_status(view);
    if (status != XR_XIR_CALL_READY) return status;
    const XrXirProgram *program = instance->program;
    const XrXirDeclarations *declarations = program->declarations;
    if (direct_entry >= program->entry_count || !program->permissions || view->arena != program->arena)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirCallEntry *target = &instance->entries[direct_entry];
    uint32_t caller = xr_xir_call_current_entry(view->activation);
    uint32_t from = declarations->functions[caller].module, to = declarations->functions[direct_entry].module;
    if (!program->active_modules[from] || !program->active_modules[to] ||
        !xr_xir_module_imports(declarations, from, to) ||
        (!xr_xir_module_same_scope(declarations, from, to) && !declarations->functions[direct_entry].exported) ||
        target->cleanup_owner || direct_entry == declarations->modules[to].initializer ||
        count != target->parameter_count || program->permissions->entries[direct_entry].worker != XR_XIR_OK)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirTypeNode *task = xr_xir_type_node(program->types, task_type);
    if (!task || task->kind != XR_XIR_TYPE_TASK || task->element != target->result)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirType result = task->element;
    if (!xr_xir_task_result_shape(program->types, result)) return XR_XIR_CALL_UNSUPPORTED;
    for (uint32_t p = 0; p < count; ++p) {
        if (!xr_xir_task_parameter_supported(program->types, target->parameters[p]))
            return XR_XIR_CALL_UNSUPPORTED;
        if (!xr_xir_value_argument(&arguments[p], program->arena, target->parameters[p]))
            return XR_XIR_CALL_BAD_ARGUMENT;
    }
    const XrXirCallRequest request = {direct_entry, arguments, count, instance_cell_role, instance->program};
    /* The executor owns one sealed worker table; a foreign metadata frame
     * cannot index it without a separate producer and invocation correspondence. */
    if (instance!=instance_current(instance)) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_task_executor_spawn_from_view(instance->executor, view, task_type, &request, output);
}
XrXirCallStatus xr_xir_instance_cell(XrXirCallView *view, XrXirType type,
    const XrXirValue *initial, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    return value_call_status(xr_xir_cell_new(instance->domain, instance->program->arena, type,
        initial, admission, output));
}
XrXirCallStatus xr_xir_instance_cell_read(XrXirCallView *view, const XrXirValue *cell, XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status = instance_cell_access(view, instance, cell);
    return status == XR_XIR_CALL_READY ? value_call_status(xr_xir_call_cell_read(view, cell, output)) : status;
}
XrXirCallStatus xr_xir_instance_cell_write(XrXirCallView *view, const XrXirValue *cell, const XrXirValue *value) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status = instance_cell_access(view, instance, cell);
    return status == XR_XIR_CALL_READY ? value_call_status(xr_xir_call_cell_write(view, cell, value)) : status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_cell_string_append(XrXirCallView *view,
    const XrXirValue *cell, const XrXirValue *suffix) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance || !cell || !suffix) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status = instance_cell_access(view, instance, cell);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirValuePlace place = {0};
    XrXirValueStatus admitted = xr_xir_call_cell_place(view, cell, &place);
    if (admitted != XR_XIR_VALUE_OK) return value_call_status(admitted);
    return value_call_status(xr_xir_string_place_append(&place, suffix, xr_xir_call_admission(view)));
}
XR_FUNC XrXirCallStatus xr_xir_instance_cell_string_append_scalar(XrXirCallView *view,
    const XrXirValue *cell, const XrXirValue *scalar) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = view_instance(view);
    if (!instance || !cell || !scalar) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status = instance_cell_access(view, instance, cell);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirValuePlace place = {0};
    XrXirValueStatus admitted = xr_xir_call_cell_place(view, cell, &place);
    if (admitted != XR_XIR_VALUE_OK) return value_call_status(admitted);
    char buffer[XR_XIR_SCALAR_TEXT_BYTES]; const char *bytes = NULL; size_t length = 0;
    if (!xr_xir_scalar_text(scalar, buffer, &bytes, &length)) return XR_XIR_CALL_BAD_ARGUMENT;
    return value_call_status(xr_xir_string_place_append_utf8(&place, bytes, length, xr_xir_call_admission(view)));
}
XrXirAction xr_xir_instance_panic_land(XrXirCallView *view, void *frame, XrXirAction action,
    uint32_t destination, uint32_t handler_pc, uint32_t *pc) {
    if (!instance_view_current(view)) return (XrXirAction){.kind = XR_XIR_ACTION_FAULT, .value = {XR_XIR_I64,0,XR_XIR_CALL_BAD_ARGUMENT}};
    if (!xr_xir_call_panic_action(&action)) return action;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    if (!frame || !pc || !handler_pc) status = XR_XIR_CALL_BAD_STATE;
    else status=value_call_status(xr_xir_panic_payload_admit(&action.panic,xr_xir_call_admission(view)));
    if (status==XR_XIR_CALL_READY && destination != UINT32_MAX) {
        XrXirInstance *instance = view_instance(view);
        XrXirValue info = {0};
        status = !instance ? XR_XIR_CALL_BAD_STATE :
            value_call_status(xr_xir_panic_info_new(instance->domain, &action.panic, &info));
        if (status == XR_XIR_CALL_READY) xr_xir_owned_slot_move(frame, destination, &info);
    }
    if (status != XR_XIR_CALL_READY)
        return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {0}, 0};
    *pc = handler_pc;
    return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}, {0}, 0};
}

#include "xxir_instance_binding.inc.c"

#include "xxir_instance_hash.inc.c"

XR_FUNC XrXirAction xr_xir_instance_native_callable(XrXirCallView *view) {
    XrXirInstance *instance=view_instance(view);XrXirValue *value=NULL;
    XrXirCallStatus status=XR_XIR_CALL_BAD_STATE;bool thrown=false;
    if (!instance || view->phase!=XR_XIR_CALL_NORMAL) goto failed;
    uint32_t index=xr_xir_call_current_entry(view->activation);
    const XrXirProgram *program=instance->program;
    if (index>=program->entry_count) goto failed;
    const XrXirCallEntry *entry=&program->entries[index];
    uint32_t id=program->declarations->functions[index].native_callable;
    XrXirType error=xir_native_callable_error(program->types,id);
    if (entry->resume!=xr_xir_instance_native_callable || entry->state_bytes!=sizeof(XrXirValue) ||
        entry->release!=xr_xir_instance_native_callable_release || entry->environment || entry->flags || entry->cleanup_owner ||
        !view->state || (uintptr_t)view->state%_Alignof(XrXirValue) || view->argument_count!=1 || !view->arguments ||
        !xir_native_callable_types(id,program->types,entry->parameters,entry->parameter_count,entry->result,error) ||
        !xr_xir_value_argument(&view->arguments[0],view->arena,entry->parameters[0])) goto failed;
    XrXirValue *state=view->state;
    if (state->type || state->reserved || state->payload) goto failed;
    value=state;
    status=xr_xir_call_execution_status(view);if (status!=XR_XIR_CALL_READY) goto failed;
    XrXirViewAccess access={0};XrXirValueStatus converted=xr_xir_call_view_access(view,&access);
    if (converted==XR_XIR_VALUE_OK) converted=xir_from_utf8(&view->arguments[0],&access,value);
    if (converted==XR_XIR_VALUE_BAD_UTF8) {
        converted=xr_xir_enum_new(error,0,NULL,0,xr_xir_call_admission(view),value);thrown=true;
    }
    status=value_call_status(converted);if (status!=XR_XIR_CALL_READY) goto failed;
    status=xr_xir_call_execution_status(view);if (status!=XR_XIR_CALL_READY) goto failed;
    return (XrXirAction){thrown?XR_XIR_ACTION_THROW:XR_XIR_ACTION_RETURN,0,NULL,0,*value,{0},0};
failed:
    if (value) xr_xir_value_drop(value);
    return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
}
XR_FUNC void xr_xir_instance_native_callable_release(XrXirCallView *view,XrXirCallStatus reason) {
    (void)reason;
    xr_xir_value_drop(view->state);
}
