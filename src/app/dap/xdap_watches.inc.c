/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_watches.inc.c - Session watch text without cached target authority
 */
#include "../../base/xchecks.h"
static bool watch_owner(const XdapAdapter *s) {
    return s && !s->busy && !s->closed && s->watch_count<=128 && s->watch_bytes<=s->config.message_limit;
}
XR_FUNC void xdap_watch_text_drop(XdapWatchText *text) {
    if (!text) return;xr_compile_resources_free(text->bytes);*text=(XdapWatchText){0};
}
XR_FUNC void xdap_watches_dispose(XdapAdapter *s) {
    XR_CHECK(s->watch_count<=128,"watch table stays within its finite owner");
    for (uint32_t i=0;i<s->watch_count;++i) xdap_watch_text_drop(&s->watches[i]);
    s->watch_count=0;s->watch_bytes=0;
}
static XrXirStatus watch_find(XdapAdapter *s,uint32_t id,uint32_t *at) {
    for (uint32_t i=0;i<s->watch_count;++i) {
        if (xr_compile_resources_work(s->resources,1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
        if (s->watches[i].id==id) { *at=i;return XR_XIR_OK; }
    }
    return XR_XIR_UNRESOLVED;
}
static XrXirStatus watch_prepare(XdapAdapter *s,const char *text,size_t length,bool intern,
    XdapWatchText *candidate,const XdapWatchText **selected) {
    if (!text || !length) return XR_XIR_BAD_TYPE;
    if (length>=s->config.message_limit) return XR_XIR_BUDGET;
    if (xr_compile_resources_work(s->resources,length+1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    if (memchr(text,0,length)) return XR_XIR_BAD_TYPE;
    if (intern) for (uint32_t i=0;i<s->watch_count;++i) {
        if (xr_compile_resources_work(s->resources,length+1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
        if (s->watches[i].length==length && !memcmp(s->watches[i].bytes,text,length)) {
            *selected=&s->watches[i];return XR_XIR_OK;
        }
    }
    if (s->watch_count==128 || length+1>s->config.message_limit-s->watch_bytes ||
        !s->next_watch_id || s->next_watch_id>INT32_MAX) return XR_XIR_BUDGET;
    char *bytes=NULL;XrCompileResourceStatus allocated=xr_compile_resources_alloc(s->resources,length+1,(void **)&bytes);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    memcpy(bytes,text,length);bytes[length]=0;
    *candidate=(XdapWatchText){s->next_watch_id,bytes,length};*selected=candidate;return XR_XIR_OK;
}
static void watch_commit(XdapAdapter *s,XdapWatchText *candidate) {
    if (!candidate->bytes) return;
    XR_CHECK(s->watch_count<128 && candidate->id==s->next_watch_id,"prepared watch retains its sole session identity");
    s->watches[s->watch_count++]=*candidate;s->watch_bytes+=candidate->length+1;++s->next_watch_id;
    *candidate=(XdapWatchText){0};
}
XR_FUNC XrXirStatus xdap_watch_add(XdapAdapter *s,const char *text,size_t length,uint32_t *id) {
    if (!watch_owner(s) || !id) return XR_XIR_BAD_STRUCTURE;
    XdapWatchText candidate={0};const XdapWatchText *selected=NULL;
    XrXirStatus status=watch_prepare(s,text,length,false,&candidate,&selected);
    if (status!=XR_XIR_OK) return status;
    *id=selected->id;watch_commit(s,&candidate);return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_watch_count(const XdapAdapter *s,uint32_t *count) {
    if (!watch_owner(s) || !count) return XR_XIR_BAD_STRUCTURE;
    *count=s->watch_count;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_watch_at(const XdapAdapter *s,uint32_t ordinal,XdapWatchText *output) {
    if (!watch_owner(s) || !output || output->id || output->bytes || output->length) return XR_XIR_BAD_STRUCTURE;
    if (ordinal>=s->watch_count) return XR_XIR_UNRESOLVED;
    const XdapWatchText *text=&s->watches[ordinal];char *bytes=NULL;
    if (xr_compile_resources_work(s->resources,text->length+1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    XrCompileResourceStatus allocated=xr_compile_resources_alloc(s->resources,text->length+1,(void **)&bytes);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    memcpy(bytes,text->bytes,text->length+1);*output=(XdapWatchText){text->id,bytes,text->length};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_watch_remove(XdapAdapter *s,uint32_t id) {
    if (!watch_owner(s) || !id) return XR_XIR_BAD_STRUCTURE;
    uint32_t at=0;XrXirStatus status=watch_find(s,id,&at);if (status!=XR_XIR_OK) return status;
    if (xr_compile_resources_work(s->resources,(uint64_t)(s->watch_count-at)*sizeof(*s->watches))!=XR_COMPILE_RESOURCE_OK)
        return XR_XIR_BUDGET;
    s->watch_bytes-=s->watches[at].length+1;xdap_watch_text_drop(&s->watches[at]);
    memmove(&s->watches[at],&s->watches[at+1],(s->watch_count-at-1)*sizeof(*s->watches));
    s->watches[--s->watch_count]=(XdapWatchText){0};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_watch_clear(XdapAdapter *s) {
    if (!watch_owner(s)) return XR_XIR_BAD_STRUCTURE;
    if (xr_compile_resources_work(s->resources,(uint64_t)s->watch_count*sizeof(*s->watches)+1)!=XR_COMPILE_RESOURCE_OK)
        return XR_XIR_BUDGET;
    xdap_watches_dispose(s);return XR_XIR_OK;
}
static XrXirStatus watch_evaluate_text(XdapAdapter *s,const char *text,size_t length,uint32_t frame,XrXirValue *output) {
    XdapLexicalView lexical={0};XrXirStatus status=xdap_lexical_open(s->launch,s->snapshot,frame,&lexical);
    if (status!=XR_XIR_OK) return status;
    XdapExpressionRequest request={s->compiler,&lexical,text,length,s->config.message_limit,NULL};
    return xdap_expression_evaluate(&request,output);
}
XR_FUNC XrXirStatus xdap_watch_evaluate(XdapAdapter *s,uint32_t id,uint32_t frame,XrXirValue *output) {
    if (!watch_owner(s) || !id || !frame || !output || output->type || output->reserved || output->payload ||
        s->running || !s->snapshot || !s->stop_epoch) return XR_XIR_BAD_STRUCTURE;
    uint32_t at=0;XrXirStatus status=watch_find(s,id,&at);if (status!=XR_XIR_OK) return status;
    uint32_t selected=inspect_frame(s,frame,false);if (selected>=s->depth) return XR_XIR_BAD_STRUCTURE;
    s->busy=true;const XdapWatchText *text=&s->watches[at];
    status=watch_evaluate_text(s,text->bytes,text->length,selected,output);s->busy=false;return status;
}
