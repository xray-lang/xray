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
#include <string.h>

typedef struct LirHomeVector { void *data; size_t count, capacity, stride; } LirHomeVector;
typedef struct LirHomeCompiler {
    const XrLirModule *input; XrLirFunction function; XrLirFunctionView view; XrLirValueLayoutStatus status;
} LirHomeCompiler;
typedef struct LirHomeBuilder {
    LirHomeCompiler compiler;
    const XrLirValueLayouts *layouts;
    XrLirValueHomes *homes;
    LirHomeVector owners, bytes, events, edges, transfers, calls, arguments;
} LirHomeBuilder;

XR_FUNCDEF void xr_lir_value_homes_destroy(XrLirValueHomes *homes) {
    if (!homes) return;
    xr_free((void *)homes->values); xr_free((void *)homes->events);
    xr_free((void *)homes->bytes); xr_free((void *)homes->transfers);
    xr_free((void *)homes->edges); xr_free((void *)homes->calls);
    xr_free((void *)homes->arguments); xr_free((void *)homes->frame.owned_offsets);
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
            shape.kind == XR_LIR_TYPE_FIXED_ARRAY || shape.kind == XR_LIR_TYPE_TUPLE;
    }
    if (!plain && type != XR_LIR_STRING && type != XR_LIR_ARRAY && type != XR_LIR_MAP_STRING_I64) return false;
    *key = (XrLirValueLayoutKey){type, 0};
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
    return lir_home_reserve(b, &spec, &home->temporary_offset, &home->temporary_first_owner);
}
static bool lir_home_values(LirHomeBuilder *b) {
    LirHomeCompiler *c = &b->compiler;
    size_t count = (size_t)c->view.value_count + 1;
    if (count > SIZE_MAX / sizeof(XrLirValueHome)) return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OVERFLOW, "activation home directory overflows");
    XrLirValueHome *values = xr_malloc(count * sizeof(*values));
    if (!values) return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_OOM, "cannot own activation home directory");
    b->homes->values = values;
    for (size_t v = 0; v < count; ++v) values[v] = (XrLirValueHome){.type=0,
        .offset=SIZE_MAX, .first_owner=SIZE_MAX, .temporary_offset=SIZE_MAX, .temporary_first_owner=SIZE_MAX};
    for (size_t index = 1; index <= c->view.value_count; ++index) {
        uint32_t v = (uint32_t)index;
        XrLirType type; XrLirValueLayoutKey key;
        if (xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, v}, &type) != XR_LIR_OK)
            return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "activation value is unreadable");
        if (!lir_home_key(c->input, c->function, v, type, &key)) continue;
        XrtValuePlanSpec spec; uint32_t ordinal;
        if (!lir_home_spec(b, key, &spec, &ordinal)) return false;
        if (!spec.owned_count && spec.kind != XRT_VALUE_RECORD && spec.kind != XRT_VALUE_NULLABLE &&
            spec.kind != XRT_VALUE_FIXED_U8 && spec.kind != XRT_VALUE_TUPLE_I64) continue;
        XrLirValueHome *home = &values[v];
        home->type = ordinal + 1; home->size = spec.size; home->owner_count = spec.owned_count;
        for (uint32_t p = 0; p < c->view.parameter_count; ++p)
            if (c->view.parameters[p] == v) home->parameter = p + 1;
        if (!home->parameter && !lir_home_reserve(b, &spec, &home->offset, &home->first_owner)) return false;
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
    }
    XrLirValueLayoutKey key; XrtValuePlanSpec result;
    if (lir_home_key(c->input, c->function, 0, c->view.result, &key)) {
        uint32_t ordinal;
        if (!lir_home_spec(b, key, &result, &ordinal)) return false;
        b->homes->result_type = ordinal + 1;
    }
    return true;
}
static bool lir_home_positions(LirHomeBuilder *b) {
    LirHomeCompiler *c = &b->compiler;
    const XrLirValueHome *values = b->homes->values;
    for (uint32_t block = 0; block < c->view.block_count; ++block) {
        XrLirBlock id; XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, block, &id); xr_lir_block_view(c->input, id, &view);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView instruction; xr_lir_instruction_view(c->input, id, n, &instruction);
            if (instruction.op == XR_LIR_CALL) {
                XrLirHomeCall call = {id.id, n + 1, instruction.callee,
                    instruction.result, b->arguments.count, instruction.operand_count};
                bool typed = instruction.result && values[instruction.result].type != 0;
                for (uint32_t p = 0; p < instruction.operand_count; ++p)
                    typed |= values[instruction.operands[p]].type != 0;
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
                    if (values[target.parameters[p]].type == 0) continue;
                    if (values[edge.arguments[p]].type != values[target.parameters[p]].type ||
                        values[target.parameters[p]].temporary_offset == SIZE_MAX)
                        return lir_home_fail(c, XR_LIR_VALUE_LAYOUT_ARGUMENT, "selected edge lost its exact receiving homes");
                    if (!lir_home_append(b, &b->transfers, &(XrLirHomeTransfer){edge.arguments[p], target.parameters[p]})) return false;
                    ++row.count;
                }
                if (row.count && !lir_home_append(b, &b->edges, &row)) return false;
            }
        }
    }
    return true;
}
static bool lir_home_function(LirHomeBuilder *b, const XrLirOwnershipPlan *ownership) {
    XrLirOwnershipFunction function;
    if (xr_lir_ownership_function_at(ownership, b->compiler.function.id, &function) != XR_LIR_OK)
        return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "function has no verified ownership inventory");
    b->homes->source_function = b->compiler.function.id;
    b->homes->value_count = b->compiler.view.value_count;
    b->homes->unmodeled = function.unmodeled;
    if (!lir_home_values(b)) return false;
    for (uint32_t i = 0; i < function.fact_count; ++i) {
        XrLirOwnershipFact fact;
        if (xr_lir_ownership_fact_at(ownership, function.fact_offset + i, &fact) != XR_LIR_OK)
            return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "ownership event disappeared during projection");
        if (fact.kind == XR_LIR_OWNERSHIP_RESULT_OWNER || fact.kind == XR_LIR_OWNERSHIP_BLOCK_OWNER) {
            if (!fact.value || fact.value > b->homes->value_count)
                return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "owner event has no receiving value");
            XrLirValueHome *home = &((XrLirValueHome *)b->homes->values)[fact.value];
            if (home->owner_count && home->parameter)
                return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_ARGUMENT, "borrowed parameter cannot become an implicit owner");
            if (home->owner_count && !lir_home_temporary(b, home)) return false;
        }
        XrLirHomeEvent row = {(uint32_t)fact.kind, fact.block, fact.instruction, fact.value,
            fact.argument, fact.edge, fact.target_block, fact.target_value, (uint32_t)fact.array_operation};
        if (!lir_home_append(b, &b->events, &row)) return false;
    }
    return lir_home_positions(b);
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
        .arguments={.stride=sizeof(uint32_t)}};
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
    if (!success) { xr_lir_value_homes_destroy(b.homes); return b.compiler.status; }
    bool typed = b.homes->event_count || b.homes->unmodeled || b.homes->frame.payload_size;
    for (size_t v = 1; !typed && v <= b.homes->value_count; ++v) typed = b.homes->values[v].type != 0;
    if (typed) *output = b.homes;
    else xr_lir_value_homes_destroy(b.homes);
    return XR_LIR_VALUE_LAYOUT_OK;
}
