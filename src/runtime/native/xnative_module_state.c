/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_module_state.c - Stable fixed-value bindings under one execution owner
 *
 * KEY CONCEPT:
 *   Value plans describe actual inline bytes and real Header owners. A complete
 *   replacement is published before any previous owner can run its destructor.
 */
#include "xnative_module_state.h"
#include "xnative_array_backing.h"
#include "xnative_capture_backing.h"
#include "xray_native_function.h"
#include "xray_native_values.h"
#include "base/xchecks.h"
#include "base/xsigned_rc.h"
#include "base/xmax_align.h"
#include <stdalign.h>
#include <stdatomic.h>
#include <string.h>

typedef struct ModuleType {
    const XrtValuePlan *plan;
    XrtValuePlanSpec spec;
    size_t depth;
    bool supported;
} ModuleType;
struct XrtModuleLayout {
    _Atomic(int32_t) references;
    XrtBillingOwner *billing;
    size_t bytes, scratch_size, max_leaves, depth;
    XrtValueImagePlans *plans;
    XrtModuleLayoutView view;
    ModuleType *types;
    uint32_t *slot_counts;
};
enum { MODULE_EMPTY, MODULE_INITIALIZING, MODULE_READY };
enum { SLOT_CONSTRUCTED = 1u, SLOT_PUBLISHED = 2u };
struct XrtModuleRef { XrtModuleState *state; uint32_t slot; uint8_t required_flags; };
typedef struct ModuleValidation {
    XrtValuePlanSpec spec;
    uint8_t *value;
    size_t next;
} ModuleValidation;
struct XrtModuleState {
    XrtModuleLayout *layout;
    XrtBillingOwner *billing;
    size_t bytes;
    uint8_t *payload, *scratch, *flags, *modules;
    uint32_t *construction, *publication, *remaining;
    XrtModuleRef *initializers, *published;
    ModuleValidation *validation;
    XrtExternalOwner *destinations, *sources;
    XrtPlaceEdge *edges;
    XrtManagedRoot *temporary_root;
    XrtManagedFrame *temporary_frame;
    uint32_t construction_count, publication_count;
    bool busy, scratch_busy;
};

static bool module_extent(size_t *bytes, size_t count, size_t stride) {
    if (stride && count > (SIZE_MAX - *bytes) / stride) return false;
    *bytes += count * stride;
    return true;
}
static bool module_align(size_t *bytes, size_t alignment) {
    if (!alignment || (alignment & (alignment - 1)) || *bytes > SIZE_MAX - (alignment - 1)) return false;
    *bytes = (*bytes + alignment - 1) & ~(alignment - 1);
    return true;
}
static bool module_part(uint8_t *base, size_t *offset, size_t count, size_t stride,
    size_t alignment, void **output) {
    if (!module_align(offset, alignment)) return false;
    if (base && output) *output = base + *offset;
    return module_extent(offset, count, stride);
}
#include "xnative_module_layout.inc.c"

