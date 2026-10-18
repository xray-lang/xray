/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_predicates.c - Owned predicates evaluated at authenticated VM stops
 */
#include "xdap_dispatch_internal.h"
#include "xdap_expression.h"
#include "xdap_lexical.h"
#include "../../xir/xxir_vm_debug_internal.h"
#include "../../xir/xxir_scalar_core.h"
#include <string.h>
static XrXirStatus predicate_copy(XdapAdapter *s,const XdapJsonNode *node,char **output) {
    if (!node || !node->length) return XR_XIR_OK;
    if (xr_compile_resources_work(s->resources,node->length+1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    char *text=NULL;XrCompileResourceStatus allocated=xr_compile_resources_alloc(s->resources,node->length+1,(void **)&text);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    memcpy(text,node->text,node->length);text[node->length]=0;*output=text;return XR_XIR_OK;
}
/* Escaped braces are literal. Every interpolation is one complete, admitted
 * expression; an unmatched brace never degrades into a silent literal. */
static XrXirStatus predicate_span(const char *text,size_t length,size_t *cursor,size_t *begin,size_t *bytes) {
    size_t at=*cursor;*begin=at;*bytes=0;
    if (text[at]=='{' && at+1<length && text[at+1]=='{') { *cursor=at+2;return XR_XIR_UNRESOLVED; }
    if (text[at]=='}') {
        if (at+1<length && text[at+1]=='}') { *cursor=at+2;return XR_XIR_UNRESOLVED; }
        return XR_XIR_BAD_TYPE;
    }
    if (text[at]!='{') { *cursor=at+1;return XR_XIR_UNRESOLVED; }
    *begin=++at;
    while (at<length && text[at]!='}') { if (text[at]=='{') return XR_XIR_BAD_TYPE;++at; }
    if (at==length || at==*begin) return XR_XIR_BAD_TYPE;
    *bytes=at-*begin;*cursor=at+1;return XR_XIR_OK;
}
XR_FUNC void xdap_predicate_free(XdapBreakpointPredicate *predicate) {
    if (!predicate) return;
    xr_compile_resources_free(predicate->condition);xr_compile_resources_free(predicate->log);*predicate=(XdapBreakpointPredicate){0};
}
XR_FUNC XrXirStatus xdap_predicate_prepare(XdapAdapter *s,const XdapRequest *request,uint32_t condition,uint32_t log,
    XdapBreakpointPredicate *output) {
    if (!s || !request || !output || output->condition || output->log || output->condition_length || output->log_length || output->logpoint) return XR_XIR_BAD_STRUCTURE;
    const XdapJsonNode *test=condition ? &request->document.nodes[condition] : NULL,*message=log ? &request->document.nodes[log] : NULL;
    if ((test && test->kind!=XDAP_JSON_STRING) || (message && message->kind!=XDAP_JSON_STRING)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=test && test->length ? xdap_expression_validate(s->compiler,test->text,test->length) : XR_XIR_OK;
    if (status!=XR_XIR_OK) return status;
    if (message) {
        if (xr_compile_resources_work(s->resources,message->length)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
        for (size_t cursor=0;cursor<message->length;) {
            size_t begin=0,bytes=0;status=predicate_span(message->text,message->length,&cursor,&begin,&bytes);
            if (status==XR_XIR_UNRESOLVED) continue;
            if (status!=XR_XIR_OK) return status;
            status=xdap_expression_validate(s->compiler,message->text+begin,bytes);if (status!=XR_XIR_OK) return status;
        }
    }
    XdapBreakpointPredicate fresh={0};status=predicate_copy(s,test,&fresh.condition);
    if (status==XR_XIR_OK) status=predicate_copy(s,message,&fresh.log);
    if (status!=XR_XIR_OK) { xdap_predicate_free(&fresh);return status; }
    fresh.condition_length=test ? test->length : 0;fresh.log_length=message ? message->length : 0;fresh.logpoint=message!=NULL;*output=fresh;return XR_XIR_OK;
}
static XrXirStatus predicate_evaluate(XdapAdapter *s,const char *text,size_t length,XrXirValue *output) {
    XdapLexicalView lexical={0};XrXirStatus status=xdap_lexical_open(s->launch,s->snapshot,0,&lexical);
    if (status!=XR_XIR_OK) return status;
    XdapExpressionRequest request={s->compiler,&lexical,text,length,s->config.message_limit,NULL};return xdap_expression_evaluate(&request,output);
}
static XrXirStatus predicate_log(XdapAdapter *s,const XdapBreakpointPredicate *predicate,XdapJsonWriter *text) {
    if (xr_compile_resources_work(s->resources,predicate->log_length)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    for (size_t cursor=0;cursor<predicate->log_length;) {
        if (text->status!=XR_XIR_OK) return text->status;
        size_t at=cursor,begin=0,bytes=0;XrXirStatus status=predicate_span(predicate->log,predicate->log_length,&cursor,&begin,&bytes);
        if (status==XR_XIR_UNRESOLVED) { xdap_json_text(text,predicate->log+at,1);continue; }
        if (status!=XR_XIR_OK) return status;
        XrXirValue value={0};status=predicate_evaluate(s,predicate->log+begin,bytes,&value);if (status!=XR_XIR_OK) return status;
        const char *content=NULL;size_t size=0;char scalar[XR_XIR_SCALAR_TEXT_BYTES];
        if (value.type==XR_XIR_STRING) { if (!xr_xir_string_view(&value,&content,&size)) status=XR_XIR_BAD_STRUCTURE; }
        else if (value.type==XR_XIR_UNIT) { content="()";size=2; }
        else if (!xr_xir_scalar_text(&value,scalar,&content,&size)) { content="<owned value>";size=13; }
        if (status==XR_XIR_OK) xdap_json_text(text,content,size);xr_xir_value_drop(&value);
        if (status!=XR_XIR_OK) return status;
    }
    xdap_json_text(text,"\n",1);return text->status;
}
XR_FUNC XrXirStatus xdap_predicates_probe(XdapAdapter *s,bool *visible) {
    if (!s || !s->snapshot || !visible) return XR_XIR_BAD_STRUCTURE;
    XirVmDebugStopCause cause={0};XrXirValueStatus observed=xir_vm_debug_snapshot_cause(s->snapshot,&cause);
    if (observed!=XR_XIR_VALUE_OK) return observed==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    bool show=cause.unfiltered;uint32_t total=s->function_breakpoint_count+s->source_breakpoint_count;
    if (total<32 && (cause.groups>>total)) return XR_XIR_BAD_STRUCTURE;
    XdapJsonWriter text=xdap_json_writer(s->resources,s->config.message_limit);XrXirStatus status=XR_XIR_OK;
    const char *error=NULL;
    for (uint32_t group=0;group<total;++group) {
        if (!(cause.groups&(UINT32_C(1)<<group))) continue;
        const XdapBreakpointPredicate *predicate=group<s->function_breakpoint_count ? &s->function_predicates[group] :
            &s->source_breakpoints[group-s->function_breakpoint_count].predicate;
        if (predicate->condition_length) {
            XrXirValue value={0};status=predicate_evaluate(s,predicate->condition,predicate->condition_length,&value);
            if (status==XR_XIR_OUT_OF_MEMORY || status==XR_XIR_BUDGET) { xr_xir_value_drop(&value);break; }
            if (status!=XR_XIR_OK || value.type!=XR_XIR_BOOL) { xr_xir_value_drop(&value);error="Breakpoint condition requires an admitted boolean expression\n";show=true;status=XR_XIR_OK;break; }
            bool matched=value.payload!=0;xr_xir_value_drop(&value);if (!matched) continue;
        }
        if (!predicate->logpoint) { show=true;continue; }
        status=predicate_log(s,predicate,&text);
        if (status==XR_XIR_OUT_OF_MEMORY || status==XR_XIR_BUDGET) break;
        if (status!=XR_XIR_OK) { error="Logpoint expression could not be evaluated\n";show=true;status=XR_XIR_OK;break; }
    }
    if (status==XR_XIR_OK && (text.length || error)) {
        XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
        XDAP_TEXT(&body,"{\"category\":");xdap_json_quote(&body,error ? "stderr" : "console",error ? 6 : 7);XDAP_TEXT(&body,",\"output\":");
        if (error) xdap_json_quote(&body,error,strlen(error));else xdap_json_quote(&body,text.bytes,text.length);
        XDAP_TEXT(&body,"}");status=xdap_dispatch_event(s,"output",&body);xdap_json_writer_free(&body);
    }
    xdap_json_writer_free(&text);
    if (status==XR_XIR_OK) *visible=show;return status;
}
