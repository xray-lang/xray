/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_loan_transport.inc.c - Sparse contextual loan demand
 *
 * KEY CONCEPT:
 *   A demand carries an existing application ordinal. Formal substitution is
 *   local to that demand and never publishes caller facts into a callee row.
 */
#include "xxir_view_loan_transport_internal.h"
#include "../base/xhash.h"

typedef struct ViewLoanEdge { uint32_t function, value, application, next; } ViewLoanEdge;
typedef struct ViewLoanState {
    XirStorageReference reference;
    uint32_t head;
    bool grounded;
} ViewLoanState;
typedef struct ViewLoanDependency { uint32_t dependent, next; } ViewLoanDependency;
typedef struct ViewLoanSlot { uint32_t value, generation; } ViewLoanSlot;
typedef struct ViewLoanBindingSlot { uint32_t application, value; } ViewLoanBindingSlot;

struct XirViewLoanTransport {
    XrXirCompileContext memory;
    const XrXirModule *module;
    const XirViewLoanFacts *facts;
    const uint8_t *flags;
    XrCompileRequestControl *control;
    uint32_t *offsets, *heads, *queue;
    ViewLoanBindingSlot *bindings;
    uint8_t *terminals;
    ViewLoanEdge *edges;
    ViewLoanState *states;
    ViewLoanDependency *dependencies;
    ViewLoanSlot *index;
    uint32_t edge_count, binding_capacity, progress, generation;
    uint32_t state_count, state_capacity, dependency_count, dependency_capacity;
    uint32_t index_capacity, queue_capacity;
    uint64_t bytes, limit;
    XrXirStatus status;
    bool indexed;
};

static bool view_loan_step(XirViewLoanTransport *a) {
    if (a->status == XR_XIR_OK) a->status = xir_receive_step(a->control, &a->progress);
    return a->status == XR_XIR_OK;
}

static void *view_loan_allocate(XirViewLoanTransport *a, uint64_t count, size_t size, bool zero) {
    if (a->status != XR_XIR_OK || !count) return NULL;
    if (count > SIZE_MAX / size || a->bytes > a->limit || count * size > a->limit - a->bytes) {
        a->status = XR_XIR_BUDGET; return NULL;
    }
    void *memory = zero ? xir_compile_calloc(&a->memory, (size_t)count, size, &a->status) :
        xir_compile_alloc(&a->memory, (size_t)count * size, &a->status);
    if (memory) a->bytes += count * size;
    return memory;
}

static bool view_loan_grow(XirViewLoanTransport *a, void **memory, uint32_t count,
    uint32_t *capacity, size_t size) {
    if (count < *capacity) return true;
    uint64_t next = *capacity ? (uint64_t)*capacity * 2 : 8;
    if (next > UINT32_MAX) { a->status = XR_XIR_OVERFLOW; return false; }
    void *fresh = view_loan_allocate(a, next, size, false);
    if (!fresh) return false;
    if (count) memcpy(fresh, *memory, (size_t)count * size);
    xr_compile_resources_free(*memory);
    a->bytes -= (uint64_t)*capacity * size;
    *memory = fresh; *capacity = (uint32_t)next;
    return true;
}

static uint32_t view_loan_value(XirViewLoanTransport *a, uint32_t function, uint32_t value) {
    if (a->status != XR_XIR_OK) return UINT32_MAX;
    if (function >= a->facts->restrictions.function_count ||
        value >= a->facts->restrictions.functions[function].value_count) {
        a->status = XR_XIR_BAD_STRUCTURE; return UINT32_MAX;
    }
    return a->offsets[function] + value;
}