static const ModuleType *module_slot_type(const XrtModuleState *state, uint32_t slot) {
    return &state->layout->types[state->layout->view.slots[slot].type - 1];
}
XR_FUNCDEF XrtManagedStatus xrt_module_state_plan(const XrtModuleState *state, uint32_t type,
    const XrtValuePlan **empty) {
    if (!state) return XRT_MANAGED_ARGUMENT;
    return xrt_module_layout_plan(state->layout, type, empty);
}
XR_FUNCDEF XrtManagedStatus xrt_module_state_inline_array(const XrtModuleState *state,
    uint32_t type, XrtBoundInlineArray *empty) {
    return xrt_module_layout_inline_array(state ? state->layout : NULL, type, empty);
}
static void *module_payload(const XrtModuleState *state, uint32_t slot) {
    return state->payload + state->layout->view.slots[slot].offset;
}
static bool module_reference_valid(const XrtModuleRef *reference) {
    if (!reference || !reference->state || !reference->state->layout ||
        reference->slot >= reference->state->layout->view.slot_count) return false;
    XrtModuleState *state = reference->state;
    return (reference == &state->initializers[reference->slot] && reference->required_flags == SLOT_CONSTRUCTED) ||
        (reference == &state->published[reference->slot] && reference->required_flags == SLOT_PUBLISHED);
}
static bool module_reference_ready(const XrtModuleRef *reference) {
    return module_reference_valid(reference) &&
        (reference->state->flags[reference->slot] & reference->required_flags) == reference->required_flags;
}
static XrtManagedStatus module_binding_guard(void *context, XrtPlaceAction action) {
    const XrtModuleRef *reference = context;
    if (!module_reference_ready(reference) || action == XRT_PLACE_MOVE_FROM) return XRT_MANAGED_ARGUMENT;
    if (action == XRT_PLACE_REPLACE &&
        !(reference->state->layout->view.slots[reference->slot].flags & XRT_MODULE_SLOT_MUTABLE))
        return XRT_MANAGED_ARGUMENT;
    return XRT_MANAGED_OK;
}
XRAY_API XrtModuleStatus xrt_module_external_owner(const XrtModuleRef *reference, size_t leaf,
    XrtExternalOwner *out) {
    if (!out || !module_reference_valid(reference)) return XRT_MANAGED_ARGUMENT;
    XrtModuleState *state = reference->state;
    const XrtValuePlanSpec *spec = &module_slot_type(state, reference->slot)->spec;
    if (leaf >= spec->owned_count) return XRT_MANAGED_ARGUMENT;
    *out = (XrtExternalOwner){(XrtManagedHeader **)((uint8_t *)module_payload(state, reference->slot) +
        spec->owned_offsets[leaf]), &state->busy, module_binding_guard, (void *)reference, NULL};
    return XRT_MANAGED_OK;
}

/* The caller has admitted the complete typed binding, including its immutable
 * backend entry and code lifetime. Storage validation cannot authenticate an
 * opaque foreign entry or infer signature identity from a plan address. */
static XrtManagedStatus module_function_storage(const void *value) {
    XrtFunction function;
    memcpy(&function, value, sizeof(function));
    if (!function.entry || !function.owner) return XRT_MANAGED_ARGUMENT;
    XrtEnvironmentBorrow environment = {0};
    XrtManagedStatus status = xrt_environment_borrow_owned(function.owner, &environment);
    if (status != XRT_MANAGED_OK) return status;
    XrtValuePlanSpec shape, signature;
    if (function.environment != (uintptr_t)environment.payload ||
        xrt_value_plan_view(environment.plan, &shape) != XRT_MANAGED_OK ||
        xrt_value_plan_view(shape.entry.signature, &signature) != XRT_MANAGED_OK ||
        signature.kind != XRT_VALUE_FUNCTION) return XRT_MANAGED_ARGUMENT;
    return XRT_MANAGED_OK;
}
/* Traverse retained plans with a bounded, preallocated stack. No recursive
 * callbacks, compiler pool queries or independent physical layout are needed. */
