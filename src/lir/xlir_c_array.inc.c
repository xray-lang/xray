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
static bool c_array_element_supported(XrLirType element) {
    return element == XR_LIR_I64 || element == XR_LIR_STRING || element == XR_LIR_U8 || element == XR_LIR_U64;
}
static void c_array_admit_shapes(LirC *c) {
    XrLirArrayShape shape = {0};
    if (c->view.result == XR_LIR_ARRAY) {
        if (xr_lir_array_result_view(c->request->module, c->function, &shape) != XR_LIR_OK ||
            !c_array_element_supported(shape.element))
            c_error(c, XR_LIR_C_UNSUPPORTED, "Array result has no native element implementation");
    }
    for (uint32_t id = 1; id <= c->view.value_count && c->status == XR_LIR_C_OK; ++id) {
        XrLirValue value = {c->request->module, c->function.id, id};
        XrLirType type = c_value_type(c, id);
        if (type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF) {
            if (xr_lir_array_shape_view(c->request->module, value, &shape) != XR_LIR_OK ||
                !c_array_element_supported(shape.element))
                c_error(c, XR_LIR_C_UNSUPPORTED, "Array value has no native element implementation");
        } else if (type == XR_LIR_STATE_REF) {
            XrLirStateReference reference = {0};
            if (xr_lir_state_reference_view(c->request->module, value, &reference) != XR_LIR_OK ||
                (reference.payload == XR_LIR_ARRAY && !c_array_element_supported(reference.array_element)))
                c_error(c, XR_LIR_C_UNSUPPORTED, "module reference has no native element implementation");
        }
    }
}
static bool c_array_point(LirC *c, const XrLirInstructionView *v) {
    XrLirArrayIntrinsic shape = {0};
    return c_array_intrinsic(c, v, &shape) || v->op == XR_LIR_ARRAY_RETAIN ||
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
    default: return NULL;
    }
}
static const char *c_array_member(XrLirType element) {
    return element==XR_LIR_STRING ? "string" : element==XR_LIR_U8 ? "u8" : element==XR_LIR_U64 ? "u64" : "i64";
}
static void c_array_require(LirC *c, uint32_t source) {
    c_format(c, "    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, UINT32_C(%" PRIu32 "));\n", source);
}
static void c_values_publish_leaf(LirC *c, uint32_t value, uint32_t source) {
    const XrLirValueHome *home=&c->homes->values[value];
    c_format(c, "    { XrtManagedStatus status = xrt_place_move_replace((XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NULLABLE},\n", home->first_owner);
    c_format(c, "        (XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NONNULL}, NULL);\n", home->temporary_first_owner);
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
    c_format(c, "{%s, {. %s = ",c_array_kind(element),c_array_member(element));
    if (element==XR_LIR_STRING) c_text(c,"(XrtStringBacking *)");
    c_value(c,operand); c_text(c,"}}");
}
static void c_array_call(LirC *c, const XrLirInstructionView *v,
    const XrLirArrayIntrinsic *shape, uint32_t source) {
    XrLirType element=v->type==XR_LIR_ARRAY ? shape->result_element : shape->parameter_elements[0];
    const char *kind=c_array_kind(element);
    if (!kind) { c_error(c,XR_LIR_C_UNSUPPORTED,"Array element has no target receiver"); return; }
    if (shape->operation != XR_LIR_ARRAY_OP_LEN &&
        (shape->operation != XR_LIR_ARRAY_OP_GET || element == XR_LIR_STRING)) c_values_ensure(c,source);
    const XrLirValueHome *home=v->result ? &c->homes->values[v->result] : NULL;
    switch (shape->operation) {
    case XR_LIR_ARRAY_OP_NEW:
        c_format(c,"    xrt_native_array_create(xrl_frame, %zu, &(XrtArrayInput){%s, ",home->temporary_first_owner,kind);
        if (!v->operand_count) c_text(c,"NULL");
        else {
            c_format(c,"(%s[]){",element==XR_LIR_STRING ? "XrtStringBacking *" : c_type(c,element));
            for (uint32_t i=0;i<v->operand_count;++i) {
                if (i) c_text(c,", "); if (element==XR_LIR_STRING) c_text(c,"(XrtStringBacking *)"); c_value(c,v->operands[i]);
            }
            c_text(c,"}");
        }
        c_format(c,", %" PRIu32 "}, UINT32_C(%" PRIu32 "));\n",v->operand_count,source);
        c_values_publish_leaf(c,v->result,source); return;
    case XR_LIR_ARRAY_OP_LEN:
        c_format(c,"    v%" PRIu32 " = (int64_t)((const XrtArrayBacking *)",v->result);
        c_value(c,v->operands[0]); c_text(c,")->length;\n"); return;
    case XR_LIR_ARRAY_OP_GET:
        c_text(c,"    { XrtArrayItem item = {0};\n    xrt_native_array_get("); c_value(c,v->operands[0]);
        c_format(c,", %s, ",kind); c_value(c,v->operands[1]);
        c_format(c,", &item, UINT32_C(%" PRIu32 "));\n",source);
        if (element==XR_LIR_STRING) { c_text(c,"    { XrtManagedHeader *owner = &item.value.string->managed"); c_array_copy_end(c,v->result,source); }
        else c_format(c,"    v%" PRIu32 " = item.value.%s;\n",v->result,c_array_member(element));
        c_text(c,"    }\n"); return;
    case XR_LIR_ARRAY_OP_SET: case XR_LIR_ARRAY_OP_PUSH: {
        const XrLirHomeWorkspace *workspace=c_array_workspace(c); if (!workspace) return;
        const XrLirHomeBinding *binding=&c->homes->bindings[v->operands[0]];
        if (!binding->type || workspace->type!=binding->type) {
            c_error(c,XR_LIR_C_INVALID_LIR,"Array mutation workspace lost its exact binding type"); return;
        }
        c_text(c,"    xrt_native_array_update_place("); c_value(c,v->operands[0]);
        c_format(c,", %s, xrl_frame, %zu, &(XrtArrayUpdate){%s, ",kind,workspace->first_owner,
            shape->operation==XR_LIR_ARRAY_OP_SET ? "XRT_ARRAY_UPDATE_SET" : "XRT_ARRAY_UPDATE_PUSH");
        if (shape->operation==XR_LIR_ARRAY_OP_SET) c_value(c,v->operands[1]); else c_text(c,"0");
        c_text(c,", "); c_array_item(c,element,v->operands[v->operand_count-1]);
        c_format(c,"}, UINT32_C(%" PRIu32 "));\n",source); return;
    }
    case XR_LIR_ARRAY_OP_REPEAT:
        c_format(c,"    xrt_native_array_repeat(xrl_frame, %zu, &(XrtArrayItem)",home->temporary_first_owner);
        c_array_item(c,element,v->operands[1]); c_text(c,", "); c_value(c,v->operands[0]);
        c_format(c,", UINT32_C(%" PRIu32 "));\n",source); c_values_publish_leaf(c,v->result,source); return;
    case XR_LIR_ARRAY_OP_DISPLAY:
        c_format(c,"    xrt_native_array_display(xrl_frame, %zu, ",home->temporary_first_owner); c_value(c,v->operands[0]);
        c_format(c,", %s, UINT32_C(%" PRIu32 "));\n",kind,source); c_values_publish_leaf(c,v->result,source); return;
    default: c_error(c,XR_LIR_C_UNSUPPORTED,"Array operation has no target receiver"); return;
    }
}
static void c_array_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    XrLirArrayIntrinsic shape={0};
    if (c_array_intrinsic(c,v,&shape)) { c_array_call(c,v,&shape,source); return; }
    if (v->op!=XR_LIR_ARRAY_BORROW) c_values_ensure(c,source);
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
        if (!read && (!home->owner_count || home->parameter)) { c_error(c,XR_LIR_C_UNSUPPORTED,"Array assignment requires its final owned source home"); return; }
        c_text(c,"    { XrtManagedStatus status=xrt_place_copy_replace(");
        if (read) c_format(c,"(XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NULLABLE}",home->first_owner);
        else c_value(c,v->op==XR_LIR_CELL_NEW ? v->result : v->operands[0]);
        c_text(c,", ");
        if (read) c_value(c,v->operands[0]);
        else c_format(c,"(XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NONNULL}",home->first_owner);
        c_text(c,", NULL);\n"); c_array_require(c,source); c_text(c,"    }\n");
    }
}
