/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_homes.c - Shared cold home placement from sealed physical facts
 */
#include "xlir_value_homes.h"
#include "xlir_internal.h"
#include "xlir_map.h"
#include "xlir_state.h"
#include "base/xmalloc.h"
#include "xray_native_array_slice.h"
#include <stdlib.h>
#include <string.h>

typedef struct LirHomeVector { void *data; size_t count, capacity, stride; } LirHomeVector;
typedef struct LirHomeCompiler {
    const XrLirModule *input; XrLirFunction function; XrLirFunctionView view; XrLirValueLayoutStatus status;
} LirHomeCompiler;
typedef struct LirHomeBuilder {
    LirHomeCompiler compiler;
    const XrLirValueLayouts *layouts;
    XrLirValueHomes *homes;
    LirHomeVector owners, bytes, events, edges, transfers, calls, arguments, workspaces;
    /* Value id -> stationary producer for a private Array Cell whose MOVE
     * round trip begins and ends in that producer's chain. Builder-owned. */
    uint32_t *stationary_cells;
} LirHomeBuilder;

XR_FUNCDEF void xr_lir_value_homes_destroy(XrLirValueHomes *homes) {
    if (!homes) return;
    xr_free((void *)homes->values); xr_free((void *)homes->events);
    xr_free((void *)homes->bytes); xr_free((void *)homes->transfers);
    xr_free((void *)homes->edges); xr_free((void *)homes->calls);
    xr_free((void *)homes->arguments); xr_free((void *)homes->frame.owned_offsets);
    xr_free((void *)homes->bindings); xr_free((void *)homes->workspaces);
    xr_free((void *)homes->array_regions); xr_free((void *)homes->array_region_steps);
    xr_free((void *)homes->array_push_windows); xr_free((void *)homes->array_push_steps);
    xr_free((void *)homes->string_windows);
    xr_free((void *)homes->string_window_slots);
    xr_free(homes);
}

static bool lir_home_fail(LirHomeCompiler *compiler, XrLirValueLayoutStatus status, const char *message) {
    (void)message;
    if (compiler->status == XR_LIR_VALUE_LAYOUT_OK) compiler->status = status;
    return false;
}
static bool lir_home_grow(LirHomeCompiler *compiler, void **data, size_t *capacity, size_t count, size_t stride) {
    if (count <= *capacity) return true;
    if (count > SIZE_MAX / stride) return lir_home_fail(compiler, XR_LIR_VALUE_LAYOUT_OVERFLOW, "home vector extent overflows");
    size_t next = *capacity ? *capacity : 16;
    while (next < count && next <= SIZE_MAX / 2) next *= 2;
    if (next < count || next > SIZE_MAX / stride) next = count;
    void *grown = xr_realloc(*data, next * stride);
    if (!grown) return lir_home_fail(compiler, XR_LIR_VALUE_LAYOUT_OOM, "home vector allocation failed");
    *data = grown; *capacity = next;
    return true;
}

