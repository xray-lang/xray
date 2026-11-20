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
static bool c_array_local_pair(LirC *c, const XrLirValueHomes *homes,
    const XrLirHomeEvent *event, const XrLirHomeWorkspace *workspace) {
    if (!c_array_local_candidate(c,homes,event)) return false;
    const XrLirHomeBinding *binding=&homes->bindings[event->source];
    if (workspace->kind!=XR_LIR_HOME_WORKSPACE_VALUE || workspace->value || workspace->type!=binding->type ||
        workspace->block!=event->block || workspace->instruction!=event->instruction ||
        binding->first_owner>=homes->frame.slot_count || workspace->first_owner>=homes->frame.slot_count ||
        binding->first_owner==workspace->first_owner || binding->offset==workspace->offset) return false;
    size_t slots[]={binding->first_owner,workspace->first_owner}, offsets[]={binding->offset,workspace->offset};
    for (size_t n=0;n<2;++n)
        if (offsets[n]>homes->frame.payload_size || sizeof(XrtManagedHeader *)>homes->frame.payload_size-offsets[n] ||
            offsets[n]%_Alignof(XrtManagedHeader *) || homes->frame.owned_offsets[slots[n]]!=offsets[n]) return false;
    return true;
}
static bool c_array_local_function(LirC *c, const XrLirValueHomes *homes) {
    for (size_t n=0;homes && n<homes->event_count && c_request_poll(c);++n)
        if (c_array_local_candidate(c,homes,&homes->events[n])) return true;
    return false;
}
/* The immutable Homes arrays need no receiving copy here. This temporary
 * numeric index only pairs existing coordinates; it introduces no new proof.
 * As with ordinary workspace lookup, the first exact row wins. */
static size_t *c_array_local_index(LirC *c, size_t *capacity) {
    *capacity=2;
    while (*capacity/2<c->homes->workspace_count) {
        if (*capacity>SIZE_MAX/2) {
            c_error(c,XR_LIR_C_OVERFLOW,"C local Array workspace inventory overflows"); return NULL;
        }
        *capacity*=2;
    }
    void *memory=NULL;
    if (!c_resource(c,xr_compile_resources_calloc(c->request->resources,*capacity,sizeof(size_t),&memory))) return NULL;
    size_t *index=memory;
    for (size_t n=0;n<c->homes->workspace_count && c_request_poll(c);++n) {
        const XrLirHomeWorkspace *workspace=&c->homes->workspaces[n];
        size_t at=c_edge_event_hash(workspace->block,workspace->instruction,0)&(*capacity-1);
        while (index[at]) {
            const XrLirHomeWorkspace *previous=&c->homes->workspaces[index[at]-1];
            if (previous->block==workspace->block && previous->instruction==workspace->instruction) break;
            at=(at+1)&(*capacity-1);
        }
        if (!index[at]) index[at]=n+1;
    }
    return index;
}
static const XrLirHomeWorkspace *c_array_local_lookup(const XrLirValueHomes *homes,
    const size_t *index, size_t capacity, const XrLirHomeEvent *event) {
    size_t at=c_edge_event_hash(event->block,event->instruction,0)&(capacity-1);
    while (index[at]) {
        const XrLirHomeWorkspace *workspace=&homes->workspaces[index[at]-1];
        if (workspace->block==event->block && workspace->instruction==event->instruction) return workspace;
        at=(at+1)&(capacity-1);
    }
    return NULL;
}
/* These fields have the generated function's true automatic lifetime. The
 * cold helper is emitted once, so multiple lazy-creation sites do not repeat
 * its pair inventory. No receiving metadata or backing pointer is retained. */
