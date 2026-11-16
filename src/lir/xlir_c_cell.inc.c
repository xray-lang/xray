/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_cell.inc.c - Consume verified identity Cells through native guards
 */
static const XrLirValueHome *c_cell_home(LirC *c, uint32_t value) {
    if (!c->homes || !value || value > c->homes->value_count || !c->cell_events || !c->cell_events[value]) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Cell value lost its unique common identity event"); return NULL;
    }
    const XrLirValueHome *home = &c->homes->values[value];
    const XrLirHomeEvent *event = c->cell_events[value];
    if (!home->type || home->type > c->value_types->count || event->cell_type != home->type ||
        !home->cell_payload_type || home->cell_payload_type > c->value_types->count ||
        event->cell_payload_type != home->cell_payload_type || event->cell_transport != home->cell_transport) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Cell identity lost its exact receiving type or transport"); return NULL;
    }
    const XrtValuePlanSpec *spec = &c->value_types->rows[home->type - 1].spec;
    const LirCValueTypeRow *payload = &c->value_types->rows[home->cell_payload_type - 1];
    if (spec->kind != XRT_VALUE_CELL || spec->element != payload->plan || spec->size != sizeof(void *) ||
        spec->owned_count != 1 || spec->owned_offsets[0] ||
        (payload->spec.kind != XRT_VALUE_I64 && payload->spec.kind != XRT_VALUE_MAP_STRING_I64)) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Cell identity lost its sealed payload plan"); return NULL;
    }
    bool borrowed = home->cell_transport == XR_LIR_CELL_TRANSPORT_BORROW;
    if (borrowed) {
        if (!home->parameter || home->owner_count || home->offset != SIZE_MAX || home->first_owner != SIZE_MAX ||
            home->temporary_offset != SIZE_MAX || home->temporary_first_owner != SIZE_MAX || home->byte_count != 1 ||
            c->homes->bytes[home->first_bytes].offset || c->homes->bytes[home->first_bytes].size != spec->size) {
            c_error(c,XR_LIR_C_INVALID_LIR,"borrowed Cell formal must preserve its complete unowned identity"); return NULL;
        }
    } else if (home->cell_transport != XR_LIR_CELL_TRANSPORT_OWNED || home->parameter || home->owner_count != 1 ||
        home->first_owner >= c->homes->frame.slot_count || home->temporary_first_owner >= c->homes->frame.slot_count ||
        home->first_owner == home->temporary_first_owner || c->homes->frame.owned_offsets[home->first_owner] != home->offset ||
        c->homes->frame.owned_offsets[home->temporary_first_owner] != home->temporary_offset) {
        c_error(c,XR_LIR_C_INVALID_LIR,"owning Cell requires distinct real identity and pending homes"); return NULL;
    }
    return home;
}
static void c_cell_events_prepare(LirC *c) {
    xr_compile_resources_free(c->cell_events); c->cell_events = NULL;
    c->cell_operation_cursor = c->cell_scope_cursor = 0;
    if (!c->homes || !c->cells) return;
    bool needed = false;
    for (size_t n = 0; n < c->homes->event_count; ++n) needed |= c->homes->events[n].kind == XR_LIR_OWNERSHIP_CELL_VALUE;
    if (!needed) return;
    void *memory = NULL;
    if (!c_resource(c,xr_compile_resources_calloc(c->request->resources,(size_t)c->view.value_count + 1,
        sizeof(*c->cell_events),&memory))) return;
    c->cell_events = memory;
    for (size_t n = 0; n < c->homes->event_count && c_request_poll(c); ++n) {
        const XrLirHomeEvent *event = &c->homes->events[n];
        if (event->kind != XR_LIR_OWNERSHIP_CELL_VALUE) continue;
        if (!event->source || event->source > c->homes->value_count || c->cell_events[event->source]) {
            c_error(c,XR_LIR_C_INVALID_LIR,"Cell has no unique final identity event"); return;
        }
        ((const XrLirHomeEvent **)c->cell_events)[event->source] = event;
    }
    for (uint32_t v = 0; v < c->homes->value_count && c_request_poll(c); ++v)
        if (c->cell_events[v + 1] && !c_cell_home(c,v + 1)) return;
}
static const XrLirHomeEvent *c_cell_operation(LirC *c, const XrLirInstructionView *v) {
    while (c->cell_operation_cursor < c->homes->event_count) {
        const XrLirHomeEvent *event = &c->homes->events[c->cell_operation_cursor++];
        if (event->kind != XR_LIR_OWNERSHIP_CELL_OPERATION) continue;
        if (event->block != c->block || event->instruction != c->instruction) break;
        uint32_t cell = v->op == XR_LIR_CELL_NEW ? v->result : v->operands[0];
        uint32_t target = v->op == XR_LIR_CELL_NEW ? v->operands[0] : v->op == XR_LIR_CELL_WRITE ? v->operands[1] : v->result;
        XrLirCellOperation operation = v->op == XR_LIR_CELL_NEW ? XR_LIR_CELL_OP_NEW : v->op == XR_LIR_CELL_READ ?
            XR_LIR_CELL_OP_READ : v->op == XR_LIR_CELL_WRITE ? XR_LIR_CELL_OP_WRITE : XR_LIR_CELL_OP_BORROW_MAP;
        if (event->cell_operation == operation && event->source == cell && event->target == target && c_cell_home(c,cell)) return event;
        break;
    }
    c_error(c,XR_LIR_C_INVALID_LIR,"Cell operation lost its exact producer position"); return NULL;
}
static void c_cell_scope_release(LirC *c, uint32_t source) {
    if (!c->cell_events) return;
    while (c->cell_scope_cursor < c->homes->event_count) {
        const XrLirHomeEvent *event = &c->homes->events[c->cell_scope_cursor++];
        if (event->kind != XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT || event->block != c->block || event->instruction != c->instruction) continue;
        while (c->cell_scope_cursor < c->homes->event_count) {
            const XrLirHomeEvent *release = &c->homes->events[c->cell_scope_cursor];
            if (release->kind != XR_LIR_OWNERSHIP_RELEASE || release->block != event->block || release->instruction != event->instruction) break;
            const XrLirValueHome *home = c_cell_home(c,release->source);
            if (!home || home->cell_transport != XR_LIR_CELL_TRANSPORT_OWNED || release->cell_scope != event->cell_scope) {
                c_error(c,XR_LIR_C_INVALID_LIR,"Cell scope release lost its real owner"); return;
            }
            c_format(c,"    if (xrl_frame) { XrtManagedStatus status = xrt_managed_slot_drop(xrl_frame, %zu);\n",home->first_owner);
            c_array_require(c,source); c_text(c,"    }\n"); ++c->cell_scope_cursor;
        }
        return;
    }
    c_error(c,XR_LIR_C_INVALID_LIR,"Cell scope exit lost its final release position");
}
static void c_cell_alias(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    const XrLirValueHome *target = c_cell_home(c,v->result);
    uint32_t value = v->operands[v->operand_count - 1];
    const XrLirValueHome *from = c_cell_home(c,value);
    if (!target || !from) return;
    const XrLirHomeEvent *event = c->cell_events[v->result];
    XrLirCellOperation operation = v->op == XR_LIR_FUNCTION_BIND ? XR_LIR_CELL_OP_BIND :
        v->op == XR_LIR_FUNCTION_SCOPE_LEAVE ? XR_LIR_CELL_OP_KEEP : XR_LIR_CELL_OP_COPY;
    if (event->cell_operation != operation || event->target != value || from->type != target->type ||
        target->cell_transport != XR_LIR_CELL_TRANSPORT_OWNED) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Cell alias lost its actual source identity"); return;
    }
    if (event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY) {
        c_values_replace_begin(c,v->result,source); c_text(c,"&"); c_value(c,value); c_values_replace_end(c,source);
    } else if (event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE && from->cell_transport == XR_LIR_CELL_TRANSPORT_OWNED) {
        c_values_ensure(c,source);
        c_format(c,"    { XrtManagedStatus status = xrt_place_move_replace((XrtPlace){.frame=xrl_frame,.slot=%zu,.nulls=XRT_PLACE_NULLABLE},\n",target->first_owner);
        c_format(c,"        (XrtPlace){.frame=xrl_frame,.slot=%zu,.nulls=XRT_PLACE_NONNULL},NULL);\n",from->first_owner);
        c_array_require(c,source); c_text(c,"    }\n");
    } else c_error(c,XR_LIR_C_INVALID_LIR,"Cell alias has no verified COPY or MOVE");
}
static void c_cell_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (v->op == XR_LIR_FUNCTION_BIND || v->op == XR_LIR_FUNCTION_SCOPE_LEAVE || v->op == XR_LIR_VALUE_RETAIN) {
        c_cell_alias(c,v,source);
        if (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE) { c_callable_scope_release(c); c_cell_scope_release(c,source); }
        return;
    }
    if (v->op == XR_LIR_VALUE_DROP) {
        const XrLirValueHome *home = c_cell_home(c,v->operands[0]);
        if (!home || home->cell_transport != XR_LIR_CELL_TRANSPORT_OWNED) {
            c_error(c,XR_LIR_C_INVALID_LIR,"borrowed Cell cannot be explicitly dropped"); return;
        }
        c_format(c,"    { XrtManagedStatus status = xrt_managed_slot_drop(xrl_frame, %zu);\n",home->first_owner);
        c_array_require(c,source); c_text(c,"    }\n"); return;
    }
    const XrLirHomeEvent *event = c_cell_operation(c,v);
    if (!event) return;
    const XrLirValueHome *cell = c_cell_home(c,event->source);
    if (!cell) return;
    XrtValueKind payload = c->value_types->rows[cell->cell_payload_type - 1].spec.kind;
    if (v->op == XR_LIR_CELL_NEW) {
        c_values_ensure(c,source);
        c_text(c,"    { XrtNativeCellInput input = {.image=&xrl_value_image, .state=");
        if (c->view.parameter_count && c->view.parameter_types[c->view.parameter_count - 1] == XR_LIR_STATE)
            c_format(c,"p%" PRIu32,c->view.parameter_count - 1);
        else c_text(c,"NULL");
        c_format(c,", .type=UINT32_C(%" PRIu32 "), .initial.%s=",cell->type,payload == XRT_VALUE_I64 ? "i64" : "map");
        c_value(c,event->target); c_text(c,"};\n");
        c_format(c,"      xrt_native_cell_create(xrl_frame, %zu, &input, UINT32_C(%" PRIu32 "));\n    }\n",cell->temporary_first_owner,source);
        c_values_publish_leaf(c,v->result,source);
    } else if (v->op == XR_LIR_CELL_READ && payload == XRT_VALUE_I64) {
        c_format(c,"    v%" PRIu32 " = xrt_native_cell_i64_read(",v->result); c_value(c,event->source);
        c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
    } else if (v->op == XR_LIR_CELL_READ) {
        c_values_ensure(c,source);
        c_format(c,"    xrt_native_cell_map_read_copy(xrl_frame, %zu, ",c->homes->values[v->result].temporary_first_owner);
        c_value(c,event->source); c_format(c,", UINT32_C(%" PRIu32 "));\n",source); c_values_publish_leaf(c,v->result,source);
    } else if (v->op == XR_LIR_CELL_WRITE) {
        c_text(c,payload == XRT_VALUE_I64 ? "    xrt_native_cell_i64_write(" : "    xrt_native_cell_map_write(");
        c_value(c,event->source); c_text(c,", "); c_value(c,event->target); c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
    } else if (v->op == XR_LIR_MAP_BORROW && payload == XRT_VALUE_MAP_STRING_I64) {
        const XrLirHomeBinding *binding = &c->homes->bindings[v->result];
        if (binding->kind != XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW || binding->source != event->source || binding->type != cell->cell_payload_type) {
            c_error(c,XR_LIR_C_INVALID_LIR,"Map reference lost its actual Cell payload source"); return;
        }
        c_format(c,"    xrl_array_binding%" PRIu32 " = xrt_native_cell_map_binding(",v->result); c_value(c,event->source);
        c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
        c_format(c,"    v%" PRIu32 " = (XrtPlace){.external=&xrl_array_binding%" PRIu32 ",.nulls=XRT_PLACE_NONNULL};\n",v->result,v->result);
    } else c_error(c,XR_LIR_C_INVALID_LIR,"Cell operation disagrees with its admitted payload");
}
