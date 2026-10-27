/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch_origin.inc.c - Owned Source operand selectors on observed references
 */
static bool inspect_origin_equal(const XdapValueOrigin *left,const XdapValueOrigin *right) {
    if (!left->epoch) return !right || !right->epoch;
    if (!right || left->epoch!=right->epoch || left->frame!=right->frame || left->operand!=right->operand || left->slot!=right->slot ||
        left->path.count!=right->path.count) return false;
    for (uint32_t i=0;i<left->path.count;++i) {
        const XrXirValuePathStep *a=&left->path.steps[i],*b=&right->path.steps[i];
        if (a->kind!=b->kind || a->container!=b->container || a->selector!=b->selector) return false;
    }
    return true;
}
static XrXirStatus inspect_origin_copy(XdapAdapter *s,const XdapValueOrigin *origin,XdapValueOrigin *output) {
    if (!origin || !origin->epoch) return XR_XIR_OK;
    if (!origin->frame || origin->path.count>128 || (origin->path.count && !origin->path.steps)) return XR_XIR_BAD_STRUCTURE;
    XrXirValuePathStep *steps=NULL;
    if (origin->path.count) {
        size_t bytes=(size_t)origin->path.count*sizeof(*steps);
        XrCompileResourceStatus status=xr_compile_resources_alloc(s->resources,bytes,(void **)&steps);
        if (status!=XR_COMPILE_RESOURCE_OK) return status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        status==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_OVERFLOW :
        status==XR_COMPILE_RESOURCE_CANCELLED ? XR_XIR_CANCELLED :
        status==XR_COMPILE_RESOURCE_DEADLINE ? XR_XIR_DEADLINE :
        status==XR_COMPILE_RESOURCE_CLOCK_FAILURE ? XR_XIR_CLOCK_FAILURE : XR_XIR_BAD_STRUCTURE;
        memcpy(steps,origin->path.steps,bytes);
    }
    *output=*origin;output->path.steps=steps;return XR_XIR_OK;
}
static XrXirStatus inspect_origin_child(XdapAdapter *s,const XdapValueRef *parent,const XdapValueChildInfo *child,
    XrXirValuePathStep steps[128],XdapValueOrigin *output) {
    if (!parent->origin.epoch || !child->selected) return XR_XIR_OK;
    if (parent->origin.epoch!=s->stop_epoch) return XR_XIR_BAD_STRUCTURE;
    uint32_t count=parent->origin.path.count;if (count>=128) return XR_XIR_BUDGET;
    if (count) memcpy(steps,parent->origin.path.steps,(size_t)count*sizeof(*steps));
    steps[count]=child->selector;*output=parent->origin;output->path=(XrXirValuePath){steps,count+1};return XR_XIR_OK;
}