static bool lir_home_append(LirHomeBuilder *b, LirHomeVector *vector, const void *row) {
    if (vector->count == SIZE_MAX) return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_OVERFLOW, "raw home vector count overflows");
    if (!lir_home_grow(&b->compiler, &vector->data, &vector->capacity, vector->count + 1, vector->stride)) return false;
    memcpy((unsigned char *)vector->data + vector->count++ * vector->stride, row, vector->stride);
    return true;
}
static bool lir_home_key(const XrLirModule *module, XrLirFunction function,
    uint32_t value, XrLirType type, XrLirValueLayoutKey *key) {
    bool plain = type >= XR_LIR_UNIT && type <= XR_LIR_F64;
    if (type >= XR_LIR_TYPE_COUNT) {
        XrLirTypeView shape;
        if (xr_lir_type_view(module, type, &shape) != XR_LIR_OK) return false;
        plain = shape.kind == XR_LIR_TYPE_RECORD || shape.kind == XR_LIR_TYPE_NULLABLE ||
            shape.kind == XR_LIR_TYPE_FIXED_ARRAY || shape.kind == XR_LIR_TYPE_TUPLE ||
            shape.kind == XR_LIR_TYPE_CALLABLE || shape.kind == XR_LIR_TYPE_SLICE ||
            xr_lir_cell_value_type(module,type,NULL);
    }
    if (!plain && type != XR_LIR_STRING && type != XR_LIR_ARRAY && type != XR_LIR_MAP_STRING_I64 &&
        type != XR_LIR_SET_I64) return false;
    *key = (XrLirValueLayoutKey){type, 0, 0};
    if (type == XR_LIR_ARRAY) {
        XrLirArrayShape shape = {0};
        XrLirStatus status = value ? xr_lir_array_shape_view(module,
            (XrLirValue){module, function.id, value}, &shape) : xr_lir_array_result_view(module, function, &shape);
        if (status != XR_LIR_OK) return false;
        key->array_element = shape.element;
    }
    return true;
}
static bool lir_home_spec(LirHomeBuilder *b, XrLirValueLayoutKey key,
    XrtValuePlanSpec *spec, uint32_t *ordinal) {
    const XrtValuePlan *plan = NULL;
    uint32_t id = 0;
    if (xr_lir_value_layouts_lookup(b->layouts, key, &plan, &id) != XR_LIR_VALUE_LAYOUT_OK ||
        xrt_value_plan_view(plan, spec) != XRT_MANAGED_OK)
        return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "value lost its common receiving layout");
    *ordinal = id - 1;
    return true;
}
static bool lir_home_reserve(LirHomeBuilder *b, const XrtValuePlanSpec *spec, size_t *offset, size_t *first) {
    size_t size = b->homes->frame.payload_size;
    if (size > SIZE_MAX - (spec->alignment - 1)) return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_OVERFLOW, "activation home alignment overflows");
    size = (size + spec->alignment - 1) & ~(spec->alignment - 1);
    if (spec->size > SIZE_MAX - size) return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_OVERFLOW, "activation home extent overflows");
    *offset = size; *first = b->owners.count;
    for (size_t i = 0; i < spec->owned_count; ++i) {
        size_t leaf = size + spec->owned_offsets[i];
        if (!lir_home_append(b, &b->owners, &leaf)) return false;
    }
    b->homes->frame.payload_size = size + spec->size;
    if (spec->alignment > b->homes->frame.payload_alignment) b->homes->frame.payload_alignment = spec->alignment;
    return true;
}
static bool lir_home_byte_spans(LirHomeBuilder *b, XrLirValueHome *home, const XrtValuePlanSpec *spec) {
    size_t start = 0;
    home->first_bytes = b->bytes.count;
    for (size_t i = 0; i <= spec->owned_count; ++i) {
        size_t end = i == spec->owned_count ? spec->size : spec->owned_offsets[i];
        if (end > start && !lir_home_append(b, &b->bytes, &(XrLirHomeBytes){start, end - start})) return false;
        if (i < spec->owned_count) start = end + sizeof(XrtManagedHeader *);
    }
    home->byte_count = b->bytes.count - home->first_bytes;
    return true;
}
static bool lir_home_temporary(LirHomeBuilder *b, XrLirValueHome *home) {
    if (home->temporary_offset != SIZE_MAX) return true;
    const XrtValuePlan *plan = NULL; XrLirValueLayoutKey key; XrtValuePlanSpec spec;
    if (xr_lir_value_layouts_at(b->layouts, home->type - 1, &key, &plan) != XR_LIR_VALUE_LAYOUT_OK ||
        xrt_value_plan_view(plan, &spec) != XRT_MANAGED_OK)
        return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "owned temporary lost its receiving layout");
    bool borrowed = home->transport == XR_LIR_CALLABLE_TRANSPORT_FRAME || home->transport == XR_LIR_CALLABLE_TRANSPORT_BORROW ||
        home->cell_transport == XR_LIR_CELL_TRANSPORT_BORROW;
    if (borrowed) { spec.owned_count = 0; spec.owned_offsets = NULL; }
    if (!lir_home_reserve(b, &spec, &home->temporary_offset, &home->temporary_first_owner)) return false;
    if (borrowed) home->temporary_first_owner = SIZE_MAX;
    return true;
}
static bool lir_home_slice(LirHomeBuilder *b, XrLirValueHome *home, uint32_t value) {
    if (home->parameter) {
        XrLirSliceFunctionFact fact;
        if (xr_lir_slice_function_fact(b->compiler.input, b->compiler.function.id, &fact) != XR_LIR_OK ||
            !fact.available || home->parameter > fact.parameter_count)
            return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_UNSUPPORTED, "Slice formal has no checked caller loan");
        home->view_storage = XR_LIR_SLICE_STORAGE_BORROWED_FORMAL;
        home->view_parameter = home->parameter - 1;
        return true;
    }
    if (b->workspaces.count >= UINT32_MAX)
        return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_OVERFLOW, "Slice workspace ordinal overflows");
    size_t leaf = offsetof(XrtArraySliceBundle, hold);
    XrtValuePlanSpec spec = {.size=sizeof(XrtArraySliceBundle),.alignment=_Alignof(XrtArraySliceBundle),
        .owned_offsets=&leaf,.owned_count=1};
    XrLirHomeWorkspace workspace = {.kind=XR_LIR_HOME_WORKSPACE_VIEW_HOLD,.type=home->type,.value=value};
    if (!lir_home_reserve(b, &spec, &workspace.offset, &workspace.first_owner) ||
        !lir_home_reserve(b, &spec, &workspace.temporary_offset, &workspace.temporary_first_owner)) return false;
    if (!lir_home_append(b, &b->workspaces, &workspace)) return false;
    home->view_workspace = (uint32_t)b->workspaces.count;
    home->view_storage = XR_LIR_SLICE_STORAGE_LOCAL_KEEPER;
    home->offset = workspace.offset + offsetof(XrtArraySliceBundle, value);
    home->temporary_offset = workspace.temporary_offset + offsetof(XrtArraySliceBundle, value);
    home->first_owner = home->temporary_first_owner = SIZE_MAX;
    return true;
}
static bool lir_home_state_binding(LirHomeCompiler *c, uint32_t value, XrLirType type) {
    XrLirStateReference reference;
    return type == XR_LIR_STATE_REF && xr_lir_state_reference_view(c->input,
        (XrLirValue){c->input,c->function.id,value},&reference) == XR_LIR_OK &&
        (reference.payload == XR_LIR_MAP_STRING_I64 || reference.payload == XR_LIR_SET_I64 || (reference.payload == XR_LIR_ARRAY &&
            (reference.array_element == XR_LIR_I64 || reference.array_element == XR_LIR_U8)));
}
static bool lir_home_binding_type(LirHomeCompiler *c, uint32_t value, XrLirType type) {
    return type == XR_LIR_I64_REF || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF ||
        type == XR_LIR_MAP_STRING_I64_CELL || type == XR_LIR_MAP_STRING_I64_REF ||
        type == XR_LIR_SET_I64_CELL || type == XR_LIR_SET_I64_REF ||
        xr_lir_string_binding(c->input,(XrLirValue){c->input,c->function.id,value}) || lir_home_state_binding(c,value,type);
}
static bool lir_home_binding_spec(LirHomeBuilder *b, uint32_t type, XrtValuePlanSpec *spec) {
    const XrtValuePlan *plan = NULL; XrLirValueLayoutKey key;
    if (!type || xr_lir_value_layouts_at(b->layouts,type - 1,&key,&plan) != XR_LIR_VALUE_LAYOUT_OK ||
        xrt_value_plan_view(plan,spec) != XRT_MANAGED_OK || spec->size != sizeof(XrtManagedHeader *) ||
        spec->owned_count != 1 || !spec->owned_offsets || spec->owned_offsets[0] ||
        (spec->kind != XRT_VALUE_STRING && spec->kind != XRT_VALUE_SET_I64 &&
         (spec->kind < XRT_VALUE_ARRAY_I64 || spec->kind > XRT_VALUE_MAP_STRING_I64)))
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding lost its exact Header payload plan");
    return true;
}
static bool lir_home_bindings(LirHomeBuilder *b) {
    LirHomeCompiler *c = &b->compiler; bool needed = false;
    for (uint32_t v = 1; v <= c->view.value_count && !needed; ++v) {
        XrLirType type; xr_lir_value_type(c->input,(XrLirValue){c->input,c->function.id,v},&type);
        needed = lir_home_binding_type(c,v,type);
    }
    if (!needed) return true;
    size_t count = (size_t)c->view.value_count + 1;
    if (count > SIZE_MAX / sizeof(XrLirHomeBinding))
        return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_OVERFLOW,"binding directory overflows");
    XrLirHomeBinding *bindings = xr_malloc(count * sizeof(*bindings));
    if (!bindings) return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_OOM,"cannot own binding directory");
    b->homes->bindings = bindings;
    for (size_t v = 0; v < count; ++v)
        bindings[v] = (XrLirHomeBinding){.offset=SIZE_MAX,.first_owner=SIZE_MAX};
    for (uint32_t p = 0; p < c->view.parameter_count; ++p)
        if (lir_home_binding_type(c,c->view.parameters[p],c->view.parameter_types[p])) {
            XrLirHomeBinding *binding = &bindings[c->view.parameters[p]];
            binding->kind = XR_LIR_HOME_BINDING_PARAMETER; binding->parameter = p + 1;
        }
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        XrLirType type; xr_lir_value_type(c->input,(XrLirValue){c->input,c->function.id,v},&type);
        if (!lir_home_binding_type(c,v,type)) continue;
        XrLirValueLayoutKey key = {XR_LIR_MAP_STRING_I64,0,0};
        if (type == XR_LIR_I64_REF) {
            key = (XrLirValueLayoutKey){XR_LIR_I64,0,0};
        } else if (xr_lir_string_binding(c->input,(XrLirValue){c->input,c->function.id,v})) {
            key = (XrLirValueLayoutKey){XR_LIR_STRING,0,0};
            if (type == XR_LIR_STATE_REF) {
                bindings[v].kind = XR_LIR_HOME_BINDING_STATE_BORROW;
                bindings[v].source = v;
            }
        } else if (type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF) {
            XrLirArrayShape shape;
            if (xr_lir_array_shape_view(c->input,(XrLirValue){c->input,c->function.id,v},&shape) != XR_LIR_OK)
                return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding lost its exact Array element");
            key = (XrLirValueLayoutKey){XR_LIR_ARRAY,shape.element,0};
        } else if (type == XR_LIR_SET_I64_CELL || type == XR_LIR_SET_I64_REF) {
            key = (XrLirValueLayoutKey){XR_LIR_SET_I64,0,0};
        } else if (lir_home_state_binding(c,v,type)) {
            XrLirStateReference reference;
            if (xr_lir_state_reference_view(c->input,(XrLirValue){c->input,c->function.id,v},&reference) != XR_LIR_OK)
                return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_ARGUMENT,"State binding lost its exact payload");
            key = (XrLirValueLayoutKey){reference.payload,reference.array_element,0};
            bindings[v].kind = XR_LIR_HOME_BINDING_STATE_BORROW;
            bindings[v].source = v;
        }
        XrtValuePlanSpec spec; uint32_t ordinal;
        if (!lir_home_spec(b,key,&spec,&ordinal)) return false;
        bindings[v].type = ordinal + 1;
        if (type != XR_LIR_I64_REF && !lir_home_binding_spec(b,bindings[v].type,&spec)) return false;
    }
    return true;
}
static bool lir_home_binding_fact(LirHomeBuilder *b, XrLirOwnershipFact fact) {
    if (fact.kind < XR_LIR_OWNERSHIP_BINDING_NEW || fact.kind > XR_LIR_OWNERSHIP_BINDING_WRITE) return true;
    if (!b->homes->bindings || !fact.value || fact.value > b->homes->value_count)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding event has no receiving row");
    XrLirHomeBinding *bindings = (XrLirHomeBinding *)b->homes->bindings, *row = &bindings[fact.value];
    if (!row->type || fact.target_value > b->homes->value_count)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding event lost its payload identity");
    if (fact.kind == XR_LIR_OWNERSHIP_BINDING_NEW) {
        if (row->kind || !fact.target_value || b->homes->values[fact.target_value].type != row->type)
            return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding constructor lost its initializer");
        XrtValuePlanSpec spec;
        if (!lir_home_binding_spec(b,row->type,&spec)) return false;
        uint32_t producer = b->stationary_cells ? b->stationary_cells[fact.value] : 0;
        if (producer) {
            const XrLirValueHome *owner = &b->homes->values[producer];
            if (owner->type != row->type || owner->owner_count != 1 || owner->first_owner == SIZE_MAX ||
                owner->offset == SIZE_MAX)
                return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"stationary Cell lost its owner home");
            row->offset = owner->offset;
            row->first_owner = owner->first_owner;
        } else if (!lir_home_reserve(b,&spec,&row->offset,&row->first_owner)) return false;
        row->kind = XR_LIR_HOME_BINDING_LOCAL;
    } else if (fact.kind == XR_LIR_OWNERSHIP_BINDING_BORROW) {
        if (row->kind || !fact.target_value)
            return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding borrow lost its actual source");
        XrLirType type;
        xr_lir_value_type(b->compiler.input,(XrLirValue){b->compiler.input,b->compiler.function.id,fact.target_value},&type);
        if (type == XR_LIR_STATE_REF) row->kind = XR_LIR_HOME_BINDING_STATE_BORROW;
        else if (xr_lir_cell_value_type(b->compiler.input,type,NULL) &&
            b->homes->values[fact.target_value].cell_payload_type == row->type)
            row->kind = XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW;
        else if ((type == XR_LIR_ARRAY_CELL || type == XR_LIR_MAP_STRING_I64_CELL || type == XR_LIR_STRING_CELL ||
                type == XR_LIR_SET_I64_CELL) &&
            bindings[fact.target_value].type == row->type)
            row->kind = XR_LIR_HOME_BINDING_CELL_BORROW;
        else return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_UNSUPPORTED,"binding source is not an activation or State binding");
        row->source = fact.target_value;
    } else if (fact.kind == XR_LIR_OWNERSHIP_BINDING_RELEASE) {
        if (row->parameter || fact.target_value)
            return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"incoming binding borrow cannot be retired");
    } else if (!fact.target_value || b->homes->values[fact.target_value].type != row->type)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding read or replacement lost its snapshot");
    return true;
}
static bool lir_home_workspace(LirHomeBuilder *b, XrLirOwnershipFact fact) {
    bool array = fact.kind == XR_LIR_OWNERSHIP_ARRAY_OPERATION &&
        (fact.array_operation == XR_LIR_ARRAY_OP_SET || fact.array_operation == XR_LIR_ARRAY_OP_PUSH);
    bool map = fact.kind == XR_LIR_OWNERSHIP_MAP_OPERATION &&
        (fact.map_operation == XR_LIR_MAP_OP_SET || fact.map_operation == XR_LIR_MAP_OP_REMOVE || fact.map_operation == XR_LIR_MAP_OP_CLEAR);
    bool set = fact.kind == XR_LIR_OWNERSHIP_SET_OPERATION &&
        (fact.set_operation == XR_LIR_SET_ADD || fact.set_operation == XR_LIR_SET_REMOVE || fact.set_operation == XR_LIR_SET_CLEAR);
    bool string = fact.kind == XR_LIR_OWNERSHIP_STRING_OPERATION;
    bool borrowed_copy = (fact.kind == XR_LIR_OWNERSHIP_BINDING_NEW || fact.kind == XR_LIR_OWNERSHIP_BINDING_WRITE) &&
        fact.target_value && fact.target_value <= b->homes->value_count && b->homes->values[fact.target_value].parameter;
    if (!array && !map && !set && !string && !borrowed_copy) return true;
    if (!fact.value || fact.value > b->homes->value_count || !b->homes->bindings || !b->homes->bindings[fact.value].type)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"mutator has no actual binding payload");
    uint32_t type = b->homes->bindings[fact.value].type;
    XrtValuePlanSpec spec;
    if (!lir_home_binding_spec(b,type,&spec)) return false;
    if (borrowed_copy && spec.kind != XRT_VALUE_STRING && spec.kind != XRT_VALUE_MAP_STRING_I64 &&
        spec.kind != XRT_VALUE_SET_I64) return true;
    XrLirHomeWorkspace workspace = {.block=fact.block,.instruction=fact.instruction,
        .type=type,.value=string || borrowed_copy ? fact.value : 0};
    if (string || borrowed_copy) workspace.temporary_offset = workspace.temporary_first_owner = SIZE_MAX;
    if (string && (fact.transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_BORROW ||
        (fact.string_operation != XR_LIR_STRING_OP_APPEND_CELL && fact.string_operation != XR_LIR_STRING_OP_APPEND_STATE &&
         fact.string_operation != XR_LIR_STRING_OP_APPEND_REF) || !fact.target_value ||
        fact.target_value > b->homes->value_count || b->homes->values[fact.target_value].type != workspace.type))
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"String append lost its borrowed binding or suffix");
    if (borrowed_copy && (b->homes->values[fact.target_value].type != workspace.type ||
        b->homes->values[fact.target_value].offset != SIZE_MAX || b->homes->values[fact.target_value].first_owner != SIZE_MAX))
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"borrowed binding copy lost its incoming formal");
    if (!lir_home_reserve(b,&spec,&workspace.offset,&workspace.first_owner)) return false;
    return lir_home_append(b,&b->workspaces,&workspace);
}
static bool lir_home_callable_rows(LirHomeBuilder *b, const XrLirOwnershipPlan *ownership, XrLirValueHome *values) {
    XrLirOwnershipFunction function;
    if (xr_lir_ownership_function_at(ownership,b->compiler.function.id,&function) != XR_LIR_OK)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"callable transport lost its ownership snapshot");
    for (uint32_t n = 0; n < function.fact_count; ++n) {
        XrLirOwnershipFact fact;
        if (xr_lir_ownership_fact_at(ownership,function.fact_offset + n,&fact) != XR_LIR_OK)
            return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"callable transport fact disappeared");
        if (fact.kind == XR_LIR_OWNERSHIP_CELL_VALUE) {
            XrLirCellType cell;
            if (!fact.value || fact.value > b->compiler.view.value_count ||
                !xr_lir_cell_value_type(b->compiler.input,fact.type,&cell) || cell.payload != fact.cell_payload ||
                (fact.cell_transport != XR_LIR_CELL_TRANSPORT_BORROW && fact.cell_transport != XR_LIR_CELL_TRANSPORT_OWNED))
                return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"Cell transport lost its exact payload or source");
            XrtValuePlanSpec payload; uint32_t ordinal;
            if (!lir_home_spec(b,(XrLirValueLayoutKey){cell.payload,0,0},&payload,&ordinal)) return false;
            values[fact.value].cell_payload_type = ordinal + 1;
            values[fact.value].cell_transport = fact.cell_transport;
            continue;
        }
        if (fact.kind != XR_LIR_OWNERSHIP_CALLABLE_VALUE) continue;
        if (!fact.value || fact.value > b->compiler.view.value_count || fact.target_value > b->compiler.view.value_count)
            return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"callable transport names an invalid SSA value");
        XrLirValueHome *home = &values[fact.value];
        home->transport = fact.callable_transport;
        home->borrow_source = home->transport == XR_LIR_CALLABLE_TRANSPORT_OWNED ? 0 : fact.target_value;
        if (home->transport == XR_LIR_CALLABLE_TRANSPORT_MIXED || home->transport == XR_LIR_CALLABLE_TRANSPORT_NONE)
            return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_UNSUPPORTED,"mixed callable transport requires explicit owning and borrowing edge handling");
        if (home->transport == XR_LIR_CALLABLE_TRANSPORT_OWNED && fact.callable_operation == XR_LIR_FUNCTION_REF) {
            if (!fact.callable_function || fact.callable_function > xr_lir_function_count(b->compiler.input))
                return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"owning constructor lost its exact environment target");
            XrLirClosureTarget target;
            if (xr_lir_closure_target_view(b->compiler.input,(XrLirFunction){b->compiler.input,fact.callable_function},&target) != XR_LIR_OK)
                return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"owning constructor has no exact target signature");
            const XrtValuePlan *plan = NULL;
            if (xr_lir_value_layouts_lookup(b->layouts,(XrLirValueLayoutKey){target.callable,0,fact.callable_function},
                    &plan,&home->environment_type) != XR_LIR_VALUE_LAYOUT_OK)
                return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"owning constructor lost its common environment projection");
        }
    }
    return true;
}
static bool lir_home_borrows_literal_key(const XrLirModule *module, const XrLirInstructionView *view) {
    if (view->op != XR_LIR_CALL_INTRINSIC) return false;
    XrLirIntrinsic intrinsic;
    return xr_lir_intrinsic_view(module, view->intrinsic, &intrinsic) == XR_LIR_OK &&
        xr_lir_map_intrinsic_valid(module, &intrinsic) && xr_lir_map_borrows_key(intrinsic.operation);
}

