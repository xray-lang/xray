/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_map.inc.c - Fixed String/I64 Map receivers over common owner homes
 *
 * KEY CONCEPT:
 *   Actual bindings supply Places; snapshots and results use stable frame
 *   owners. GET transports its scalar presence through the ordinary POD type.
 */
static void c_path(LirC *, const XrLirSource *);
static bool c_map_literal_view(LirC *c, uint64_t ordinal, XrLirMapLiteral *literal) {
    if (!ordinal || ordinal > UINT32_MAX || ordinal > xr_lir_map_literal_count(c->request->module) ||
        xr_lir_map_literal_view(c->request->module, (uint32_t)ordinal, literal) != XR_LIR_OK ||
        (literal->path_length != 0) != (literal->path != NULL) ||
        (literal->count != 0) != (literal->items != NULL) || literal->count > 65536) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Map literal lost its exact module provenance ordinal"); return false;
    }
    return true;
}
static bool c_map_literal_admit(LirC *c, const XrLirInstructionView *v) {
    XrLirMapLiteral literal = {0};
    if (v->type != XR_LIR_MAP_STRING_I64 || !v->result || (v->operand_count && !v->operands) ||
        !c_map_literal_view(c, v->bits, &literal)) {
        if (c->status == XR_LIR_C_OK) c_error(c, XR_LIR_C_INVALID_LIR, "Map literal requires its fixed Map result");
        return false;
    }
    uint32_t operand = 0;
    for (uint32_t n = 0; n < literal.count && c_request_poll(c); ++n) {
        XrLirMapLiteralOrigin origin = literal.items[n];
        uint32_t width = origin.kind ? 1u : 2u;
        XrLirType key_type = origin.kind ? XR_LIR_MAP_STRING_I64 : XR_LIR_STRING;
        if (origin.kind > 1 || !origin.line || !origin.column || operand > v->operand_count ||
            width > v->operand_count - operand ||
            c_value_type(c, v->operands[operand]) != key_type ||
            (!origin.kind && c_value_type(c, v->operands[operand + 1]) != XR_LIR_I64)) {
            c_error(c, XR_LIR_C_INVALID_LIR, "Map literal inputs differ from their fixed provenance rows"); return false;
        }
        operand += width;
    }
    if (!c_request_poll(c)) return false;
    if (operand != v->operand_count) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Map literal operand span has unmatched values"); return false;
    }
    if (!c->used_map_literals) {
        void *used = NULL;
        if (!c_resource(c, xr_compile_resources_calloc(c->request->resources,
            xr_lir_map_literal_count(c->request->module), 1, &used))) return false;
        c->used_map_literals = used;
    }
    c->used_map_literals[(uint32_t)v->bits - 1] = 1;
    return true;
}
static void c_map_literals(LirC *c) {
    if (!c->used_map_literals) return;
    uint32_t count = xr_lir_map_literal_count(c->request->module);
    for (uint32_t index = 0; index < count && c_request_poll(c); ++index) {
        if (!c->used_map_literals[index]) continue;
        uint32_t ordinal = index + 1;
        XrLirMapLiteral literal = {0};
        if (!c_map_literal_view(c, ordinal, &literal)) return;
        if (literal.count) {
            c_format(c, "static const XrtMapLiteralOrigin xrl_map_items_%" PRIu32 "[] = {", ordinal);
            for (uint32_t n = 0; n < literal.count && c_request_poll(c); ++n) {
                XrLirMapLiteralOrigin origin = literal.items[n];
                c_format(c, "%s{UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 ")}",
                    n ? ", " : "", origin.kind, origin.line, origin.column);
            }
            c_text(c, "};\n");
        }
        c_format(c, "static const XrtMapLiteral xrl_map_literal_%" PRIu32 " = {", ordinal);
        if (literal.path_length) c_path(c, &(XrLirSource){.path=literal.path, .path_length=literal.path_length});
        else c_text(c, "NULL");
        c_format(c, ", (size_t)UINT32_C(%" PRIu32 "), ", literal.path_length);
        if (literal.count) c_format(c, "xrl_map_items_%" PRIu32, ordinal); else c_text(c, "NULL");
        c_format(c, ", (size_t)UINT32_C(%" PRIu32 ")};\n", literal.count);
    }
}
static uint32_t c_map_role(LirC *c, const XrLirInstructionView *v) {
    XrLirIntrinsic entry;
    return v->op == XR_LIR_CALL_INTRINSIC &&
        xr_lir_intrinsic_view(c->request->module, v->intrinsic, &entry) == XR_LIR_OK &&
        xr_lir_map_intrinsic_valid(c->request->module, &entry) ? entry.operation : 0;
}
static bool c_map_point(LirC *c, const XrLirInstructionView *v) {
    return c_map_role(c, v) || v->op == XR_LIR_MAP_LITERAL_NEW || v->op == XR_LIR_MAP_RETAIN || v->op == XR_LIR_MAP_BORROW ||
        v->op == XR_LIR_MAP_REF_READ || v->op == XR_LIR_MAP_REF_WRITE ||
        (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_MAP_STRING_I64_CELL) ||
        (v->op == XR_LIR_CELL_READ && v->type == XR_LIR_MAP_STRING_I64) ||
        (v->op == XR_LIR_CELL_WRITE && c_value_type(c, v->operands[0]) == XR_LIR_MAP_STRING_I64_CELL);
}
static void c_map_call(LirC *c, const XrLirInstructionView *v, uint32_t role, uint32_t source) {
    if (role == XR_LIR_MAP_OP_NEW) {
        c_values_ensure(c, source);
        c_format(c, "    xrt_native_map_create(xrl_frame, %zu, &(XrtMapInput){", c->homes->values[v->result].temporary_first_owner);
        for (uint32_t field = 0; field < 2; ++field) {
            if (!v->operand_count) c_text(c, "NULL");
            else {
                c_text(c, field ? "(const int64_t[]){" : "(XrtStringBacking *const[]){");
                for (uint32_t n = field; n < v->operand_count; n += 2) {
                    if (n > field) c_text(c, ", ");
                    if (!field) c_text(c, "(XrtStringBacking *)");
                    c_value(c, v->operands[n]);
                }
                c_text(c, "}");
            }
            c_text(c, ", ");
        }
        c_format(c, "%" PRIu32 "}, UINT32_C(%" PRIu32 "));\n", v->operand_count / 2, source);
        c_values_publish_leaf(c, v->result, source);
        return;
    }
    if (role == XR_LIR_MAP_OP_LEN) {
        c_format(c, "    v%" PRIu32 " = (int64_t)((const XrtMapBacking *)", v->result);
        c_value(c, v->operands[0]); c_text(c, ")->length;\n"); return;
    }
    if (role == XR_LIR_MAP_OP_DISPLAY || xr_lir_map_snapshot_element(role)) {
        c_values_ensure(c, source);
        c_format(c, "    xrt_native_map_project(xrl_frame, %zu, ",
            c->homes->values[v->result].temporary_first_owner);
        c_value(c, v->operands[0]);
        c_format(c, ", %s, UINT32_C(%" PRIu32 "));\n",
            role == XR_LIR_MAP_OP_DISPLAY ? "XRT_MAP_DISPLAY" :
            role == XR_LIR_MAP_OP_KEYS ? "XRT_MAP_KEYS" : "XRT_MAP_VALUES", source);
        c_values_publish_leaf(c, v->result, source);
        return;
    }
    if (role == XR_LIR_MAP_OP_GET || role == XR_LIR_MAP_OP_INDEX || role == XR_LIR_MAP_OP_CONTAINS) {
        if (role == XR_LIR_MAP_OP_GET)
            c_format(c, "    v%" PRIu32 " = (%s){0};\n    v%" PRIu32 ".tag = ", v->result, c_type(c, v->type), v->result);
        else if (role == XR_LIR_MAP_OP_CONTAINS)
            c_format(c, "    { int64_t ignored;\n    v%" PRIu32 " = ", v->result);
        else c_format(c, "    v%" PRIu32 " = ", v->result);
        c_text(c, role == XR_LIR_MAP_OP_INDEX ? "xrt_native_map_index(" : "xrt_native_map_get(");
        c_value(c, v->operands[0]); c_text(c, ", (const XrtStringBacking *)"); c_value(c, v->operands[1]);
        if (role == XR_LIR_MAP_OP_GET) c_format(c, ", &v%" PRIu32 ".payload", v->result);
        else if (role == XR_LIR_MAP_OP_CONTAINS) c_text(c, ", &ignored");
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        if (role == XR_LIR_MAP_OP_CONTAINS) c_text(c, "    }\n");
        return;
    }
    const XrLirHomeWorkspace *workspace = NULL;
    for (size_t i = 0; i < c->homes->workspace_count; ++i)
        if (c->homes->workspaces[i].block == c->block && c->homes->workspaces[i].instruction == c->instruction)
            workspace = &c->homes->workspaces[i];
    const XrLirHomeBinding *binding = &c->homes->bindings[v->operands[0]];
    if (!workspace || !binding->type || workspace->type != binding->type) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Map mutation lost its common binding workspace"); return;
    }
    c_values_ensure(c, source);
    c_text(c, "    { bool removed = false;\n    ");
    if (binding->kind == XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW) {
        c_text(c,"xrt_native_cell_map_update("); c_value(c,binding->source);
    } else { c_text(c,"xrt_native_map_update_place("); c_value(c,v->operands[0]); }
    c_format(c, ", xrl_frame, %zu, &(XrtMapUpdate){%s, ", workspace->first_owner,
        role == XR_LIR_MAP_OP_SET ? "XRT_MAP_UPDATE_SET" : role == XR_LIR_MAP_OP_REMOVE ? "XRT_MAP_UPDATE_REMOVE" : "XRT_MAP_UPDATE_CLEAR");
    if (role == XR_LIR_MAP_OP_CLEAR) c_text(c, "NULL");
    else { c_text(c, "(const XrtStringBacking *)"); c_value(c, v->operands[1]); }
    c_text(c, ", ");
    if (role == XR_LIR_MAP_OP_SET) c_value(c, v->operands[2]); else c_text(c, "0");
    c_format(c, "}, &removed, UINT32_C(%" PRIu32 "));\n", source);
    if (v->type != XR_LIR_UNIT) c_format(c, "    v%" PRIu32 " = removed;\n", v->result);
    c_text(c, "    }\n");
}
static void c_map_binding_stage_copy(LirC *c,uint32_t receiver,uint32_t input,uint32_t source) {
    const XrLirHomeBinding *binding=&c->homes->bindings[receiver];
    const XrLirValueHome *home=&c->homes->values[input];
    const XrtValueImage *image=xr_lir_value_image_view(c->value_image);
    const XrLirHomeEvent *event=NULL; const XrLirHomeWorkspace *stage=NULL;
    if (!image || !binding->type || binding->type>image->type_count ||
        image->types[binding->type-1].kind!=XRT_VALUE_MAP_STRING_I64 || home->type!=binding->type ||
        !home->parameter || home->offset!=SIZE_MAX || home->first_owner!=SIZE_MAX) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Map binding copy lost its genuine borrowed formal"); return;
    }
    while (c->map_stage_event_cursor<c->homes->event_count && c_request_poll(c)) {
        const XrLirHomeEvent *candidate=&c->homes->events[c->map_stage_event_cursor++];
        if ((candidate->kind!=XR_LIR_OWNERSHIP_BINDING_NEW && candidate->kind!=XR_LIR_OWNERSHIP_BINDING_WRITE) ||
            !candidate->target || !c->homes->values[candidate->target].parameter ||
            c_value_type(c,candidate->target)!=XR_LIR_MAP_STRING_I64) continue;
        if (candidate->block==c->block && candidate->instruction==c->instruction &&
            candidate->source==receiver && candidate->target==input && !candidate->operation &&
            candidate->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE) event=candidate;
        break;
    }
    while (event && c->map_stage_workspace_cursor<c->homes->workspace_count && c_request_poll(c)) {
        const XrLirHomeWorkspace *candidate=&c->homes->workspaces[c->map_stage_workspace_cursor++];
        if (candidate->kind!=XR_LIR_HOME_WORKSPACE_VALUE || !candidate->value || !candidate->type ||
            candidate->type>image->type_count || image->types[candidate->type-1].kind!=XRT_VALUE_MAP_STRING_I64) continue;
        if (candidate->block==c->block && candidate->instruction==c->instruction &&
            candidate->value==receiver && candidate->type==binding->type) stage=candidate;
        break;
    }
    const XrtManagedFrameSpec *frame=&c->homes->frame;
    if (!stage || !c->homes->owned_frame_needed || !frame->owned_offsets ||
        stage->temporary_offset!=SIZE_MAX || stage->temporary_first_owner!=SIZE_MAX ||
        stage->first_owner>=frame->slot_count || stage->offset%_Alignof(XrtManagedHeader *) ||
        stage->offset>frame->payload_size || sizeof(XrtManagedHeader *)>frame->payload_size-stage->offset ||
        frame->owned_offsets[stage->first_owner]!=stage->offset || stage->first_owner==binding->first_owner) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Map binding copy lost its exact common pending workspace"); return;
    }
    c_values_ensure(c,source);
    if (c_value_type(c,receiver)==XR_LIR_STATE_REF) {
        c_text(c,"    { XrtManagedStatus status = xrt_module_external_owner("); c_value(c,receiver);
        c_format(c,", 0, &xrl_array_binding%" PRIu32 ");\n",receiver); c_array_require(c,source); c_text(c,"    }\n");
    }
    c_text(c,"    { XrtManagedHeader *owner = "); c_value(c,input); c_text(c,";\n");
    c_format(c,"      if (!xrt_managed_retain(owner)) xrt_native_managed_failure(XRT_MANAGED_REFCOUNT, UINT32_C(%" PRIu32 "));\n",source);
    c_format(c,"      XrtManagedStatus status = xrt_managed_slot_adopt(xrl_frame, %zu, &owner);\n",stage->first_owner);
    c_text(c,"      if (status != XRT_MANAGED_OK) { xrt_managed_drop(owner);\n"); c_array_require(c,source); c_text(c,"      }\n");
    c_text(c,"      status = xrt_place_move_replace(");
    if (c_value_type(c,receiver)==XR_LIR_STATE_REF)
        c_format(c,"(XrtPlace){.external=&xrl_array_binding%" PRIu32 ",.nulls=XRT_PLACE_NONNULL}",receiver);
    else c_value(c,receiver);
    c_format(c,", (XrtPlace){.frame=xrl_frame,.slot=%zu,.nulls=XRT_PLACE_NONNULL}, NULL);\n",stage->first_owner);
    c_format(c,"      XrtManagedStatus cleanup = xrt_managed_slot_drop(xrl_frame, %zu);\n",stage->first_owner);
    c_text(c,"      if (status == XRT_MANAGED_OK) status = cleanup;\n"); c_array_require(c,source); c_text(c,"    }\n");
}
static void c_map_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (c->status != XR_LIR_C_OK) return;
    if (v->op == XR_LIR_MAP_LITERAL_NEW) {
        XrLirMapLiteral literal = {0};
        if (!c_map_literal_view(c, v->bits, &literal)) return;
        const XrLirValueHome *home = &c->homes->values[v->result];
        const XrtValuePlan *plan = NULL;
        XrtValuePlanSpec shape = {0};
        uint32_t type_id = 0;
        bool typed = xr_lir_value_layouts_lookup(c->value_layouts,
            (XrLirValueLayoutKey){XR_LIR_MAP_STRING_I64,0,0},&plan,&type_id) == XR_LIR_VALUE_LAYOUT_OK &&
            xrt_value_plan_view(plan,&shape) == XRT_MANAGED_OK && shape.kind == XRT_VALUE_MAP_STRING_I64;
        const XrtManagedFrameSpec *frame = &c->homes->frame;
        if (!typed || !home->type || type_id != home->type ||
            home->parameter || home->owner_count != 1 || home->size != sizeof(XrtManagedHeader *) ||
            !frame->owned_offsets || home->first_owner >= frame->slot_count ||
            home->temporary_first_owner >= frame->slot_count || home->first_owner == home->temporary_first_owner ||
            frame->owned_offsets[home->first_owner] != home->offset ||
            frame->owned_offsets[home->temporary_first_owner] != home->temporary_offset) {
            c_error(c, XR_LIR_C_INVALID_LIR, "Map literal lost its distinct common receiving temporary"); return;
        }
        c_values_ensure(c, source);
        c_format(c, "    xrt_native_map_literal_create(xrl_frame, %zu, &xrl_map_literal_%" PRIu64 ", ",
            home->temporary_first_owner, v->bits);
        if (!literal.count) c_text(c, "NULL");
        else {
            c_text(c, "(const XrtMapLiteralInput[]){"); uint32_t operand = 0;
            for (uint32_t n = 0; n < literal.count && c_request_poll(c); ++n) {
                bool spread = literal.items[n].kind != 0;
                c_format(c, "%s{.source={.%s=(const %s *)", n ? ", " : "", spread ? "spread" : "key",
                    spread ? "XrtMapBacking" : "XrtStringBacking");
                c_value(c, v->operands[operand++]); c_text(c, "}, .value=");
                if (spread) c_text(c, "INT64_C(0)"); else c_value(c, v->operands[operand++]);
                c_text(c, "}");
            }
            c_text(c, "}");
        }
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        c_values_publish_leaf(c, v->result, source); return;
    }
    uint32_t role = c_map_role(c, v);
    if (role) { c_map_call(c, v, role, source); return; }
    bool binding_copy=v->op==XR_LIR_MAP_REF_WRITE ||
        (v->op==XR_LIR_CELL_NEW && v->type==XR_LIR_MAP_STRING_I64_CELL) ||
        (v->op==XR_LIR_CELL_WRITE && c_value_type(c,v->operands[0])==XR_LIR_MAP_STRING_I64_CELL);
    if (binding_copy && c->homes->values[v->operands[v->operand_count-1]].parameter) {
        c_map_binding_stage_copy(c,v->op==XR_LIR_CELL_NEW ? v->result : v->operands[0],
            v->operands[v->operand_count-1],source); return;
    }
    if (v->op == XR_LIR_MAP_REF_WRITE && c->homes->bindings[v->operands[0]].kind == XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW) {
        const XrLirHomeBinding *binding = &c->homes->bindings[v->operands[0]];
        c_text(c,"    xrt_native_cell_map_write("); c_value(c,binding->source); c_text(c,", ");
        c_value(c,v->operands[1]); c_format(c,", UINT32_C(%" PRIu32 "));\n",source); return;
    }
    if (v->op != XR_LIR_MAP_BORROW) c_values_ensure(c, source);
    if (v->op == XR_LIR_MAP_BORROW) {
        const XrLirHomeBinding *binding = &c->homes->bindings[v->result];
        if (binding->kind == XR_LIR_HOME_BINDING_STATE_BORROW) {
            c_text(c, "    { XrtManagedStatus status = xrt_module_external_owner("); c_value(c, binding->source);
            c_format(c, ", 0, &xrl_array_binding%" PRIu32 ");\n", v->result); c_array_require(c, source); c_text(c, "    }\n");
            c_format(c, "    v%" PRIu32 " = (XrtPlace){.external=&xrl_array_binding%" PRIu32 ", .nulls=XRT_PLACE_NONNULL};\n", v->result, v->result);
        } else { c_format(c, "    v%" PRIu32 " = ", v->result); c_value(c, binding->source); c_text(c, ";\n"); }
    } else if (v->op == XR_LIR_MAP_RETAIN) {
        c_text(c, "    { XrtManagedHeader *owner = "); c_value(c, v->operands[0]); c_array_copy_end(c, v->result, source);
    } else if (v->op == XR_LIR_MAP_DROP || v->op == XR_LIR_MAP_CELL_DROP) {
        size_t slot = v->op == XR_LIR_MAP_DROP ? c->homes->values[v->operands[0]].first_owner : c->homes->bindings[v->operands[0]].first_owner;
        c_format(c, "    { XrtManagedStatus status = xrt_managed_slot_drop(xrl_frame, %zu);\n", slot); c_array_require(c, source); c_text(c, "    }\n");
    } else {
        bool read = v->op == XR_LIR_CELL_READ || v->op == XR_LIR_MAP_REF_READ;
        const XrLirValueHome *home = &c->homes->values[read ? v->result : v->operands[v->operand_count - 1]];
        if (!read && (!home->owner_count || home->parameter)) { c_error(c, XR_LIR_C_UNSUPPORTED, "Map assignment requires its final owned source home"); return; }
        c_text(c, "    { XrtManagedStatus status = xrt_place_copy_replace(");
        if (read) c_format(c, "(XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NULLABLE}", home->first_owner);
        else c_value(c, v->op == XR_LIR_CELL_NEW ? v->result : v->operands[0]);
        c_text(c, ", ");
        if (read) c_value(c, v->operands[0]);
        else c_format(c, "(XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NONNULL}", home->first_owner);
        c_text(c, ", NULL);\n"); c_array_require(c, source); c_text(c, "    }\n");
    }
}
