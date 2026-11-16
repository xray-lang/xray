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