/* Managed owners and their last use stay in their defining block. Only
 * synchronous uses can read the keeper. A Map write retains a new key itself
 * before publication, just as when the caller owns a separate reference. */
static bool lir_home_literal_borrows(LirHomeCompiler *c, XrLirValueHome *values, size_t count) {
    enum { BORROW = 1, RELEASE = 2, OTHER = 4 };
    uint8_t *uses = xr_calloc(count, sizeof(*uses));
    if (!uses) return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OOM, "cannot classify String literal borrows");
    for (uint32_t block = 0; block < c->view.block_count; ++block) {
        XrLirBlock id; XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, block, &id); xr_lir_block_view(c->input, id, &view);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView v;
            if (xr_lir_instruction_view(c->input, id, n, &v) != XR_LIR_OK) {
                xr_free(uses);
                return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "String literal use disappeared");
            }
            for (uint32_t a = 0; a < v.operand_count; ++a) {
                uint32_t operand = v.operands[a];
                if (values[operand].literal_cache_owner == SIZE_MAX) continue;
                if (v.op == XR_LIR_STRING_DROP) uses[operand] |= RELEASE;
                else if (a == 1 && (xr_lir_string_role(c->input, &v) == XR_LIR_STRING_OP_APPEND_OWNED ||
                    lir_home_borrows_literal_key(c->input, &v)))
                    uses[operand] |= BORROW;
                else uses[operand] |= OTHER;
            }
            for (uint32_t e = 0; e < v.edge_count; ++e)
                for (uint32_t a = 0; a < v.edges[e].argument_count; ++a)
                    uses[v.edges[e].arguments[a]] |= OTHER;
        }
    }
    for (size_t v = 1; v < count; ++v)
        values[v].literal_cache_borrow = uses[v] == (BORROW | RELEASE);
    xr_free(uses);
    return true;
}

typedef struct LirHomeChain {
    uint32_t parent, producer, last_block;
    uint32_t binding_input, binding_output, handoff_source;
    uint8_t rank;
    bool rejected, array_region, array_cell, owning_call;
} LirHomeChain;

