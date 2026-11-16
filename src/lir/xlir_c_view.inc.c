/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_view.inc.c - Natural readonly Slices with separate stable keepers
 */
static bool c_view_intrinsic(LirC *c, const XrLirInstructionView *v, XrLirSliceIntrinsic *shape) {
    return v->op == XR_LIR_CALL_INTRINSIC &&
        xr_lir_slice_intrinsic_view(c->request->module, v->intrinsic, shape) == XR_LIR_OK && shape->id;
}
static bool c_view_point(LirC *c, const XrLirInstructionView *v) {
    XrLirSliceIntrinsic shape = {0};
    return v->op == XR_LIR_SLICE_CLEANUP || v->op == XR_LIR_SLICE_FORMAL_CLEANUP ||
        c_view_intrinsic(c, v, &shape);
}
static void c_view_admit_signature(LirC *c) {
    if (c_slice_type(c, c->view.result))
        c_error(c, XR_LIR_C_UNSUPPORTED, "Slice return requires a receiving loan boundary");
    for (uint32_t p = 0; p < c->view.parameter_count && c_request_poll(c); ++p)
        if (c_slice_type(c, c->view.parameter_types[p]))
            c_error(c, XR_LIR_C_UNSUPPORTED, "Slice parameter requires a receiving loan boundary");
}
/* These are positions in the same common Frame, never ownership inferred from
 * the natural Slice plan. The runtime checks each actual slot again when the
 * transaction acquires the Root's guard. */