static void c_array_local_specs(LirC *c) {
    if (!c->array_local) return;
    c_format(c,"typedef struct XrlArrayLocals_%" PRIu32 " {\n",c->function.id);
    for (size_t n=0;n<c->homes->event_count && c_request_poll(c);++n)
        if (c_array_local_candidate(c,c->homes,&c->homes->events[n])) c_format(c,"    XrtArrayI64LocalView e%zu;\n",n);
    c_format(c,"} XrlArrayLocals_%" PRIu32 ";\n",c->function.id);
    if (!c_request_poll(c)) return;
    size_t capacity=0; size_t *index=c_array_local_index(c,&capacity);
    if (!index) return;
    c_format(c,"static void xrl_array_local_prepare_%" PRIu32 "(XrtManagedFrame *frame, void *payload,\n"
        "    const XrtManagedFrameLocations *locations, XrlArrayLocals_%" PRIu32 " *views) {\n",c->function.id,c->function.id);
    c_text(c,"    (void)frame; (void)payload; (void)locations; (void)views;\n");
    for (size_t n=0;n<c->homes->event_count && c_request_poll(c);++n) {
        const XrLirHomeEvent *event=&c->homes->events[n];
        if (!c_array_local_candidate(c,c->homes,event)) continue;
        const XrLirHomeWorkspace *workspace=c_array_local_lookup(c->homes,index,capacity,event);
        if (!workspace || !c_array_local_pair(c,c->homes,event,workspace)) continue;
        const XrLirHomeBinding *binding=&c->homes->bindings[event->source];
        c_format(c,"    (void)xrt_array_i64_local_prepare(frame, payload, locations, &xrl_frame_%" PRIu32 ",\n"
            "        (XrtArrayI64LocalGeometry){%zu, %zu, %zu, %zu}, &views->e%zu);\n",
            c->function.id,binding->first_owner,binding->offset,workspace->first_owner,workspace->offset,n);
    }
    c_text(c,"}\n");
    xr_compile_resources_free(index);
}
static void c_array_local_ready(LirC *c) {
    /* Emit only after an actual full create/repeat/update and its required
     * publication returned. Frame creation and reference transfers do not seed
     * this execution-initialization fact. */
    if (c->array_local) c_text(c,"    xrl_array_ready = true;\n");
}
/* Generated code keeps one held sole-owner acquisition per local binding slot
 * (xrt_array_i64_update_held). The slot list is rebuilt for each function. */
static void c_array_held_prepare(LirC *c) {
    xr_compile_resources_free(c->array_held_slots);
    c->array_held_slots=NULL; c->array_held_count=0;
    if (!c->array_local) return;
    size_t capacity=0;
    for (size_t n=0;n<c->homes->event_count && c->status==XR_LIR_C_OK;++n) {
        const XrLirHomeEvent *event=&c->homes->events[n];
        if (!c_array_local_candidate(c,c->homes,event)) continue;
        size_t slot=c->homes->bindings[event->source].first_owner, k=0;
        while (k<c->array_held_count && c->array_held_slots[k]!=slot) ++k;
        if (k<c->array_held_count) continue;
        if (c->array_held_count==capacity) {
            size_t next=capacity ? capacity*2 : 4;
            if (next<=capacity || next>SIZE_MAX/sizeof(*c->array_held_slots)) {
                c_error(c,XR_LIR_C_OVERFLOW,"held Array slot inventory overflows"); return;
            }
            void *memory=c->array_held_slots;
            if (!c_resource(c,xr_compile_resources_resize(c->request->resources,&memory,next*sizeof(*c->array_held_slots))))
                return;
            c->array_held_slots=memory; capacity=next;
        }
        c->array_held_slots[c->array_held_count++]=slot;
    }
}
static size_t c_array_held_index(const LirC *c, size_t slot) {
    for (size_t k=0;k<c->array_held_count;++k)
        if (c->array_held_slots[k]==slot) return k;
    return SIZE_MAX;
}
static void c_array_held_declarations(LirC *c) {
    for (size_t k=0;k<c->array_held_count;++k)
        c_format(c,"    XrtArrayBacking *xrl_held%zu = NULL;\n    (void)xrl_held%zu;\n",k,k);
}
static void c_array_held_clear(LirC *c) {
    /* Region helper functions own no held acquisitions; their callers clear
     * before entering because region blocks never keep them. */
    if (!c->array_held_count || c->array_helper_kind != XR_LIR_ARRAY_REGION_PRIMARY_NONE) return;
    c_text(c,"   ");
    for (size_t k=0;k<c->array_held_count;++k) c_format(c," xrl_held%zu = NULL;",k);
    c_text(c,"\n");
}
/* Region and push-window code moves real owners between its own slots. */
static bool c_array_held_region_block(const LirC *c, uint32_t block) {
    if (!c->array_region) return false;
    const XrLirArrayRegionBindings *sets[]={c->array_region->bindings,c->array_region->push_windows};
    for (size_t n=0;n<2;++n)
        if (sets[n] && block<sets[n]->block_index_count && sets[n]->block_index[block].region) return true;
    return false;
}
/* An edge keeps held acquisitions when it only assigns C locals: scalar
 * arguments and stationary owners whose shared slot does not change. */
