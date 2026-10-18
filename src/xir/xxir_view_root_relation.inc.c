/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_root_relation.inc.c - Producer-derived storage path separation
 */

#include "xxir_key.h"
typedef enum ViewRootKey { VIEW_ROOT_KEY_UNKNOWN,VIEW_ROOT_KEY_EQUAL,VIEW_ROOT_KEY_DISTINCT } ViewRootKey;
typedef struct ViewRootSaved {
    struct ViewRootSaved *next;
    XrXirViewRootEvent event;
    uint64_t bytes;
} ViewRootSaved;
typedef struct ViewRootComparison {
    XirViewRootResolver *resolver;
    ViewRootSaved *left;
    bool disjoint;
} ViewRootComparison;

static XrXirStatus view_root_key_source(ViewRootCheck *c,ViewOriginPair *site,const XrXirInstruction **op) {
    const XrXirFunction *f=c->module->functions+site->first;*op=NULL;
    if (site->second>=(uint64_t)f->parameter_count+f->instruction_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t depth=0;depth<=f->instruction_count;++depth) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        if (site->second<f->parameter_count) return XR_XIR_OK;
        *op=f->instructions+site->second-f->parameter_count;
        if ((*op)->op!=XR_XIR_COPY && (*op)->op!=XR_XIR_SCALAR_COPY && (*op)->op!=XR_XIR_OWNED_RETAIN)
            return XR_XIR_OK;
        site->second=(*op)->args[0];
    }
    return XR_XIR_BAD_VALUE;
}
static uint64_t view_root_float_key(uint64_t bits,uint32_t width) {
    uint64_t sign=width==32?UINT64_C(0x80000000):UINT64_C(0x8000000000000000);
    uint64_t infinity=width==32?UINT64_C(0x7f800000):UINT64_C(0x7ff0000000000000);
    if (width==32) bits&=UINT64_C(0xffffffff);
    uint64_t magnitude=bits&~sign;
    return magnitude>infinity?infinity+1u:!magnitude?0:bits;
}
static XrXirStatus view_root_key_scalar(ViewRootCheck *c,const XrXirInstruction *left,
    const XrXirInstruction *right,XrXirType type,ViewRootKey *result) {
    if (!left || !right) return XR_XIR_OK;
    XrXirOp kind=type==XR_XIR_BOOL?XR_XIR_CONST_BOOL:type==XR_XIR_RUNE?XR_XIR_CONST_RUNE:
        xr_xir_type_is_integer(type)?XR_XIR_CONST_INT:xr_xir_float_bits(type)?XR_XIR_CONST_FLOAT:
        type==XR_XIR_STRING?XR_XIR_CONST_STRING:XR_XIR_INVALID;
    if (!kind || left->op!=kind || right->op!=kind) return XR_XIR_OK;
    bool equal=false;
    if (kind==XR_XIR_CONST_STRING) {
        const XrXirDeclarations *d=c->module->declarations;
        if (!d || left->immediate<0 || right->immediate<0 ||
            (uint64_t)left->immediate>=d->literal_count || (uint64_t)right->immediate>=d->literal_count)
            return XR_XIR_BAD_STRUCTURE;
        XrXirLiteral a=d->literals[left->immediate],b=d->literals[right->immediate];
        if (a.length==b.length) {
            if (!xir_compile_work(c->context,a.length)) return XR_XIR_BUDGET;
            equal=!a.length || !memcmp(a.bytes,b.bytes,a.length);
        }
    } else if (kind==XR_XIR_CONST_FLOAT) {
        uint32_t width=xr_xir_float_bits(type);
        equal=view_root_float_key((uint64_t)left->immediate,width)==view_root_float_key((uint64_t)right->immediate,width);
    } else equal=left->immediate==right->immediate;
    *result=equal?VIEW_ROOT_KEY_EQUAL:VIEW_ROOT_KEY_DISTINCT;return XR_XIR_OK;
}
static XrXirStatus view_root_keys(ViewRootCheck *c,const XrXirViewRootStep *left,
    const XrXirViewRootStep *right,ViewRootKey *result) {
    *result=VIEW_ROOT_KEY_UNKNOWN;
    const XrXirTypeNode *container=xr_xir_type_node(c->module->types,left->container);
    if (!container || (container->kind!=XR_XIR_TYPE_MAP && container->kind!=XR_XIR_TYPE_ARRAY))
        return XR_XIR_BAD_TYPE;
    XrXirType type=container->kind==XR_XIR_TYPE_MAP?container->element:XR_XIR_I64;
    /* Structural shape never supplies a user key's equality contract. */
    if (!xr_xir_key_builtin_type(type)) return XR_XIR_OK;
    ViewOriginPair a={left->function,left->selector},b={right->function,right->selector};
    const XrXirInstruction *av=NULL,*bv=NULL;XrXirStatus status=view_root_key_source(c,&a,&av);
    if (status==XR_XIR_OK) status=view_root_key_source(c,&b,&bv);
    if (status!=XR_XIR_OK) return status;
    if (xr_xir_operand_type(c->module->functions+a.first,a.second)!=type ||
        xr_xir_operand_type(c->module->functions+b.first,b.second)!=type) return XR_XIR_BAD_TYPE;
    if (a.first==b.first && a.second==b.second) {*result=VIEW_ROOT_KEY_EQUAL;return XR_XIR_OK;}
    return view_root_key_scalar(c,av,bv,type,result);
}
static XrXirStatus view_root_class_suffix(ViewRootCheck *c,const XrXirViewRootEvent *event,
    uint32_t *start,bool *object) {
    *start=0;*object=event->dynamic_identity;
    for (uint32_t i=0;i<event->step_count;++i) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        if (xr_xir_type_is_class(c->module->types,event->steps[i].container)) {*start=i;*object=true;}
    }
    return XR_XIR_OK;
}
static XrXirStatus view_root_path_pair(ViewRootCheck *c,const XrXirViewRootEvent *left,
    const XrXirViewRootEvent *right,bool *output) {
    *output=false;if (!xir_compile_work(c->context,4)) return XR_XIR_BUDGET;
    if (left->kind==XR_XIR_VIEW_ROOT_ABSENT || right->kind==XR_XIR_VIEW_ROOT_ABSENT) {*output=true;return XR_XIR_OK;}
    if (left->opaque_suffix || right->opaque_suffix || left->kind==XR_XIR_VIEW_ROOT_UNRESOLVED ||
        right->kind==XR_XIR_VIEW_ROOT_UNRESOLVED) return XR_XIR_OK;
    uint32_t a=0,b=0;bool ao=false,bo=false;XrXirStatus status=view_root_class_suffix(c,left,&a,&ao);
    if (status==XR_XIR_OK) status=view_root_class_suffix(c,right,&b,&bo);if (status!=XR_XIR_OK) return status;
    bool same=left->kind==right->kind && left->root_function==right->root_function && left->root_value==right->root_value;
    if (!ao && !bo && left->kind==XR_XIR_VIEW_ROOT_CELL && right->kind==XR_XIR_VIEW_ROOT_CELL && !same) {
        *output=true;return XR_XIR_OK;
    }
    /* An owning Cell's value storage is separate from a Class body. This
     * does not infer distinct identities for two Class values or parameters. */
    if ((!ao && left->kind==XR_XIR_VIEW_ROOT_CELL && bo) ||
        (!bo && right->kind==XR_XIR_VIEW_ROOT_CELL && ao)) {*output=true;return XR_XIR_OK;}
    if (!(same && !ao && !bo) && !(ao && bo)) return XR_XIR_OK;
    while (a<left->step_count && b<right->step_count) {
        if (!xir_compile_work(c->context,2)) return XR_XIR_BUDGET;
        const XrXirViewRootStep *l=left->steps+a++,*r=right->steps+b++;
        if (l->operation!=r->operation || l->container!=r->container) return XR_XIR_OK;
        if (l->operation==XR_XIR_FIELD_PLACE) {
            if (l->selector!=r->selector) {*output=true;return XR_XIR_OK;}
        } else if (l->operation==XR_XIR_INDEX_PLACE) {
            ViewRootKey relation=VIEW_ROOT_KEY_UNKNOWN;status=view_root_keys(c,l,r,&relation);
            if (status!=XR_XIR_OK) return status;
            if (relation==VIEW_ROOT_KEY_DISTINCT) {*output=true;return XR_XIR_OK;}
        } else return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_root_save(void *user,const XrXirCompileContext *context,const XrXirViewRootEvent *event) {
    ViewRootComparison *comparison=user;ViewRootCheck *c=&comparison->resolver->check;
    uint64_t steps=(uint64_t)event->step_count*sizeof(*event->steps),selected=(uint64_t)event->selection_count*sizeof(uint32_t);
    uint64_t bytes=sizeof(ViewRootSaved)+steps+selected;
    if (bytes>SIZE_MAX || c->reserved>context->limits.frame_bytes || bytes>context->limits.frame_bytes-c->reserved)
        return XR_XIR_BUDGET;
    if (!xir_compile_work(context,bytes)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;ViewRootSaved *saved=xir_compile_alloc(context,(size_t)bytes,&status);if (!saved) return status;
    c->reserved+=bytes;saved->next=comparison->left;saved->event=*event;saved->bytes=bytes;
    XrXirViewRootStep *path=(XrXirViewRootStep *)(saved+1);uint32_t *selection=(uint32_t *)((unsigned char *)path+steps);
    if (steps) memcpy(path,event->steps,(size_t)steps);if (selected) memcpy(selection,event->selection,(size_t)selected);
    saved->event.steps=event->step_count?path:NULL;saved->event.selection=event->selection_count?selection:NULL;
    comparison->left=saved;return XR_XIR_OK;
}
static XrXirStatus view_root_compare(void *user,const XrXirCompileContext *context,const XrXirViewRootEvent *event) {
    ViewRootComparison *comparison=user;
    for (ViewRootSaved *p=comparison->left;p;p=p->next) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        bool disjoint=false;XrXirStatus status=view_root_path_pair(&comparison->resolver->check,&p->event,event,&disjoint);
        if (status!=XR_XIR_OK) return status;comparison->disjoint&=disjoint;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_view_roots_disjoint(XirViewRootResolver *r,
    const XirViewRootRequest *left,const XirViewRootRequest *right,bool *output,XrXirDiagnostic *diagnostic) {
    if (output) *output=false;
    if (!r || !left || !right || !output || left->function!=right->function) return XR_XIR_BAD_STRUCTURE;
    if (r->busy) return XR_XIR_BAD_STAGE;r->busy=true;
    ViewRootComparison comparison={r,NULL,true};XrXirStatus status=view_root_resolve(r,left,view_root_save,&comparison);
    if (status==XR_XIR_OK) status=view_root_resolve(r,right,view_root_compare,&comparison);
    while (comparison.left) {
        ViewRootSaved *p=comparison.left;comparison.left=p->next;
        XR_CHECK(r->check.reserved>=p->bytes,"saved root paths remain owned by their replay ledger");
        r->check.reserved-=p->bytes;xr_compile_resources_free(p);
    }
    r->busy=false;if (status==XR_XIR_OK) *output=comparison.disjoint;
    else if (diagnostic) {*diagnostic=r->check.location;diagnostic->status=status;}
    return status;
}
