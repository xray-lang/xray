/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_array_forwarding.inc.c - Receive exact adjacent logical handoffs
 */
typedef struct LirCArrayForward {
    size_t source, source_offset, destination, destination_offset;
} LirCArrayForward;

static bool c_array_forward_geometry(LirC *c, const XrLirHomeEvent *event,
    const XrLirInstructionView *op, LirCArrayForward *output) {
    const XrLirValueHomes *homes=c->homes;
    bool create=event->kind==XR_LIR_OWNERSHIP_BINDING_NEW;
    if (homes->source_function!=c->function.id || (!create && event->kind!=XR_LIR_OWNERSHIP_BINDING_READ) ||
        event->forwarding_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE || event->transfer_mode ||
        !event->source || !event->target || event->source>homes->value_count || event->target>homes->value_count ||
        event->instruction==UINT32_MAX || event->forwarding_drop!=event->instruction+1 ||
        !homes->owned_frame_needed || !homes->bindings || !homes->frame.owned_offsets || !c->value_types ||
        c_value_type(c,event->source)!=XR_LIR_ARRAY_CELL || c_value_type(c,event->target)!=XR_LIR_ARRAY ||
        op->operand_count!=1 || op->op!=(create?XR_LIR_CELL_NEW:XR_LIR_CELL_READ) ||
        op->type!=(create?XR_LIR_ARRAY_CELL:XR_LIR_ARRAY) ||
        op->result!=(create?event->source:event->target) || op->operands[0]!=(create?event->target:event->source)) return false;
    XrLirArrayShape shape={0};
    if (xr_lir_array_shape_view(c->request->module,(XrLirValue){c->request->module,c->function.id,event->source},&shape)!=XR_LIR_OK ||
        shape.element!=XR_LIR_I64 ||
        xr_lir_array_shape_view(c->request->module,(XrLirValue){c->request->module,c->function.id,event->target},&shape)!=XR_LIR_OK ||
        shape.element!=XR_LIR_I64) return false;
    XrLirInstructionView drop={0};
    if (xr_lir_instruction_view(c->request->module,(XrLirBlock){c->request->module,c->function.id,event->block},
        event->forwarding_drop-1,&drop)!=XR_LIR_OK || drop.op!=(create?XR_LIR_ARRAY_DROP:XR_LIR_ARRAY_CELL_DROP) ||
        drop.operand_count!=1 || drop.operands[0]!=(create?event->target:event->source)) return false;
    const XrLirHomeBinding *binding=&homes->bindings[event->source];
    const XrLirValueHome *value=&homes->values[event->target];
    if (binding->kind!=XR_LIR_HOME_BINDING_LOCAL || !binding->type || binding->type!=value->type ||
        binding->type>c->value_types->count || value->parameter || value->owner_count!=1 ||
        value->size!=sizeof(XrtManagedHeader *) || value->byte_count || value->transport || value->cell_transport ||
        value->view_workspace || value->view_storage) return false;
    const XrtValuePlanSpec *spec=&c->value_types->rows[binding->type-1].spec;
    if (spec->kind!=XRT_VALUE_ARRAY_I64 || spec->size!=sizeof(XrtManagedHeader *) ||
        spec->alignment!=_Alignof(XrtManagedHeader *) ||
        spec->owned_count!=1 || !spec->owned_offsets || spec->owned_offsets[0]) return false;
    size_t slots[]={binding->first_owner,value->first_owner}, offsets[]={binding->offset,value->offset};
    for (size_t n=0;n<2;++n)
        if (slots[n]>=homes->frame.slot_count || offsets[n]>homes->frame.payload_size ||
            sizeof(XrtManagedHeader *)>homes->frame.payload_size-offsets[n] ||
            offsets[n]%_Alignof(XrtManagedHeader *) || homes->frame.owned_offsets[slots[n]]!=offsets[n]) return false;
    if (slots[0]==slots[1] || offsets[0]==offsets[1]) return false;
    *output=create ? (LirCArrayForward){slots[1],offsets[1],slots[0],offsets[0]} :
        (LirCArrayForward){slots[0],offsets[0],slots[1],offsets[1]};
    return true;
}
static bool c_array_forward_begin(LirC *c, const XrLirInstructionView *op) {
    if (!c->array_forwarding || (op->op!=XR_LIR_CELL_NEW && op->op!=XR_LIR_CELL_READ)) return false;
    while (c->array_forward_cursor<c->homes->event_count) {
        const XrLirHomeEvent *event=&c->homes->events[c->array_forward_cursor];
        /* Only this producer's flagged substream is position ordered. Ignore
         * unrelated events before comparing positions, without rescanning. */
        if (!event->forwarding_mode) { ++c->array_forward_cursor; continue; }
        if (event->block>c->block || (event->block==c->block && event->instruction>c->instruction)) break;
        ++c->array_forward_cursor;
        if (event->block!=c->block || event->instruction!=c->instruction) continue;
        LirCArrayForward transfer={0};
        if (!c_array_forward_geometry(c,event,op,&transfer)) return false;
        c_format(c,"    xrl_array_forwarded = xrt_managed_frame_try_forward(xrl_frame, xrl_payload, &xrl_frame_locations, %zu, %zu, %zu, %zu);\n",
            transfer.source,transfer.source_offset,transfer.destination,transfer.destination_offset);
        c_text(c,"    if (!xrl_array_forwarded) {\n");
        c->array_forward_pending=event;
        return true;
    }
    return false;
}
static bool c_array_forward_drop(LirC *c, const XrLirInstructionView *op) {
    const XrLirHomeEvent *event=c->array_forward_pending;
    if (!event) return false;
    c->array_forward_pending=NULL;
    bool create=event->kind==XR_LIR_OWNERSHIP_BINDING_NEW;
    if (event->block!=c->block || event->forwarding_drop!=c->instruction || op->operand_count!=1 ||
        op->op!=(create?XR_LIR_ARRAY_DROP:XR_LIR_ARRAY_CELL_DROP) ||
        op->operands[0]!=(create?event->target:event->source)) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Array forwarding lost its adjacent logical release"); return false;
    }
    /* The preceding complete COPY or successful forwarding already established
     * the Frame. Do not insert a lazy constructor between the paired events. */
    c_text(c,"    if (!xrl_array_forwarded) {\n");
    return true;
}