static bool c_array_held_edge(LirC *c, const XrLirEdgeView *edge) {
    if (c_array_held_region_block(c,edge->target)) return false;
    XrLirBlock target={c->request->module,c->function.id,edge->target};
    XrLirBlockView view;
    if (xr_lir_block_view(c->request->module,target,&view)!=XR_LIR_OK || view.parameter_count!=edge->argument_count)
        return false;
    for (uint32_t a=0;a<edge->argument_count;++a) {
        XrLirType type=c_value_type(c,edge->arguments[a]);
        if (type>=XR_LIR_TYPE_COUNT) return false;
        if (c_managed_leaf(type) && !xr_lir_home_stationary_edge(c->homes,edge->arguments[a],view.parameters[a]))
            return false;
    }
    return true;
}
/* A Cell handoff whose binding and value share one owner field emits nothing. */
static bool c_array_held_shared_handoff(LirC *c, const XrLirInstructionView *op) {
    if (!c->homes->bindings || op->operand_count!=1) return false;
    uint32_t cell=op->op==XR_LIR_CELL_NEW ? op->result : op->operands[0];
    uint32_t value=op->op==XR_LIR_CELL_NEW ? op->operands[0] : op->result;
    if (!cell || !value || c_value_type(c,cell)!=XR_LIR_ARRAY_CELL) return false;
    const XrLirHomeBinding *binding=&c->homes->bindings[cell];
    const XrLirValueHome *home=&c->homes->values[value];
    return binding->kind==XR_LIR_HOME_BINDING_LOCAL && binding->first_owner!=SIZE_MAX &&
        binding->first_owner==home->first_owner && binding->offset==home->offset;
}
/* Held acquisitions survive only operations that cannot run guest code,
 * retain or release a binding, move an owner or replace a backing. Scalar
 * operations may panic, which never returns to the held state. Array edits
 * through local views maintain their own acquisition; other edits clear it. */