static uint32_t lir_home_chain_root(LirHomeChain *chains, uint32_t value) {
    while (chains[value].parent != value) {
        chains[value].parent = chains[chains[value].parent].parent;
        value = chains[value].parent;
    }
    return value;
}

static void lir_home_chain_join(LirHomeChain *chains, uint32_t source, uint32_t target, bool move) {
    uint32_t to = lir_home_chain_root(chains, target);
    if (!move || !chains[source].parent) { chains[to].rejected = true; return; }
    uint32_t from = lir_home_chain_root(chains, source);
    if (to == from) return;
    if (chains[to].rank < chains[from].rank) { uint32_t swap = to; to = from; from = swap; }
    LirHomeChain *root = &chains[to], *part = &chains[from];
    root->rejected |= part->rejected || (root->producer && part->producer);
    if (!root->producer) root->producer = part->producer;
    if (root->rank == part->rank) ++root->rank;
    part->parent = to;
}

/* Collect owning calls and pair existing last-use handoffs around a private Cell.
 * The Cell keeps its distinct physical slot through mutation. Its source home
 * is empty by the time the adjacent read and release return ownership. */
static bool lir_home_chain_origins(LirHomeCompiler *c, const XrLirOwnershipPlan *plan,
    XrLirOwnershipFunction function, LirHomeChain *chains) {
    const XrLirFunctionData *f = &c->input->functions[c->function.id - 1];
    for (uint32_t n = 0; n < function.fact_count; ++n) {
        XrLirOwnershipFact fact;
        if (xr_lir_ownership_fact_at(plan, function.fact_offset + n, &fact) != XR_LIR_OK)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "stationary binding fact disappeared");
        if (fact.kind == XR_LIR_OWNERSHIP_RESULT_OWNER && fact.type == XR_LIR_ARRAY &&
            fact.array_element == XR_LIR_I64) {
            if (!fact.value || fact.value > c->view.value_count)
                return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Array result lost its owner");
            const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, fact.value);
            if (definition && definition->op == XR_LIR_CALL)
                chains[fact.value].owning_call = true;
        }
        bool array = fact.type == XR_LIR_ARRAY_CELL && fact.array_element == XR_LIR_I64;
        if ((!array && fact.type != XR_LIR_MAP_STRING_I64_CELL) ||
            fact.forwarding_mode != XR_LIR_OWNERSHIP_TRANSFER_MOVE)
            continue;
        bool create = fact.kind == XR_LIR_OWNERSHIP_BINDING_NEW;
        if ((!create && fact.kind != XR_LIR_OWNERSHIP_BINDING_READ) ||
            !fact.value || fact.value > c->view.value_count ||
            !fact.target_value || fact.target_value > c->view.value_count)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "stationary binding lost its handoff");
        LirHomeChain *cell = &chains[fact.value];
        if (cell->array_region || chains[fact.target_value].array_region) continue;
        uint32_t *endpoint = create ? &cell->binding_input : &cell->binding_output;
        if (*endpoint)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "stationary binding has conflicting handoffs");
        *endpoint = fact.target_value;
        cell->array_cell = array;
    }
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        const LirHomeChain *cell = &chains[v];
        if (!cell->binding_input || !cell->binding_output) continue;
        LirHomeChain *output = &chains[cell->binding_output];
        if (output->handoff_source)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "stationary value has multiple handoff sources");
        output->handoff_source = cell->binding_input;
    }
    return true;
}

static bool lir_home_chain_transfers(LirHomeCompiler *c, const XrLirOwnershipPlan *plan,
    XrLirOwnershipFunction function, LirHomeChain *chains) {
    for (uint32_t n = 0; n < function.fact_count; ++n) {
        XrLirOwnershipFact fact;
        if (xr_lir_ownership_fact_at(plan, function.fact_offset + n, &fact) != XR_LIR_OK)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "stationary home fact disappeared");
        switch (fact.kind) {
        case XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER:
            if (chains[fact.target_value].parent)
                lir_home_chain_join(chains, fact.value, fact.target_value, fact.transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE);
            break;
        case XR_LIR_OWNERSHIP_APPEND_TRANSFER:
        case XR_LIR_OWNERSHIP_RETURN_TRANSFER:
            if (chains[fact.value].parent)
                chains[lir_home_chain_root(chains, fact.value)].rejected = true;
            break;
        default: break;
        }
    }
    return true;
}

static void lir_home_chain_visit(LirHomeChain *chains, uint32_t value, uint32_t block) {
    if (!value || !chains[value].parent) return;
    LirHomeChain *root = &chains[lir_home_chain_root(chains, value)];
    root->rejected |= root->last_block == block;
    root->last_block = block;
}

static bool lir_home_chain_type(const XrLirValueData *value) {
    return value->type == XR_LIR_STRING || value->type == XR_LIR_MAP_STRING_I64 ||
        (value->type == XR_LIR_ARRAY && value->array_element == XR_LIR_I64);
}

/* The independent literal cache or caller's loan keeps the backing alive
 * throughout Frame cleanup. Coalescing its local owner therefore cannot move
 * the final release relative to other cleanup. Keep the original producer
 * and explicit DROP, including failure at the original retain. */
static bool lir_home_chain_producer(const XrLirFunctionData *f, uint32_t id) {
    const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, id);
    if (!definition) return false;
    if (definition->op == XR_LIR_STRING_LITERAL) return true;
    if (definition->op != XR_LIR_STRING_RETAIN && definition->op != XR_LIR_MAP_RETAIN) return false;
    const XrLirValueData *source = &f->values[definition->operands[0] - 1];
    return source->parameter && !source->block;
}

static bool lir_home_container_chain_start(const XrLirModule *module, const XrLirFunctionData *f, uint32_t id) {
    const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, id);
    if (!definition) return false;
    if (definition->type == XR_LIR_ARRAY) {
        if (definition->op == XR_LIR_ARRAY_RETAIN) return true;
        const XrLirArrayIntrinsic *shape = definition->op == XR_LIR_CALL_INTRINSIC ?
            xr_lir_internal_array_intrinsic(module, definition->intrinsic) : NULL;
        return shape && (shape->operation == XR_LIR_ARRAY_OP_NEW || shape->operation == XR_LIR_ARRAY_OP_REPEAT);
    }
    if (definition->type != XR_LIR_MAP_STRING_I64) return false;
    if (definition->op == XR_LIR_MAP_LITERAL_NEW || definition->op == XR_LIR_MAP_RETAIN) return true;
    /* Reading a local Map Cell creates an independent owning snapshot. */
    if (definition->op == XR_LIR_CELL_READ)
        return definition->operand_count == 1 &&
            f->values[definition->operands[0] - 1].type == XR_LIR_MAP_STRING_I64_CELL;
    XrLirIntrinsic entry;
    return definition->op == XR_LIR_CALL_INTRINSIC &&
        xr_lir_intrinsic_view(module, definition->intrinsic, &entry) == XR_LIR_OK &&
        xr_lir_map_intrinsic_valid(module, &entry) && entry.operation == XR_LIR_MAP_OP_NEW;
}

/* Region consumers project these exact moves to distinct physical homes.
 * Preserve those endpoints, including their original entry and exit moves.
 * A chain in the same function can still use every other verified MOVE. */
static void lir_home_chain_array_regions(LirHomeChain *chains, const XrLirArraySetRegionStep *steps, uint32_t count) {
    for (uint32_t n = 0; n < count; ++n) {
        chains[steps[n].source].array_region = true;
        chains[steps[n].target].array_region = true;
    }
}

/* A sealed MOVE chain has one origin and at most one independent definition
 * per block. A certified binding round trip continues that same owner, even
 * if the mutation replaces its backing. COPY edges and unrelated origins do
 * not share homes. Explicit releases and checked operations keep their sites.
 * The fixed container payloads contain only identity-free values, so failure
 * drain can release the one live owner from the shared physical slot. */
