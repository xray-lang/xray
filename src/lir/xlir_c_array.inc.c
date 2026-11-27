/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_array.inc.c - Consume owned Array roles through the common runtime
 */
static bool c_array_intrinsic(LirC *c, const XrLirInstructionView *v, XrLirArrayIntrinsic *shape) {
    return v->op == XR_LIR_CALL_INTRINSIC &&
        xr_lir_array_intrinsic_view(c->request->module, v->intrinsic, shape) == XR_LIR_OK && shape->id;
}
static bool c_array_element_supported(const LirC *c, XrLirType element) {
    return xr_lir_internal_array_element(c->request->module, element);
}
static void c_array_admit_shapes(LirC *c) {
    XrLirArrayShape shape = {0};
    if (c->view.result == XR_LIR_ARRAY) {
        if (xr_lir_array_result_view(c->request->module, c->function, &shape) != XR_LIR_OK ||
            !c_array_element_supported(c, shape.element))
            c_error(c, XR_LIR_C_UNSUPPORTED, "Array result has no native element implementation");
    }
    for (uint32_t id = 1; id <= c->view.value_count && c->status == XR_LIR_C_OK; ++id) {
        XrLirValue value = {c->request->module, c->function.id, id};
        XrLirType type = c_value_type(c, id);
        if (type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF) {
            if (xr_lir_array_shape_view(c->request->module, value, &shape) != XR_LIR_OK ||
                !c_array_element_supported(c, shape.element))
                c_error(c, XR_LIR_C_UNSUPPORTED, "Array value has no native element implementation");
        } else if (type == XR_LIR_STATE_REF) {
            XrLirStateReference reference = {0};
            if (xr_lir_state_reference_view(c->request->module, value, &reference) != XR_LIR_OK ||
                (reference.payload == XR_LIR_ARRAY && !c_array_element_supported(c, reference.array_element)))
                c_error(c, XR_LIR_C_UNSUPPORTED, "module reference has no native element implementation");
        }
    }
}
static bool c_array_point(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_CHECKED_ARRAY_PUSH) return true;
    XrLirArrayIntrinsic shape = {0};
    return v->op == XR_LIR_CHECKED_EMPTY_ARRAY || v->op == XR_LIR_MANAGED_FAILURE ||
        c_array_intrinsic(c, v, &shape) || v->op == XR_LIR_ARRAY_RETAIN ||
        v->op == XR_LIR_ARRAY_BORROW || v->op == XR_LIR_ARRAY_REF_READ || v->op == XR_LIR_ARRAY_REF_WRITE ||
        (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_ARRAY_CELL) ||
        (v->op == XR_LIR_CELL_READ && v->type == XR_LIR_ARRAY) ||
        (v->op == XR_LIR_CELL_WRITE && c_value_type(c, v->operands[0]) == XR_LIR_ARRAY_CELL);
}
static const char *c_array_kind(XrLirType element) {
    switch (element) {
    case XR_LIR_I64: return "XRT_BACKING_ARRAY_I64";
    case XR_LIR_STRING: return "XRT_BACKING_ARRAY_STRING";
    case XR_LIR_U8: return "XRT_BACKING_ARRAY_U8";
    case XR_LIR_U64: return "XRT_BACKING_ARRAY_U64";
    case XR_LIR_F64: return "XRT_BACKING_ARRAY_F64";
    default: return NULL;
    }
}
static const char *c_array_member(XrLirType element) {
    switch (element) {
    case XR_LIR_STRING: return "string";
    case XR_LIR_U8: return "u8";
    case XR_LIR_U64: return "u64";
    case XR_LIR_F64: return "f64";
    default: return "i64";
    }
}
static void c_array_require(LirC *c, uint32_t source) {
    c_format(c, "    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 "));\n", source);
}
static void c_values_publish_leaf(LirC *c, uint32_t value, uint32_t source) {
    const XrLirValueHome *home=&c->homes->values[value];
    XR_CHECK(c->homes->owned_frame_needed && home->owner_count==1 && !home->parameter &&
        home->first_owner<c->homes->frame.slot_count && home->temporary_first_owner<c->homes->frame.slot_count &&
        home->first_owner!=home->temporary_first_owner,
        "leaf publication uses distinct real owner slots in its one stable Frame");
    c_format(c, "    { XrtManagedStatus status = xrt_place_frame_move_replace(xrl_frame, %zu, %zu);\n",
        home->first_owner,home->temporary_first_owner);
    c_array_require(c,source); c_text(c, "    }\n");
}
/* Borrowed input stays protected by its actual caller or source home. Retain
 * and adoption return normally before a failure can leave the C activation. */
