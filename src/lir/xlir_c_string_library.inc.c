/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_string_library.inc.c - Owned append through common stable String homes
 */
static bool c_string_owner_slot(LirC *c, const XrLirValueHome *home, bool temporary) {
    const XrtManagedFrameSpec *frame = &c->homes->frame;
    size_t slot = temporary ? home->temporary_first_owner : home->first_owner;
    size_t offset = temporary ? home->temporary_offset : home->offset;
    if (home->parameter || !frame->owned_offsets || slot >= frame->slot_count ||
        frame->payload_alignment < _Alignof(XrtManagedHeader *) ||
        offset % _Alignof(XrtManagedHeader *) || offset > frame->payload_size ||
        sizeof(XrtManagedHeader *) > frame->payload_size - offset || frame->owned_offsets[slot] != offset) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String append lost its exact common owner slot");
        return false;
    }
    return true;
}
static const XrLirValueHome *c_string_home(LirC *c, uint32_t value, bool borrowed) {
    if (!value || value > c->homes->value_count || c_value_type(c, value) != XR_LIR_STRING) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String append has no common String home");
        return NULL;
    }
    const XrLirValueHome *home = &c->homes->values[value];
    XrLirValueLayoutKey key;
    const XrtValuePlan *plan = NULL;
    XrtValuePlanSpec spec;
    if (!home->type || xr_lir_value_layouts_at(c->value_layouts, home->type - 1, &key, &plan) != XR_LIR_VALUE_LAYOUT_OK ||
        key.type != XR_LIR_STRING || key.array_element || key.environment_function ||
        xrt_value_plan_view(plan, &spec) != XRT_MANAGED_OK || spec.kind != XRT_VALUE_STRING ||
        spec.size != sizeof(XrtManagedHeader *) || spec.owned_count != 1 || !spec.owned_offsets ||
        spec.owned_offsets[0] || home->size != spec.size || home->owner_count != 1) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String append home disagrees with its sealed physical plan");
        return NULL;
    }
    if (home->parameter) {
        if (!borrowed || home->parameter > c->view.parameter_count ||
            c->view.parameters[home->parameter - 1] != value ||
            home->offset != SIZE_MAX || home->first_owner != SIZE_MAX) {
            c_error(c, XR_LIR_C_INVALID_LIR, "String append cannot consume a borrowed formal");
            return NULL;
        }
    } else if (!c_string_owner_slot(c, home, false)) return NULL;
    return home;
}
static bool c_string_append_events(LirC *c, const XrLirInstructionView *v) {
    unsigned matched = 0;
    for (size_t i = 0; i < c->homes->event_count && c_request_poll(c); ++i) {
        const XrLirHomeEvent *event = &c->homes->events[i];
        if (event->block != c->block || event->instruction != c->instruction) continue;
        if (event->kind == XR_LIR_OWNERSHIP_APPEND_TRANSFER && event->source == v->operands[0] &&
            !event->argument && event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE) matched |= 1;
        if (event->kind == XR_LIR_OWNERSHIP_OPERAND_BORROW && event->source == v->operands[1] &&
            event->argument == 1 && event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_BORROW) matched |= 2;
        if (event->kind == XR_LIR_OWNERSHIP_RESULT_OWNER && event->source == v->result &&
            !event->argument && event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_NONE) matched |= 4;
    }
    if (!c_request_poll(c)) return false;
    if (matched != 7) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String append has no exact move, borrow and result ownership events");
        return false;
    }
    return true;
}
static bool c_string_slot(LirC *c, size_t slot, size_t offset) {
    const XrtManagedFrameSpec *frame = &c->homes->frame;
    if (!frame->owned_offsets || slot >= frame->slot_count ||
        frame->payload_alignment < _Alignof(XrtManagedHeader *) || offset % _Alignof(XrtManagedHeader *) ||
        offset > frame->payload_size || sizeof(XrtManagedHeader *) > frame->payload_size - offset ||
        frame->owned_offsets[slot] != offset) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String binding lost its exact common owner slot"); return false;
    }
    return true;
}
static const XrLirHomeBinding *c_string_binding_home(LirC *c, uint32_t value) {
    if (c->status != XR_LIR_C_OK) return NULL;
    if (!c->homes || !c->homes->bindings || !c->homes->values || !value || value > c->homes->value_count ||
        !xr_lir_string_binding(c->request->module, (XrLirValue){c->request->module,c->function.id,value})) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String binding has no common receiving row"); return NULL;
    }
    const XrLirHomeBinding *binding = &c->homes->bindings[value];
    if (c->homes->values[value].type || c->homes->values[value].owner_count) {
        c_error(c,XR_LIR_C_INVALID_LIR,"String binding cannot use an owning SSA value carrier"); return NULL;
    }
    XrLirValueLayoutKey key; const XrtValuePlan *plan = NULL; XrtValuePlanSpec spec;
    if (!binding->type || xr_lir_value_layouts_at(c->value_layouts,binding->type - 1,&key,&plan) != XR_LIR_VALUE_LAYOUT_OK ||
        key.type != XR_LIR_STRING || key.array_element || key.environment_function ||
        xrt_value_plan_view(plan,&spec) != XRT_MANAGED_OK || spec.kind != XRT_VALUE_STRING ||
        spec.size != sizeof(XrtManagedHeader *) || spec.alignment != _Alignof(XrtManagedHeader *) ||
        spec.owned_count != 1 || !spec.owned_offsets || spec.owned_offsets[0]) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String binding lost its sealed payload plan"); return NULL;
    }
    if (binding->kind == XR_LIR_HOME_BINDING_LOCAL) {
        if (binding->parameter) {
            c_error(c,XR_LIR_C_INVALID_LIR,"local String binding cannot name a borrowed formal"); return NULL;
        }
        if (!c_string_slot(c,binding->first_owner,binding->offset)) return NULL;
    } else if (binding->offset != SIZE_MAX || binding->first_owner != SIZE_MAX) {
        c_error(c, XR_LIR_C_INVALID_LIR, "borrowed String binding cannot own a local slot"); return NULL;
    } else if (binding->kind == XR_LIR_HOME_BINDING_PARAMETER) {
        if (!binding->parameter || binding->parameter > c->view.parameter_count ||
            c->view.parameters[binding->parameter - 1] != value) {
            c_error(c, XR_LIR_C_INVALID_LIR, "String binding formal lost its actual parameter"); return NULL;
        }
    } else if (binding->kind != XR_LIR_HOME_BINDING_CELL_BORROW && binding->kind != XR_LIR_HOME_BINDING_STATE_BORROW) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String binding has no actual storage source"); return NULL;
    }
    return binding;
}
static const XrLirHomeEvent *c_string_binding_event(LirC *c, XrLirOwnershipFactKind kind,
    uint32_t binding, uint32_t target, uint32_t operation) {
    if (!c->homes) { c_error(c,XR_LIR_C_INVALID_LIR,"String operation lost its common events"); return NULL; }
    while (c->string_event_cursor < c->homes->event_count && c_request_poll(c)) {
        const XrLirHomeEvent *event = &c->homes->events[c->string_event_cursor++];
        bool ordinary = event->kind >= XR_LIR_OWNERSHIP_BINDING_NEW && event->kind <= XR_LIR_OWNERSHIP_BINDING_WRITE;
        if ((!ordinary && event->kind != XR_LIR_OWNERSHIP_STRING_OPERATION) || !event->source ||
            !xr_lir_string_binding(c->request->module,
                (XrLirValue){c->request->module,c->function.id,event->source})) continue;
        if (event->block == c->block && event->instruction == c->instruction && event->kind == (uint32_t)kind &&
            event->source == binding && event->target == target && event->operation == operation) return event;
        break;
    }
    c_error(c,XR_LIR_C_INVALID_LIR,"String operation lost its exact common producer event"); return NULL;
}
static const XrLirHomeWorkspace *c_string_binding_workspace(LirC *c, uint32_t value, uint32_t type) {
    while (c->string_workspace_cursor < c->homes->workspace_count && c_request_poll(c)) {
        const XrLirHomeWorkspace *workspace = &c->homes->workspaces[c->string_workspace_cursor++];
        if (workspace->kind != XR_LIR_HOME_WORKSPACE_VALUE || workspace->type != type) continue;
        if (workspace->block == c->block && workspace->instruction == c->instruction && workspace->value == value &&
            workspace->temporary_offset == SIZE_MAX && workspace->temporary_first_owner == SIZE_MAX &&
            c_string_slot(c,workspace->first_owner,workspace->offset)) return workspace;
        break;
    }
    c_error(c,XR_LIR_C_INVALID_LIR,"String operation lost its exact common pending workspace"); return NULL;
}
static void c_string_binding_cache(LirC *c, uint32_t value, uint32_t source) {
    const XrLirHomeBinding *binding = c_string_binding_home(c,value);
    if (!binding || binding->kind != XR_LIR_HOME_BINDING_STATE_BORROW) return;
    if (!binding->source || binding->source > c->homes->value_count ||
        c_value_type(c,binding->source) != XR_LIR_STATE_REF) {
        c_error(c,XR_LIR_C_INVALID_LIR,"String State binding lost its actual reference"); return;
    }
    c_text(c,"    { XrtManagedStatus status = xrt_module_external_owner("); c_value(c,binding->source);
    c_format(c,", 0, &xrl_array_binding%" PRIu32 ");\n",value); c_array_require(c,source); c_text(c,"    }\n");
}
static void c_string_binding_place(LirC *c, uint32_t value) {
    if (c_value_type(c,value) == XR_LIR_STATE_REF)
        c_format(c,"(XrtPlace){.external=&xrl_array_binding%" PRIu32 ",.nulls=XRT_PLACE_NONNULL}",value);
    else c_value(c,value);
}
static void c_string_binding_copy(LirC *c, uint32_t binding_value, uint32_t string_value,
    bool read, uint32_t source) {
    const XrLirHomeBinding *binding = c_string_binding_home(c,binding_value);
    if (!binding) return;
    const XrLirValueHome *home = c_string_home(c,string_value,!read);
    if (!home) return;
    if (home->type != binding->type) {
        c_error(c,XR_LIR_C_INVALID_LIR,"String binding copy lost its exact source type"); return;
    }
    const XrLirHomeWorkspace *stage = !read && home->parameter ?
        c_string_binding_workspace(c,binding_value,binding->type) : NULL;
    if (c->status != XR_LIR_C_OK) return;
    c_values_ensure(c,source);
    c_string_binding_cache(c,binding_value,source);
    if (stage) {
        c_text(c,"    { XrtManagedHeader *owner = "); c_value(c,string_value); c_text(c,";\n");
        c_format(c,"      if (!xrt_managed_retain(owner)) xrt_native_managed_failure(XRT_MANAGED_REFCOUNT, UINT32_C(%" PRIu32 "));\n",source);
        c_format(c,"      XrtManagedStatus status = xrt_managed_slot_replace(xrl_frame, %zu, &owner);\n",stage->first_owner);
        c_text(c,"      if (status != XRT_MANAGED_OK) { xrt_managed_drop(owner);\n"); c_array_require(c,source); c_text(c,"      } }\n");
    }
    c_text(c,stage ? "    { XrtManagedStatus status = xrt_place_move_replace(" :
        "    { XrtManagedStatus status = xrt_place_copy_replace(");
    if (read) c_format(c,"(XrtPlace){.frame=xrl_frame,.slot=%zu,.nulls=XRT_PLACE_NULLABLE}",home->first_owner);
    else c_string_binding_place(c,binding_value);
    c_text(c,", ");
    if (read) c_string_binding_place(c,binding_value);
    else c_format(c,"(XrtPlace){.frame=xrl_frame,.slot=%zu,.nulls=XRT_PLACE_NONNULL}",stage ? stage->first_owner : home->first_owner);
    c_text(c,", NULL);\n"); c_array_require(c,source); c_text(c,"    }\n");
}
static void c_string_binding_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    uint32_t role = xr_lir_string_role(c->request->module,v);
    bool borrow = role == XR_LIR_STRING_OP_BORROW_CELL || role == XR_LIR_STRING_OP_BORROW_STATE;
    bool append = role == XR_LIR_STRING_OP_APPEND_CELL || role == XR_LIR_STRING_OP_APPEND_STATE || role == XR_LIR_STRING_OP_APPEND_REF;
    bool read = v->op == XR_LIR_CELL_READ || v->op == XR_LIR_STATE_READ || role == XR_LIR_STRING_OP_READ_REF;
    bool release = v->op == XR_LIR_STRING_CELL_DROP;
    uint32_t binding_value = borrow || v->op == XR_LIR_CELL_NEW ? v->result : v->operands[0];
    uint32_t target = borrow || v->op == XR_LIR_CELL_NEW ? v->operands[0] : read ? v->result : release ? 0 : v->operands[1];
    XrLirOwnershipFactKind kind = borrow ? XR_LIR_OWNERSHIP_BINDING_BORROW : append ? XR_LIR_OWNERSHIP_STRING_OPERATION :
        v->op == XR_LIR_CELL_NEW ? XR_LIR_OWNERSHIP_BINDING_NEW : read ? XR_LIR_OWNERSHIP_BINDING_READ :
        release ? XR_LIR_OWNERSHIP_BINDING_RELEASE : XR_LIR_OWNERSHIP_BINDING_WRITE;
    const XrLirHomeEvent *event = c_string_binding_event(c,kind,binding_value,target,role);
    const XrLirHomeBinding *binding = c_string_binding_home(c,binding_value);
    if (!event || !binding) return;
    if (borrow) {
        if (binding->source != target) { c_error(c,XR_LIR_C_INVALID_LIR,"String borrow lost its actual binding"); return; }
        c_string_binding_cache(c,binding_value,source);
        c_format(c,"    v%" PRIu32 " = ",v->result);
        if (binding->kind == XR_LIR_HOME_BINDING_STATE_BORROW)
            c_format(c,"(XrtPlace){.external=&xrl_array_binding%" PRIu32 ",.nulls=XRT_PLACE_NONNULL}",v->result);
        else c_value(c,target);
        c_text(c,";\n"); return;
    }
    if (release) {
        if (binding->kind != XR_LIR_HOME_BINDING_LOCAL) { c_error(c,XR_LIR_C_INVALID_LIR,"String release requires its real local binding"); return; }
        c_format(c,"    { XrtManagedStatus status = xrt_managed_slot_drop(xrl_frame, %zu);\n",binding->first_owner);
        c_array_require(c,source); c_text(c,"    }\n"); return;
    }
    if (!append) { c_string_binding_copy(c,binding_value,target,read,source); return; }
    const XrLirValueHome *right = c_string_home(c,target,true);
    const XrLirHomeWorkspace *workspace = c_string_binding_workspace(c,binding_value,binding->type);
    if (!right || !workspace || right->type != binding->type || event->transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_BORROW) {
        c_error(c,XR_LIR_C_INVALID_LIR,"String append lost its nonconsuming binding and suffix"); return;
    }
    c_values_ensure(c,source);
    c_string_binding_cache(c,binding_value,source);
    c_text(c,"    xrt_native_string_append_place("); c_string_binding_place(c,binding_value);
    c_format(c,", xrl_frame, %zu, (const XrtStringBacking *)",workspace->first_owner); c_value(c,target);
    c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
}
static void c_string_library(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (xr_lir_string_role(c->request->module, v) != XR_LIR_STRING_OP_APPEND_OWNED ||
        v->operand_count != 2 || v->type != XR_LIR_STRING || !c->homes || !c->homes->values ||
        !c->homes->owned_frame_needed || c->homes->source_function != c->function.id) {
        c_error(c, XR_LIR_C_UNSUPPORTED, "String operation has no owned target execution binding");
        return;
    }
    const XrLirValueHome *left = c_string_home(c, v->operands[0], false);
    const XrLirValueHome *right = c_string_home(c, v->operands[1], true);
    const XrLirValueHome *result = c_string_home(c, v->result, false);
    if (!left || !right || !result) return;
    if (left->type != result->type || right->type != result->type ||
        !c_string_owner_slot(c, result, true) || left->first_owner == result->first_owner ||
        left->first_owner == result->temporary_first_owner || result->first_owner == result->temporary_first_owner) {
        c_error(c, XR_LIR_C_INVALID_LIR, "String append requires three distinct common owner slots");
        return;
    }
    if (!c_string_append_events(c, v)) return;
    if (right->literal_cache_borrow && !c_string_slot(c, right->literal_cache_owner, right->literal_cache_offset))
        return;
    /* Frame setup precedes the suffix borrow. The runtime consumes the left
     * owner and publishes the complete result under one registered guard. */
    c_values_ensure(c, source);
    c_format(c, "    xrt_native_string_append_owned(xrl_frame, &(const XrtStringAppendOwned){.source_slot=%zu,\n",
        left->first_owner);
    c_format(c, "        .result_slot=%zu, .temporary_slot=%zu, .suffix=(const XrtStringBacking *)",
        result->first_owner, result->temporary_first_owner);
    c_value(c, v->operands[1]);
    c_format(c, "}, UINT32_C(%" PRIu32 "));\n", source);
}

static void c_string_window(LirC *c, const XrLirStringWindow *window) {
    uint32_t index = window->index;
    const size_t *empty_slots = c->homes->string_window_slots + window->header_empty_first;
    c_format(c, "    if (xrl_frame && v%" PRIu32 " >= INT64_C(0) && v%" PRIu32 " < ", index,index);
    c_value(c, window->limit);
    c_text(c, ") {\n        static const size_t xrl_string_empty[] = {");
    for (size_t n = 0; n < window->empty_count; ++n)
        c_format(c, "%s%zu", n ? ", " : "", empty_slots[n]);
    c_format(c, "};\n        v%" PRIu32 " += (int64_t)xrt_native_string_append_window(xrl_frame, %zu, %zu, (size_t)(",
        index,window->header_owner,window->suffix_owner);
    c_value(c, window->limit);
    c_format(c, " - v%" PRIu32 "), xrl_string_empty, %zu);\n    }\n", index,window->empty_count);
}