static bool c_array_held_keeps(LirC *c, const XrLirInstructionView *op) {
    if (c_array_held_region_block(c,c->block)) return false;
    switch (op->op) {
    case XR_LIR_CONST: case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL: case XR_LIR_NEG:
    case XR_LIR_DIV: case XR_LIR_REM: case XR_LIR_FADD: case XR_LIR_FSUB: case XR_LIR_FMUL:
    case XR_LIR_FDIV: case XR_LIR_FNEG: case XR_LIR_ICMP: case XR_LIR_FCMP: case XR_LIR_INTCAST:
    case XR_LIR_ITOF: case XR_LIR_FCAST: case XR_LIR_FTOI:
        return true;
    case XR_LIR_BR:
        return c_array_held_edge(c,&op->edges[0]);
    case XR_LIR_CONDBR:
        return c_array_held_edge(c,&op->edges[0]) && c_array_held_edge(c,&op->edges[1]);
    case XR_LIR_CELL_NEW: case XR_LIR_CELL_READ:
        return c_array_held_shared_handoff(c,op);
    case XR_LIR_ARRAY_DROP: case XR_LIR_ARRAY_CELL_DROP:
        /* The adjacent release of a forwarding pair: an elided pair emits
         * nothing, and a guarded COPY already cleared before itself. */
        return c->binding_forward_pending != NULL;
    case XR_LIR_CALL_INTRINSIC: {
        XrLirArrayIntrinsic shape={0};
        return c_array_intrinsic(c,op,&shape) && (shape.operation==XR_LIR_ARRAY_OP_GET ||
            shape.operation==XR_LIR_ARRAY_OP_LEN || shape.operation==XR_LIR_ARRAY_OP_SET ||
            shape.operation==XR_LIR_ARRAY_OP_PUSH);
    }
    default:
        return false;
    }
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
    if (c->array_held_count && !c_array_held_keeps(c,op)) c_array_held_clear(c);
}
static bool c_array_local_begin(LirC *c, const XrLirInstructionView *op,
    const XrLirArrayIntrinsic *shape, const XrLirHomeWorkspace *workspace) {
    if (!c->array_local || c->homes->source_function!=c->function.id) return false;
    const XrLirHomeEvent *event=NULL; size_t ordinal=0;
    while (c->array_local_event_cursor<c->homes->event_count) {
        const XrLirHomeEvent *row=&c->homes->events[c->array_local_event_cursor];
        if (row->kind!=XR_LIR_OWNERSHIP_ARRAY_OPERATION) { ++c->array_local_event_cursor; continue; }
        if (row->block>c->block || (row->block==c->block && row->instruction>c->instruction)) break;
        ++c->array_local_event_cursor;
        if (row->block==c->block && row->instruction==c->instruction) { event=row; ordinal=c->array_local_event_cursor-1; break; }
    }
    if (!event || !c_array_local_candidate(c,c->homes,event) || event->operation!=(uint32_t)shape->operation ||
        op->op!=XR_LIR_CALL_INTRINSIC || !op->operand_count || event->source!=op->operands[0] || op->type!=XR_LIR_UNIT || op->result ||
        shape->parameter_count!=op->operand_count || !shape->parameter_elements || shape->parameter_elements[0]!=XR_LIR_I64 ||
        (shape->operation==XR_LIR_ARRAY_OP_SET && op->operand_count!=3) ||
        (shape->operation==XR_LIR_ARRAY_OP_PUSH && op->operand_count!=2) ||
        (shape->operation==XR_LIR_ARRAY_OP_SET && c_value_type(c,op->operands[1])!=XR_LIR_I64) ||
        c_value_type(c,op->operands[op->operand_count-1])!=XR_LIR_I64) return false;
    if (!c_array_local_pair(c,c->homes,event,workspace)) return false;
    c_text(c,"    {\n    const int64_t xrl_array_index = ");
    if (shape->operation==XR_LIR_ARRAY_OP_SET) c_value(c,op->operands[1]); else c_text(c,"0");
    c_text(c,";\n    const int64_t xrl_array_item = "); c_value(c,op->operands[op->operand_count-1]);
    c_text(c,";\n");
    size_t held=c_array_held_index(c,c->homes->bindings[event->source].first_owner);
    if (held==SIZE_MAX) {
        c_error(c,XR_LIR_C_INVALID_LIR,"local Array edit lost its held binding slot"); return false;
    }
    const char *kind=shape->operation==XR_LIR_ARRAY_OP_SET ? "XRT_ARRAY_UPDATE_SET" : "XRT_ARRAY_UPDATE_PUSH";
    c_format(c,"    if (!(xrl_array_ready && xrt_array_i64_update_held(&xrl_held%zu, &xrl_array_locals.e%zu,\n",held,ordinal);
    c_format(c,"        (XrtArrayI64LocalUpdate){%s, xrl_array_index, xrl_array_item}))) {\n",kind);
    /* Only the kernel takes the request by address; building it here keeps
     * its stores off the held path. */
    c_format(c,"    const XrtArrayUpdate xrl_array_update = {%s, xrl_array_index, {XRT_BACKING_ARRAY_I64, {.i64=xrl_array_item}}};\n"
        "    (void)xrl_array_update;\n",kind);
    /* The kernel below may grow, copy or replace a backing. */
    c_array_held_clear(c);
    return true;
}