static void c_array_copy_end(LirC *c, uint32_t value, uint32_t source) {
    const XrLirValueHome *home=&c->homes->values[value];
    c_text(c, ";\n");
    c_format(c, "    if (!xrt_managed_retain(owner)) xrt_native_managed_failure(XRT_MANAGED_REFCOUNT, UINT32_C(%" PRIu32 "));\n",source);
    c_format(c, "    XrtManagedStatus status=xrt_managed_slot_replace(xrl_frame, %zu, &owner);\n",home->temporary_first_owner);
    c_text(c, "    if (status != XRT_MANAGED_OK) { xrt_managed_drop(owner);\n"); c_array_require(c,source);
    c_text(c, "    } }\n"); c_values_publish_leaf(c,value,source);
}
static const XrLirHomeWorkspace *c_array_workspace(LirC *c) {
    for (size_t i=0;i<c->homes->workspace_count;++i) {
        const XrLirHomeWorkspace *workspace=&c->homes->workspaces[i];
        if (workspace->block==c->block && workspace->instruction==c->instruction) return workspace;
    }
    c_error(c,XR_LIR_C_INVALID_LIR,"Array mutation has no common pending workspace"); return NULL;
}
static void c_array_item(LirC *c, XrLirType element, uint32_t operand) {
    const char *kind = c_array_kind(element);
    if (!kind) {
        c_error(c, XR_LIR_C_UNSUPPORTED, "Array element has no target receiver");
        return;
    }
    c_format(c, "{%s, {. %s = ", kind, c_array_member(element));
    if (element==XR_LIR_STRING) c_text(c,"(XrtStringBacking *)");
    c_value(c,operand); c_text(c,"}}");
}
/* Exact scalar tails have no child owner to retain. The original receiver
 * owns rejection and its diagnostic order; only admitted reads load bytes. */
