/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_access_relation.inc.c - Correlated stable ancestry and storage facts
 */
typedef enum ViewAccessDecision { VIEW_ACCESS_SAFE,VIEW_ACCESS_BLOCKED,VIEW_ACCESS_UNKNOWN } ViewAccessDecision;
typedef enum ViewAccessAncestor { VIEW_ACCESS_NO_ANCESTOR,VIEW_ACCESS_ANCESTOR,VIEW_ACCESS_ANCESTRY_UNKNOWN } ViewAccessAncestor;
static XrXirStatus view_access_compatible(const XrXirViewLoanOrigin *left,
    const XrXirViewLoanOrigin *right,bool *compatible) {
    *compatible=true;
    for (uint32_t i=0;i<left->choice_count;++i) for (uint32_t j=0;j<right->choice_count;++j) {

        if (left->choices[i].block==right->choices[j].block &&
            left->choices[i].predecessor!=right->choices[j].predecessor) *compatible=false;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_origin_stable(ViewAccess *a,const ViewAccessOrigin *origin,bool *stable) {
    const XrXirViewLoanOrigin *event=&origin->event;
    *stable=false;if (event->repeated_phi || !view_access_leaf(a,event->type) || event->kind==XR_XIR_VIEW_LOAN_UNRESOLVED)
        return XR_XIR_OK;
    for (uint32_t i=0;i<event->choice_count;++i) {

        if (a->repeated[event->choices[i].block]) return XR_XIR_OK;
    }
    XrXirStatus status=view_access_stable(a,event->value,stable);
    if (status!=XR_XIR_OK || *stable) return status;
    if (event->kind==XR_XIR_VIEW_LOAN_NEW || event->kind==XR_XIR_VIEW_LOAN_REBORROW)
        return view_access_epoch_current(a,origin,stable);
    bool prefix=false;status=view_access_prefix_scope(a,event->value,&prefix);
    if (status!=XR_XIR_OK || !prefix) return status;
    if (origin->queried_value==event->value) {*stable=true;return XR_XIR_OK;}
    return view_access_epoch_current(a,origin,stable);
}
static bool view_access_same_scope(const XrXirViewLoanOrigin *left,const XrXirViewLoanOrigin *right) {
    return left->kind==right->kind && left->function==right->function && left->value==right->value &&
        left->selection_count==right->selection_count && (!left->selection_count ||
        !memcmp(left->selection,right->selection,left->selection_count*sizeof(uint32_t)));
}
static XrXirStatus view_access_pinned_choices(const XrXirViewLoanOrigin *event,
    const XrXirViewLoanOrigin *left,const XrXirViewLoanOrigin *right,bool *pinned) {
    *pinned=true;
    for (uint32_t i=0;i<event->choice_count;++i) {
        bool found=false;
        for (uint32_t side=0;side<2;++side) {
            const XrXirViewLoanOrigin *source=side?right:left;
            for (uint32_t j=0;j<source->choice_count;++j) {

                found|=event->choices[i].block==source->choices[j].block &&
                    event->choices[i].predecessor==source->choices[j].predecessor;
            }
        }
        if (!found) *pinned=false;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_ancestor(ViewAccess *a,const ViewAccessOrigin *parent_origin,
    const ViewAccessOrigin *child_origin,ViewAccessAncestor *output) {
    const XrXirViewLoanOrigin *parent=&parent_origin->event,*child=&child_origin->event;
    *output=VIEW_ACCESS_ANCESTRY_UNKNOWN;const ViewAccessOrigin *current_origin=child_origin;
    uint64_t bound=(uint64_t)a->body->parameter_count+a->body->instruction_count;
    for (uint64_t depth=0;depth<bound;++depth) {
        const XrXirViewLoanOrigin *current=&current_origin->event;

        if (current->kind==XR_XIR_VIEW_LOAN_NEW) {*output=VIEW_ACCESS_NO_ANCESTOR;return XR_XIR_OK;}
        if (current->kind==XR_XIR_VIEW_LOAN_INPUT) {
            if (parent->kind!=XR_XIR_VIEW_LOAN_INPUT) *output=VIEW_ACCESS_NO_ANCESTOR;
            return XR_XIR_OK;
        }
        if (current->kind!=XR_XIR_VIEW_LOAN_REBORROW && current->kind!=XR_XIR_VIEW_LOAN_CALL) return XR_XIR_OK;
        XrXirType source=xr_xir_operand_type(a->body,current->source);
        if (xr_xir_type_is_cell(a->resolver->check.module->types,source)) {*output=VIEW_ACCESS_NO_ANCESTOR;return XR_XIR_OK;}
        if (!view_access_leaf(a,source)) return XR_XIR_OK;
        ViewAccessOrigin *origins=NULL;XrXirStatus status=view_access_origins(a,current->source,&origins);
        if (status!=XR_XIR_OK) return status;
        const ViewAccessOrigin *next=NULL;uint32_t count=0,matches=0;
        for (ViewAccessOrigin *p=origins;p;p=p->next) {
            bool compatible=false,other=false,stable=false;
            status=view_access_compatible(parent,&p->event,&compatible);
            if (status==XR_XIR_OK) status=view_access_compatible(child,&p->event,&other);
            if (status!=XR_XIR_OK) return status;if (!compatible || !other || p->event.kind==XR_XIR_VIEW_LOAN_ABSENT) continue;
            status=view_access_origin_stable(a,p,&stable);if (status!=XR_XIR_OK) return status;
            if (stable) {
                bool unique=false;status=view_access_stable(a,p->event.value,&unique);
                /* The child must still refer to this execution of its parent. */
                if (status==XR_XIR_OK && !unique)
                    status=view_access_root_window(a,current->value,p->event.value,a->instruction,&stable);
                if (status!=XR_XIR_OK) return status;
            }
            if (!stable) return XR_XIR_OK;
            ++count;matches+=view_access_same_scope(parent,&p->event);next=p;
        }
        if (!count) return XR_XIR_OK;
        if (matches==count) {*output=VIEW_ACCESS_ANCESTOR;return XR_XIR_OK;}
        if (count!=1) return XR_XIR_OK;
        XR_CHECK(next,"a unique compatible parent keeps its owned scope observation");
        bool pinned=false;status=view_access_pinned_choices(&next->event,parent,child,&pinned);
        if (status!=XR_XIR_OK || !pinned) return status;current_origin=next;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_fresh_input(ViewAccess *a,const XrXirViewRootEvent *fresh,
    const XrXirViewRootEvent *input,uint32_t queried,bool *disjoint) {
    *disjoint=false;
    if (fresh->opaque_suffix || input->opaque_suffix || fresh->dynamic_identity || input->dynamic_identity ||
        fresh->kind!=XR_XIR_VIEW_ROOT_CELL || fresh->root_function!=a->function || input->root_function!=a->function ||
        (input->kind!=XR_XIR_VIEW_ROOT_PARAMETER && input->kind!=XR_XIR_VIEW_ROOT_BORROWED_PARAMETER)) return XR_XIR_OK;
    for (uint32_t i=0;i<fresh->step_count;++i) {

        if (xr_xir_type_is_class(a->resolver->check.module->types,fresh->steps[i].container)) return XR_XIR_OK;
    }
    XrXirStatus status=view_access_stable(a,fresh->root_value,disjoint);
    return status==XR_XIR_OK && !*disjoint?view_access_root_epoch(a,fresh,queried,disjoint):status;
}
static XrXirStatus view_access_overlap(ViewAccess *a,const XrXirViewRootEvent *left,
    const XrXirViewRootEvent *right,uint32_t left_query,uint32_t right_query,bool *overlap) {
    *overlap=false;ViewRootCheck *c=&a->resolver->check;
    if (left->opaque_suffix || right->opaque_suffix || left->dynamic_identity || right->dynamic_identity ||
        left->kind!=right->kind || left->root_function!=a->function || right->root_function!=a->function ||
        left->selection_count!=right->selection_count || (left->selection_count &&
        memcmp(left->selection,right->selection,left->selection_count*sizeof(uint32_t))) ||
        left->root_value!=right->root_value || (left->kind!=XR_XIR_VIEW_ROOT_CELL &&
        left->kind!=XR_XIR_VIEW_ROOT_PARAMETER && left->kind!=XR_XIR_VIEW_ROOT_BORROWED_PARAMETER)) return XR_XIR_OK;
    bool stable=false;XrXirStatus status=view_access_stable(a,left->root_value,&stable);
    if (status!=XR_XIR_OK) return status;
    if (!stable) {
        bool l=false,r=false;status=view_access_root_epoch(a,left,left_query,&l);
        if (status==XR_XIR_OK && l) status=view_access_root_epoch(a,right,right_query,&r);
        if (status!=XR_XIR_OK || !l || !r) return status;
    }
    uint32_t common=left->step_count<right->step_count?left->step_count:right->step_count;
    for (uint32_t i=0;i<common;++i) {

        const XrXirViewRootStep *l=left->steps+i,*r=right->steps+i;
        if (xr_xir_type_is_class(c->module->types,l->container) ||
            l->operation!=r->operation || l->container!=r->container) return XR_XIR_OK;
        if (l->operation==XR_XIR_FIELD_PLACE) {if (l->selector!=r->selector) return XR_XIR_OK;}
        else if (l->operation==XR_XIR_INDEX_PLACE) {
            ViewRootKey relation=VIEW_ROOT_KEY_UNKNOWN;status=view_root_keys(c,l,r,&relation);
            if (status!=XR_XIR_OK || relation!=VIEW_ROOT_KEY_EQUAL) return status;
        } else return XR_XIR_OK;
    }
    /* A Class boundary in either suffix denotes another storage identity. */
    for (uint32_t side=0;side<2;++side) {
        const XrXirViewRootEvent *event=side?right:left;
        for (uint32_t i=common;i<event->step_count;++i) {

            if (xr_xir_type_is_class(c->module->types,event->steps[i].container)) return XR_XIR_OK;
        }
    }
    *overlap=true;return XR_XIR_OK;
}
static void view_access_roots_free(ViewAccess *a,ViewRootComparison *roots) {
    while (roots->left) {ViewRootSaved *p=roots->left;roots->left=p->next;view_access_release(a,p,p->bytes);}
}
static XrXirStatus view_access_storage(ViewAccess *a,uint32_t left,uint32_t right,
    uint32_t left_query,uint32_t right_query,ViewAccessDecision *output) {
    *output=VIEW_ACCESS_UNKNOWN;ViewRootComparison l={a->resolver,NULL,false},r=l;
    uint64_t reserved=a->resolver->check.reserved;
    XirViewRootRequest request={a->function,left,a->instruction,NULL,0};
    XrXirStatus status=xir_compile_view_root_resolver_query(a->resolver,&request,view_root_save,&l,NULL);
    request.value=right;if (status==XR_XIR_OK) status=xir_compile_view_root_resolver_query(a->resolver,&request,view_root_save,&r,NULL);
    bool all_safe=true,all_overlap=true;
    for (ViewRootSaved *p=l.left;p && status==XR_XIR_OK;p=p->next) for (ViewRootSaved *q=r.left;q;q=q->next) {
        bool disjoint=false,fresh=false,entry=false,epochs=false,overlap=false;
        status=view_root_path_pair(&a->resolver->check,&p->event,&q->event,&disjoint);
        if (status==XR_XIR_OK && !disjoint) status=view_access_fresh_input(a,&p->event,&q->event,left_query,&fresh);
        if (status==XR_XIR_OK && !disjoint && !fresh) status=view_access_fresh_input(a,&q->event,&p->event,right_query,&fresh);
        if (status==XR_XIR_OK && !disjoint && !fresh) status=view_access_entry_regions(a,&p->event,&q->event,&entry);
        if (status==XR_XIR_OK && !disjoint && !fresh && !entry) status=view_access_epoch_separation(a,&p->event,&q->event,left_query,right_query,&epochs);
        if (status==XR_XIR_OK && !disjoint && !fresh && !entry && !epochs) status=view_access_overlap(a,&p->event,&q->event,left_query,right_query,&overlap);
        if (status!=XR_XIR_OK) break;all_safe&=disjoint || fresh || entry || epochs;all_overlap&=overlap;
    }
    if (status==XR_XIR_OK && l.left && r.left) *output=all_safe?VIEW_ACCESS_SAFE:all_overlap?VIEW_ACCESS_BLOCKED:VIEW_ACCESS_UNKNOWN;
    view_access_roots_free(a,&l);view_access_roots_free(a,&r);
    XR_CHECK(a->resolver->check.reserved==reserved,"storage comparisons retire every saved producer path");
    return status;
}