static const XrLirHomeWorkspace *c_view_workspace(LirC *c, const XrLirValueHomes *homes, uint32_t ordinal) {
    if (!ordinal || ordinal > homes->workspace_count) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice has no common keeper workspace"); return NULL;
    }
    const XrLirHomeWorkspace *workspace = &homes->workspaces[ordinal - 1];
    if (workspace->kind != XR_LIR_HOME_WORKSPACE_VIEW_HOLD || !homes->owned_frame_needed ||
        workspace->first_owner >= homes->frame.slot_count ||
        workspace->temporary_first_owner >= homes->frame.slot_count ||
        workspace->first_owner == workspace->temporary_first_owner ||
        workspace->offset > homes->frame.payload_size ||
        sizeof(XrtArraySliceBundle) > homes->frame.payload_size - workspace->offset ||
        workspace->temporary_offset > homes->frame.payload_size ||
        sizeof(XrtArraySliceBundle) > homes->frame.payload_size - workspace->temporary_offset ||
        (workspace->offset <= workspace->temporary_offset ?
            workspace->temporary_offset - workspace->offset : workspace->offset - workspace->temporary_offset) < sizeof(XrtArraySliceBundle) ||
        homes->frame.owned_offsets[workspace->first_owner] != workspace->offset + offsetof(XrtArraySliceBundle, hold) ||
        homes->frame.owned_offsets[workspace->temporary_first_owner] !=
            workspace->temporary_offset + offsetof(XrtArraySliceBundle, hold)) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice keeper lost its exact bundle and temporary slots"); return NULL;
    }
    return workspace;
}
static const XrLirHomeWorkspace *c_view_value_workspace(LirC *c, const XrLirValueHomes *homes, uint32_t value) {
    if (!value || value > homes->value_count) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice has no common value home"); return NULL;
    }
    const XrLirValueHome *home = &homes->values[value];
    const XrLirHomeWorkspace *workspace = c_view_workspace(c, homes, home->view_workspace);
    if (workspace && (home->parameter || home->owner_count || home->first_owner != SIZE_MAX ||
        workspace->type != home->type || home->size != sizeof(XrtReadonlyArraySlice) ||
        home->first_bytes >= homes->byte_count || home->byte_count != 1 ||
        homes->bytes[home->first_bytes].offset != 0 || homes->bytes[home->first_bytes].size != home->size ||
        home->offset != workspace->offset + offsetof(XrtArraySliceBundle, value) ||
        home->temporary_offset != workspace->temporary_offset + offsetof(XrtArraySliceBundle, value))) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice natural bytes disagree with their common keeper projection"); return NULL;
    }
    return workspace;
}
static bool c_view_value_valid(LirC *c, const XrLirValueHomes *homes, uint32_t value) {
    if (!value || value > homes->value_count || !homes->values) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice has no receiving value identity"); return false;
    }
    const XrLirValueHome *home = &homes->values[value];
    if (home->view_storage != XR_LIR_SLICE_STORAGE_BORROWED_FORMAL)
        return home->view_storage == XR_LIR_SLICE_STORAGE_LOCAL_KEEPER &&
            c_view_value_workspace(c, homes, value);
    if (!home->type || !home->parameter || home->view_parameter != home->parameter - 1 ||
        home->view_workspace || home->owner_count || home->first_owner != SIZE_MAX ||
        home->temporary_first_owner != SIZE_MAX || home->offset != SIZE_MAX ||
        home->temporary_offset != SIZE_MAX || home->size != sizeof(XrtReadonlyArraySlice) ||
        !homes->bytes || home->byte_count != 1 || home->first_bytes >= homes->byte_count ||
        homes->bytes[home->first_bytes].offset || homes->bytes[home->first_bytes].size != home->size) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice formal lost its complete slotless borrowed representation"); return false;
    }
    return true;
}
static const XrLirHomeWorkspace *c_view_borrow_stage(LirC *c, const XrLirValueHomes *homes,
    XrLirHomeTransfer transfer, const XrLirHomeEdge *edge) {
    const XrLirValueHome *source = &homes->values[transfer.source], *target = &homes->values[transfer.target];
    if (transfer.borrowed_owner_workspace >= homes->workspace_count || source->type != target->type ||
        transfer.source_storage != XR_LIR_SLICE_STORAGE_BORROWED_FORMAL ||
        source->view_storage != XR_LIR_SLICE_STORAGE_BORROWED_FORMAL ||
        target->view_storage != XR_LIR_SLICE_STORAGE_LOCAL_KEEPER ||
        transfer.transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_BORROW ||
        transfer.hold_transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_COPY ||
        !c_view_value_valid(c, homes, transfer.source)) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice edge lost its borrowed source and COPY hold"); return NULL;
    }
    const XrLirHomeWorkspace *target_workspace = c_view_value_workspace(c, homes, transfer.target);
    const XrLirHomeWorkspace *stage = &homes->workspaces[transfer.borrowed_owner_workspace];
    if (!target_workspace || stage->kind != XR_LIR_HOME_WORKSPACE_VIEW_BORROW_STAGE ||
        stage->type != target->type || stage->value != transfer.source || stage->block != edge->block ||
        !stage->instruction || stage->first_owner >= homes->frame.slot_count ||
        stage->first_owner == target_workspace->first_owner || stage->first_owner == target_workspace->temporary_first_owner ||
        stage->offset % _Alignof(XrtManagedHeader *) || stage->offset > homes->frame.payload_size ||
        sizeof(XrtManagedHeader *) > homes->frame.payload_size - stage->offset ||
        homes->frame.owned_offsets[stage->first_owner] != stage->offset ||
        stage->temporary_offset != SIZE_MAX || stage->temporary_first_owner != SIZE_MAX) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice edge stage is not an independent Header owner"); return NULL;
    }
    return stage;
}
static void c_view_home(LirC *c, const XrLirHomeWorkspace *workspace) {
    c_format(c, "&(XrtInlineValueHome){xrl_frame, (unsigned char *)xrl_payload + %zu,\n", workspace->offset);
    c_format(c, "        (unsigned char *)xrl_payload + %zu, %zu, %zu}",
        workspace->temporary_offset, workspace->first_owner, workspace->temporary_first_owner);
}
static bool c_view_event(LirC *c, const XrLirInstructionView *v, XrLirSliceOperation operation,
    uint32_t value, uint32_t workspace, uint32_t target) {
    (void)v;
    bool ending = operation == XR_LIR_SLICE_OP_END || operation == XR_LIR_SLICE_OP_CLEANUP ||
        operation == XR_LIR_SLICE_OP_FORMAL_CLEANUP;
    while (c->view_event_cursor < c->homes->event_count && c_request_poll(c)) {
        const XrLirHomeEvent *event = &c->homes->events[c->view_event_cursor];
        if (event->block > c->block || (event->block == c->block && event->instruction > c->instruction)) break;
        ++c->view_event_cursor;
        if (event->block != c->block || event->instruction != c->instruction ||
            event->kind != (uint32_t)(ending ? XR_LIR_OWNERSHIP_VIEW_END : XR_LIR_OWNERSHIP_VIEW_VALUE)) continue;
        if (event->source == value && event->view_operation == (uint32_t)operation &&
            event->view_workspace == workspace &&
            event->view_storage == c->homes->values[value].view_storage &&
            (event->view_storage != XR_LIR_SLICE_STORAGE_BORROWED_FORMAL ||
                event->view_parameter == c->homes->values[value].view_parameter) &&
            event->transfer_mode == (ending ? XR_LIR_OWNERSHIP_TRANSFER_NONE : XR_LIR_OWNERSHIP_TRANSFER_BORROW) &&
            event->hold_transfer_mode == (ending ? XR_LIR_OWNERSHIP_TRANSFER_NONE : XR_LIR_OWNERSHIP_TRANSFER_COPY) &&
            event->target == target) return true;
    }
    c_error(c, XR_LIR_C_INVALID_LIR, "Slice operation has no matching final loan event"); return false;
}
static void c_view_cleanup(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (!c->homes || v->type != XR_LIR_UNIT || v->result || v->operand_count ||
        !v->bits || v->bits > c->homes->value_count) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice cleanup has no exact local resource target"); return;
    }
    uint32_t target = (uint32_t)v->bits;
    const XrLirValueHome *home = &c->homes->values[target];
    if (v->op == XR_LIR_SLICE_FORMAL_CLEANUP) {
        if (home->view_storage != XR_LIR_SLICE_STORAGE_BORROWED_FORMAL ||
            !c_view_value_valid(c, c->homes, target) ||
            !c_view_event(c, v, XR_LIR_SLICE_OP_FORMAL_CLEANUP, target, 0, 0))
            c_error(c, XR_LIR_C_INVALID_LIR, "Slice formal cleanup lost its physical no-op event");
        return;
    }
    const XrLirHomeWorkspace *workspace = c_view_workspace(c, c->homes, home->view_workspace);
    if (!workspace) return;
    if (home->parameter || home->owner_count || home->first_owner != SIZE_MAX ||
        workspace->type != home->type || !c_slice_type(c, c_value_type(c, target))) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice cleanup target disagrees with its keeper workspace"); return;
    }
    if (!c_view_event(c, v, XR_LIR_SLICE_OP_CLEANUP, target, home->view_workspace, 0)) return;
    /* A resource target can be unconstructed on this path. Inspect only the
     * common storage metadata; do not evaluate its natural Slice bytes or
     * allocate a Frame solely to retire a keeper that was never published. */
    c_text(c, "    if (xrl_frame) {\n    XrtManagedStatus status = xrt_array_slice_clear(");
    c_view_home(c, workspace); c_text(c, ");\n"); c_array_require(c, source); c_text(c, "    }\n");
}
static void c_view_bind(LirC *c, const XrLirInstructionView *v,
    const XrLirSliceIntrinsic *shape, const XrLirHomeWorkspace *workspace, uint32_t source) {
    const char *kind = c_array_kind(shape->element);
    /* All allocating setup precedes the checked binding borrow. The receiver
     * then retains into a true temporary without an allocating callback. */
    c_values_ensure(c, source);
    c_text(c, "    { XrtManagedStatus status;\n");
    if (shape->operation == XR_LIR_SLICE_OP_NEW) {
        c_text(c, "    const XrtManagedHeader *owner = NULL;\n    status = xrt_place_borrow(");
        c_value(c, v->operands[0]); c_text(c, ", &owner);\n"); c_array_require(c, source);
        c_text(c, "    status = xrt_array_slice_range_bind("); c_view_home(c, workspace);
        c_format(c, ", %s, (const XrtArrayBacking *)owner, ", kind);
        c_value(c, v->operands[1]); c_text(c, ", "); c_value(c, v->operands[2]);
    } else if (shape->operation == XR_LIR_SLICE_OP_REBORROW) {
        c_text(c, "    status = xrt_array_slice_range_reborrow("); c_view_home(c, workspace);
        c_format(c, ", %s, &", kind); c_value(c, v->operands[0]);
        for (uint32_t a = 1; a < v->operand_count; ++a) { c_text(c, ", "); c_value(c, v->operands[a]); }
    } else {
        if (!c_view_value_valid(c, c->homes, v->operands[0])) return;
        /* The common loan grants the read; the fixed backing prefix supplies
         * only its physical keepalive, never a second permission check. */
        c_text(c, "    const XrtReadonlyArraySlice *parent = &"); c_value(c, v->operands[0]);
        c_text(c, ";\n    XrtArraySliceBundle incoming = {*parent, (XrtManagedHeader *)&parent->backing->managed};\n");
        c_text(c, "    XrtInlineValueSpec spec = xrt_array_slice_bundle_spec();\n");
        c_text(c, "    status = xrt_inline_value_replace("); c_view_home(c, workspace);
        c_text(c, ", &spec, &incoming");
    }
    c_text(c, ");\n"); c_array_require(c, source); c_text(c, "    }\n");
}
static void c_view_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (v->op == XR_LIR_SLICE_CLEANUP || v->op == XR_LIR_SLICE_FORMAL_CLEANUP) {
        c_view_cleanup(c, v, source); return;
    }
    XrLirSliceIntrinsic shape = {0};
    if (!c_view_intrinsic(c, v, &shape) || (shape.element != XR_LIR_I64 && shape.element != XR_LIR_U8) || !c->homes) {
        c_error(c, XR_LIR_C_UNSUPPORTED, "Slice role has no common native loan receiver"); return;
    }
    bool binding = shape.operation == XR_LIR_SLICE_OP_NEW || shape.operation == XR_LIR_SLICE_OP_REBORROW ||
        shape.operation == XR_LIR_SLICE_OP_BIND;
    uint32_t value = binding ? v->result : v->operands[0];
    if (!c_view_value_valid(c, c->homes, value)) return;
    const XrLirValueHome *home = &c->homes->values[value];
    const XrLirHomeWorkspace *workspace = home->view_workspace ?
        c_view_workspace(c, c->homes, home->view_workspace) : NULL;
    if (binding || shape.operation == XR_LIR_SLICE_OP_END) {
        if (!c_view_event(c, v, shape.operation, value, c->homes->values[value].view_workspace,
            binding ? v->operands[0] : 0)) return;
        if (binding) { c_view_bind(c, v, &shape, workspace, source); return; }
        if (home->view_storage == XR_LIR_SLICE_STORAGE_BORROWED_FORMAL) return;
        c_text(c, "    { XrtManagedStatus status = xrt_array_slice_clear("); c_view_home(c, workspace);
        c_text(c, ");\n"); c_array_require(c, source); c_text(c, "    }\n"); return;
    }
    if (shape.operation == XR_LIR_SLICE_OP_COPY) {
        const XrLirValueHome *result = &c->homes->values[v->result];
        if (result->owner_count != 1 || result->temporary_first_owner == SIZE_MAX) {
            c_error(c, XR_LIR_C_INVALID_LIR, "Slice copy has no true Array result staging"); return;
        }
        c_values_ensure(c, source);
        c_format(c, "    xrt_native_array_slice_copy(xrl_frame, %zu, %s, &", result->temporary_first_owner, c_array_kind(shape.element));
        c_value(c, value); c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        c_values_publish_leaf(c, v->result, source); return;
    }
    if (shape.operation == XR_LIR_SLICE_OP_LEN) {
        c_text(c, "    { XrtManagedStatus status = xrt_array_slice_length(&"); c_value(c, value);
        c_format(c, ", &v%" PRIu32 ");\n", v->result); c_array_require(c, source); c_text(c, "    }\n"); return;
    }
    if (shape.operation == XR_LIR_SLICE_OP_GET) {
        c_text(c, "    { uint64_t bits = 0;\n    XrtManagedStatus status = xrt_array_slice_read_bits(&"); c_value(c, value);
        c_format(c, ", %s, ", c_array_kind(shape.element)); c_value(c, v->operands[1]); c_text(c, ", &bits);\n");
        c_array_require(c, source);
        if (shape.element == XR_LIR_I64) c_format(c, "    memcpy(&v%" PRIu32 ", &bits, sizeof(bits));\n", v->result);
        else c_format(c, "    v%" PRIu32 " = (uint8_t)bits;\n", v->result);
        c_text(c, "    }\n"); return;
    }
    c_error(c, XR_LIR_C_UNSUPPORTED, "Slice operation has no target execution receiver");
}
static void c_view_call_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    const XrLirHomeCall *call = NULL;
    if (!c->homes || !c->value_homes || !v->result || !v->callee ||
        v->callee > xr_lir_function_count(c->request->module)) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice call has no common receiving boundary"); return;
    }
    while (c->view_call_cursor < c->homes->call_count) {
        const XrLirHomeCall *candidate = &c->homes->calls[c->view_call_cursor];
        if (candidate->block > c->block || (candidate->block == c->block && candidate->instruction > c->instruction)) break;
        ++c->view_call_cursor;
        if (candidate->block == c->block && candidate->instruction == c->instruction) { call = candidate; break; }
    }
    const XrLirValueHomes *callee = c->value_homes[v->callee - 1];
    const XrLirHomeWorkspace *workspace = c_view_value_workspace(c, c->homes, v->result);
    if (!call || !callee || !workspace || call->callee != v->callee || call->result != v->result ||
        call->count != v->operand_count || call->first > c->homes->argument_count ||
        call->count > c->homes->argument_count - call->first || (call->count && !c->homes->arguments) ||
        callee->result_type != c->homes->values[v->result].type ||
        call->view_result_workspace != c->homes->values[v->result].view_workspace ||
        call->view_result_parameter >= v->operand_count || call->view_result_parameter != callee->view_result_parameter ||
        call->view_result_source_argument != v->operands[call->view_result_parameter] ||
        !c_view_value_valid(c, c->homes, call->view_result_source_argument) ||
        !c_view_event(c, v, XR_LIR_SLICE_OP_CALL, v->result, call->view_result_workspace,
            call->view_result_source_argument)) {
        c_error(c, XR_LIR_C_INVALID_LIR, "Slice call lost its exact caller landing and formal origin"); return;
    }
    for (uint32_t a = 0; a < v->operand_count; ++a)
        if (c->homes->arguments[call->first + a] != v->operands[a]) {
            c_error(c, XR_LIR_C_INVALID_LIR, "Slice call arguments disagree with their final homes"); return;
        }
    /* Common loan facts establish permission before this physical receiver is
     * used. Preparing the landing cannot retire the caller's live source. */
    c_values_ensure(c, source);
    c_text(c, "    "); c_function_name(c, v->callee);
    c_format(c, "((XrlLanding_%" PRIu32 "){xrl_frame, %zu, %zu, ",
        v->callee, workspace->first_owner, workspace->temporary_first_owner);
    c_format(c, "(void *)((unsigned char *)xrl_payload + %zu), (void *)((unsigned char *)xrl_payload + %zu)}",
        workspace->offset, workspace->temporary_offset);
    for (uint32_t a = 0; a < v->operand_count && c_request_poll(c); ++a) {
        c_text(c, ", "); c_value(c, v->operands[a]);
    }
    c_text(c, ");\n");
}
