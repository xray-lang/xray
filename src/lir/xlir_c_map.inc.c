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
static void c_path(LirC *,const XrLirSource *);
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
    c_text(c, "    { bool removed = false;\n    xrt_native_map_update_place("); c_value(c, v->operands[0]);
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
static void c_map_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    uint32_t role = c_map_role(c, v);
    if (role) { c_map_call(c, v, role, source); return; }
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