static XrtManagedStatus module_validate(XrtModuleState *state, const ModuleType *type, void *value) {
    size_t depth = 1;
    state->validation[0] = (ModuleValidation){type->spec, value, 0};
    while (depth) {
        ModuleValidation *top = &state->validation[depth - 1];
        XrtValuePlanSpec *spec = &top->spec;
        const XrtValuePlan *child = NULL;
        uint8_t *payload = NULL;
        if (xrt_value_kind_has_fields(spec->kind)) {
            if (top->next < spec->field_count) {
                const XrtValueSlotSpec *field = &spec->fields[top->next++];
                child = field->type; payload = top->value + field->offset;
            }
        } else if (spec->kind == XRT_VALUE_NULLABLE && !top->next++) {
            uint8_t tag = top->value[spec->nullable.tag_offset];
            if (tag > 1) return XRT_MANAGED_ARGUMENT;
            if (tag) { child = spec->nullable.element; payload = top->value + spec->nullable.payload_offset; }
            else {
                for (size_t i = 0; i < spec->owned_count; ++i) {
                    XrtManagedHeader *owner;
                    memcpy(&owner, top->value + spec->owned_offsets[i], sizeof(owner));
                    if (owner) return XRT_MANAGED_ARGUMENT;
                }
                memset(top->value, 0, spec->size);
                for (size_t i = 0; i < spec->owned_count; ++i)
                    *(XrtManagedHeader **)(top->value + spec->owned_offsets[i]) = NULL;
            }
        } else if (spec->kind == XRT_VALUE_BOOL && *top->value > 1) return XRT_MANAGED_ARGUMENT;
        else if (spec->kind == XRT_VALUE_RUNE) {
            int64_t rune; memcpy(&rune, top->value, sizeof(rune));
            if (rune < 0 || rune > 0x10ffff || (rune >= 0xd800 && rune <= 0xdfff)) return XRT_MANAGED_ARGUMENT;
        } else if ((spec->kind >= XRT_VALUE_STRING && spec->kind <= XRT_VALUE_MAP_STRING_I64) ||
            spec->kind == XRT_VALUE_SET_I64) {
            XrtManagedHeader *owner; memcpy(&owner, top->value, sizeof(owner));
            if (!owner) return XRT_MANAGED_ARGUMENT;
        } else if (spec->kind == XRT_VALUE_ARRAY_INLINE) {
            XrtManagedHeader *owner; memcpy(&owner, top->value, sizeof(owner));
            if (!xrt_array_backing_inline_matches(owner, spec->element)) return XRT_MANAGED_ARGUMENT;
        } else if (spec->kind == XRT_VALUE_FUNCTION) {
            XrtManagedStatus status = module_function_storage(top->value);
            if (status != XRT_MANAGED_OK) return status;
        }
        if (child) {
            XR_CHECK(depth < state->layout->depth, "the earlier-row type DAG bounds validation depth");
            ModuleValidation *next = &state->validation[depth++];
            next->value = payload; next->next = 0;
            XR_CHECK(xrt_value_plan_view(child, &next->spec) == XRT_MANAGED_OK,
                "the retained module layout owns every child plan");
        } else --depth;
    }
    return XRT_MANAGED_OK;
}
static bool module_overlaps(const void *a, size_t as, const void *b, size_t bs) {
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return (x <= y ? y - x < as : x - y < bs) && as && bs;
}
static XrtManagedStatus module_prepare_copy(XrtModuleState *state, const ModuleType *type, const void *value) {
    const XrtValuePlanSpec *spec = &type->spec;
    if ((!value && spec->size) || (value && (uintptr_t)value % spec->alignment) ||
        module_overlaps(value, spec->size, state->scratch, state->layout->scratch_size)) return XRT_MANAGED_ARGUMENT;
    if (spec->size) memcpy(state->scratch, value, spec->size);
    XrtManagedStatus status = module_validate(state, type, state->scratch);
    size_t retained = 0;
    if (status == XRT_MANAGED_OK) {
        for (; retained < spec->owned_count; ++retained) {
            XrtManagedHeader *owner;
            memcpy(&owner, state->scratch + spec->owned_offsets[retained], sizeof(owner));
            if (owner && !xrt_managed_retain(owner)) { status = XRT_MANAGED_REFCOUNT; break; }
        }
    }
    if (status != XRT_MANAGED_OK) {
        for (size_t i = 0; i < retained; ++i) {
            XrtManagedHeader *owner;
            memcpy(&owner, state->scratch + spec->owned_offsets[i], sizeof(owner));
            xrt_managed_drop(owner);
        }
        xrt_value_zero(type->plan, state->scratch);
    }
    return status;
}
static void module_nonowners(const XrtValuePlanSpec *spec, uint8_t *to, const uint8_t *from) {
    size_t previous = 0;
    for (size_t i = 0; i <= spec->owned_count; ++i) {
        size_t end = i == spec->owned_count ? spec->size : spec->owned_offsets[i];
        if (from) memcpy(to + previous, from + previous, end - previous);
        else memset(to + previous, 0, end - previous);
        previous = end + sizeof(XrtManagedHeader *);
    }
}
typedef struct ModulePublication { XrtModuleState *state; const ModuleType *type; void *destination; } ModulePublication;
static void module_publish_bytes(void *context) {
    ModulePublication *publication = context;
    module_nonowners(&publication->type->spec, publication->destination, publication->state->scratch);
    module_nonowners(&publication->type->spec, publication->state->scratch, NULL);
}
static XrtManagedStatus module_scratch_guard(void *context, XrtPlaceAction action) {
    (void)context;
    return action == XRT_PLACE_MOVE_FROM ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
}
XRAY_API XrtModuleStatus xrt_module_replace(XrtModuleRef *reference, const void *value) {
    if (!module_reference_ready(reference) || reference->state->busy ||
        !(reference->state->layout->view.slots[reference->slot].flags & XRT_MODULE_SLOT_MUTABLE)) return XRT_MANAGED_ARGUMENT;
    XrtModuleState *state = reference->state;
    const ModuleType *type = module_slot_type(state, reference->slot);
    state->busy = true;
    XrtManagedStatus status = module_prepare_copy(state, type, value);
    state->busy = false;
    if (status != XRT_MANAGED_OK) return status;
    for (size_t i = 0; i < type->spec.owned_count; ++i) {
        xrt_module_external_owner(reference, i, &state->destinations[i]);
        state->sources[i] = (XrtExternalOwner){(XrtManagedHeader **)(state->scratch + type->spec.owned_offsets[i]),
            &state->scratch_busy, module_scratch_guard, state, NULL};
        state->edges[i] = (XrtPlaceEdge){.destination={.external=&state->destinations[i], .nulls=XRT_PLACE_NULLABLE},
            .source={.external=&state->sources[i], .nulls=XRT_PLACE_NULLABLE},
            .temporary={.frame=state->temporary_frame, .slot=i, .nulls=XRT_PLACE_NULLABLE},
            .transfer=XRT_PLACE_TRANSFER_MOVE};
    }
    ModulePublication context = {state, type, module_payload(state, reference->slot)};
    XrtPlacePublication publication = {module_publish_bytes, &context};
    if (!type->spec.owned_count) state->busy = true;
    status = xrt_place_edge_transfer(state->edges, type->spec.owned_count, &publication);
    if (!type->spec.owned_count) state->busy = false;
    state->busy = true;
    xrt_value_drop(type->plan, state->scratch);
    state->busy = false;
    return status;
}

