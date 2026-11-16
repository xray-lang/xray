/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_homes.c - Shared cold home placement from sealed physical facts
 */
#include "xlir_value_homes.h"
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
} LirHomeBuilder;

XR_FUNCDEF void xr_lir_value_homes_destroy(XrLirValueHomes *homes) {
    if (!homes) return;
    xr_free((void *)homes->values); xr_free((void *)homes->events);
    xr_free((void *)homes->bytes); xr_free((void *)homes->transfers);
    xr_free((void *)homes->edges); xr_free((void *)homes->calls);
    xr_free((void *)homes->arguments); xr_free((void *)homes->frame.owned_offsets);
    xr_free((void *)homes->bindings); xr_free((void *)homes->workspaces);
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
    if (!plain && type != XR_LIR_STRING && type != XR_LIR_ARRAY && type != XR_LIR_MAP_STRING_I64) return false;
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
    if (home->parameter) return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_UNSUPPORTED, "Slice formal has no caller loan hook");
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
    home->offset = workspace.offset + offsetof(XrtArraySliceBundle, value);
    home->temporary_offset = workspace.temporary_offset + offsetof(XrtArraySliceBundle, value);
    home->first_owner = home->temporary_first_owner = SIZE_MAX;
    return true;
}
static bool lir_home_binding_type(XrLirType type) {
    return type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF ||
        type == XR_LIR_MAP_STRING_I64_CELL || type == XR_LIR_MAP_STRING_I64_REF;
}
static bool lir_home_binding_spec(LirHomeBuilder *b, uint32_t type, XrtValuePlanSpec *spec) {
    const XrtValuePlan *plan = NULL; XrLirValueLayoutKey key;
    if (!type || xr_lir_value_layouts_at(b->layouts,type - 1,&key,&plan) != XR_LIR_VALUE_LAYOUT_OK ||
        xrt_value_plan_view(plan,spec) != XRT_MANAGED_OK || spec->size != sizeof(XrtManagedHeader *) ||
        spec->owned_count != 1 || !spec->owned_offsets || spec->owned_offsets[0] ||
        spec->kind < XRT_VALUE_ARRAY_I64 || spec->kind > XRT_VALUE_MAP_STRING_I64)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding lost its exact Header payload plan");
    return true;
}
static bool lir_home_bindings(LirHomeBuilder *b) {
    LirHomeCompiler *c = &b->compiler; bool needed = false;
    for (uint32_t v = 1; v <= c->view.value_count && !needed; ++v) {
        XrLirType type; xr_lir_value_type(c->input,(XrLirValue){c->input,c->function.id,v},&type);
        needed = lir_home_binding_type(type);
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
        if (lir_home_binding_type(c->view.parameter_types[p])) {
            XrLirHomeBinding *binding = &bindings[c->view.parameters[p]];
            binding->kind = XR_LIR_HOME_BINDING_PARAMETER; binding->parameter = p + 1;
        }
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        XrLirType type; xr_lir_value_type(c->input,(XrLirValue){c->input,c->function.id,v},&type);
        if (!lir_home_binding_type(type)) continue;
        XrLirValueLayoutKey key = {XR_LIR_MAP_STRING_I64,0,0};
        if (type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF) {
            XrLirArrayShape shape;
            if (xr_lir_array_shape_view(c->input,(XrLirValue){c->input,c->function.id,v},&shape) != XR_LIR_OK)
                return lir_home_fail(c,XR_LIR_VALUE_LAYOUT_ARGUMENT,"binding lost its exact Array element");
            key = (XrLirValueLayoutKey){XR_LIR_ARRAY,shape.element,0};
        }
        XrtValuePlanSpec spec; uint32_t ordinal;
        if (!lir_home_spec(b,key,&spec,&ordinal)) return false;
        bindings[v].type = ordinal + 1;
        if (!lir_home_binding_spec(b,bindings[v].type,&spec)) return false;
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
        if (!lir_home_binding_spec(b,row->type,&spec) || !lir_home_reserve(b,&spec,&row->offset,&row->first_owner)) return false;
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
        else if ((type == XR_LIR_ARRAY_CELL || type == XR_LIR_MAP_STRING_I64_CELL) && bindings[fact.target_value].type == row->type)
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
    if (!array && !map) return true;
    if (!fact.value || fact.value > b->homes->value_count || !b->homes->bindings || !b->homes->bindings[fact.value].type)
        return lir_home_fail(&b->compiler,XR_LIR_VALUE_LAYOUT_ARGUMENT,"mutator has no actual binding payload");
    XrLirHomeWorkspace workspace = {.block=fact.block,.instruction=fact.instruction,
        .type=b->homes->bindings[fact.value].type};
    XrtValuePlanSpec spec;
    if (!lir_home_binding_spec(b,workspace.type,&spec) ||
        !lir_home_reserve(b,&spec,&workspace.offset,&workspace.first_owner)) return false;
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
static bool lir_home_values(LirHomeBuilder *b, const XrLirOwnershipPlan *ownership) {
    LirHomeCompiler *c = &b->compiler;
    size_t count = (size_t)c->view.value_count + 1;
    if (count > SIZE_MAX / sizeof(XrLirValueHome)) return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OVERFLOW, "activation home directory overflows");
    XrLirValueHome *values = xr_malloc(count * sizeof(*values));
    if (!values) return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OOM, "cannot own activation home directory");
    b->homes->values = values;
    for (size_t v = 0; v < count; ++v) values[v] = (XrLirValueHome){.type=0,
        .offset=SIZE_MAX, .first_owner=SIZE_MAX, .temporary_offset=SIZE_MAX, .temporary_first_owner=SIZE_MAX,
        .literal_cache_offset=SIZE_MAX, .literal_cache_owner=SIZE_MAX};
    if (!lir_home_callable_rows(b,ownership,values)) return false;
    for (size_t index = 1; index <= c->view.value_count; ++index) {
        uint32_t v = (uint32_t)index;
        XrLirType type; XrLirValueLayoutKey key;
        if (xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, v}, &type) != XR_LIR_OK)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "activation value is unreadable");
        if (!lir_home_key(c->input, c->function, v, type, &key)) continue;
        XrtValuePlanSpec spec; uint32_t ordinal;
        if (!lir_home_spec(b, key, &spec, &ordinal)) return false;
        if (!spec.owned_count && spec.kind != XRT_VALUE_RECORD && spec.kind != XRT_VALUE_NULLABLE &&
            spec.kind != XRT_VALUE_FIXED_U8 && spec.kind != XRT_VALUE_TUPLE_I64 &&
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
        else if (!home->parameter && !lir_home_reserve(b, &spec, &home->offset, &home->first_owner)) return false;
        if (borrowed) home->first_owner = SIZE_MAX;
        if (!lir_home_byte_spans(b, home, &spec)) return false;
    }
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
        }
    }
    XrLirValueLayoutKey key; XrtValuePlanSpec result;
    if (lir_home_key(c->input, c->function, 0, c->view.result, &key)) {
        uint32_t ordinal;
        if (!lir_home_spec(b, key, &result, &ordinal)) return false;
        b->homes->result_type = ordinal + 1;
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
                XrLirHomeCall call = {id.id, n + 1, instruction.callee,
                    instruction.result, b->arguments.count, instruction.operand_count};
                bool typed = instruction.result && values[instruction.result].type != 0;
                for (uint32_t p = 0; p < instruction.operand_count; ++p) {
                    uint32_t value = instruction.operands[p];
                    typed |= values[value].type != 0 || (b->homes->bindings && b->homes->bindings[value].type != 0);
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
                XrLirHomeEdge row = {id.id, e + 1, edge.target, b->transfers.count, 0};
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
                    bool slice = values[target.parameters[p]].view_workspace != 0;
                    if (slice && (!matched || !values[edge.arguments[p]].view_workspace ||
                        event->transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_BORROW ||
                        event->hold_transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_COPY))
                        return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Slice edge lost its verified loan keeper copy");
                    XrLirHomeTransfer transfer = {.source=edge.arguments[p],.target=target.parameters[p],
                        .transfer_mode=matched ? event->transfer_mode : XR_LIR_OWNERSHIP_TRANSFER_NONE,
                        .hold_transfer_mode=matched ? event->hold_transfer_mode : XR_LIR_OWNERSHIP_TRANSFER_NONE,
                        .source_workspace=slice ? values[edge.arguments[p]].view_workspace : 0,
                        .target_workspace=slice ? values[target.parameters[p]].view_workspace : 0,
                        .borrowed_owner_workspace=SIZE_MAX};
                    if (!lir_home_cell_stage(b,&values[transfer.source],&values[transfer.target],
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
    b->homes->source_function = b->compiler.function.id;
    b->homes->value_count = b->compiler.view.value_count;
    b->homes->unmodeled = function.unmodeled;
    if (!lir_home_values(b,ownership) || !lir_home_bindings(b)) return false;
    for (uint32_t i = 0; i < function.fact_count; ++i) {
        XrLirOwnershipFact fact;
        if (xr_lir_ownership_fact_at(ownership, function.fact_offset + i, &fact) != XR_LIR_OK)
            return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "ownership event disappeared during projection");
        if (!lir_home_binding_fact(b,fact) || !lir_home_workspace(b,fact)) return false;
        uint32_t view_workspace = 0;
        if (fact.kind == XR_LIR_OWNERSHIP_VIEW_VALUE || fact.kind == XR_LIR_OWNERSHIP_VIEW_END) {
            if (!fact.value || fact.value > b->homes->value_count ||
                !(view_workspace = b->homes->values[fact.value].view_workspace))
                return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "Slice event has no real keeper workspace");
            if (fact.kind == XR_LIR_OWNERSHIP_VIEW_VALUE) {
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
            .operation=fact.cell_transport ? (uint32_t)fact.cell_operation :
                fact.kind >= XR_LIR_OWNERSHIP_CALLABLE_VALUE ? (uint32_t)fact.callable_operation :
                fact.kind == XR_LIR_OWNERSHIP_MAP_OPERATION ? (uint32_t)fact.map_operation : (uint32_t)fact.array_operation,
            .callable_function=fact.callable_function,.callable_scope=fact.callable_scope,
            .callable_transport=fact.callable_transport,.callable_materialize=fact.callable_materialize,.transfer_mode=fact.transfer_mode,
            .view_operation=fact.view_operation,.view_workspace=view_workspace,.hold_transfer_mode=fact.hold_transfer_mode,
            .cell_operation=fact.cell_operation,.cell_transport=fact.cell_transport,.cell_scope=fact.cell_scope};
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
    bool success = lir_home_function(&b, request->ownership);
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
    bool typed = b.homes->event_count || b.homes->unmodeled || b.homes->frame.payload_size;
    for (size_t v = 1; !typed && v <= b.homes->value_count; ++v) typed = b.homes->values[v].type != 0;
    if (typed) *output = b.homes;
    else xr_lir_value_homes_destroy(b.homes);
    return XR_LIR_VALUE_LAYOUT_OK;
}
