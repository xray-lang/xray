/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_access_epoch.inc.c - Selected value-flow evidence for current scopes
 */
typedef struct ViewAccessEpoch {
    ViewAccess *access;
    const XrXirViewLoanOrigin *target;
    ViewOriginSet selections,contexts,tasks;
    bool matched,current;
} ViewAccessEpoch;
static XrXirStatus view_access_epoch_push(ViewAccessEpoch *q,uint32_t value,uint32_t selection,bool old) {
    ViewRootCheck *c=&q->access->resolver->check;uint32_t context=0,task=0;
    XrXirStatus status=view_root_intern(c,&q->contexts,(ViewOriginPair){selection,old},&context);
    /* Both current and old paths must survive interning at the same SSA site. */
    return status==XR_XIR_OK?view_root_intern(c,&q->tasks,(ViewOriginPair){value,context},&task):status;
}
static XrXirStatus view_access_epoch_select(ViewAccessEpoch *q,uint32_t value,uint32_t selector,ViewOriginPair context) {
    uint32_t selection=0;XrXirStatus status=view_root_intern(&q->access->resolver->check,
        &q->selections,(ViewOriginPair){selector,context.first},&selection);
    return status==XR_XIR_OK?view_access_epoch_push(q,value,selection+1u,context.second!=0):status;
}
static XrXirStatus view_access_epoch_match(ViewAccessEpoch *q,uint32_t selection,bool *same) {
    *same=false;uint32_t index=0;
    for (uint32_t p=selection;p;p=q->selections.pairs[p-1u].second) {

        if (index>=q->target->selection_count || q->selections.pairs[p-1u].first!=q->target->selection[index++])
            return XR_XIR_OK;
    }
    *same=index==q->target->selection_count;return XR_XIR_OK;
}
static XrXirStatus view_access_epoch_tuple(ViewAccessEpoch *q,const XrXirInstruction *op,ViewOriginPair context) {
    ViewRootCheck *c=&q->access->resolver->check;const XrXirFunction *body=q->access->body;
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(c->module->types,op->type);if (!tuple) return XR_XIR_BAD_TYPE;
    ViewOriginPair selection=context.first?q->selections.pairs[context.first-1u]:(ViewOriginPair){UINT32_MAX,0};
    if (context.first && selection.first>=tuple->parameter_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t operand=0;
    for (uint32_t p=0;p<tuple->parameter_count;++p) {

        XrXirType type=tuple->parameters[p].type;if (type==XR_XIR_UNIT) continue;
        uint32_t value=body->operands[op->args[0]+operand++];
        if (!view_flags_at(c->module->types,c->flags,type) || (context.first && selection.first!=p)) continue;
        XrXirStatus status=view_access_epoch_push(q,value,context.first?selection.second:0,context.second!=0);
        if (status!=XR_XIR_OK) return status;
    }
    return operand==op->args[1]?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus view_access_epoch_phi(ViewAccessEpoch *q,uint32_t value,
    const XrXirInstruction *op,ViewOriginPair context) {
    ViewAccess *a=q->access;uint32_t instruction=value-a->body->parameter_count,block=UINT32_MAX,pred=UINT32_MAX;
    for (uint32_t b=0;b<a->body->block_count;++b) {

        if (instruction>=a->body->blocks[b].first && instruction-a->body->blocks[b].first<a->body->blocks[b].count) block=b;
    }
    if (block==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0;i<q->target->choice_count;++i) {

        if (q->target->choices[i].block==block) pred=q->target->choices[i].predecessor;
    }
    /* An unobserved PHI belongs to another conditional origin. Cyclic choices
     * were already rejected before entering this value-flow query. */
    if (pred==UINT32_MAX) return XR_XIR_OK;
    for (uint32_t p=0;p<op->args[1];p+=2) {

        if (a->body->operands[op->args[0]+p]==pred)
            return view_access_epoch_push(q,a->body->operands[op->args[0]+p+1u],context.first,context.second!=0);
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus view_access_scope_window(ViewAccess *a,const XrXirViewLoanOrigin *target,uint32_t value,bool *current);
static XrXirStatus view_access_epoch_step(ViewAccessEpoch *q,ViewOriginPair task) {
    ViewAccess *a=q->access;uint32_t value=task.first;ViewOriginPair context=q->contexts.pairs[task.second];

    if (value<a->body->parameter_count) return XR_XIR_OK;
    bool prefix=false;XrXirStatus status=view_access_scope_window(a,q->target,value,&prefix);if (status!=XR_XIR_OK) return status;
    context.second|=!prefix;
    if (value==q->target->value) {
        bool same=false;status=view_access_epoch_match(q,context.first,&same);
        if (same) {q->matched=true;q->current&=!context.second;}return status;
    }
    const XrXirInstruction *op=a->body->instructions+value-a->body->parameter_count;
    switch (op->op) {
    case XR_XIR_COPY:case XR_XIR_OWNED_RETAIN:
        return view_access_epoch_push(q,op->args[0],context.first,context.second!=0);
    case XR_XIR_NULLABLE_SOME:
        if (!context.first) return view_access_epoch_push(q,op->args[0],0,context.second!=0);
        if (q->selections.pairs[context.first-1u].first!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        return view_access_epoch_push(q,op->args[0],q->selections.pairs[context.first-1u].second,context.second!=0);
    case XR_XIR_NULLABLE_UNWRAP:return view_access_epoch_select(q,op->args[0],UINT32_MAX,context);
    case XR_XIR_TUPLE_FIELD:return view_access_epoch_select(q,op->args[0],(uint32_t)op->immediate,context);
    case XR_XIR_TUPLE_NEW:return view_access_epoch_tuple(q,op,context);
    case XR_XIR_PHI:return view_access_epoch_phi(q,value,op,context);
    default:return XR_XIR_OK;
    }
}
static XrXirStatus view_access_epoch_current(ViewAccess *a,const ViewAccessOrigin *origin,bool *current) {
    *current=false;ViewRootCheck *c=&a->resolver->check;uint64_t before=c->reserved,fixed=sizeof(ViewAccessEpoch);
    if (before>c->context->limits.frame_bytes || fixed>c->context->limits.frame_bytes-before) return XR_XIR_BUDGET;
    c->reserved+=fixed;ViewAccessEpoch q={.access=a,.target=&origin->event,.current=true};
    XrXirStatus status=view_access_epoch_push(&q,origin->queried_value,0,false);
    for (uint32_t i=0;i<q.tasks.count && status==XR_XIR_OK && q.current;++i) {
        /* Interning may grow the sets; carry only a copied task across it. */
        ViewOriginPair task=q.tasks.pairs[i];status=view_access_epoch_step(&q,task);
    }
    if (status==XR_XIR_OK) *current=q.matched && q.current;
    view_root_set_drop(c,&q.selections);view_root_set_drop(c,&q.contexts);view_root_set_drop(c,&q.tasks);
    XR_CHECK(c->reserved==before+fixed,"epoch value-flow scratch retires on its exact compile resource owner");
    c->reserved-=fixed;return status;
}

typedef struct ViewAccessRootEpoch { uint32_t value,target,observer,remaining; } ViewAccessRootEpoch;
typedef struct ViewAccessRootFlow {
    uint32_t value,target,observer,head,tail,count;
    uint8_t observed;
} ViewAccessRootFlow;
static XrXirStatus view_access_root_flow_push(ViewAccess *a,ViewAccessRootFlow *q,uint32_t block,uint8_t state) {
    if (block>=a->body->block_count) return XR_XIR_BAD_STRUCTURE;

    uint8_t old=a->seen[block];if (!state || (old&state)==state) return XR_XIR_OK;
    a->seen[block]|=state;if (old&4u) return XR_XIR_OK;
    XR_CHECK(q->count<a->body->block_count,"each root-flow block occupies the existing queue at most once");
    a->queue[q->tail]=block;q->tail=q->tail+1u==a->body->block_count?0:q->tail+1u;
    a->seen[block]|=4u;++q->count;return XR_XIR_OK;
}
static XrXirStatus view_access_root_flow_block(ViewAccess *a,ViewAccessRootFlow *q,uint32_t block) {
    const XrXirBlock *body=a->body->blocks+block;
    uint8_t state=a->seen[block]&3u,panic=state;
    for (uint32_t i=body->first;i-body->first<body->count;++i) {

        if (i==q->observer) q->observed|=state;
        if (i==q->target) state=1u;
        if (i==q->value) state=2u;
        /* A protected block may leave at any prefix, before a reset commits. */
        panic|=state;
    }
    const XrXirInstruction *term=a->body->instructions+body->first+body->count-1u;
    for (uint32_t edge=0;edge<view_access_rules[term->op].edges;++edge) {
        XrXirStatus status=view_access_root_flow_push(a,q,term->targets[edge],state);
        if (status!=XR_XIR_OK) return status;
    }
    return body->panic?view_access_root_flow_push(a,q,body->panic,panic):XR_XIR_OK;
}
static XrXirStatus view_access_root_flow(ViewAccess *a,uint32_t value,uint32_t target,
    uint32_t observer,uint8_t expected,bool *current) {
    *current=false;ViewRootCheck *c=&a->resolver->check;
    uint64_t before=c->reserved,fixed=sizeof(ViewAccessRootFlow);
    if (before>c->context->limits.frame_bytes || fixed>c->context->limits.frame_bytes-before) return XR_XIR_BUDGET;

    c->reserved+=fixed;
    /* Graph construction has finished; these per-function buffers are idle. */
    memset(a->seen,0,a->body->block_count);
    ViewAccessRootFlow q={.value=value-a->body->parameter_count,.target=target-a->body->parameter_count,.observer=observer};
    XrXirStatus status=view_access_root_flow_push(a,&q,0,1u);
    while (q.count && status==XR_XIR_OK) {

        uint32_t block=a->queue[q.head];q.head=q.head+1u==a->body->block_count?0:q.head+1u;
        --q.count;a->seen[block]&=3u;status=view_access_root_flow_block(a,&q,block);
    }
    if (status==XR_XIR_OK) *current=q.observed==expected;
    XR_CHECK(c->reserved==before+fixed,"root flow retires its exact frame record without retaining scratch state");
    c->reserved-=fixed;return status;
}
static XrXirStatus view_access_root_window(ViewAccess *a,uint32_t value,uint32_t target,
    uint32_t observer,bool *current) {
    *current=false;if (value==target) {*current=true;return XR_XIR_OK;}
    if (value<a->body->parameter_count) return XR_XIR_OK;
    uint32_t producer=value-a->body->parameter_count,root=target-a->body->parameter_count;
    for (uint32_t b=0;b<a->body->block_count;++b) {

        const XrXirBlock *block=a->body->blocks+b;
        if (observer<block->first || observer-block->first>=block->count) continue;
        if (producer>=block->first && producer<observer) {
            *current=!(root>producer && root<observer);return XR_XIR_OK;
        }
        return view_access_root_flow(a,value,target,observer,2u,current);
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus view_access_scope_window(ViewAccess *a,const XrXirViewLoanOrigin *target,uint32_t value,bool *current) {
    if (target->kind!=XR_XIR_VIEW_LOAN_NEW && target->kind!=XR_XIR_VIEW_LOAN_REBORROW)
        return view_access_prefix_scope(a,value,current);
    if (value!=target->value) return view_access_root_window(a,value,target->value,a->instruction,current);
    XrXirStatus status=view_access_prefix_scope(a,value,current);
    /* A direct scope must have committed on every incoming path, including
     * protected prefixes; its SSA name alone does not establish availability. */
    return status==XR_XIR_OK && !*current?view_access_root_flow(a,value,target->value,a->instruction,2u,current):status;
}
static XrXirStatus view_access_root_local(ViewAccess *a,uint32_t local,bool *unchanged) {
    *unchanged=true;
    for (uint32_t i=0;i<a->body->instruction_count;++i) {

        const XrXirInstruction *op=a->body->instructions+i;
        if ((op->op==XR_XIR_LOCAL_WRITE || op->op==XR_XIR_OWNED_LOCAL_WRITE) && op->args[0]==local) *unchanged=false;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_root_chain(ViewAccess *a,ViewAccessRootEpoch *q,bool *current,bool *phi) {
    *current=false;*phi=false;
    XrXirStatus status=XR_XIR_OK;
    while (q->remaining) {
        --q->remaining;

        bool prefix=false;status=view_access_root_window(a,q->value,q->target,q->observer,&prefix);
        if (status!=XR_XIR_OK || !prefix || q->value<a->body->parameter_count) break;
        if (q->value==q->target) {*current=true;break;}
        const XrXirInstruction *op=a->body->instructions+q->value-a->body->parameter_count;
        q->observer=q->value-a->body->parameter_count;
        switch (op->op) {
        case XR_XIR_COPY:case XR_XIR_OWNED_RETAIN:case XR_XIR_CELL_PROJECT:case XR_XIR_CELL_PLACE:
        case XR_XIR_FIELD_PLACE:case XR_XIR_INDEX_PLACE:case XR_XIR_LOCAL_READ:case XR_XIR_OWNED_LOCAL_READ:
            q->value=op->args[0];break;
        case XR_XIR_LOCAL_NEW:case XR_XIR_OWNED_LOCAL_NEW: {
            bool unchanged=false;status=view_access_root_local(a,q->value,&unchanged);
            q->value=op->args[0];if (!unchanged) q->remaining=0;break;
        }
        case XR_XIR_VIEW_NEW:case XR_XIR_VIEW_REBORROW:
            q->value=a->body->operands[op->args[0]];break;
        case XR_XIR_PHI:*phi=true;q->remaining=0;break;
        default:q->remaining=0;break;
        }
        if (status!=XR_XIR_OK) break;
    }
    return status;
}
typedef struct ViewAccessRootPhi {
    ViewAccess *access;
    ViewOriginSet tasks;
    uint32_t target;
    bool matched,unknown;
} ViewAccessRootPhi;
static XrXirStatus view_access_root_phi_push(ViewAccessRootPhi *q,uint32_t value,uint32_t observer) {
    uint32_t ignored=0;
    return view_root_intern(&q->access->resolver->check,&q->tasks,(ViewOriginPair){value,observer},&ignored);
}
static XrXirStatus view_access_root_phi_edges(ViewAccessRootPhi *q,const ViewAccessRootEpoch *incoming) {
    ViewAccess *a=q->access;uint32_t block=UINT32_MAX;
    for (uint32_t b=0;b<a->body->block_count;++b) {

        const XrXirBlock *candidate=a->body->blocks+b;
        if (incoming->observer>=candidate->first && incoming->observer-candidate->first<candidate->count) block=b;
    }
    if (block==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=a->body->instructions+incoming->observer;
    if (op->op!=XR_XIR_PHI || !op->args[1]) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<op->args[1];p+=2) {

        uint32_t pred=a->body->operands[op->args[0]+p];
        if (pred>=a->body->block_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirBlock *source=a->body->blocks+pred;
        /* A panic edge observes all protected prefixes, not the terminator's
         * final values, so it cannot consume this normal-edge observation. */
        if (source->panic && source->panic==block) {q->unknown=true;return XR_XIR_OK;}
        uint32_t edge=source->first+source->count-1u;
        const XrXirInstruction *term=a->body->instructions+edge;bool normal=false;
        for (uint32_t e=0;e<view_access_rules[term->op].edges;++e) normal|=term->targets[e]==block;
        if (!normal) {q->unknown=true;return XR_XIR_OK;}
        XrXirStatus status=view_access_root_phi_push(q,a->body->operands[op->args[0]+p+1u],edge);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_root_phi_step(ViewAccessRootPhi *q,ViewOriginPair task) {
    ViewAccess *a=q->access;
    uint32_t values=a->body->parameter_count+a->body->instruction_count;
    ViewAccessRootEpoch incoming={task.first,q->target,task.second,values};
    bool matched=false,phi=false;
    XrXirStatus status=view_access_root_chain(a,&incoming,&matched,&phi);
    if (status!=XR_XIR_OK) return status;
    if (matched) {q->matched=true;return XR_XIR_OK;}
    if (!phi) {q->unknown=true;return XR_XIR_OK;}
    return view_access_root_phi_edges(q,&incoming);
}
static XrXirStatus view_access_root_phi(ViewAccess *a,const ViewAccessRootEpoch *entry,bool *current) {
    *current=false;ViewRootCheck *c=&a->resolver->check;
    uint64_t before=c->reserved,fixed=sizeof(ViewAccessRootPhi);
    if (before>c->context->limits.frame_bytes || fixed>c->context->limits.frame_bytes-before) return XR_XIR_BUDGET;

    c->reserved+=fixed;ViewAccessRootPhi q={.access=a,.target=entry->target};
    XrXirStatus status=view_access_root_phi_edges(&q,entry);
    /* Every admitted edge preserves the newest root epoch while tracing an
     * earlier executed instruction or edge. Ordinary SSA initialization makes
     * every concrete trace finite, even when its static graph contains a cycle. */
    for (uint32_t i=0;i<q.tasks.count && status==XR_XIR_OK && !q.unknown;++i) {
        ViewOriginPair task=q.tasks.pairs[i];status=view_access_root_phi_step(&q,task);
    }
    if (status==XR_XIR_OK) *current=q.matched && !q.unknown;
    view_root_set_drop(c,&q.tasks);
    XR_CHECK(c->reserved==before+fixed,"PHI root tasks release their exact compile frame reservation");
    c->reserved-=fixed;return status;
}
static XrXirStatus view_access_root_epoch(ViewAccess *a,const XrXirViewRootEvent *root,
    uint32_t queried,bool *current) {
    *current=false;
    if (root->kind!=XR_XIR_VIEW_ROOT_CELL || root->root_function!=a->function ||
        root->root_value<a->body->parameter_count ||
        root->root_value-a->body->parameter_count>=a->body->instruction_count) return XR_XIR_OK;
    if (a->body->instructions[root->root_value-a->body->parameter_count].op!=XR_XIR_CELL_NEW) return XR_XIR_BAD_STRUCTURE;
    ViewRootCheck *c=&a->resolver->check;uint64_t before=c->reserved,fixed=2u*sizeof(ViewAccessRootEpoch);
    if (before>c->context->limits.frame_bytes || fixed>c->context->limits.frame_bytes-before) return XR_XIR_BUDGET;

    uint64_t values=(uint64_t)a->body->parameter_count+a->body->instruction_count;
    if (values>UINT32_MAX) return XR_XIR_BUDGET;
    c->reserved+=fixed;ViewAccessRootEpoch q={queried,root->root_value,a->instruction,(uint32_t)values};
    bool phi=false;XrXirStatus status=view_access_root_chain(a,&q,current,&phi);
    if (status==XR_XIR_OK && phi) status=view_access_root_phi(a,&q,current);
    XR_CHECK(c->reserved==before+fixed,"root epoch observations release their exact compile frame reservation");
    c->reserved-=fixed;return status;
}

static XrXirStatus view_access_root_older(ViewAccess *a,uint32_t value,uint32_t target,bool *older) {
    *older=false;ViewRootCheck *c=&a->resolver->check;
    uint64_t before=c->reserved,fixed=sizeof(ViewAccessRootEpoch);
    if (before>c->context->limits.frame_bytes || fixed>c->context->limits.frame_bytes-before) return XR_XIR_BUDGET;

    c->reserved+=fixed;ViewAccessRootEpoch q={.value=value,.target=target,.remaining=a->body->instruction_count};
    XrXirStatus status=XR_XIR_OK;
    while (q.remaining && q.value>=a->body->parameter_count && q.value!=target) {
        --q.remaining;

        if (xr_xir_place_kind(a->body,q.value)!=XR_XIR_PLACE_NONE) break;
        const XrXirInstruction *op=a->body->instructions+q.value-a->body->parameter_count;
        /* Observe Cell identities directly. A PHI of view owners remains
         * outside this query's forwarding rules and needs its own proof. */
        if (xr_xir_type_is_cell(c->module->types,xr_xir_operand_type(a->body,q.value)))
            status=view_access_root_flow(a,q.value,target,a->instruction,1u,older);
        if (status!=XR_XIR_OK || *older) break;
        /* An immutable forwarding value holds an identity from some earlier
         * execution of its source. If even the newest source precedes the
         * fresh allocation, every identity it could forward precedes it too. */
        if (op->op==XR_XIR_COPY || op->op==XR_XIR_OWNED_RETAIN) q.value=op->args[0];
        else if (op->op==XR_XIR_VIEW_NEW || op->op==XR_XIR_VIEW_REBORROW)
            q.value=a->body->operands[op->args[0]];
        else break;
    }
    XR_CHECK(c->reserved==before+fixed,"older root observations retire on their original compile owner");
    c->reserved-=fixed;return status;
}
static XrXirStatus view_access_epoch_separation(ViewAccess *a,const XrXirViewRootEvent *left,
    const XrXirViewRootEvent *right,uint32_t left_query,uint32_t right_query,bool *disjoint) {
    *disjoint=false;
    if (left->kind!=XR_XIR_VIEW_ROOT_CELL || right->kind!=XR_XIR_VIEW_ROOT_CELL ||
        left->root_function!=a->function || right->root_function!=a->function ||
        left->root_value!=right->root_value || left->opaque_suffix || right->opaque_suffix ||
        left->dynamic_identity || right->dynamic_identity || left->selection_count || right->selection_count)
        return XR_XIR_OK;
    for (uint32_t side=0;side<2;++side) {
        const XrXirViewRootEvent *root=side?right:left;
        for (uint32_t p=0;p<root->step_count;++p) {

            if (xr_xir_type_is_class(a->resolver->check.module->types,root->steps[p].container)) return XR_XIR_OK;
        }
    }
    for (uint32_t side=0;side<2;++side) {
        bool older=false;XrXirStatus status=view_access_root_older(a,side?right_query:left_query,left->root_value,&older);
        if (status!=XR_XIR_OK) return status;
        if (older) {
            status=view_access_root_epoch(a,left,side?left_query:right_query,disjoint);
            if (status!=XR_XIR_OK || *disjoint) return status;
        }
    }
    return XR_XIR_OK;
}