static bool module_state_storage(XrtModuleState *state, const XrtModuleLayout *layout, size_t *bytes) {
    uint8_t *base = (uint8_t *)state;
    void *part = NULL;
    *bytes = sizeof(XrtModuleState);
#define MODULE_PART(field,n,t,a) do { \
    if (!module_part(base, bytes, (n), sizeof(t), (a), &part)) return false; \
    if (state) state->field = part; \
} while (0)
    size_t payload = layout->view.payload_size ? layout->view.payload_size : 1;
    size_t scratch = layout->scratch_size ? layout->scratch_size : 1;
    MODULE_PART(payload, payload, uint8_t, alignof(XrMaxAlignment));
    MODULE_PART(scratch, scratch, uint8_t, alignof(XrMaxAlignment));
    MODULE_PART(flags, layout->view.slot_count, uint8_t, alignof(uint8_t));
    MODULE_PART(modules, layout->view.module_count, uint8_t, alignof(uint8_t));
    MODULE_PART(construction, layout->view.slot_count, uint32_t, alignof(uint32_t));
    MODULE_PART(publication, layout->view.slot_count, uint32_t, alignof(uint32_t));
    MODULE_PART(remaining, layout->view.module_count, uint32_t, alignof(uint32_t));
    MODULE_PART(initializers, layout->view.slot_count, XrtModuleRef, alignof(XrtModuleRef));
    MODULE_PART(published, layout->view.slot_count, XrtModuleRef, alignof(XrtModuleRef));
    MODULE_PART(validation, layout->depth, ModuleValidation, alignof(ModuleValidation));
    MODULE_PART(destinations, layout->max_leaves, XrtExternalOwner, alignof(XrtExternalOwner));
    MODULE_PART(sources, layout->max_leaves, XrtExternalOwner, alignof(XrtExternalOwner));
    MODULE_PART(edges, layout->max_leaves, XrtPlaceEdge, alignof(XrtPlaceEdge));