static bool lir_home_stationary_owners(LirHomeBuilder *builder, const XrLirOwnershipPlan *plan,
    XrLirValueHome *values, size_t count) {
    LirHomeCompiler *c = &builder->compiler;
    const XrLirFunctionData *f = &c->input->functions[c->function.id - 1];
    bool needed = false;
    for (uint32_t v = 0; v < f->value_count && !needed; ++v)
        needed = lir_home_chain_type(&f->values[v]) && f->values[v].parameter && f->values[v].block;
    if (!needed) return true;
    XrLirOwnershipFunction function;
    if (xr_lir_ownership_function_at(plan, c->function.id, &function) != XR_LIR_OK)
        return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "stationary homes lost their ownership inventory");
    LirHomeChain *chains = xr_calloc(count, sizeof(*chains));
    if (!chains) return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OOM, "cannot classify owner forwarding chains");
    lir_home_chain_array_regions(chains, builder->homes->array_region_steps, builder->homes->array_region_step_count);
    lir_home_chain_array_regions(chains, builder->homes->array_push_steps, builder->homes->array_push_step_count);
    if (!lir_home_chain_origins(c, plan, function, chains)) { xr_free(chains); return false; }
    for (uint32_t v = 1; v <= f->value_count; ++v) {
        const XrLirValueData *value = &f->values[v - 1];
        if (!lir_home_chain_type(value) || !value->block || chains[v].array_region) continue;
        chains[v].parent = v;
        if (!value->parameter && !chains[v].handoff_source) {
            chains[v].producer = v;
            /* The successful direct call publishes an owner in this activation.
             * Its backing may still be shared with the callee or another binding. */
            chains[v].rejected = !chains[v].owning_call && !lir_home_chain_producer(f, v) &&
                !lir_home_container_chain_start(c->input, f, v);
        }
    }
    for (uint32_t v = 1; v <= f->value_count; ++v) {
        uint32_t source = chains[v].handoff_source;
        if (!source) continue;
        if (!chains[source].parent || !chains[v].parent) {
            xr_free(chains);
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "stationary handoff lost its value homes");
        }
        lir_home_chain_join(chains, source, v, true);
    }
    if (!lir_home_chain_transfers(c, plan, function, chains)) { xr_free(chains); return false; }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        for (uint32_t p = 0; p < block->parameter_count; ++p)
            lir_home_chain_visit(chains, block->parameters[p], b + 1);
        for (uint32_t n = 0; n < block->instruction_count; ++n) {
            uint32_t result = block->instructions[n].view.result;
            if (!chains[result].handoff_source)
                lir_home_chain_visit(chains, result, b + 1);
        }
    }
    for (uint32_t v = 1; v <= f->value_count; ++v) {
        if (!chains[v].parent) continue;
        const LirHomeChain *root = &chains[lir_home_chain_root(chains, v)];
        if (root->rank && root->producer && !root->rejected)
            values[v].stationary_owner = root->producer;
    }
    /* A mutation Cell between two names of one stationary owner holds that
     * same owner for its whole life: the MOVE in empties the source and the
     * MOVE out refills it. Giving the Cell the owner's slot turns both
     * handoffs into no-ops while the mutation edits the owner in place. */
    for (uint32_t v = 1; v <= f->value_count; ++v) {
        const LirHomeChain *cell = &chains[v];
        if (!cell->array_cell || cell->array_region || !cell->binding_input || !cell->binding_output) continue;
        uint32_t producer = values[cell->binding_input].stationary_owner;
        if (!producer || producer != values[cell->binding_output].stationary_owner) continue;
        if (!builder->stationary_cells) {
            builder->stationary_cells = xr_calloc(count, sizeof(*builder->stationary_cells));
            if (!builder->stationary_cells) {
                xr_free(chains);
                return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OOM, "cannot record stationary mutation Cells");
            }
        }
        builder->stationary_cells[v] = producer;
    }
    xr_free(chains);
    return true;
}