static void c_array_scalar_get(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    c_text(c,"    { const XrtArrayBacking *array = (const XrtArrayBacking *)");
    c_value(c,v->operands[0]); c_text(c,";\n    const int64_t index = ");
    c_value(c,v->operands[1]); c_text(c,";\n");
    c_text(c,"    if (array && array->length <= array->capacity && index >= 0 && (uint64_t)index < array->length) {\n");
    c_format(c,"        memcpy(&v%" PRIu32 ", array->elements + (size_t)index * sizeof(v%" PRIu32 "), sizeof(v%" PRIu32 "));\n",
        v->result,v->result,v->result);
    c_text(c,"    } else { XrtArrayItem item = {0};\n");
    c_format(c,"        xrt_native_array_get((const XrtManagedHeader *)array, %s, index, &item, UINT32_C(%" PRIu32 "));\n",
        c_array_kind(v->type),source);
    c_format(c,"        v%" PRIu32 " = item.value.%s;\n    }\n    }\n",v->result,c_array_member(v->type));
}
static void c_array_inline(LirC *c, const XrLirInstructionView *v, const XrLirArrayIntrinsic *shape,
    XrLirType element, uint32_t source) {
    c_values_ensure(c,source);
    if (shape->operation == XR_LIR_ARRAY_OP_NEW) {
        const XrLirValueHome *home = &c->homes->values[v->result];
        c_format(c,"    xrt_native_array_inline_create(xrl_frame, %zu, &xrl_value_image, UINT32_C(%" PRIu32 "), ",
            home->temporary_first_owner,home->type);
        if (!v->operand_count) c_text(c,"NULL");
        else {
            c_format(c,"(const %s[]){",c_type(c,element));
            for (uint32_t i=0;i<v->operand_count;++i) {
                if (i) c_text(c,", ");
                c_value(c,v->operands[i]);
            }
            c_text(c,"}");
        }
        c_format(c,", UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "));\n",v->operand_count,source);
        c_values_publish_leaf(c,v->result,source);
        return;
    }
    if (shape->operation == XR_LIR_ARRAY_OP_PUSH) {
        const XrLirHomeWorkspace *workspace = c_array_workspace(c);
        if (!workspace) return;
        const XrLirHomeBinding *binding = &c->homes->bindings[v->operands[0]];
        XR_CHECK(binding->kind == XR_LIR_HOME_BINDING_LOCAL && workspace->type == binding->type &&
            workspace->kind == XR_LIR_HOME_WORKSPACE_VALUE &&
            workspace->first_owner < c->homes->frame.slot_count &&
            binding->first_owner < c->homes->frame.slot_count &&
            workspace->first_owner != binding->first_owner,
            "verified local Class Array append has an exact distinct pending home");
        c_text(c, "    xrt_native_array_inline_class_append(");
        c_value(c, v->operands[0]);
        c_format(c, ", %zu, &xrl_value_image, UINT32_C(%" PRIu32 "), (XrtManagedHeader *)",
            workspace->first_owner, binding->type);
        c_value(c, v->operands[1]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        return;
    }
    if (shape->operation != XR_LIR_ARRAY_OP_GET) {
        c_error(c,XR_LIR_C_UNSUPPORTED,"inline Array operation has no target receiver"); return;
    }
    const XrLirValueHome *array = &c->homes->values[v->operands[0]];
    c_text(c,"    { const void *row = xrt_native_array_inline_at("); c_value(c,v->operands[0]);
    c_format(c,", &xrl_value_image, UINT32_C(%" PRIu32 "), ",array->type);
    c_value(c,v->operands[1]);
    c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
    if (c->homes->values[v->result].owner_count) {
        c_values_replace_begin(c,v->result,source); c_text(c,"row"); c_values_replace_end(c,source);
    } else {
        c_text(c,"    memcpy(&"); c_value(c,v->result); c_text(c,", row, sizeof(");
        c_value(c,v->result); c_text(c,"));\n");
    }
    c_text(c,"    }\n");
}
static void c_array_call(LirC *c, const XrLirInstructionView *v,
    const XrLirArrayIntrinsic *shape, uint32_t source) {
    XrLirType element=v->type==XR_LIR_ARRAY ? shape->result_element : shape->parameter_elements[0];
    if (shape->operation == XR_LIR_ARRAY_OP_LEN) {
        c_format(c,"    v%" PRIu32 " = (int64_t)((const XrtArrayBacking *)",v->result);
        c_value(c,v->operands[0]); c_text(c,")->length;\n"); return;
    }
    if (xr_lir_inline_array_ordinal(c->request->module,element)) {
        c_array_inline(c,v,shape,element,source); return;
    }
    const char *kind=c_array_kind(element);
    if (!kind) { c_error(c,XR_LIR_C_UNSUPPORTED,"Array element has no target receiver"); return; }
    if (shape->operation == XR_LIR_ARRAY_OP_TIMING_EQUAL) {
        c_text(c,"    { bool equal = false; XrtManagedStatus status = xrt_native_array_timing_equal(");
        c_value(c,v->operands[0]); c_text(c,", "); c_value(c,v->operands[1]); c_text(c,", &equal);\n");
        c_format(c,"      if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 "));\n",source);
        c_format(c,"      v%" PRIu32 " = equal; }\n",v->result);
        return;
    }
    if (shape->operation == XR_LIR_ARRAY_OP_CAPACITY) {
        c_format(c,"    v%" PRIu32 " = xrt_native_array_capacity(",v->result); c_value(c,v->operands[0]);
        c_format(c,", %s, UINT32_C(%" PRIu32 "));\n",kind,source); return;
    }
    if (shape->operation != XR_LIR_ARRAY_OP_GET || element == XR_LIR_STRING) c_values_ensure(c,source);
    const XrLirValueHome *home=v->result ? &c->homes->values[v->result] : NULL;
    if (shape->operation == XR_LIR_ARRAY_OP_WITH_CAPACITY) {
        c_format(c,"    xrt_native_array_with_capacity(xrl_frame, %zu, %s, ",home->temporary_first_owner,kind);
        c_value(c,v->operands[0]);
        c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
        c_values_publish_leaf(c,v->result,source);
        c_array_local_ready(c);
        if (c->status == XR_LIR_C_OK) c->array_frame_block = c->block;
        return;
    }
    switch (shape->operation) {
    case XR_LIR_ARRAY_OP_NEW:
        c_format(c,"    xrt_native_array_create(xrl_frame, %zu, &(XrtArrayInput){%s, ",home->temporary_first_owner,kind);
        if (!v->operand_count) c_text(c,"NULL");
        else {
            c_format(c,"(%s[]){",element==XR_LIR_STRING ? "XrtStringBacking *" : c_type(c,element));
            for (uint32_t i=0;i<v->operand_count;++i) {
                if (i) c_text(c,", ");
                if (element==XR_LIR_STRING) c_text(c,"(XrtStringBacking *)");
                c_value(c,v->operands[i]);
            }
            c_text(c,"}");
        }
        c_format(c,", %" PRIu32 "}, UINT32_C(%" PRIu32 "));\n",v->operand_count,source);
        c_values_publish_leaf(c,v->result,source);
        c_array_local_ready(c);
        /* Both setup and publication must succeed before later instructions
         * run. This function's Frame cannot leave until a terminating return.
         * Merely emitting an ensure inside conditional C proves nothing. */
        if (c->status == XR_LIR_C_OK) c->array_frame_block = c->block;
        return;
    case XR_LIR_ARRAY_OP_GET:
        if (element != XR_LIR_STRING) { c_array_scalar_get(c,v,source); return; }
        c_format(c,"    xrt_native_array_string_read_copy(xrl_frame, %zu, ",
            c->homes->values[v->result].temporary_first_owner);
        c_value(c,v->operands[0]); c_text(c,", "); c_value(c,v->operands[1]);
        c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
        c_values_publish_leaf(c,v->result,source);
        return;
    case XR_LIR_ARRAY_OP_RANDOM_BYTES: {
        const XrLirHomeWorkspace *workspace = c_array_workspace(c);
        if (!workspace) return;
        const XrLirHomeBinding *binding = &c->homes->bindings[v->operands[0]];
        if (element != XR_LIR_U8 || !binding->type || workspace->type != binding->type ||
            workspace->value != v->operands[0]) {
            c_error(c, XR_LIR_C_INVALID_LIR, "random byte fill lost its exact Array reference and workspace");
            return;
        }
        c_text(c, "    xrt_native_array_update_place(");
        c_value(c, v->operands[0]);
        c_format(c, ", XRT_BACKING_ARRAY_U8, xrl_frame, %zu, "
            "&(XrtArrayUpdate){.operation=XRT_ARRAY_UPDATE_RANDOM_BYTES,.item={.kind=XRT_BACKING_ARRAY_U8}}, "
            "UINT32_C(%" PRIu32 "));\n", workspace->first_owner, source);
        return;
    }
    case XR_LIR_ARRAY_OP_SET: case XR_LIR_ARRAY_OP_PUSH: {
        const XrLirHomeWorkspace *workspace=c_array_workspace(c); if (!workspace) return;
        const XrLirHomeBinding *binding=&c->homes->bindings[v->operands[0]];
        if (!binding->type || workspace->type!=binding->type) {
            c_error(c,XR_LIR_C_INVALID_LIR,"Array mutation workspace lost its exact binding type"); return;
        }
        bool local=c_array_local_begin(c,v,shape,workspace);
        size_t window = local && shape->operation == XR_LIR_ARRAY_OP_PUSH ? c_array_push_window_at(c) : 0;
        if (window) {
            c_format(c,"    if (xrl_ap_%zu_ready) {\n    xrt_native_array_push_local_i64(",window-1);
            c_value(c,v->operands[0]);
            c_format(c,", %zu, xrl_array_update.item.value.i64, UINT32_C(%" PRIu32 "));\n    } else {\n",
                workspace->first_owner,source);
        }
        c_text(c,"    xrt_native_array_update_place("); c_value(c,v->operands[0]);
        if (local) c_format(c,", %s, xrl_frame, %zu, &xrl_array_update, UINT32_C(%" PRIu32 "));\n",kind,workspace->first_owner,source);
        else {
            c_format(c,", %s, xrl_frame, %zu, &(XrtArrayUpdate){%s, ",kind,workspace->first_owner,
                shape->operation==XR_LIR_ARRAY_OP_SET ? "XRT_ARRAY_UPDATE_SET" : "XRT_ARRAY_UPDATE_PUSH");
            if (shape->operation==XR_LIR_ARRAY_OP_SET) c_value(c,v->operands[1]); else c_text(c,"0");
            c_text(c,", "); c_array_item(c,element,v->operands[v->operand_count-1]);
            c_format(c,"}, UINT32_C(%" PRIu32 "));\n",source);
        }
        if (window) c_text(c,"    }\n");
        c_array_local_ready(c);
        if (local) c_text(c,"    }\n    }\n");
        return;
    }
    case XR_LIR_ARRAY_OP_REPEAT:
        c_format(c,"    xrt_native_array_repeat(xrl_frame, %zu, &(XrtArrayItem)",home->temporary_first_owner);
        c_array_item(c,element,v->operands[1]); c_text(c,", "); c_value(c,v->operands[0]);
        c_format(c,", UINT32_C(%" PRIu32 "));\n",source); c_values_publish_leaf(c,v->result,source); c_array_local_ready(c); return;
    case XR_LIR_ARRAY_OP_RESERVE:
        c_format(c,"    xrt_native_array_reserve(xrl_frame, %zu, ",home->temporary_first_owner);
        c_value(c,v->operands[0]); c_format(c,", %s, ",kind); c_value(c,v->operands[1]);
        c_format(c,", UINT32_C(%" PRIu32 "));\n",source); c_values_publish_leaf(c,v->result,source); return;
    case XR_LIR_ARRAY_OP_COPY_STRING_BYTES:
        c_format(c,"    xrt_native_string_copy_bytes(xrl_frame, %zu, (const XrtStringBacking *)",
            home->temporary_first_owner);
        c_value(c,v->operands[0]); c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
        c_values_publish_leaf(c,v->result,source); return;
    case XR_LIR_ARRAY_OP_DISPLAY:
        c_format(c,"    xrt_native_array_display(xrl_frame, %zu, ",home->temporary_first_owner); c_value(c,v->operands[0]);
        c_format(c,", %s, UINT32_C(%" PRIu32 "));\n",kind,source); c_values_publish_leaf(c,v->result,source); return;
    default: c_error(c,XR_LIR_C_UNSUPPORTED,"Array operation has no target receiver"); return;
    }
}

