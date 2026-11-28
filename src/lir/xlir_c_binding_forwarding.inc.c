/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_binding_forwarding.inc.c - Receive exact adjacent logical handoffs
 */
typedef struct LirCBindingForward {
    size_t source, source_offset, destination, destination_offset;
} LirCBindingForward;

static XrLirOp c_binding_forward_release(LirC *c, const XrLirHomeEvent *event) {
    bool create=event->kind==XR_LIR_OWNERSHIP_BINDING_NEW;
    if (c_value_type(c,event->source)==XR_LIR_MAP_STRING_I64_CELL)
        return create ? XR_LIR_MAP_DROP : XR_LIR_MAP_CELL_DROP;
    return create ? XR_LIR_ARRAY_DROP : XR_LIR_ARRAY_CELL_DROP;
}

static bool c_binding_forward_geometry(LirC *c, const XrLirHomeEvent *event,
    const XrLirInstructionView *op, LirCBindingForward *output) {
    const XrLirValueHomes *homes=c->homes;
    if (!event->source || !event->target || event->source>homes->value_count || event->target>homes->value_count)
        return false;
    bool create=event->kind==XR_LIR_OWNERSHIP_BINDING_NEW;
    XrLirOp copy_op=create?XR_LIR_CELL_NEW:XR_LIR_CELL_READ;
    XrLirType cell_type=c_value_type(c,event->source);
    bool map=cell_type==XR_LIR_MAP_STRING_I64_CELL;
    XrLirType value_type=map ? XR_LIR_MAP_STRING_I64 : XR_LIR_ARRAY;
    XrLirType copy_type=create ? cell_type : value_type;
    if (homes->source_function!=c->function.id || (!create && event->kind!=XR_LIR_OWNERSHIP_BINDING_READ) ||
        event->forwarding_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE || event->transfer_mode ||
        event->instruction==UINT32_MAX || event->forwarding_drop!=event->instruction+1 ||
        !homes->owned_frame_needed || !homes->bindings || !homes->frame.owned_offsets || !c->value_types ||
        (!map && cell_type!=XR_LIR_ARRAY_CELL) || c_value_type(c,event->target)!=value_type ||
        op->operand_count!=1 || op->op!=copy_op || op->type!=copy_type ||
        op->result!=(create?event->source:event->target) || op->operands[0]!=(create?event->target:event->source)) return false;
    XrLirArrayShape cell_shape={0},value_shape={0};
    if (!map && (xr_lir_array_shape_view(c->request->module,
        (XrLirValue){c->request->module,c->function.id,event->source},&cell_shape)!=XR_LIR_OK ||
        xr_lir_array_shape_view(c->request->module,
        (XrLirValue){c->request->module,c->function.id,event->target},&value_shape)!=XR_LIR_OK ||
        cell_shape.element!=value_shape.element)) return false;
    XrLirInstructionView drop={0};
    if (xr_lir_instruction_view(c->request->module,(XrLirBlock){c->request->module,c->function.id,event->block},
        event->forwarding_drop-1,&drop)!=XR_LIR_OK || drop.op!=c_binding_forward_release(c,event) ||
        drop.operand_count!=1 || drop.operands[0]!=(create?event->target:event->source)) return false;
    const XrLirHomeBinding *binding=&homes->bindings[event->source];
    const XrLirValueHome *value=&homes->values[event->target];
    if (binding->kind!=XR_LIR_HOME_BINDING_LOCAL || binding->parameter || binding->source ||
        !binding->type || binding->type!=value->type ||
        binding->type>c->value_types->count || value->parameter || value->borrow_source || value->owner_count!=1 ||
        value->size!=sizeof(XrtManagedHeader *) || value->byte_count || value->transport || value->cell_transport ||
        value->view_workspace || value->view_storage) return false;
    const XrtValuePlanSpec *spec=&c->value_types->rows[binding->type-1].spec;
    if ((map ? spec->kind!=XRT_VALUE_MAP_STRING_I64 : !xrt_value_kind_is_array(spec->kind)) ||
        spec->size!=sizeof(XrtManagedHeader *) ||
        spec->alignment!=_Alignof(XrtManagedHeader *) ||
        spec->owned_count!=1 || !spec->owned_offsets || spec->owned_offsets[0]) return false;
    size_t slots[]={binding->first_owner,value->first_owner}, offsets[]={binding->offset,value->offset};
    for (size_t n=0;n<2;++n)
        if (slots[n]>=homes->frame.slot_count || offsets[n]>homes->frame.payload_size ||
            sizeof(XrtManagedHeader *)>homes->frame.payload_size-offsets[n] ||
            offsets[n]%_Alignof(XrtManagedHeader *) || homes->frame.owned_offsets[slots[n]]!=offsets[n]) return false;
    if (slots[0]==slots[1] || offsets[0]==offsets[1]) return false;
    *output=create ? (LirCBindingForward){slots[1],offsets[1],slots[0],offsets[0]} :
        (LirCBindingForward){slots[0],offsets[0],slots[1],offsets[1]};
    return true;
}
static bool c_binding_forward_begin(LirC *c, const XrLirInstructionView *op) {
    if (!c->binding_forwarding || (op->op!=XR_LIR_CELL_NEW && op->op!=XR_LIR_CELL_READ)) return false;
    while (c->binding_forward_cursor<c->homes->event_count) {
        const XrLirHomeEvent *event=&c->homes->events[c->binding_forward_cursor];
        /* Only this producer's flagged substream is position ordered. Ignore
         * unrelated events before comparing positions, without rescanning. */
        if (!event->forwarding_mode) { ++c->binding_forward_cursor; continue; }
        if (event->block>c->block || (event->block==c->block && event->instruction>c->instruction)) break;
        ++c->binding_forward_cursor;
        if (event->block!=c->block || event->instruction!=c->instruction) continue;
        LirCBindingForward transfer={0};
        if (!c_binding_forward_geometry(c,event,op,&transfer)) return false;
        c_format(c,"    xrl_binding_forwarded = xrt_managed_frame_try_forward(xrl_frame, xrl_payload, &xrl_frame_locations, (XrtManagedFrameForward){%zu, %zu, %zu, %zu});\n",
            transfer.source,transfer.source_offset,transfer.destination,transfer.destination_offset);
        c_text(c,"    if (!xrl_binding_forwarded) {\n");
        c->binding_forward_pending=event;
        return true;
    }
    return false;
}
static bool c_binding_forward_drop(LirC *c, const XrLirInstructionView *op) {
    const XrLirHomeEvent *event=c->binding_forward_pending;
    if (!event) return false;
    c->binding_forward_pending=NULL;
    bool create=event->kind==XR_LIR_OWNERSHIP_BINDING_NEW;
    if (event->block!=c->block || event->forwarding_drop!=c->instruction || op->operand_count!=1 ||
        op->op!=c_binding_forward_release(c,event) ||
        op->operands[0]!=(create?event->target:event->source)) {
        c_error(c,XR_LIR_C_INVALID_LIR,"binding forwarding lost its adjacent logical release"); return false;
    }
    /* The preceding complete COPY or successful forwarding already established
     * the Frame. Do not insert a lazy constructor between the paired events. */
    c_text(c,"    if (!xrl_binding_forwarded) {\n");
    return true;
}