static bool lir_home_values(LirHomeBuilder *b, const XrLirOwnershipPlan *ownership) {
    LirHomeCompiler *c = &b->compiler;
    size_t count = (size_t)c->view.value_count + 1;
    if (count > SIZE_MAX / sizeof(XrLirValueHome)) return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OVERFLOW, "activation home directory overflows");
    XrLirValueHome *values = xr_malloc(count * sizeof(*values));
    if (!values) return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OOM, "cannot own activation home directory");
    b->homes->values = values;
    for (size_t v = 0; v < count; ++v) values[v] = (XrLirValueHome){.type=0,
        .offset=SIZE_MAX, .first_owner=SIZE_MAX, .temporary_offset=SIZE_MAX, .temporary_first_owner=SIZE_MAX,
        .literal_cache_offset=SIZE_MAX, .literal_cache_owner=SIZE_MAX,.view_parameter=UINT32_MAX};
    if (!lir_home_callable_rows(b,ownership,values) || !lir_home_stationary_owners(b, ownership, values, count)) return false;
    for (size_t index = 1; index <= c->view.value_count; ++index) {
        uint32_t v = (uint32_t)index;
        XrLirType type; XrLirValueLayoutKey key;
        if (xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, v}, &type) != XR_LIR_OK)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "activation value is unreadable");
        if (!lir_home_key(c->input, c->function, v, type, &key)) continue;
        XrtValuePlanSpec spec; uint32_t ordinal;
        if (!lir_home_spec(b, key, &spec, &ordinal)) return false;
        if (!spec.owned_count && !xrt_value_kind_has_fields(spec.kind) && spec.kind != XRT_VALUE_NULLABLE &&
            spec.kind != XRT_VALUE_FIXED_U8 &&
            spec.kind != XRT_VALUE_SLICE_I64 && spec.kind != XRT_VALUE_SLICE_U8) continue;
        XrLirValueHome *home = &values[v];
        home->type = ordinal + 1; home->size = spec.size; home->owner_count = spec.owned_count;
        bool callable = spec.kind == XRT_VALUE_FUNCTION;
        bool cell = spec.kind == XRT_VALUE_CELL;
        bool borrowed = callable && (home->transport == XR_LIR_CALLABLE_TRANSPORT_FRAME ||
            home->transport == XR_LIR_CALLABLE_TRANSPORT_BORROW);
        borrowed |= cell && home->cell_transport == XR_LIR_CELL_TRANSPORT_BORROW;
        if (cell && (!home->cell_transport || !home->cell_payload_type))
            return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_ARGUMENT,"Cell value has no verified identity transport");
        if (callable && !home->transport)
            return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_UNSUPPORTED,"callable value has no verified transport fact");
        if (borrowed) { home->owner_count = 0; spec.owned_count = 0; spec.owned_offsets = NULL; }
        for (uint32_t p = 0; p < c->view.parameter_count; ++p)
            if (c->view.parameters[p] == v) home->parameter = p + 1;
        bool slice = spec.kind == XRT_VALUE_SLICE_I64 || spec.kind == XRT_VALUE_SLICE_U8;
        if (slice) { if (!lir_home_slice(b, home, v)) return false; }
        else if (!home->parameter && (!home->stationary_owner || home->stationary_owner == v) &&
            !lir_home_reserve(b, &spec, &home->offset, &home->first_owner)) return false;
        if (borrowed) home->first_owner = SIZE_MAX;
        if (!lir_home_byte_spans(b, home, &spec)) return false;
    }
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        XrLirValueHome *home = &values[v];
        if (!home->stationary_owner || home->stationary_owner == v) continue;
        const XrLirValueHome *owner = &values[home->stationary_owner];
        if (home->type != owner->type || owner->owner_count != 1 || owner->first_owner == SIZE_MAX)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "stationary owner lost its producer home");
        home->offset = owner->offset;
        home->first_owner = owner->first_owner;
    }
    bool has_literal_cache = false;
    for (uint32_t block = 0; block < c->view.block_count; ++block) {
        XrLirBlock id; XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, block, &id); xr_lir_block_view(c->input, id, &view);
        for (uint32_t p = 0; p < view.parameter_count; ++p) {
            XrLirValueHome *home = &values[view.parameters[p]];
            if (home->type == 0 || home->owner_count) continue;
            if (!lir_home_temporary(b, home)) return false;
        }
        /* One linear instruction scan selects actual literal producers. An
         * unused String descriptor does not allocate activation cache storage. */
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView instruction;
            if (xr_lir_instruction_view(c->input, id, n, &instruction) != XR_LIR_OK)
                return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "literal home producer disappeared");
            if (instruction.op != XR_LIR_STRING_LITERAL) continue;
            XrLirValueHome *home = &values[instruction.result];
            XrtValuePlanSpec spec; uint32_t ordinal;
            if (!lir_home_spec(b, (XrLirValueLayoutKey){XR_LIR_STRING,0,0}, &spec, &ordinal)) return false;
            if (home->parameter || home->type != ordinal + 1 || home->owner_count != 1 ||
                spec.kind != XRT_VALUE_STRING || home->literal_cache_owner != SIZE_MAX)
                return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "literal cache lost its exact String home");
            if (!lir_home_reserve(b, &spec, &home->literal_cache_offset, &home->literal_cache_owner)) return false;
            has_literal_cache = true;
        }
    }
    if (has_literal_cache && !lir_home_literal_borrows(c, values, count)) return false;
    XrLirValueLayoutKey key; XrtValuePlanSpec result;
    if (lir_home_key(c->input, c->function, 0, c->view.result, &key)) {
        uint32_t ordinal;
        if (!lir_home_spec(b, key, &result, &ordinal)) return false;
        b->homes->result_type = ordinal + 1;
        if (result.kind == XRT_VALUE_SLICE_I64 || result.kind == XRT_VALUE_SLICE_U8) {
            XrLirSliceFunctionFact fact;
            if (xr_lir_slice_function_fact(c->input,c->function.id,&fact) != XR_LIR_OK ||
                !fact.available || fact.result_parameter >= c->view.parameter_count)
                return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_ARGUMENT,"Slice result lost its unique borrowed origin");
            b->homes->view_result_parameter = fact.result_parameter;
        }
    }
    return true;
}
static int lir_home_edge_order(const void *left, const void *right) {
    const XrLirHomeEvent *a = *(const XrLirHomeEvent *const *)left;
    const XrLirHomeEvent *c = *(const XrLirHomeEvent *const *)right;
    if (a->block != c->block) return a->block < c->block ? -1 : 1;
    if (a->edge != c->edge) return a->edge < c->edge ? -1 : 1;
    return a->argument < c->argument ? -1 : a->argument != c->argument;
}
static bool lir_home_cell_stage(LirHomeBuilder *b, const XrLirValueHome *source,
    const XrLirValueHome *target, XrLirHomeWorkspace workspace, XrLirHomeTransfer *transfer) {
    if (source->cell_transport != XR_LIR_CELL_TRANSPORT_BORROW ||
        target->cell_transport != XR_LIR_CELL_TRANSPORT_OWNED) return true;
    if (!source->parameter || source->owner_count || source->first_owner != SIZE_MAX ||
        target->owner_count != 1 || transfer->transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_COPY)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"borrowed Cell edge lost its actual formal COPY");
    const XrtValuePlan *plan = NULL; XrLirValueLayoutKey key; XrtValuePlanSpec spec;
    if (xr_lir_value_layouts_at(b->layouts,target->type - 1,&key,&plan) != XR_LIR_VALUE_LAYOUT_OK ||
        xrt_value_plan_view(plan,&spec) != XRT_MANAGED_OK || spec.kind != XRT_VALUE_CELL ||
        spec.owned_count != 1 || spec.owned_offsets[0] != 0)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"borrowed Cell stage lost its same-image identity plan");
    workspace.kind = XR_LIR_HOME_WORKSPACE_CELL_BORROW_STAGE;
    workspace.type = target->type; workspace.value = transfer->source;
    workspace.temporary_offset = workspace.temporary_first_owner = SIZE_MAX;
    if (!lir_home_reserve(b,&spec,&workspace.offset,&workspace.first_owner)) return false;
    transfer->borrowed_owner_workspace = b->workspaces.count;
    return lir_home_append(b,&b->workspaces,&workspace);
}
static bool lir_home_header_stage(LirHomeBuilder *b, const XrLirValueHome *source,
    const XrLirValueHome *target, XrLirHomeWorkspace workspace, XrLirHomeTransfer *transfer) {
    if (!source->parameter || source->cell_transport || source->view_storage) return true;
    const XrtValuePlan *plan = NULL; XrLirValueLayoutKey key; XrtValuePlanSpec spec;
    if (xr_lir_value_layouts_at(b->layouts,source->type - 1,&key,&plan) != XR_LIR_VALUE_LAYOUT_OK ||
        xrt_value_plan_view(plan,&spec) != XRT_MANAGED_OK)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"borrowed Header edge lost its receiving plan");
    if (key.type != XR_LIR_SET_I64) return true;
    if (spec.kind != XRT_VALUE_SET_I64 || spec.size != sizeof(XrtManagedHeader *) ||
        spec.owned_count != 1 || spec.owned_offsets[0] != 0 || source->type != target->type ||
        source->owner_count != 1 || source->offset != SIZE_MAX || source->first_owner != SIZE_MAX ||
        source->temporary_offset != SIZE_MAX || source->temporary_first_owner != SIZE_MAX ||
        target->parameter || target->owner_count != 1 || target->offset == SIZE_MAX ||
        target->first_owner == SIZE_MAX || target->temporary_offset == SIZE_MAX ||
        target->temporary_first_owner == SIZE_MAX || transfer->transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_COPY ||
        transfer->hold_transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_NONE)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"borrowed Set edge lost its actual formal COPY");
    workspace.kind = XR_LIR_HOME_WORKSPACE_HEADER_BORROW_STAGE;
    workspace.type = target->type; workspace.value = transfer->source;
    workspace.temporary_offset = workspace.temporary_first_owner = SIZE_MAX;
    if (!lir_home_reserve(b,&spec,&workspace.offset,&workspace.first_owner)) return false;
    transfer->borrowed_owner_workspace = b->workspaces.count;
    return lir_home_append(b,&b->workspaces,&workspace);
}
static bool lir_home_slice_stage(LirHomeBuilder *b, const XrLirValueHome *source,
    const XrLirValueHome *target, XrLirHomeWorkspace workspace, XrLirHomeTransfer *transfer) {
    if (source->view_storage != XR_LIR_SLICE_STORAGE_BORROWED_FORMAL) return true;
    if (!source->parameter || source->view_workspace || source->owner_count ||
        source->offset != SIZE_MAX || source->first_owner != SIZE_MAX ||
        target->view_storage != XR_LIR_SLICE_STORAGE_LOCAL_KEEPER || !target->view_workspace ||
        transfer->transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_BORROW ||
        transfer->hold_transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_COPY)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"Slice formal edge lost its borrowed COPY hold");
    size_t leaf = 0;
    XrtValuePlanSpec spec = {.size=sizeof(XrtManagedHeader *),.alignment=_Alignof(XrtManagedHeader *),
        .owned_offsets=&leaf,.owned_count=1};
    workspace.kind = XR_LIR_HOME_WORKSPACE_VIEW_BORROW_STAGE;
    workspace.type = target->type; workspace.value = transfer->source;
    workspace.temporary_offset = workspace.temporary_first_owner = SIZE_MAX;
    if (!lir_home_reserve(b,&spec,&workspace.offset,&workspace.first_owner)) return false;
    transfer->borrowed_owner_workspace = b->workspaces.count;
    return lir_home_append(b,&b->workspaces,&workspace);
}
static bool lir_home_positions(LirHomeBuilder *b, const XrLirHomeEvent *const *selected, size_t selected_count) {
    LirHomeCompiler *c = &b->compiler;
    const XrLirValueHome *values = b->homes->values;
    size_t position = 0;
    for (uint32_t block = 0; block < c->view.block_count; ++block) {
        XrLirBlock id; XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, block, &id); xr_lir_block_view(c->input, id, &view);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView instruction; xr_lir_instruction_view(c->input, id, n, &instruction);
            if (instruction.op == XR_LIR_CALL) {
                XrLirHomeCall call = {.block=id.id,.instruction=n + 1,.callee=instruction.callee,
                    .result=instruction.result,.first=b->arguments.count,.count=instruction.operand_count,
                    .view_result_parameter=UINT32_MAX};
                if (instruction.result && values[instruction.result].view_storage) {
                    XrLirSliceFunctionFact callee;
                    if (xr_lir_slice_function_fact(c->input,instruction.callee,&callee) != XR_LIR_OK ||
                        !callee.available || callee.result_parameter >= instruction.operand_count ||
                        values[instruction.result].view_storage != XR_LIR_SLICE_STORAGE_LOCAL_KEEPER)
                        return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_ARGUMENT,"Slice call lost its receiving landing");
                    call.view_result_workspace = values[instruction.result].view_workspace;
                    call.view_result_parameter = callee.result_parameter;
                    call.view_result_source_argument = instruction.operands[callee.result_parameter];
                    if (values[call.view_result_source_argument].type != values[instruction.result].type)
                        return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_ARGUMENT,"Slice call lost its same-family input origin");
                }
                bool typed = instruction.result && values[instruction.result].type != 0;
                const XrLirFunctionData *target = &c->input->functions[instruction.callee - 1];
                for (uint32_t p = 0; p < instruction.operand_count; ++p) {
                    uint32_t value = instruction.operands[p];
                    typed |= target->parameter_types[p] == XR_LIR_I64_REF || values[value].type != 0 ||
                        (b->homes->bindings && b->homes->bindings[value].type != 0);
                }
                if (typed) {
                    for (uint32_t p = 0; p < instruction.operand_count; ++p)
                        if (!lir_home_append(b, &b->arguments, &instruction.operands[p])) return false;
                    if (!lir_home_append(b, &b->calls, &call)) return false;
                }
            }
            for (uint32_t e = 0; e < instruction.edge_count; ++e) {
                XrLirEdgeView edge = instruction.edges[e]; XrLirBlockView target;
                xr_lir_block_view(c->input, (XrLirBlock){c->input, c->function.id, edge.target}, &target);
                XrLirHomeEdge row = {.block=id.id,.edge=e + 1,.target_block=edge.target,.first=b->transfers.count};
                for (uint32_t p = 0; p < edge.argument_count; ++p) {
                    const XrLirHomeEvent *event = position < selected_count ? selected[position] : NULL;
                    bool matched = event && event->block == id.id && event->edge == e + 1 && event->argument == p;
                    if (matched) {
                        if (event->source != edge.arguments[p] || event->target != target.parameters[p] ||
                            event->target_block != edge.target || event->instruction != n + 1)
                            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "selected transfer lost its verified source or target");
                        ++position;
                    }
                    if (values[target.parameters[p]].type == 0) continue;
                    if (values[edge.arguments[p]].type != values[target.parameters[p]].type ||
                        values[target.parameters[p]].temporary_offset == SIZE_MAX)
                        return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "selected edge lost its exact receiving homes");
                    if (values[target.parameters[p]].owner_count && !matched)
                        return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "owned edge has no verified transfer event");
                    if (xr_lir_home_stationary_edge(b->homes, edge.arguments[p], target.parameters[p])) {
                        if (!matched || event->transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_MOVE)
                            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "stationary owner requires its original MOVE");
                        continue;
                    }
                    bool slice = values[target.parameters[p]].view_storage != XR_LIR_SLICE_STORAGE_NONE;
                    if (slice && (!matched || !values[edge.arguments[p]].view_storage ||
                        event->view_storage != values[edge.arguments[p]].view_storage ||
                        event->transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_BORROW ||
                        event->hold_transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_COPY))
                        return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Slice edge lost its verified loan keeper copy");
                    XrLirHomeTransfer transfer = {.source=edge.arguments[p],.target=target.parameters[p],
                        .transfer_mode=matched ? event->transfer_mode : XR_LIR_OWNERSHIP_TRANSFER_NONE,
                        .hold_transfer_mode=matched ? event->hold_transfer_mode : XR_LIR_OWNERSHIP_TRANSFER_NONE,
                        .source_workspace=slice ? values[edge.arguments[p]].view_workspace : 0,
                        .target_workspace=slice ? values[target.parameters[p]].view_workspace : 0,
                        .borrowed_owner_workspace=SIZE_MAX,
                        .source_storage=slice ? values[edge.arguments[p]].view_storage : XR_LIR_SLICE_STORAGE_NONE};
                    if (!lir_home_cell_stage(b,&values[transfer.source],&values[transfer.target],
                        (XrLirHomeWorkspace){.block=id.id,.instruction=n + 1},&transfer)) return false;
                    if (!lir_home_header_stage(b,&values[transfer.source],&values[transfer.target],
                        (XrLirHomeWorkspace){.block=id.id,.instruction=n + 1},&transfer)) return false;
                    if (slice && !lir_home_slice_stage(b,&values[transfer.source],&values[transfer.target],
                        (XrLirHomeWorkspace){.block=id.id,.instruction=n + 1},&transfer)) return false;
                    if (!lir_home_append(b, &b->transfers, &transfer)) return false;
                    ++row.count;
                }
                if (row.count && !lir_home_append(b, &b->edges, &row)) return false;
            }
        }
    }
    return position == selected_count || lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "selected transfer has no matching edge");
}
/* Callable and ordinary owner events are collected independently. Sort only
 * their existing positions once, then merge while projecting real LIR edges. */