static void c_checked_empty_array(LirC *c, const XrLirInstructionView *instruction) {
    const XrLirModule *module = c->request->module;
    XrLirBlockView normal, failure;
    xr_lir_block_view(module, (XrLirBlock){module,c->function.id,instruction->edges[0].target}, &normal);
    xr_lir_block_view(module, (XrLirBlock){module,c->function.id,instruction->edges[1].target}, &failure);
    XrLirArrayIntrinsic shape = {0};
    if (xr_lir_array_intrinsic_view(module, (uint32_t)instruction->bits, &shape) != XR_LIR_OK ||
        shape.operation != XR_LIR_ARRAY_OP_NEW) {
        c_error(c, XR_LIR_C_INVALID_LIR, "checked empty Array has no exact constructor");
        return;
    }
    const XrLirValueHome *home = &c->homes->values[normal.parameters[0]];
    if (!c->homes->owned_frame_needed || home->owner_count != 1 || home->first_owner >= c->homes->frame.slot_count ||
        (c->seen[c->function.id - 1] & LIR_C_FUNCTION_STRING_BINDING)) {
        c_error(c, XR_LIR_C_INVALID_LIR, "checked empty Array has no private receiving home");
        return;
    }
    const char *kind = c_array_kind(shape.result_element);
    bool inline_element = xr_lir_inline_array_ordinal(module, shape.result_element) != 0;
    if (!kind && !inline_element) {
        c_error(c, XR_LIR_C_INVALID_LIR, "checked empty Array has no physical array family");
        return;
    }
    bool locations = c->header_edges || c->binding_forwarding || c->array_local ||
        c_array_region_present(c) || c_array_push_present(c);
    c_text(c, "    { XrtManagedStatus status = XRT_MANAGED_OK;\n"
        "      if (!xrl_frame) {\n"
        "        status = xrt_native_managed_root_checked(&xrl_root);\n"
        "        if (status == XRT_MANAGED_OK) {\n");
    if (locations) {
        c_format(c, "          status = xrt_managed_frame_enter_locations(xrl_root, &xrl_frame_%" PRIu32
            ", &xrl_frame_locations);\n"
            "          if (status == XRT_MANAGED_OK) {\n"
            "            xrl_frame = xrl_frame_locations.frame; xrl_payload = xrl_frame_locations.payload;\n",
            c->function.id);
        if (c->array_local)
            c_format(c,"            xrl_array_local_prepare_%" PRIu32
                "(xrl_frame, xrl_payload, &xrl_frame_locations, &xrl_array_locals);\n", c->function.id);
        c_text(c, "          }\n");
    } else
        c_format(c, "          status = xrt_managed_frame_enter_payload(xrl_root, &xrl_frame_%" PRIu32
            ", &xrl_frame, &xrl_payload);\n", c->function.id);
    c_text(c, "        }\n      }\n      if (status == XRT_MANAGED_OK)\n");
    if (inline_element)
        c_format(c, "        status = xrt_native_array_inline_empty_checked(xrl_frame, %zu, &xrl_value_image,"
            " UINT32_C(%" PRIu32 "));\n", home->first_owner, home->type);
    else
        c_format(c, "        status = xrt_native_array_empty_checked(xrl_frame, %zu, %s);\n",
            home->first_owner, kind);
    c_format(c, "      if (status != XRT_MANAGED_OK) { v%" PRIu32 " = (uint64_t)status; goto bb%" PRIu32
        "; }\n", failure.parameters[0], instruction->edges[1].target);
    c_array_local_ready(c);
    c_format(c, "      goto bb%" PRIu32 ";\n    }\n", instruction->edges[0].target);
}

