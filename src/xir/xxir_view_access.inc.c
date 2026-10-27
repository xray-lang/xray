/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_access.inc.c - Independent view, Cell and retained-place decisions
 */
#include "xxir_view_access_graph.inc.c"
#include "xxir_view_access_entry.inc.c"
#include "xxir_view_access_epoch.inc.c"
#include "xxir_view_access_relation.inc.c"

static XrXirStatus view_access_pair(ViewAccess *a,const ViewAccessOrigin *left_origin,
    const ViewAccessOrigin *right_origin,bool writable,ViewAccessDecision *output) {
    const XrXirViewLoanOrigin *left=&left_origin->event,*right=&right_origin->event;
    *output=VIEW_ACCESS_UNKNOWN;
    if (left->kind==XR_XIR_VIEW_LOAN_ABSENT || right->kind==XR_XIR_VIEW_LOAN_ABSENT) {*output=VIEW_ACCESS_SAFE;return XR_XIR_OK;}
    if (!writable && view_access_shared(a,right->type)) {*output=VIEW_ACCESS_SAFE;return XR_XIR_OK;}
    if (left->kind==XR_XIR_VIEW_LOAN_UNRESOLVED || right->kind==XR_XIR_VIEW_LOAN_UNRESOLVED) return XR_XIR_OK;
    bool entry=false;XrXirStatus status=view_access_entry_pair(a,left,right,&entry);
    if (status!=XR_XIR_OK) return status;if (entry) {*output=VIEW_ACCESS_SAFE;return XR_XIR_OK;}
    bool ls=false,rs=false;status=view_access_origin_stable(a,left_origin,&ls);
    if (status==XR_XIR_OK) status=view_access_origin_stable(a,right_origin,&rs);if (status!=XR_XIR_OK) return status;
    if (ls && rs && view_access_same_scope(left,right)) {
        XR_CHECK(left->function==a->function && right->function==a->function,
            "scope equality compares owned observations of the current function");
        *output=view_access_mutable(a,left->type) && view_access_mutable(a,right->type)?VIEW_ACCESS_BLOCKED:VIEW_ACCESS_SAFE;
        return XR_XIR_OK;
    }
    ViewAccessAncestor down=VIEW_ACCESS_ANCESTRY_UNKNOWN,up=down;
    if (ls && rs) {
        status=view_access_ancestor(a,left_origin,right_origin,&down);
        if (status==XR_XIR_OK) status=view_access_ancestor(a,right_origin,left_origin,&up);if (status!=XR_XIR_OK) return status;
        if (down==VIEW_ACCESS_ANCESTOR) {*output=writable || view_access_mutable(a,right->type)?VIEW_ACCESS_BLOCKED:VIEW_ACCESS_SAFE;return XR_XIR_OK;}
        if (up==VIEW_ACCESS_ANCESTOR) {*output=VIEW_ACCESS_SAFE;return XR_XIR_OK;}
    }
    status=view_access_storage(a,left->value,right->value,left_origin->queried_value,right_origin->queried_value,output);
    if (status!=XR_XIR_OK) return status;
    if (*output==VIEW_ACCESS_BLOCKED && (!ls || !rs || down!=VIEW_ACCESS_NO_ANCESTOR || up!=VIEW_ACCESS_NO_ANCESTOR))
        *output=VIEW_ACCESS_UNKNOWN;
    return XR_XIR_OK;
}
static XrXirStatus view_access_presence(ViewAccess *a,uint32_t owner,const XrXirViewLoanOrigin *left,
    const XrXirViewLoanOrigin *right,bool *present) {
    *present=true;if ((!left || !left->choice_count) && !right->choice_count) return XR_XIR_OK;
    ViewRootCheck *c=&a->resolver->check;
    uint64_t fixed=sizeof(XrXirCompileContext)+sizeof(XirViewOwnerPathRequest);
    if (c->reserved>c->context->limits.frame_bytes || fixed>c->context->limits.frame_bytes-c->reserved)
        return XR_XIR_BUDGET;
    XrXirCompileContext remaining=*c->context;remaining.limits.frame_bytes-=c->reserved+fixed;
    XirViewOwnerPathRequest request={a->function,a->instruction,owner,left?left->choices:NULL,right->choices,
        left?left->choice_count:0,right->choice_count};
    return xir_compile_view_owner_path_possible(&remaining,c->module,&request,present,NULL);
}
static XrXirStatus view_access_same_owned_leaf(ViewAccess *a,const ViewAccessOrigin *left,
    const ViewAccessOrigin *right,bool *same) {
    *same=false;
    if (view_access_same_scope(&left->event,&right->event)) {
        bool l=false,r=false;XrXirStatus status=view_access_entry_origin(a,&left->event,&l);
        if (status==XR_XIR_OK) status=view_access_entry_origin(a,&right->event,&r);
        if (status!=XR_XIR_OK) return status;if (l && r) {*same=true;return XR_XIR_OK;}
    }
    if (!view_access_mutable(a,left->event.type) || !view_access_same_scope(&left->event,&right->event)) return XR_XIR_OK;
    bool ls=false,rs=false;XrXirStatus status=view_access_origin_stable(a,left,&ls);
    if (status==XR_XIR_OK) status=view_access_origin_stable(a,right,&rs);
    if (status==XR_XIR_OK) *same=ls && rs;return status;
}
static XrXirStatus view_access_owner(ViewAccess *a,uint32_t value,uint32_t owner,bool writable,bool view,bool *blocked) {
    if (blocked) *blocked=false;
    if (view && value==owner && view_leaves_at(a->resolver->check.module->types,a->resolver->check.flags,
        xr_xir_operand_type(a->body,value))==1) return XR_XIR_OK;
    /* Shared owners never block a read; aggregate input identities do not
     * need expansion to establish this permission-independent relation. */
    if (!writable && view_access_shared(a,xr_xir_operand_type(a->body,owner))) return XR_XIR_OK;
    ViewAccessOrigin *left=NULL,*right=NULL;XrXirStatus status=view_access_origins(a,owner,&right);
    if (status==XR_XIR_OK && view) status=view_access_origins(a,value,&left);if (status!=XR_XIR_OK) return status;
    bool all_safe=true,all_blocked=true,any=false;
    for (ViewAccessOrigin *r=right;r;r=r->next) {
        if (!view) {
            ViewAccessDecision decision=VIEW_ACCESS_UNKNOWN;
            if (r->event.kind==XR_XIR_VIEW_LOAN_ABSENT || (!writable && view_access_shared(a,r->event.type)))
                decision=VIEW_ACCESS_SAFE;
            else if (r->event.kind==XR_XIR_VIEW_LOAN_UNRESOLVED) decision=VIEW_ACCESS_UNKNOWN;
            else {status=view_access_storage(a,value,r->event.value,value,r->queried_value,&decision);if (status!=XR_XIR_OK) return status;}
            if (decision!=VIEW_ACCESS_SAFE) {
                bool present=true;status=view_access_presence(a,owner,NULL,&r->event,&present);
                if (status!=XR_XIR_OK) return status;if (!present) decision=VIEW_ACCESS_SAFE;
            }
            all_safe&=decision==VIEW_ACCESS_SAFE;all_blocked&=decision==VIEW_ACCESS_BLOCKED;any=true;continue;
        }
        for (ViewAccessOrigin *l=left;l;l=l->next) {
            bool compatible=false;status=view_access_compatible(&l->event,&r->event,&compatible);
            if (status!=XR_XIR_OK) return status;if (!compatible) continue;
            /* An owner contains its own admitted input subtree or Mut leaf.
             * Every other leaf still participates, including local children. */
            bool own=false;
            if (value==owner) status=view_access_same_owned_leaf(a,l,r,&own);
            if (status!=XR_XIR_OK) return status;
            ViewAccessDecision decision=own?VIEW_ACCESS_SAFE:VIEW_ACCESS_UNKNOWN;
            bool write=writable && !view_access_shared(a,l->event.type);
            if (!own) status=view_access_pair(a,l,r,write,&decision);
            if (status!=XR_XIR_OK) return status;
            if (decision!=VIEW_ACCESS_SAFE) {
                bool present=true;status=view_access_presence(a,owner,&l->event,&r->event,&present);
                if (status!=XR_XIR_OK) return status;if (!present) decision=VIEW_ACCESS_SAFE;
            }
            all_safe&=decision==VIEW_ACCESS_SAFE;all_blocked&=decision==VIEW_ACCESS_BLOCKED;any=true;
        }
    }
    XrXirStatus result=!any || all_safe?XR_XIR_OK:all_blocked?XR_XIR_BAD_VALUE:XR_XIR_UNRESOLVED;
    if (blocked) *blocked=result==XR_XIR_BAD_VALUE;return result;
}
static XrXirStatus view_access_use(ViewAccess *a,const XrXirViewOwnerSet *owners,uint32_t value,bool writable,bool view,bool *blocked) {
    if (blocked) *blocked=false;
    for (uint32_t p=0;p<owners->count;++p) {

        if (!(owners->bits[p/64]&(UINT64_C(1)<<(p%64)))) continue;
        XrXirStatus status=view_access_owner(a,value,owners->values[p],writable,view,blocked);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_constructor(ViewAccess *a,uint32_t value) {
    ViewRootComparison roots={a->resolver,NULL,false};XirViewRootRequest request={a->function,value,a->instruction,NULL,0};
    XrXirStatus status=xir_compile_view_root_resolver_query(a->resolver,&request,view_root_save,&roots,NULL);
    bool unresolved=!roots.left;
    for (ViewRootSaved *p=roots.left;p && status==XR_XIR_OK;p=p->next) {
        const XrXirViewRootEvent *event=&p->event;
        unresolved|=event->opaque_suffix || event->dynamic_identity || event->kind==XR_XIR_VIEW_ROOT_UNRESOLVED || event->kind==XR_XIR_VIEW_ROOT_MODULE;
        for (uint32_t i=0;i<event->step_count;++i) {

            unresolved|=xr_xir_type_is_class(a->resolver->check.module->types,event->steps[i].container);
        }
    }
    view_access_roots_free(a,&roots);return status!=XR_XIR_OK?status:unresolved?XR_XIR_UNRESOLVED:XR_XIR_OK;
}
static bool view_access_structural(XrXirOp op) {
    return op==XR_XIR_PHI || op==XR_XIR_COPY || op==XR_XIR_OWNED_RETAIN || op==XR_XIR_RETURN ||
        op==XR_XIR_VIEW_END || op==XR_XIR_VIEW_CLEANUP || op==XR_XIR_NULLABLE_NONE || op==XR_XIR_NULLABLE_SOME ||
        op==XR_XIR_NULLABLE_UNWRAP || op==XR_XIR_NULLABLE_IS_SOME || op==XR_XIR_TUPLE_NEW || op==XR_XIR_TUPLE_FIELD ||
        op==XR_XIR_CELL_NEW || op==XR_XIR_CELL_PLACE || op==XR_XIR_OBJECT_PLACE || op==XR_XIR_FIELD_PLACE || op==XR_XIR_INDEX_PLACE;
}
static uint32_t view_access_operands(const ViewAccess *a,const XrXirInstruction *op) {
    if (op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE) {
        const XrXirDeclarations *d=a->resolver->check.module->declarations;
        return xr_xir_slot_payload_operands(d->slots,d->slot_count,op);
    }
    if (op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE)
        return xr_xir_cell_payload_operands(a->resolver->check.module->types,xr_xir_operand_type(a->body,op->args[0]),op->op);
    return xr_xir_op_uses_operand_table(op->op)?op->args[1]:view_access_rules[op->op].operands;
}
static XrXirStatus view_access_binding(ViewAccess *a,const XrXirInstruction *op,bool *handled) {
    *handled=op->op==XR_XIR_LOCAL_NEW || op->op==XR_XIR_LOCAL_READ || op->op==XR_XIR_LOCAL_WRITE ||
        op->op==XR_XIR_LOCAL_UNINIT || op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ ||
        op->op==XR_XIR_OWNED_LOCAL_WRITE || op->op==XR_XIR_SCALAR_LOCAL_NEW ||
        op->op==XR_XIR_SCALAR_LOCAL_READ || op->op==XR_XIR_SCALAR_LOCAL_WRITE ||
        ((op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PLACE_WRITE) &&
        xr_xir_place_kind(a->body,op->args[0])==XR_XIR_PLACE_LOCAL);
    if (!*handled) return XR_XIR_OK;
    ViewRootCheck *c=&a->resolver->check;
    /* Slot copies and replacements transport bindings, not referent payloads.
     * Borrowed local transfers still require their separate availability proof. */
    if (view_flags_at(c->module->types,c->flags,op->type)) return XR_XIR_UNRESOLVED;
    for (uint32_t p=0;p<view_access_operands(a,op);++p) {

        if (view_flags_at(c->module->types,c->flags,xr_xir_operand_type(a->body,op->args[p]))) return XR_XIR_UNRESOLVED;
    }
    return XR_XIR_OK;
}
static bool view_access_scalar_storage(XrXirType type) {
    return type==XR_XIR_BOOL || type==XR_XIR_RUNE || xr_xir_type_is_number(type);
}
static XrXirStatus view_access_instruction(ViewAccess *a,const XrXirViewOwnerEvent *event) {
    const XrXirInstruction *op=a->body->instructions+event->instruction;
    const XrXirTypes *types=a->resolver->check.module->types;const uint8_t *flags=a->resolver->check.flags;
    uint32_t result=a->body->parameter_count+event->instruction;
    bool binding=false;XrXirStatus bound=view_access_binding(a,op,&binding);if (binding) return bound;
    if (op->op==XR_XIR_SLOT_INIT) {
        const XrXirDeclarations *d=a->resolver->check.module->declarations;
        if (d && d->slots && d->modules && d->functions && op->immediate>=0 &&
            (uint64_t)op->immediate<d->slot_count && a->function<a->resolver->check.module->function_count) {
            const XrXirSlot *slot=d->slots+(uint32_t)op->immediate;
            XrXirType payload=xr_xir_operand_type(a->body,op->args[0]);
            if (slot->mutable && slot->type==payload && slot->module<d->module_count &&
                d->functions[a->function].module==slot->module &&
                d->modules[slot->module].initializer==a->function && xr_xir_type_is_cell(types,payload)) {
                XrXirType scalar=xr_xir_cell_element(types,payload);
                /* Publishing scalar storage does not access another referent.
                 * Ordinary IR, Cell origin and runtime publication checks retain authority. */
                if (view_access_scalar_storage(scalar)) return XR_XIR_OK;
                const XrXirTypeNode *array=xr_xir_type_node(types,scalar);
                /* Publishing an owned Cell handle does not read its referent.
                 * String and scalar Array storage have no borrowed descendants. */
                bool storage=scalar==XR_XIR_STRING || (array && array->kind==XR_XIR_TYPE_ARRAY &&
                    view_access_scalar_storage(array->element));
                if (storage && !view_flags_at(types,flags,payload) &&
                    !view_flags_at(types,flags,scalar==XR_XIR_STRING?XR_XIR_STRING:array->element) &&
                    a->cells && a->cells->owner &&
                    xr_xir_cell_provenance_origin(a->cells->owner,a->function,op->args[0])==
                        (XR_XIR_CELL_ORIGIN_OWNED|XR_XIR_CELL_ORIGIN_MODULE)) return XR_XIR_OK;
            }
        }
    }
    if (view_access_structural(op->op)) return XR_XIR_OK;
    if (op->op==XR_XIR_VIEW_NEW || op->op==XR_XIR_VIEW_REBORROW) {
        uint32_t source=a->body->operands[op->args[0]];bool sub=op->op==XR_XIR_VIEW_REBORROW;
        XrXirStatus status=sub?XR_XIR_OK:view_access_constructor(a,source);
        return status==XR_XIR_OK?view_access_use(a,&event->before,source,view_access_mutable(a,op->type),sub,NULL):status;
    }
    if (op->op==XR_XIR_VIEW_LEN || op->op==XR_XIR_VIEW_GET || op->op==XR_XIR_VIEW_SET || op->op==XR_XIR_VIEW_COPY) {
        uint32_t source=op->op==XR_XIR_VIEW_SET?a->body->operands[op->args[0]]:op->args[0];
        return view_access_use(a,&event->before,source,op->op==XR_XIR_VIEW_SET,true,NULL);
    }
    bool call=op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT;
    if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        if (!a->cells || !a->cells->default_access || view_flags_at(types,flags,op->type))
            return XR_XIR_UNRESOLVED;
        /* Declaration identity is not SSA input. The scope authenticates the
         * actual helper closure without retiring any caller loan or epoch. */
        return xir_effects_default_access_site(a->cells->default_access,
            a->resolver->check.context,a->function,event->instruction);
    }
    if (op->op==XR_XIR_CALL_REQUIREMENT) return XR_XIR_UNRESOLVED;
    if (((op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_INDIRECT) || op->op==XR_XIR_INVOKE_RESULT) && view_flags_at(types,flags,op->type)) {
        XrXirStatus status=view_access_constructor(a,result);if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t p=0;p<view_access_operands(a,op);++p) {

        uint32_t value=xr_xir_op_uses_operand_table(op->op)?a->body->operands[op->args[0]+p]:op->args[p];
        XrXirType type=xr_xir_operand_type(a->body,value);uint32_t borrowed=view_flags_at(types,flags,type);
        bool cell=xr_xir_type_is_cell(types,type),object=xr_xir_type_is_class(types,type);
        XrXirPlaceKind place=xr_xir_place_kind(a->body,value);XrXirOperandRole role=xr_xir_operand_role(op->op,p);
        if (borrowed) {
            if (!call) return XR_XIR_UNRESOLVED;
            XrXirStatus status=view_access_use(a,&event->before,value,(borrowed&XR_XIR_VIEW_MUTABLE)!=0,true,NULL);
            if (status!=XR_XIR_OK) return status;
        } else if (cell || object || place!=XR_XIR_PLACE_NONE) {
            bool read=op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CLASS_GET || role==XR_XIR_OPERAND_READ;
            bool write=call || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE ||
                op->op==XR_XIR_CELL_STRING_APPEND || op->op==XR_XIR_CLASS_SET || role==XR_XIR_OPERAND_WRITE;
            if (!read && !write) return XR_XIR_UNRESOLVED;
            bool blocked=false;XrXirStatus status=view_access_use(a,&event->before,value,write,false,&blocked);
            if (place==XR_XIR_PLACE_LOCAL && (status==XR_XIR_BAD_TYPE || (status==XR_XIR_BAD_VALUE && !blocked)))
                return XR_XIR_UNRESOLVED;
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_visit(void *user,const XrXirCompileContext *remaining,const XrXirViewOwnerEvent *event) {
    if (event->kind!=XR_XIR_VIEW_OWNER_INSTRUCTION) return XR_XIR_OK;
    ViewAccess *a=user;ViewRootCheck *c=&a->resolver->check;const XrXirCompileContext *original=c->context;
    XrXirCompileContext limited=*original;
    /* The scoped producer already subtracted this function's initial receiver
     * storage. Add it back exactly once to cap their simultaneous residents. */
    if (a->initial_reservation>UINT64_MAX-remaining->limits.frame_bytes) return XR_XIR_BUDGET;
    limited.limits.frame_bytes=remaining->limits.frame_bytes+a->initial_reservation;
    a->instruction=event->instruction;
    c->context=&limited;XrXirStatus status=view_access_instruction(a,event);c->context=original;return status;
}
static XrXirStatus view_access_function_needed(ViewAccess *a,uint32_t function,bool *needed) {
    ViewRootCheck *c=&a->resolver->check;const XrXirFunction *body=c->module->functions+function;*needed=false;
    uint64_t values=(uint64_t)body->parameter_count+body->instruction_count;
    if (values>UINT32_MAX) return XR_XIR_BUDGET;
    for (uint32_t v=0;v<(uint32_t)values;++v) {

        *needed|=view_flags_at(c->module->types,c->flags,xr_xir_operand_type(body,v))!=0;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_check(const XrXirCompileContext *context,
    const XrXirModule *module,const XirViewAccessCells *cells,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !module->functions || !module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    bool needed=false;
    if (module->types) for (uint32_t i=0;i<module->types->count;++i) {

        uint32_t kind=module->types->nodes[i].kind;needed|=kind==XR_XIR_TYPE_SLICE || kind==XR_XIR_TYPE_MUT_SLICE;
    }
    if (!needed) return XR_XIR_OK;
    XrXirStatus status=xir_compile_view_roots_verified(context,module,NULL,NULL,diagnostic);
    XirViewRootResolver *resolver=NULL;
    if (status==XR_XIR_OK) status=xir_compile_view_root_resolver_new(context,module,&resolver,diagnostic);
    if (status!=XR_XIR_OK) return status;
    ViewRootCheck *c=&resolver->check;uint64_t fixed=sizeof(ViewAccess)+sizeof(XrXirCompileContext);
    if (c->reserved>context->limits.frame_bytes || fixed>context->limits.frame_bytes-c->reserved) {
        xir_compile_view_root_resolver_free(resolver);return XR_XIR_BUDGET;
    }
    c->reserved+=fixed;ViewAccess access={.resolver=resolver,.cells=cells};
    for (uint32_t f=0;f<module->function_count;++f) {
        status=view_access_function_needed(&access,f,&needed);if (status!=XR_XIR_OK) break;
        if (!needed) continue;
        status=view_access_function_new(&access,f);
        if (status==XR_XIR_OK) {
            access.initial_reservation=c->reserved;XrXirCompileContext remaining=*context;
            remaining.limits.frame_bytes-=access.initial_reservation;
            status=xir_compile_view_may_owned_function_scoped(&remaining,module,f,view_access_visit,&access,diagnostic);
        }
        view_access_function_free(&access);if (status!=XR_XIR_OK) break;
    }
    XR_CHECK(c->reserved>=fixed,"access checking retires its receiver after every borrowed callback");
    c->reserved-=fixed;xir_compile_view_root_resolver_free(resolver);return status;
}
XR_FUNC XrXirStatus xir_compile_view_access_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirDiagnostic *diagnostic) {
    return view_access_check(context,module,NULL,diagnostic);
}
XR_FUNC XrXirStatus xir_compile_view_access_cells_verified(const XrXirCompileContext *context,
    const XrXirModule *module,const XirViewAccessCells *cells,XrXirDiagnostic *diagnostic) {
    if (!cells || !cells->owner || !cells->role) return XR_XIR_BAD_STRUCTURE;
    return view_access_check(context,module,cells,diagnostic);
}