static bool lir_home_edges(LirHomeBuilder *b) {
    size_t count = 0;
    const XrLirHomeEvent *events = b->events.data;
    for (size_t i = 0; i < b->events.count; ++i)
        count += events[i].kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER;
    if (!count) return lir_home_positions(b, NULL, 0);
    if (count > SIZE_MAX / sizeof(XrLirHomeEvent *))
        return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_OVERFLOW, "edge event index overflows");
    const XrLirHomeEvent **selected = xr_malloc(count * sizeof(*selected));
    if (!selected) return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_OOM, "edge event index allocation failed");
    size_t next = 0;
    for (size_t i = 0; i < b->events.count; ++i)
        if (events[i].kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) selected[next++] = &events[i];
    qsort(selected, count, sizeof(*selected), lir_home_edge_order);
    bool result = lir_home_positions(b, selected, count);
    xr_free(selected);
    return result;
}
static bool lir_home_function(LirHomeBuilder *b, const XrLirOwnershipPlan *ownership) {
    XrLirOwnershipFunction function;
    if (xr_lir_ownership_function_at(ownership, b->compiler.function.id, &function) != XR_LIR_OK)
        return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "function has no verified ownership inventory");
    b->homes->unmodeled = function.unmodeled;
    if (!lir_home_values(b,ownership) || !lir_home_bindings(b)) return false;
    for (uint32_t i = 0; i < function.fact_count; ++i) {
        XrLirOwnershipFact fact;
        if (xr_lir_ownership_fact_at(ownership, function.fact_offset + i, &fact) != XR_LIR_OK)
            return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "ownership event disappeared during projection");
        if ((uint32_t)fact.view_cleanup>XR_LIR_SLICE_CLEANUP_UNCONSTRUCTED ||
            (fact.view_cleanup && (fact.kind!=XR_LIR_OWNERSHIP_VIEW_END ||
             fact.view_operation!=XR_LIR_SLICE_OP_CLEANUP)))
            return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"invalid physical Slice cleanup fact");
        if (!lir_home_binding_fact(b,fact) || !lir_home_workspace(b,fact)) return false;
        if ((fact.forwarding_mode || fact.forwarding_drop) &&
            (fact.forwarding_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE ||
             (fact.kind!=XR_LIR_OWNERSHIP_BINDING_NEW && fact.kind!=XR_LIR_OWNERSHIP_BINDING_READ) ||
             (fact.type!=XR_LIR_MAP_STRING_I64_CELL &&
              (fact.type!=XR_LIR_ARRAY_CELL || fact.array_element!=XR_LIR_I64)) ||
             !fact.instruction || fact.instruction==UINT32_MAX || fact.forwarding_drop!=fact.instruction+1))
            return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"invalid local binding forwarding preparation");
        uint32_t view_workspace = 0;
        if (fact.kind == XR_LIR_OWNERSHIP_VIEW_VALUE || fact.kind == XR_LIR_OWNERSHIP_VIEW_END ||
            (fact.kind == XR_LIR_OWNERSHIP_RETURN_TRANSFER && fact.view_storage)) {
            if (!fact.value || fact.value > b->homes->value_count ||
                fact.view_storage != b->homes->values[fact.value].view_storage)
                return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Slice event lost its actual storage");
            view_workspace = b->homes->values[fact.value].view_workspace;
            bool formal = fact.view_storage == XR_LIR_SLICE_STORAGE_BORROWED_FORMAL;
            if ((formal && view_workspace) || (!formal && !view_workspace) ||
                (fact.view_operation == XR_LIR_SLICE_OP_FORMAL_CLEANUP && !formal) ||
                (fact.view_operation == XR_LIR_SLICE_OP_CLEANUP && formal))
                return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Slice cleanup confused a formal and a real keeper");
            if (fact.view_cleanup) {
                const XrLirHomeWorkspace *workspace=&((const XrLirHomeWorkspace *)b->workspaces.data)[view_workspace-1];
                if (formal || workspace->kind!=XR_LIR_HOME_WORKSPACE_VIEW_HOLD || workspace->value!=fact.value)
                    return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"cleanup proof lost its independent keeper");
            }
            if (fact.kind == XR_LIR_OWNERSHIP_VIEW_VALUE && view_workspace) {
                XrLirHomeWorkspace *workspace = &((XrLirHomeWorkspace *)b->workspaces.data)[view_workspace - 1];
                workspace->block = fact.block; workspace->instruction = fact.instruction;
            }
        }
        if (fact.kind == XR_LIR_OWNERSHIP_RESULT_OWNER || fact.kind == XR_LIR_OWNERSHIP_BLOCK_OWNER) {
            if (!fact.value || fact.value > b->homes->value_count)
                return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "owner event has no receiving value");
            XrLirValueHome *home = &((XrLirValueHome *)b->homes->values)[fact.value];
            if (home->owner_count && home->parameter)
                return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "borrowed parameter cannot become an implicit owner");
            if (home->owner_count && !lir_home_temporary(b, home)) return false;
        }
        XrLirHomeEvent row = {.kind=(uint32_t)fact.kind,.block=fact.block,.instruction=fact.instruction,.source=fact.value,
            .argument=fact.argument,.edge=fact.edge,.target_block=fact.target_block,.target=fact.target_value,
            .operation=fact.set_operation ? (uint32_t)fact.set_operation :
                fact.string_operation ? (uint32_t)fact.string_operation :
                fact.cell_transport ? (uint32_t)fact.cell_operation :
                fact.kind >= XR_LIR_OWNERSHIP_CALLABLE_VALUE ? (uint32_t)fact.callable_operation :
                fact.kind == XR_LIR_OWNERSHIP_MAP_OPERATION ? (uint32_t)fact.map_operation : (uint32_t)fact.array_operation,
            .callable_function=fact.callable_function,.callable_scope=fact.callable_scope,
            .callable_transport=fact.callable_transport,.callable_materialize=fact.callable_materialize,.transfer_mode=fact.transfer_mode,
            .view_operation=fact.view_operation,.view_workspace=view_workspace,.view_cleanup=fact.view_cleanup,
            .hold_transfer_mode=fact.hold_transfer_mode,
            .view_storage=fact.view_storage,.view_parameter=fact.view_parameter,
            .cell_operation=fact.cell_operation,.cell_transport=fact.cell_transport,.cell_scope=fact.cell_scope,
            .forwarding_mode=fact.forwarding_mode,.forwarding_drop=fact.forwarding_drop,
            .array_result=fact.array_result,.array_index=fact.array_index,.array_item=fact.array_item};
        if (fact.cell_transport) {
            if (!fact.value || fact.value > b->homes->value_count || !b->homes->values[fact.value].cell_payload_type)
                return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"Cell event lost its actual identity home");
            row.cell_type = b->homes->values[fact.value].type;
            row.cell_payload_type = b->homes->values[fact.value].cell_payload_type;
        }
        if (!lir_home_append(b, &b->events, &row)) return false;
    }
    for (uint32_t v = 1; b->homes->bindings && v <= b->homes->value_count; ++v) {
        const XrLirHomeBinding *row = &b->homes->bindings[v];
        if (row->type && row->kind == XR_LIR_HOME_BINDING_NONE)
            return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding has no verified source event");
    }
    return lir_home_edges(b);
}
/* Opaque Plan and this same-process receiver share the exact final module.
 * Copy the producer tables once; independent current-body proof is explicit. */