static uint32_t view_loan_binding_hash(uint32_t application, uint32_t function, uint32_t value) {
    uint32_t fields[] = {application, function, value};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool view_loan_bindings(XirViewLoanTransport *a) {
    const XirStorageFlowView *view = &a->facts->storage;
    uint64_t count = 0;
    for (uint32_t n = 0; n < view->application_count && view_loan_step(a); ++n) {
        XirStorageApplication app = view->applications[n];
        if (app.function >= view->function_count || app.declaration >= view->function_count ||
            app.first > view->binding_count || app.count > view->binding_count - app.first ||
            (app.environment < XIR_EXECUTION_UNKNOWN_ENVIRONMENT && app.environment >= view->application_count)) {
            a->status = XR_XIR_BAD_STRUCTURE; return false;
        }
        count += app.count;
        if (count > UINT32_MAX/2u) { a->status = XR_XIR_OVERFLOW; return false; }
    }
    uint64_t capacity = 1;
    while (capacity < count * 2 && capacity <= UINT32_MAX / 2u) capacity *= 2;
    if (capacity < count * 2 || capacity > UINT32_MAX) { a->status = XR_XIR_OVERFLOW; return false; }
    a->binding_capacity = (uint32_t)capacity;
    a->bindings = view_loan_allocate(a, capacity, sizeof(*a->bindings), true);
    if (!a->bindings) return false;
    for (uint32_t n = 0; n < view->application_count && view_loan_step(a); ++n) {
        XirStorageApplication app = view->applications[n];
        for (uint32_t b = 0; b < app.count && view_loan_step(a); ++b) {
            uint32_t id = app.first + b;
            XirStorageBinding binding = view->bindings[id];
            if (binding.formal_function != app.declaration ||
                view_loan_value(a, binding.formal_function, binding.formal_value) == UINT32_MAX ||
                view_loan_value(a, binding.actual_function, binding.actual_value) == UINT32_MAX ||
                (binding.actual_environment < XIR_EXECUTION_UNKNOWN_ENVIRONMENT &&
                    binding.actual_environment >= view->application_count)) {
                if (a->status == XR_XIR_OK) a->status = XR_XIR_BAD_STRUCTURE;
                return false;
            }
            uint32_t slot = view_loan_binding_hash(n, binding.formal_function, binding.formal_value) &
                (a->binding_capacity - 1);
            while (a->bindings[slot].value && view_loan_step(a)) {
                XirStorageBinding previous = view->bindings[a->bindings[slot].value - 1];
                if (a->bindings[slot].application == n && previous.formal_function == binding.formal_function &&
                    previous.formal_value == binding.formal_value) {
                    a->status = XR_XIR_BAD_STRUCTURE; return false;
                }
                slot = (slot + 1) & (a->binding_capacity - 1);
            }
            if (a->status != XR_XIR_OK) return false;
            a->bindings[slot] = (ViewLoanBindingSlot){n,id + 1};
        }
    }
    return a->status == XR_XIR_OK;
}

static void view_loan_edge(XirViewLoanTransport *a, uint32_t function, uint32_t value,
    uint32_t target, uint32_t application) {
    a->edges[a->edge_count] = (ViewLoanEdge){function, value, application, a->heads[target]};
    a->heads[target] = a->edge_count++;
}

static bool view_loan_index(XirViewLoanTransport *a) {
    const XirRestrictionView *r = &a->facts->restrictions;
    const XirStorageFlowView *s = &a->facts->storage;
    uint64_t values = 0, edges = s->crossflow_count;
    a->offsets = view_loan_allocate(a, (uint64_t)r->function_count + 1, sizeof(*a->offsets), false);
    if (!a->offsets) return false;
    for (uint32_t f = 0; f < r->function_count && view_loan_step(a); ++f) {
        XirRestrictionFunction row = r->functions[f];
        uint64_t physical = (uint64_t)a->module->functions[f].parameter_count + a->module->functions[f].instruction_count;
        if (row.physical_values != physical || s->functions[f].physical_values != physical ||
            row.value_count < row.physical_values || s->functions[f].value_count < physical ||
            row.value_count < s->functions[f].value_count || (row.flow_count && !row.flows)) {
            a->status = XR_XIR_BAD_STRUCTURE; return false;
        }
        a->offsets[f] = (uint32_t)values; values += row.value_count; edges += row.flow_count;
        if (values > UINT32_MAX || edges > UINT32_MAX) { a->status = XR_XIR_OVERFLOW; return false; }
    }
    a->offsets[r->function_count] = (uint32_t)values;
    a->heads = view_loan_allocate(a, values, sizeof(*a->heads), false);
    a->terminals = view_loan_allocate(a, values, sizeof(*a->terminals), true);
    a->edges = view_loan_allocate(a, edges, sizeof(*a->edges), false);
    if (a->status != XR_XIR_OK) return false;
    if (values) memset(a->heads, 255, (size_t)values * sizeof(*a->heads));
    for (uint32_t f = 0; f < r->function_count && view_loan_step(a); ++f)
        for (uint32_t e = 0; e < r->functions[f].flow_count && view_loan_step(a); ++e) {
            XrXirValueFlow edge = r->functions[f].flows[e];
            uint32_t source = view_loan_value(a, f, edge.source), target = view_loan_value(a, f, edge.target);
            if (source == UINT32_MAX || target == UINT32_MAX) return false;
            view_loan_edge(a, f, edge.source, target, UINT32_MAX);
        }
    for (uint32_t e = 0; e < s->crossflow_count && view_loan_step(a); ++e) {
        XirStorageCrossFlow edge = s->crossflows[e];
        if ((uint32_t)edge.bank > XIR_STORAGE_FOOTPRINT) { a->status = XR_XIR_BAD_STRUCTURE; return false; }
        if (edge.bank == XIR_STORAGE_EXPOSURE) continue;
        uint32_t source = view_loan_value(a, edge.source_function, edge.source);
        uint32_t target = view_loan_value(a, edge.target_function, edge.target);
        if (source == UINT32_MAX || target == UINT32_MAX ||
            (edge.application != UINT32_MAX && edge.application >= s->application_count)) {
            if (a->status == XR_XIR_OK) a->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        view_loan_edge(a, edge.source_function, edge.source, target, edge.application);
    }
    for (uint32_t n = 0; n < s->input_count && view_loan_step(a); ++n) {
        XirStorageInput input = s->inputs[n];
        if ((uint32_t)input.bank > XIR_STORAGE_FOOTPRINT) {a->status=XR_XIR_BAD_STRUCTURE;return false;}
        if (input.bank == XIR_STORAGE_EXPOSURE) continue;
        uint32_t value = view_loan_value(a, input.function, input.value);
        if (value == UINT32_MAX) return false;
        a->terminals[value] = 1;
    }
    for (uint32_t n = 0; n < s->unknown_count && view_loan_step(a); ++n) {
        uint32_t value = view_loan_value(a, s->unknowns[n].function, s->unknowns[n].value);
        if (value == UINT32_MAX) return false;
        a->terminals[value] = 1;
    }
    return a->status == XR_XIR_OK && view_loan_bindings(a);
}

XR_FUNC void xir_view_loan_transport_free(XirViewLoanTransport *a) {
    if (!a) return;
    xr_compile_resources_free(a->offsets); xr_compile_resources_free(a->heads);
    xr_compile_resources_free(a->bindings); xr_compile_resources_free(a->terminals);
    xr_compile_resources_free(a->edges); xr_compile_resources_free(a->states);
    xr_compile_resources_free(a->dependencies); xr_compile_resources_free(a->index);
    xr_compile_resources_free(a->queue); xr_compile_resources_free(a);
}

XR_FUNC XrXirStatus xir_view_loan_transport_new(const XrXirCompileContext *context,
    const XrXirModule *module, const XirViewLoanFacts *facts, const uint8_t *flags,
    XrCompileRequestControl *control, XirViewLoanTransport **output) {
    if (!xir_compile_context_valid(context) || !module || !module->functions || !facts || !output || *output ||
        facts->restrictions.function_count != module->function_count || !facts->restrictions.functions ||
        facts->storage.function_count != module->function_count || !facts->storage.functions ||
        (facts->storage.application_count && !facts->storage.applications) ||
        (facts->storage.binding_count && !facts->storage.bindings) ||
        (facts->storage.crossflow_count && !facts->storage.crossflows) ||
        (facts->storage.input_count && !facts->storage.inputs) ||
        (facts->storage.unknown_count && !facts->storage.unknowns)) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes=sizeof(XirViewLoanTransport)+(flags && module->types?module->types->count:0u);
    if (bytes > context->limits.frame_bytes) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    XirViewLoanTransport *a = xir_compile_calloc(context, 1, sizeof(*a), &status);
    if (!a) return status;
    *a = (XirViewLoanTransport){.memory=*context, .module=module, .facts=facts, .flags=flags,
        .control=control, .bytes=bytes, .limit=context->limits.frame_bytes, .status=XR_XIR_OK};
    *output = a; return XR_XIR_OK;
}

static uint32_t view_loan_state_hash(XirStorageReference reference) {
    uint32_t fields[] = {reference.function, reference.value, reference.environment};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool view_loan_state_index(XirViewLoanTransport *a) {
    uint64_t next = a->index_capacity ? (uint64_t)a->index_capacity * 2 : 16;
    if (next > UINT32_MAX) { a->status = XR_XIR_OVERFLOW; return false; }
    ViewLoanSlot *index = view_loan_allocate(a, next, sizeof(*index), true);
    if (!index) return false;
    for (uint32_t n = 0; n < a->state_count && view_loan_step(a); ++n) {
        uint32_t slot = view_loan_state_hash(a->states[n].reference) & ((uint32_t)next - 1);
        while (index[slot].generation == a->generation) slot = (slot + 1) & ((uint32_t)next - 1);
        index[slot] = (ViewLoanSlot){n, a->generation};
    }
    if (a->status != XR_XIR_OK) { xr_compile_resources_free(index); a->bytes -= next*sizeof(*index); return false; }
    xr_compile_resources_free(a->index); a->bytes -= (uint64_t)a->index_capacity*sizeof(*index);
    a->index = index; a->index_capacity = (uint32_t)next; return true;
}

static bool view_loan_push(XirViewLoanTransport *a, XirStorageReference reference, uint32_t dependent) {
    if (view_loan_value(a, reference.function, reference.value) == UINT32_MAX) return false;
    if (reference.environment == XIR_EXECUTION_UNKNOWN_ENVIRONMENT) { a->status = XR_XIR_UNRESOLVED; return false; }
    if (reference.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT &&
        reference.environment >= a->facts->storage.application_count) { a->status = XR_XIR_BAD_STRUCTURE; return false; }
    if (!a->index_capacity || a->state_count >= a->index_capacity/2)
        if (!view_loan_state_index(a)) return false;
    uint32_t slot = view_loan_state_hash(reference) & (a->index_capacity - 1), id = UINT32_MAX;
    while (a->index[slot].generation == a->generation && view_loan_step(a)) {
        uint32_t candidate = a->index[slot].value;
        XirStorageReference previous = a->states[candidate].reference;
        if (previous.function == reference.function && previous.value == reference.value &&
            previous.environment == reference.environment) { id=candidate; break; }
        slot = (slot + 1) & (a->index_capacity - 1);
    }
    if (a->status != XR_XIR_OK) return false;
    if (id == UINT32_MAX) {
        if (!view_loan_grow(a, (void **)&a->states, a->state_count, &a->state_capacity, sizeof(*a->states))) return false;
        id=a->state_count++; a->states[id]=(ViewLoanState){reference, UINT32_MAX, false};
        a->index[slot]=(ViewLoanSlot){id, a->generation};
    }
    if (dependent == UINT32_MAX) return true;
    if (!view_loan_grow(a, (void **)&a->dependencies, a->dependency_count,
        &a->dependency_capacity, sizeof(*a->dependencies))) return false;
    a->dependencies[a->dependency_count]=(ViewLoanDependency){dependent,a->states[id].head};
    a->states[id].head=a->dependency_count++; return true;
}

static bool view_loan_substitute(XirViewLoanTransport *a, uint32_t id, bool *bound) {
    const XirStorageFlowView *view = &a->facts->storage;
    XirStorageReference reference = a->states[id].reference;
    *bound=false;
    if (reference.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT) return true;
    XirStorageApplication app = view->applications[reference.environment];
    if (app.declaration != reference.function) return true;
    uint32_t slot = view_loan_binding_hash(reference.environment,reference.function,reference.value) &
        (a->binding_capacity - 1);
    while (a->bindings[slot].value && view_loan_step(a)) {
        uint32_t index = a->bindings[slot].value - 1;
        XirStorageBinding binding = view->bindings[index];
        if (a->bindings[slot].application == reference.environment && index >= app.first &&
            index-app.first < app.count && binding.formal_function == reference.function &&
            binding.formal_value == reference.value) {
            *bound=true;
            return view_loan_push(a, (XirStorageReference){binding.actual_function,binding.actual_value,
                binding.actual_environment == XIR_EXECUTION_CURRENT_ENVIRONMENT ? app.environment : binding.actual_environment}, id);
        }
        slot=(slot+1)&(a->binding_capacity-1);
    }
    return a->status == XR_XIR_OK;
}

static XrXirStatus view_loan_empty(void *context, const XirExecutionSite *site) {
    bool *empty=context;
    if (site->kind != XIR_EXECUTION_CALL || site->protocol != 1 || site->argument_count) *empty=false;
    return XR_XIR_OK;
}

static bool view_loan_borrowed(const XirViewLoanTransport *a, XrXirType type) {
    return a->flags && xr_xir_type_node(a->module->types,type) &&
        (a->flags[(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE]&3u)!=0;
}

static bool view_loan_terminal(XirViewLoanTransport *a, uint32_t id, const XirViewLoanDemand *demand) {
    XirStorageReference reference=a->states[id].reference;
    const XrXirFunction *function=&a->module->functions[reference.function];
    uint32_t physical=a->facts->restrictions.functions[reference.function].physical_values;
    if (reference.value >= physical) {a->status=XR_XIR_UNRESOLVED;return false;}
    XrXirType type=xr_xir_operand_type(function,reference.value);
    bool borrowed=view_loan_borrowed(a,type),callable=xr_xir_type_is_callable(a->module->types,type);
    if (borrowed &&
        (reference.function != demand->function || reference.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT)) {
        a->status=XR_XIR_UNRESOLVED;return false;
    }
    bool scalar=type==XR_XIR_UNIT || type==XR_XIR_BOOL || type==XR_XIR_RUNE || type==XR_XIR_STRING ||
        xr_xir_type_is_integer(type) || type==XR_XIR_F32 || type==XR_XIR_F64;
    if (!borrowed && !callable && !scalar) {a->status=XR_XIR_UNRESOLVED;return false;}
    if (callable && !xr_xir_type_is_c_callable(a->module->types,type)) {
        bool empty=true,complete=false;uint32_t remainder=0;
        a->status=xir_execution_visit_value_environment_in(a->facts->execution,&reference,
            view_loan_empty,&empty,&complete,&remainder);
        if (a->status == XR_XIR_OK && (!empty || !complete || remainder)) a->status=XR_XIR_UNRESOLVED;
        if (a->status != XR_XIR_OK) return false;
    }
    a->states[id].grounded=true;return true;
}

static bool view_loan_expand(XirViewLoanTransport *a, uint32_t id, const XirViewLoanDemand *demand) {
    XirStorageReference reference=a->states[id].reference;
    bool bound=false;
    if (!view_loan_substitute(a,id,&bound) || bound) return a->status == XR_XIR_OK;
    uint32_t value=view_loan_value(a,reference.function,reference.value);
    if (a->terminals[value]) {a->status=XR_XIR_UNRESOLVED;return false;}
    if (reference.function == demand->function && reference.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT &&
        reference.value < a->facts->restrictions.functions[reference.function].physical_values) {
        a->status=demand->physical(demand->context,reference.value);
        if (a->status != XR_XIR_OK) return false;
    }
    bool predecessor=false;
    for (uint32_t e=a->heads[value];e!=UINT32_MAX && view_loan_step(a);e=a->edges[e].next) {
        ViewLoanEdge edge=a->edges[e];uint32_t environment=reference.environment;
        if (edge.application != UINT32_MAX) {
            XirStorageApplication app=a->facts->storage.applications[edge.application];
            if (app.function == reference.function && app.environment != reference.environment) continue;
            environment=edge.application;
        } else if (edge.function != reference.function) {a->status=XR_XIR_UNRESOLVED;return false;}
        predecessor=true;
        if (!view_loan_push(a,(XirStorageReference){edge.function,edge.value,environment},id)) return false;
    }
    return a->status == XR_XIR_OK && (predecessor || view_loan_terminal(a,id,demand));
}

static bool view_loan_grounded(XirViewLoanTransport *a) {
    uint32_t count=0;
    for (uint32_t n=0;n<a->state_count && view_loan_step(a);++n) {
        if (!a->states[n].grounded) continue;
        if (!view_loan_grow(a,(void **)&a->queue,count,&a->queue_capacity,sizeof(*a->queue))) return false;
        a->queue[count++]=n;
    }
    for (uint32_t cursor=0;cursor<count && view_loan_step(a);++cursor)
        for (uint32_t e=a->states[a->queue[cursor]].head;e!=UINT32_MAX && view_loan_step(a);e=a->dependencies[e].next) {
            uint32_t parent=a->dependencies[e].dependent;
            if (a->states[parent].grounded) continue;
            a->states[parent].grounded=true;
            if (!view_loan_grow(a,(void **)&a->queue,count,&a->queue_capacity,sizeof(*a->queue))) return false;
            a->queue[count++]=parent;
        }
    if (a->status == XR_XIR_OK && count != a->state_count) a->status=XR_XIR_UNRESOLVED;
    return a->status == XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_view_loan_transport_demand(XirViewLoanTransport *a, const XirViewLoanDemand *demand) {
    if (!a || !demand || !demand->physical) return XR_XIR_BAD_STRUCTURE;
    if (a->status != XR_XIR_OK) return a->status;
    if (demand->frame_bytes > a->memory.limits.frame_bytes) return a->status=XR_XIR_BUDGET;
    a->limit=a->memory.limits.frame_bytes-demand->frame_bytes;
    if (a->bytes > a->limit) return a->status=XR_XIR_BUDGET;
    if (!a->indexed) {
        if (!view_loan_index(a)) return a->status;
        a->indexed=true;
    }
    a->state_count=0;a->dependency_count=0;
    if (++a->generation == 0) {memset(a->index,0,(size_t)a->index_capacity*sizeof(*a->index));++a->generation;}
    if (!view_loan_push(a,(XirStorageReference){demand->function,demand->carrier,
        XIR_EXECUTION_CURRENT_ENVIRONMENT},UINT32_MAX)) return a->status;
    for (uint32_t n=0;n<a->state_count && view_loan_step(a);++n)
        if (!view_loan_expand(a,n,demand)) return a->status;
    view_loan_grounded(a);return a->status;
}

XR_FUNC uint64_t xir_view_loan_transport_frame_bytes(const XirViewLoanTransport *a) {
    return a ? a->bytes : 0;
}
