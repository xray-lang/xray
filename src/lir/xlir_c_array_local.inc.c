/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_array_local.inc.c - Pointwise scalar edits after real cold setup
 */
static bool c_array_local_candidate(LirC *c, const XrLirValueHomes *homes, const XrLirHomeEvent *event) {
    if (!homes || !homes->bindings || !c->value_types || event->kind!=XR_LIR_OWNERSHIP_ARRAY_OPERATION ||
        (event->operation!=XR_LIR_ARRAY_OP_SET && event->operation!=XR_LIR_ARRAY_OP_PUSH) ||
        !event->source || event->source>homes->value_count) return false;
    const XrLirHomeBinding *binding=&homes->bindings[event->source];
    if (binding->kind!=XR_LIR_HOME_BINDING_LOCAL || !binding->type || binding->type>c->value_types->count) return false;
    const XrtValuePlanSpec *spec=&c->value_types->rows[binding->type-1].spec;
    XrLirValue value={c->request->module,homes->source_function,event->source};
    XrLirType type=0; XrLirArrayShape shape={0};
    return spec->kind==XRT_VALUE_ARRAY_I64 && spec->size==sizeof(XrtManagedHeader *) &&
        spec->alignment==_Alignof(XrtManagedHeader *) && spec->owned_count==1 && spec->owned_offsets &&
        !spec->owned_offsets[0] && xr_lir_value_type(c->request->module,value,&type)==XR_LIR_OK &&
        type==XR_LIR_ARRAY_CELL && xr_lir_array_shape_view(c->request->module,value,&shape)==XR_LIR_OK && shape.element==XR_LIR_I64;
}
static bool c_array_local_function(LirC *c, const XrLirValueHomes *homes) {
    for (size_t n=0;homes && n<homes->event_count;++n)
        if (c_array_local_candidate(c,homes,&homes->events[n])) return true;
    return false;
}
static void c_array_local_ready(LirC *c) {
    /* Emit only after an actual full create/repeat/update and its required
     * publication returned. Frame creation and reference transfers do not seed
     * this execution-initialization fact. */
    if (c->array_local) c_text(c,"    xrl_array_ready = true;\n");
}
static void c_array_local_invalidate(LirC *c, const XrLirInstructionView *op) {
    if (!c->array_local) return;
    bool unknown=op->op==XR_LIR_CALL || op->op==XR_LIR_CALL_INDIRECT;
    if (op->op==XR_LIR_CALL_INTRINSIC) {
        XrLirArrayIntrinsic shape={0};
        unknown=!c_array_intrinsic(c,op,&shape) || shape.operation==XR_LIR_ARRAY_OP_DISPLAY;
    }
    /* Clearing before the boundary also covers exceptional return edges.
     * No source pointer, uniqueness result or capacity is cached with it. */
    if (unknown) c_text(c,"    xrl_array_ready = false;\n");
}
static bool c_array_local_begin(LirC *c, const XrLirInstructionView *op,
    const XrLirArrayIntrinsic *shape, const XrLirHomeWorkspace *workspace) {
    if (!c->array_local || c->homes->source_function!=c->function.id) return false;
    const XrLirHomeEvent *event=NULL;
    while (c->array_local_event_cursor<c->homes->event_count) {
        const XrLirHomeEvent *row=&c->homes->events[c->array_local_event_cursor];
        if (row->kind!=XR_LIR_OWNERSHIP_ARRAY_OPERATION) { ++c->array_local_event_cursor; continue; }
        if (row->block>c->block || (row->block==c->block && row->instruction>c->instruction)) break;
        ++c->array_local_event_cursor;
        if (row->block==c->block && row->instruction==c->instruction) { event=row; break; }
    }
    if (!event || !c_array_local_candidate(c,c->homes,event) || event->operation!=(uint32_t)shape->operation ||
        op->op!=XR_LIR_CALL_INTRINSIC || !op->operand_count || event->source!=op->operands[0] || op->type!=XR_LIR_UNIT || op->result ||
        shape->parameter_count!=op->operand_count || !shape->parameter_elements || shape->parameter_elements[0]!=XR_LIR_I64 ||
        (shape->operation==XR_LIR_ARRAY_OP_SET && op->operand_count!=3) ||
        (shape->operation==XR_LIR_ARRAY_OP_PUSH && op->operand_count!=2) ||
        (shape->operation==XR_LIR_ARRAY_OP_SET && c_value_type(c,op->operands[1])!=XR_LIR_I64) ||
        c_value_type(c,op->operands[op->operand_count-1])!=XR_LIR_I64) return false;
    const XrLirHomeBinding *binding=&c->homes->bindings[event->source];
    if (workspace->kind!=XR_LIR_HOME_WORKSPACE_VALUE || workspace->value || workspace->type!=binding->type ||
        workspace->block!=c->block || workspace->instruction!=c->instruction ||
        binding->first_owner>=c->homes->frame.slot_count || workspace->first_owner>=c->homes->frame.slot_count ||
        binding->first_owner==workspace->first_owner || binding->offset==workspace->offset) return false;
    size_t slots[]={binding->first_owner,workspace->first_owner}, offsets[]={binding->offset,workspace->offset};
    for (size_t n=0;n<2;++n)
        if (offsets[n]>c->homes->frame.payload_size || sizeof(XrtManagedHeader *)>c->homes->frame.payload_size-offsets[n] ||
            offsets[n]%_Alignof(XrtManagedHeader *) || c->homes->frame.owned_offsets[slots[n]]!=offsets[n]) return false;
    c_format(c,"    if (!(xrl_array_ready && xrt_array_i64_try_update_local(xrl_frame, xrl_payload, &xrl_frame_locations, &xrl_frame_%" PRIu32 ",\n",
        c->function.id);
    c_format(c,"        (XrtArrayI64LocalUpdate){%zu, %zu, %zu, %zu, %s, ",slots[0],offsets[0],slots[1],offsets[1],
        shape->operation==XR_LIR_ARRAY_OP_SET?"XRT_ARRAY_UPDATE_SET":"XRT_ARRAY_UPDATE_PUSH");
    if (shape->operation==XR_LIR_ARRAY_OP_SET) c_value(c,op->operands[1]); else c_text(c,"0");
    c_text(c,", "); c_value(c,op->operands[op->operand_count-1]); c_text(c,"}))) {\n");
    return true;
}