static bool lir_home_array_regions(LirHomeBuilder *b, const XrLirOwnershipPlan *plan, bool push) {
    XrLirOwnershipFunction function;
    if (xr_lir_ownership_function_at(plan, b->compiler.function.id, &function) != XR_LIR_OK)
        return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Array region function is unavailable");
    uint32_t count=push ? function.push_window_count : function.region_count;
    uint32_t offset=push ? function.push_window_offset : function.region_offset;
    uint32_t *received=push ? &b->homes->array_push_window_count : &b->homes->array_region_count;
    uint32_t *received_steps=push ? &b->homes->array_push_step_count : &b->homes->array_region_step_count;
    if (!count) return true;
    size_t capacity = 0, step_capacity = 0;
    XrLirArraySetRegion *regions = NULL; XrLirArraySetRegionStep *steps = NULL;
    if (!lir_home_grow(&b->compiler, (void **)&regions, &capacity, count, sizeof(*regions))) return false;
    if (push) b->homes->array_push_windows=regions; else b->homes->array_regions=regions;
    for (uint32_t n = 0; n < count; ++n) {
        XrLirArraySetRegion region;
        if (offset > UINT32_MAX - n ||
            (push ? xr_lir_ownership_array_push_window_at(plan,offset+n,&region) :
                xr_lir_ownership_array_region_at(plan,offset+n,&region)) != XR_LIR_OK ||
            region.function != b->compiler.function.id || !region.header || region.header > b->compiler.view.block_count ||
            !region.entry_value || !region.canonical_value || !region.exit_value ||
            region.entry_value > b->homes->value_count || region.canonical_value > b->homes->value_count ||
            region.exit_value > b->homes->value_count || region.step_count > UINT32_MAX - *received_steps)
            return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Array region lost its receiving representation");
        uint32_t first = *received_steps;
        bool grown = lir_home_grow(&b->compiler, (void **)&steps, &step_capacity,
            (size_t)first + region.step_count, sizeof(*steps));
        if (push) b->homes->array_push_steps=steps; else b->homes->array_region_steps=steps;
        if (!grown) return false;
        for (uint32_t s = 0; s < region.step_count; ++s) {
            XrLirArraySetRegionStep step;
            if (region.first_step > UINT32_MAX - s ||
                (push ? xr_lir_ownership_array_push_step_at(plan,region.first_step+s,&step) :
                    xr_lir_ownership_array_region_step_at(plan,region.first_step+s,&step)) != XR_LIR_OK ||
                !step.source || step.source > b->homes->value_count || !step.target || step.target > b->homes->value_count)
                return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Array region step lost its receiving representation");
            steps[first + s] = step;
        }
        region.first_step = first; regions[n] = region;
        ++*received; *received_steps += region.step_count;
    }
    return true;
}

static bool lir_home_direct_header_move(const LirHomeBuilder *b, const XrLirHomeEdge *edge) {
    const XrLirValueHomes *homes = b->homes;
    if (edge->count != 1 || !homes->owned_frame_needed) return false;
    XrLirHomeTransfer transfer = homes->transfers[edge->first];
    if (transfer.transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_MOVE || transfer.hold_transfer_mode ||
        transfer.source_workspace || transfer.target_workspace || transfer.source_storage ||
        transfer.borrowed_owner_workspace != SIZE_MAX) return false;
    const XrLirValueHome *from = &homes->values[transfer.source], *to = &homes->values[transfer.target];
    if (!from->type || from->type != to->type || from->parameter || to->parameter ||
        from->borrow_source || to->borrow_source || from->transport || to->transport ||
        from->cell_transport || to->cell_transport || from->view_workspace || to->view_workspace ||
        from->view_storage || to->view_storage || from->owner_count != 1 || to->owner_count != 1 ||
        from->byte_count || to->byte_count || from->size != sizeof(XrtManagedHeader *) || to->size != from->size)
        return false;
    XrLirValueLayoutKey key;
    const XrtValuePlan *plan = NULL;
    XrtValuePlanSpec spec;
    if (xr_lir_value_layouts_at(b->layouts, from->type - 1, &key, &plan) != XR_LIR_VALUE_LAYOUT_OK ||
        xrt_value_plan_view(plan, &spec) != XRT_MANAGED_OK || spec.kind < XRT_VALUE_STRING ||
        spec.kind > XRT_VALUE_MAP_STRING_I64 || spec.owned_count != 1 || !spec.owned_offsets || spec.owned_offsets[0])
        return false;
    size_t slots[] = {from->first_owner, to->first_owner, to->temporary_first_owner};
    size_t offsets[] = {from->offset, to->offset, to->temporary_offset};
    for (size_t n = 0; n < 3; ++n) {
        if (slots[n] >= homes->frame.slot_count || offsets[n] > homes->frame.payload_size ||
            sizeof(XrtManagedHeader *) > homes->frame.payload_size - offsets[n] ||
            offsets[n] % _Alignof(XrtManagedHeader *) || homes->frame.owned_offsets[slots[n]] != offsets[n]) return false;
        for (size_t p = 0; p < n; ++p) if (slots[n] == slots[p] || offsets[n] == offsets[p]) return false;
    }
    return true;
}

#include "xlir_value_string_windows.inc.c"

XR_FUNCDEF XrLirValueLayoutStatus xr_lir_value_homes_create(const XrLirValueHomesRequest *request,
    XrLirValueHomes **output) {
    if (!request || !request->module || request->function.module != request->module || !request->layouts ||
        !request->ownership || !output || *output ||
        xr_lir_ownership_function_count(request->ownership) != xr_lir_function_count(request->module))
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    LirHomeBuilder b = {.compiler={.input=request->module, .function=request->function}, .layouts=request->layouts,
        .owners={.stride=sizeof(size_t)}, .bytes={.stride=sizeof(XrLirHomeBytes)},
        .events={.stride=sizeof(XrLirHomeEvent)}, .edges={.stride=sizeof(XrLirHomeEdge)},
        .transfers={.stride=sizeof(XrLirHomeTransfer)}, .calls={.stride=sizeof(XrLirHomeCall)},
        .arguments={.stride=sizeof(uint32_t)}, .workspaces={.stride=sizeof(XrLirHomeWorkspace)}};
    if (xr_lir_function_view(request->module, request->function, &b.compiler.view) != XR_LIR_OK)
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    b.homes = xr_calloc(1, sizeof(*b.homes));
    if (!b.homes) return XR_LIR_VALUE_LAYOUT_OOM;
    b.homes->frame.payload_alignment = 1;
    b.homes->view_result_parameter = UINT32_MAX;
    b.homes->source_function = request->function.id;
    b.homes->value_count = b.compiler.view.value_count;
    bool success = lir_home_array_regions(&b, request->ownership,false) &&
        lir_home_array_regions(&b, request->ownership,true) && lir_home_function(&b, request->ownership);
    xr_free(b.stationary_cells);
    b.stationary_cells = NULL;
    b.homes->frame.slot_count = b.owners.count; b.homes->frame.owned_offsets = b.owners.data;
    b.homes->owned_frame_needed = b.owners.count != 0;
    b.homes->bytes = b.bytes.data; b.homes->byte_count = b.bytes.count;
    b.homes->events = b.events.data; b.homes->event_count = b.events.count;
    b.homes->edges = b.edges.data; b.homes->edge_count = b.edges.count;
    b.homes->transfers = b.transfers.data; b.homes->transfer_count = b.transfers.count;
    b.homes->calls = b.calls.data; b.homes->call_count = b.calls.count;
    b.homes->arguments = b.arguments.data; b.homes->argument_count = b.arguments.count;
    b.homes->workspaces = b.workspaces.data; b.homes->workspace_count = b.workspaces.count;
    if (!success) { xr_lir_value_homes_destroy(b.homes); return b.compiler.status; }
    XrLirHomeEdge *edges = b.edges.data;
    for (size_t n = 0; n < b.edges.count; ++n)
        edges[n].direct_header_move = lir_home_direct_header_move(&b, &edges[n]);
    if (!lir_home_string_windows(&b)) { xr_lir_value_homes_destroy(b.homes); return b.compiler.status; }
    bool typed = b.homes->event_count || b.homes->unmodeled || b.homes->frame.payload_size ||
        b.homes->bindings || b.homes->call_count;
    for (size_t v = 1; !typed && v <= b.homes->value_count; ++v) typed = b.homes->values[v].type != 0;
    if (typed) *output = b.homes;
    else xr_lir_value_homes_destroy(b.homes);
    return XR_LIR_VALUE_LAYOUT_OK;
}