static void c_array_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (v->op == XR_LIR_CHECKED_EMPTY_ARRAY) {
        c_checked_empty_array(c, v);
        return;
    }
    if (v->op == XR_LIR_CHECKED_ARRAY_PUSH) {
        const XrLirHomeWorkspace *workspace = c_array_workspace(c);
        if (!workspace) return;
        c_format(c, "    v%" PRIu32 " = xrt_native_array_update_place_checked(", v->result);
        c_value(c, v->operands[0]);
        c_format(c, ", XRT_BACKING_ARRAY_STRING, xrl_frame, %zu, &(XrtArrayUpdate){XRT_ARRAY_UPDATE_PUSH, 0, ",
            workspace->first_owner);
        c_array_item(c, XR_LIR_STRING, v->operands[1]);
        c_text(c, "});\n");
        return;
    }
    if (v->op == XR_LIR_MANAGED_FAILURE) {
        c_text(c, v->operand_count == 2 ? "    xrt_native_managed_failure_with_directory((XrtManagedStatus)" :
            "    xrt_native_managed_failure((XrtManagedStatus)");
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 ")", source);
        if (v->operand_count == 2) {
            c_text(c, ", ");
            c_value(c, v->operands[1]);
        }
        c_text(c, ");\n");
        return;
    }
    XrLirArrayIntrinsic shape={0};
    if (c_array_intrinsic(c,v,&shape)) { c_array_call(c,v,&shape,source); return; }
    bool binding_copy=v->op==XR_LIR_CELL_NEW || v->op==XR_LIR_CELL_WRITE || v->op==XR_LIR_ARRAY_REF_WRITE;
    if (binding_copy && c->homes->values[v->operands[v->operand_count-1]].parameter) {
        c_collection_binding_stage_copy(c,v->op==XR_LIR_CELL_NEW ? v->result : v->operands[0],
            v->operands[v->operand_count-1],source); return;
    }
    bool forwarded_drop=(v->op==XR_LIR_ARRAY_DROP || v->op==XR_LIR_ARRAY_CELL_DROP) && c_binding_forward_drop(c,v);
    if (!forwarded_drop && v->op!=XR_LIR_ARRAY_BORROW) c_values_ensure(c,source);
    if (v->op==XR_LIR_ARRAY_BORROW) {
        const XrLirHomeBinding *binding=&c->homes->bindings[v->result];
        if (binding->kind==XR_LIR_HOME_BINDING_STATE_BORROW) {
            c_text(c,"    { XrtManagedStatus status=xrt_module_external_owner("); c_value(c,binding->source);
            c_format(c,", 0, &xrl_array_binding%" PRIu32 ");\n",v->result); c_array_require(c,source); c_text(c,"    }\n");
            c_format(c,"    v%" PRIu32 " = (XrtPlace){.external=&xrl_array_binding%" PRIu32 ", .nulls=XRT_PLACE_NONNULL};\n",v->result,v->result);
        } else { c_format(c,"    v%" PRIu32 " = ",v->result); c_value(c,binding->source); c_text(c,";\n"); }
    } else if (v->op==XR_LIR_ARRAY_RETAIN) {
        c_text(c,"    { XrtManagedHeader *owner = "); c_value(c,v->operands[0]); c_array_copy_end(c,v->result,source);
    } else if (v->op==XR_LIR_ARRAY_DROP || v->op==XR_LIR_ARRAY_CELL_DROP) {
        size_t slot=v->op==XR_LIR_ARRAY_DROP ? c->homes->values[v->operands[0]].first_owner : c->homes->bindings[v->operands[0]].first_owner;
        c_format(c,"    { XrtManagedStatus status=xrt_managed_slot_drop(xrl_frame, %zu);\n",slot); c_array_require(c,source); c_text(c,"    }\n");
    } else {
        bool read=v->op==XR_LIR_CELL_READ || v->op==XR_LIR_ARRAY_REF_READ;
        const XrLirValueHome *home=&c->homes->values[read ? v->result : v->operands[v->operand_count-1]];
        if (!read && !home->owner_count) { c_error(c,XR_LIR_C_UNSUPPORTED,"Array assignment requires its final owned source home"); return; }
        bool forwarded_copy=c_binding_forward_begin(c,v);
        c_text(c,"    { XrtManagedStatus status=xrt_place_copy_replace(");
        if (read) c_format(c,"(XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NULLABLE}",home->first_owner);
        else c_value(c,v->op==XR_LIR_CELL_NEW ? v->result : v->operands[0]);
        c_text(c,", ");
        if (read) c_value(c,v->operands[0]);
        else c_format(c,"(XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NONNULL}",home->first_owner);
        c_text(c,", NULL);\n"); c_array_require(c,source); c_text(c,"    }\n");
        if (forwarded_copy) c_text(c,"    }\n");
    }
    if (forwarded_drop) c_text(c,"    }\n");
}