#undef MODULE_PART
    return true;
}
XRAY_API XrtModuleStatus xrt_module_state_new(XrtModuleLayout *layout, XrtBillingOwner *billing,
    XrtModuleState **out) {
    if (!layout || !billing || !out || *out) return XRT_MANAGED_ARGUMENT;
    size_t bytes;
    if (!module_state_storage(NULL, layout, &bytes)) return XRT_MANAGED_OVERFLOW;
    if (!xrt_module_layout_retain(layout)) return XRT_MANAGED_REFCOUNT;
    if (!xrt_billing_owner_retain(billing)) { xrt_module_layout_drop(&layout); return XRT_MANAGED_REFCOUNT; }
    void *memory = NULL;
    XrtManagedStatus status = xrt_managed_metadata_allocate(billing, bytes, &memory);
    if (status != XRT_MANAGED_OK) {
        xrt_module_layout_drop(&layout); xrt_billing_owner_drop(billing); return status;
    }
    memset(memory, 0, bytes);
    XrtModuleState *state = memory;
    *state = (XrtModuleState){.layout=layout, .billing=billing, .bytes=bytes};
    bool valid = module_state_storage(state, layout, &bytes);
    XR_CHECK(valid && bytes == state->bytes, "state vectors match their reserved extent");
    memcpy(state->remaining, layout->slot_counts, (size_t)layout->view.module_count * sizeof(uint32_t));
    for (uint32_t s = 0; s < layout->view.slot_count; ++s) {
        state->initializers[s] = (XrtModuleRef){state, s, SLOT_CONSTRUCTED};
        state->published[s] = (XrtModuleRef){state, s, SLOT_PUBLISHED};
        xrt_value_zero(module_slot_type(state, s)->plan, module_payload(state, s));
    }
    if (layout->max_leaves) {
        status = xrt_managed_root_new(billing, &state->temporary_root);
        if (status == XRT_MANAGED_OK)
            status = xrt_managed_frame_enter(state->temporary_root, layout->max_leaves, &state->temporary_frame);
    }
    if (status != XRT_MANAGED_OK) { xrt_module_state_drop(&state); return status; }
    *out = state;
    return XRT_MANAGED_OK;
}
XRAY_API void xrt_module_state_clear(XrtModuleState *state) {
    if (!state || state->busy) return;
    state->busy = true;
    while (state->construction_count) {
        uint32_t slot = state->construction[--state->construction_count];
        if (state->flags[slot] == SLOT_CONSTRUCTED) {
            state->flags[slot] = 0;
            xrt_value_drop(module_slot_type(state, slot)->plan, module_payload(state, slot));
        }
    }
    while (state->publication_count) {
        uint32_t slot = state->publication[--state->publication_count];
        state->flags[slot] = 0;
        xrt_value_drop(module_slot_type(state, slot)->plan, module_payload(state, slot));
    }
    memset(state->modules, 0, state->layout->view.module_count);
    memcpy(state->remaining, state->layout->slot_counts,
        (size_t)state->layout->view.module_count * sizeof(uint32_t));
    state->busy = false;
}
XRAY_API void xrt_module_state_drop(XrtModuleState **owner) {
    if (!owner || !*owner || (*owner)->busy) return;
    XrtModuleState *state = *owner;
    *owner = NULL;
    xrt_module_state_clear(state);
    state->busy = true;
    XrtBillingOwner *billing = state->billing;
    size_t bytes = state->bytes;
    xrt_managed_root_destroy(&state->temporary_root);
    xrt_module_layout_drop(&state->layout);
    xrt_managed_metadata_free(billing, state, bytes);
    xrt_billing_owner_drop(billing);
}
XRAY_API XrtModuleStatus xrt_module_begin(XrtModuleState *state, uint32_t module) {
    if (!state || state->busy || module >= state->layout->view.module_count ||
        state->modules[module] != MODULE_EMPTY) return XRT_MANAGED_ARGUMENT;
    XrtModuleSpec spec = state->layout->view.modules[module];
    for (uint32_t d = 0; d < spec.dependency_count; ++d)
        if (state->modules[state->layout->view.dependencies[spec.first_dependency + d]] != MODULE_READY)
            return XRT_MANAGED_ARGUMENT;
    state->modules[module] = MODULE_INITIALIZING;
    return XRT_MANAGED_OK;
}
XRAY_API XrtModuleStatus xrt_module_ready(XrtModuleState *state, uint32_t module) {
    if (!state || state->busy || module >= state->layout->view.module_count ||
        state->modules[module] != MODULE_INITIALIZING || state->remaining[module]) return XRT_MANAGED_ARGUMENT;
    state->modules[module] = MODULE_READY;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus module_initial_copy(XrtModuleState *state, uint32_t slot, const void *value) {
    const ModuleType *type = module_slot_type(state, slot);
    state->busy = true;
    XrtManagedStatus status = module_prepare_copy(state, type, value);
    if (status == XRT_MANAGED_OK) {
        if (type->spec.size) memcpy(module_payload(state, slot), state->scratch, type->spec.size);
        xrt_value_zero(type->plan, state->scratch);
    }
    state->busy = false;
    return status;
}
XRAY_API XrtModuleStatus xrt_module_construct(XrtModuleState *state, uint32_t slot,
    const void *value, XrtModuleRef **out) {
    if (!state || state->busy || !out || *out || slot >= state->layout->view.slot_count) return XRT_MANAGED_ARGUMENT;
    const XrtModuleSlotSpec *spec = &state->layout->view.slots[slot];
    if (!(spec->flags & XRT_MODULE_SLOT_MUTABLE) || state->flags[slot] ||
        state->modules[spec->module] != MODULE_INITIALIZING) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = module_initial_copy(state, slot, value);
    if (status != XRT_MANAGED_OK) return status;
    state->flags[slot] = SLOT_CONSTRUCTED;
    state->construction[state->construction_count++] = slot;
    *out = &state->initializers[slot];
    return XRT_MANAGED_OK;
}
XRAY_API XrtModuleStatus xrt_module_publish(XrtModuleRef *reference) {
    if (!module_reference_ready(reference) || reference->state->busy || reference->required_flags != SLOT_CONSTRUCTED)
        return XRT_MANAGED_ARGUMENT;
    XrtModuleState *state = reference->state;
    uint32_t slot = reference->slot;
    const XrtModuleSlotSpec *spec = &state->layout->view.slots[slot];
    if (state->flags[slot] != SLOT_CONSTRUCTED || state->modules[spec->module] != MODULE_INITIALIZING)
        return XRT_MANAGED_ARGUMENT;
    state->flags[slot] |= SLOT_PUBLISHED;
    state->publication[state->publication_count++] = slot;
    --state->remaining[spec->module];
    return XRT_MANAGED_OK;
}
XRAY_API XrtModuleStatus xrt_module_initialize(XrtModuleState *state, uint32_t slot, const void *value) {
    if (!state || state->busy || slot >= state->layout->view.slot_count) return XRT_MANAGED_ARGUMENT;
    const XrtModuleSlotSpec *spec = &state->layout->view.slots[slot];
    if ((spec->flags & XRT_MODULE_SLOT_MUTABLE) || state->flags[slot] ||
        state->modules[spec->module] != MODULE_INITIALIZING) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = module_initial_copy(state, slot, value);
    if (status != XRT_MANAGED_OK) return status;
    state->flags[slot] = SLOT_CONSTRUCTED | SLOT_PUBLISHED;
    state->publication[state->publication_count++] = slot;
    --state->remaining[spec->module];
    return XRT_MANAGED_OK;
}
XRAY_API XrtModuleStatus xrt_module_initialize_i64_group(XrtModuleState *state, uint32_t first,
    uint32_t count, const void *values, size_t value_size) {
    if (!state || state->busy || !count || !values || first >= state->layout->view.slot_count ||
        count > state->layout->view.slot_count - first) return XRT_MANAGED_ARGUMENT;
    size_t bytes = 0;
    if (!module_extent(&bytes, count, sizeof(int64_t))) return XRT_MANAGED_OVERFLOW;
    if (value_size != bytes) return XRT_MANAGED_ARGUMENT;
    uint32_t module = state->layout->view.slots[first].module;
    if (state->modules[module] != MODULE_INITIALIZING) return XRT_MANAGED_ARGUMENT;
    bool initialize = false;
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t slot = first + n;
        const XrtModuleSlotSpec *spec = &state->layout->view.slots[slot];
        uint8_t required = spec->flags & XRT_MODULE_SLOT_MUTABLE ? SLOT_CONSTRUCTED : 0;
        if (spec->module != module || module_slot_type(state, slot)->spec.kind != XRT_VALUE_I64 ||
            state->flags[slot] != required) return XRT_MANAGED_ARGUMENT;
        initialize |= !(spec->flags & XRT_MODULE_SLOT_MUTABLE);
    }
    /* Sources may alias any mutable binding: snapshot the entire group before
     * publishing an immutable member. Reservation failure changes no slot. */
    state->busy = true;
    void *prepared = NULL;
    XrtManagedStatus status = initialize ? xrt_managed_metadata_allocate(state->billing, bytes, &prepared) : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) {
        if (initialize) memcpy(prepared, values, bytes);
        for (uint32_t n = 0; n < count; ++n)
            if (!(state->layout->view.slots[first + n].flags & XRT_MODULE_SLOT_MUTABLE))
                memcpy(module_payload(state, first + n),
                    (const unsigned char *)prepared + (size_t)n * sizeof(int64_t), sizeof(int64_t));
        for (uint32_t n = 0; n < count; ++n) {
            state->flags[first + n] = SLOT_CONSTRUCTED | SLOT_PUBLISHED;
            state->publication[state->publication_count++] = first + n;
        }
        state->remaining[module] -= count;
        if (initialize) xrt_managed_metadata_free(state->billing, prepared, bytes);
    }
    state->busy = false;
    return status;
}
XRAY_API XrtModuleStatus xrt_module_reference(XrtModuleState *state, uint32_t slot, XrtModuleRef **out) {
    if (!state || state->busy || !out || *out || slot >= state->layout->view.slot_count ||
        !(state->flags[slot] & SLOT_PUBLISHED)) return XRT_MANAGED_ARGUMENT;
    *out = &state->published[slot];
    return XRT_MANAGED_OK;
}
XRAY_API XrtModuleStatus xrt_module_borrow(const XrtModuleRef *reference, const void **out) {
    if (!out || !module_reference_ready(reference) || reference->state->busy) return XRT_MANAGED_ARGUMENT;
    *out = module_payload(reference->state, reference->slot);
    return XRT_MANAGED_OK;
}
XRAY_API XrtModuleStatus xrt_module_copy(const XrtModuleRef *reference, void *empty) {
    if (!module_reference_ready(reference) || reference->state->busy) return XRT_MANAGED_ARGUMENT;
    XrtModuleState *state = reference->state;
    const ModuleType *type = module_slot_type(state, reference->slot);
    if ((!empty && type->spec.size) || (uintptr_t)empty % type->spec.alignment ||
        module_overlaps(empty, type->spec.size, state, state->bytes)) return XRT_MANAGED_ARGUMENT;
    for (size_t i = 0; i < type->spec.owned_count; ++i) {
        XrtManagedHeader *owner;
        memcpy(&owner, (uint8_t *)empty + type->spec.owned_offsets[i], sizeof(owner));
        if (owner) return XRT_MANAGED_ARGUMENT;
    }
    state->busy = true;
    XrtManagedStatus status = module_prepare_copy(state, type, module_payload(state, reference->slot));
    if (status == XRT_MANAGED_OK) {
        if (type->spec.size) memcpy(empty, state->scratch, type->spec.size);
        xrt_value_zero(type->plan, state->scratch);
    }
    state->busy = false;
    return status;
}
XRAY_API XrtModuleStatus xrt_module_reference_cache(const XrtModuleRef *reference, XrtModuleAccess *out) {
    if (!out || !module_reference_valid(reference)) return XRT_MANAGED_ARGUMENT;
    XrtModuleState *state = reference->state;
    const XrtModuleSlotSpec *slot = &state->layout->view.slots[reference->slot];
    const ModuleType *type = module_slot_type(state, reference->slot);
    *out = (XrtModuleAccess){.reference=(XrtModuleRef *)reference, .payload=module_payload(state, reference->slot),
        .flags=&state->flags[reference->slot], .required_flags=reference->required_flags,
        .size=type->spec.size, .alignment=type->spec.alignment, .type=slot->type, .kind=type->spec.kind,
        .is_mutable=(slot->flags & XRT_MODULE_SLOT_MUTABLE) != 0};
    return XRT_MANAGED_OK;
}
XRAY_API XrtModuleStatus xrt_module_cache(const XrtModuleState *state, uint32_t slot, XrtModuleAccess *out) {
    if (!state || !state->layout || slot >= state->layout->view.slot_count) return XRT_MANAGED_ARGUMENT;
    return xrt_module_reference_cache(&state->published[slot], out);
}
XRAY_API void xrt_module_require(XrtModuleStatus status, uint32_t source_id) {
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API_NORET void xrt_module_uninitialized(uint32_t source_id) {
    xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source_id);
}
