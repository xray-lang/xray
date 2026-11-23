/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_set.inc.c - Fixed I64 Set consumers of common homes and transactions
 */
static bool c_set_point(LirC *c,const XrLirInstructionView *v) {
    return (v->op>=XR_LIR_SET_NEW && v->op<=XR_LIR_SET_REF_WRITE) ||
        (v->op==XR_LIR_CELL_NEW && v->type==XR_LIR_SET_I64_CELL) ||
        (v->op==XR_LIR_CELL_READ && v->type==XR_LIR_SET_I64) ||
        (v->op==XR_LIR_CELL_WRITE && c_value_type(c,v->operands[0])==XR_LIR_SET_I64_CELL);
}
static bool c_set_operation_event(LirC *c,const XrLirInstructionView *v) {
    uint32_t receiver=v->op==XR_LIR_SET_NEW?0:v->operands[0];
    while (c->homes && c->set_event_cursor<c->homes->event_count && c_request_poll(c)) {
        const XrLirHomeEvent *event=&c->homes->events[c->set_event_cursor++];
        if (event->kind!=XR_LIR_OWNERSHIP_SET_OPERATION) continue;
        if (event->block<c->block || (event->block==c->block && event->instruction<c->instruction)) continue;
        if (event->block==c->block && event->instruction==c->instruction &&
            event->source==receiver && event->operation==(uint32_t)v->op && !event->target &&
            event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE) return true;
        break;
    }
    c_error(c,XR_LIR_C_INVALID_LIR,"Set operation has no matching common ownership event");return false;
}
static const XrLirHomeWorkspace *c_set_workspace(LirC *c,uint32_t receiver) {
    if (!c->homes || !c->homes->bindings || receiver>c->homes->value_count) return NULL;
    const XrLirHomeBinding *binding=&c->homes->bindings[receiver];
    const XrtValuePlan *plan=NULL;
    uint32_t set_type=0;
    if (xr_lir_value_layouts_lookup(c->value_layouts,(XrLirValueLayoutKey){XR_LIR_SET_I64,0,0},&plan,&set_type)!=
        XR_LIR_VALUE_LAYOUT_OK || binding->type!=set_type) return NULL;
    for (size_t n=0;n<c->homes->workspace_count && c_request_poll(c);++n) {
        const XrLirHomeWorkspace *work=&c->homes->workspaces[n];
        const XrtManagedFrameSpec *frame=&c->homes->frame;
        if (work->block!=c->block || work->instruction!=c->instruction) continue;
        if (work->kind!=XR_LIR_HOME_WORKSPACE_VALUE || work->value || work->type!=binding->type ||
            !c->homes->owned_frame_needed || !frame->owned_offsets || work->first_owner>=frame->slot_count ||
            work->first_owner==binding->first_owner || work->offset%_Alignof(XrtManagedHeader *) ||
            work->offset>frame->payload_size || sizeof(XrtManagedHeader *)>frame->payload_size-work->offset ||
            frame->owned_offsets[work->first_owner]!=work->offset) return NULL;
        return work;
    }
    return NULL;
}
static void c_set_operation(LirC *c,const XrLirInstructionView *v,uint32_t source) {
    if (!c_set_operation_event(c,v)) return;
    if (v->op==XR_LIR_SET_NEW) {
        const XrLirValueHome *home=&c->homes->values[v->result];
        c_values_ensure(c,source);
        c_format(c,"    xrt_native_set_create(xrl_frame, %zu, &(XrtSetInput){NULL, 0}, UINT32_C(%" PRIu32 "));\n",
            home->temporary_first_owner,source);c_values_publish_leaf(c,v->result,source);return;
    }
    if (v->op==XR_LIR_SET_LEN) {
        c_format(c,"    v%" PRIu32 " = (int64_t)((const XrtSetBacking *)",v->result);
        c_value(c,v->operands[0]);c_text(c,")->length;\n");return;
    }
    if (v->op==XR_LIR_SET_CONTAINS) {
        c_format(c,"    v%" PRIu32 " = xrt_native_set_contains(",v->result);
        c_value(c,v->operands[0]);c_text(c,", ");c_value(c,v->operands[1]);
        c_format(c,", UINT32_C(%" PRIu32 "));\n",source);return;
    }
    if (v->op==XR_LIR_SET_DISPLAY) {
        c_values_ensure(c,source);
        c_format(c,"    xrt_native_set_display(xrl_frame, %zu, ",c->homes->values[v->result].temporary_first_owner);
        c_value(c,v->operands[0]);
        c_format(c,", UINT32_C(%" PRIu32 "));\n",source);
        c_values_publish_leaf(c,v->result,source);return;
    }
    const XrLirHomeWorkspace *workspace=c_set_workspace(c,v->operands[0]);
    if (!workspace) {c_error(c,XR_LIR_C_INVALID_LIR,"Set mutation lost its actual common binding workspace");return;}
    c_values_ensure(c,source);c_text(c,"    { bool removed = false;\n    xrt_native_set_update_place(");
    c_value(c,v->operands[0]);c_format(c,", xrl_frame, %zu, &(XrtSetUpdate){%s, ",workspace->first_owner,
        v->op==XR_LIR_SET_ADD?"XRT_SET_UPDATE_ADD":v->op==XR_LIR_SET_REMOVE?"XRT_SET_UPDATE_REMOVE":"XRT_SET_UPDATE_CLEAR");
    if (v->op==XR_LIR_SET_CLEAR) c_text(c,"INT64_C(0)");else c_value(c,v->operands[1]);
    c_format(c,"}, &removed, UINT32_C(%" PRIu32 "));\n",source);
    if (v->op==XR_LIR_SET_REMOVE) c_format(c,"    v%" PRIu32 " = removed;\n",v->result);
    c_text(c,"    }\n");
}
static void c_set_instruction(LirC *c,const XrLirInstructionView *v,uint32_t source) {
    if (v->op>=XR_LIR_SET_NEW && v->op<=XR_LIR_SET_CLEAR) {c_set_operation(c,v,source);return;}
    bool copy=v->op==XR_LIR_SET_REF_WRITE || (v->op==XR_LIR_CELL_NEW && v->type==XR_LIR_SET_I64_CELL) ||
        (v->op==XR_LIR_CELL_WRITE && c_value_type(c,v->operands[0])==XR_LIR_SET_I64_CELL);
    if (copy && c->homes->values[v->operands[v->operand_count-1]].parameter) {
        c_collection_binding_stage_copy(c,v->op==XR_LIR_CELL_NEW?v->result:v->operands[0],
            v->operands[v->operand_count-1],source);return;
    }
    if (v->op!=XR_LIR_SET_BORROW) c_values_ensure(c,source);
    if (v->op==XR_LIR_SET_BORROW) {
        const XrLirHomeBinding *binding=&c->homes->bindings[v->result];
        if (binding->kind==XR_LIR_HOME_BINDING_STATE_BORROW) {
            c_text(c,"    { XrtManagedStatus status = xrt_module_external_owner(");c_value(c,binding->source);
            c_format(c,", 0, &xrl_array_binding%" PRIu32 ");\n",v->result);c_array_require(c,source);c_text(c,"    }\n");
            c_format(c,"    v%" PRIu32 " = (XrtPlace){.external=&xrl_array_binding%" PRIu32 ", .nulls=XRT_PLACE_NONNULL};\n",v->result,v->result);
        } else {c_format(c,"    v%" PRIu32 " = ",v->result);c_value(c,binding->source);c_text(c,";\n");}
    } else if (v->op==XR_LIR_SET_RETAIN) {
        c_text(c,"    { XrtManagedHeader *owner = ");c_value(c,v->operands[0]);c_array_copy_end(c,v->result,source);
    } else if (v->op==XR_LIR_SET_DROP || v->op==XR_LIR_SET_CELL_DROP) {
        size_t slot=v->op==XR_LIR_SET_DROP?c->homes->values[v->operands[0]].first_owner:
            c->homes->bindings[v->operands[0]].first_owner;
        c_format(c,"    { XrtManagedStatus status = xrt_managed_slot_drop(xrl_frame, %zu);\n",slot);
        c_array_require(c,source);c_text(c,"    }\n");
    } else {
        bool read=v->op==XR_LIR_CELL_READ || v->op==XR_LIR_SET_REF_READ;
        const XrLirValueHome *home=&c->homes->values[read?v->result:v->operands[v->operand_count-1]];
        if (!read && (!home->owner_count || home->parameter)) {
            c_error(c,XR_LIR_C_INVALID_LIR,"Set assignment lost its real owning source home");return;
        }
        c_text(c,"    { XrtManagedStatus status = xrt_place_copy_replace(");
        if (read) c_format(c,"(XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NULLABLE}",home->first_owner);
        else c_value(c,v->op==XR_LIR_CELL_NEW?v->result:v->operands[0]);
        c_text(c,", ");
        if (read) c_value(c,v->operands[0]);
        else c_format(c,"(XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NONNULL}",home->first_owner);
        c_text(c,", NULL);\n");c_array_require(c,source);c_text(c,"    }\n");
    }
}
