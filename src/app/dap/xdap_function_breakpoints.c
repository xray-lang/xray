/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_function_breakpoints.c - Atomic owned function rules and predicates
 */
#include "xdap_dispatch_internal.h"
#include "xdap_hit.h"
#include <string.h>

static XrXirStatus function_name_copy(XdapAdapter *s,const char *source,char **output) {
    size_t length=strlen(source);char *name=NULL;
    if (xr_compile_resources_work(s->resources,length+1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    XrCompileResourceStatus allocated=xr_compile_resources_alloc(s->resources,length+1,(void **)&name);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    memcpy(name,source,length+1);*output=name;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_dispatch_function_breakpoints(XdapAdapter *s,const XdapRequest *r) {
    if (!s->launch || s->running || s->terminal) return xdap_dispatch_response(s,r,false,NULL,"No configurable program");
    uint32_t list=0;XrXirStatus status=xdap_dispatch_argument(r,"breakpoints",&list);
    if (status!=XR_XIR_OK) return status;
    if (!list || r->document.nodes[list].kind!=XDAP_JSON_ARRAY || r->document.nodes[list].count>32)
        return xdap_dispatch_response(s,r,false,NULL,"Invalid function breakpoint list");
    char *names[32]={0};XdapBreakpointPredicate predicates[32]={{0}};
    XrXirVmDebugRule rules[32]={{0}},saved[32]={{0}};uint32_t locations=0,count=0,number=0;
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit),response={0};const char *error=NULL;
    XDAP_TEXT(&body,"{\"breakpoints\":[");
    for (uint32_t item=r->document.nodes[list].first;item;item=r->document.nodes[item].next,++number) {
        uint32_t name=0,condition=0,hits=0;
        status=xdap_json_member(&r->document,item,"name",&name);
        if (status==XR_XIR_OK) status=xdap_json_member(&r->document,item,"condition",&condition);
        if (status==XR_XIR_OK) status=xdap_json_member(&r->document,item,"hitCondition",&hits);
        if (status!=XR_XIR_OK || !name || r->document.nodes[name].kind!=XDAP_JSON_STRING ||
            (condition && r->document.nodes[condition].kind!=XDAP_JSON_STRING) ||
            (hits && r->document.nodes[hits].kind!=XDAP_JSON_STRING)) { error="Invalid function breakpoint";goto done; }
        XrXirVmDebugRule rule={0};
        if (hits) status=xdap_hit_parse(s->resources,r->document.nodes[hits].text,r->document.nodes[hits].length,&rule);
        if (status!=XR_XIR_OK) { if (status==XR_XIR_BAD_STRUCTURE) error="Invalid hit count predicate";goto done; }
        uint32_t matches=0;rule.group=count+1;
        XdapFunctionSelection selection={r->document.nodes[name].text,r->document.nodes[name].length,rule};
        status=xdap_launch_function_rules(s->launch,&selection,rules+locations,32-locations,&matches);
        if (status!=XR_XIR_OK && status!=XR_XIR_UNRESOLVED) goto done;
        if (status==XR_XIR_UNRESOLVED) matches=0;
        if (matches) {
            status=xdap_predicate_prepare(s,r,condition,0,&predicates[count]);
            if (status!=XR_XIR_OK) { if (status==XR_XIR_BAD_TYPE) error="Invalid function breakpoint expression";goto done; }
            const char *canonical=s->launch->functions[rules[locations].location.function].source_name;
            status=function_name_copy(s,canonical,&names[count]);if (status!=XR_XIR_OK) goto done;
            saved[count++]=rules[locations];locations+=matches;
        }
        if (number) XDAP_TEXT(&body,",");
        if (matches) XDAP_TEXT(&body,"{\"verified\":true}");
        else XDAP_TEXT(&body,"{\"verified\":false,\"message\":\"Unknown or ambiguous function breakpoint\"}");
        status=body.status;if (status!=XR_XIR_OK) goto done;
    }
    XDAP_TEXT(&body,"]}");status=body.status;
    XdapBreakpointSet combined={names,saved,count,s->source_breakpoints,s->source_breakpoint_count};
    if (status==XR_XIR_OK) status=xdap_breakpoints_collect(s->launch,&combined,rules,&locations);
    if (status==XR_XIR_OK) status=xdap_dispatch_prepare_response(s,r,true,&body,NULL,&response);
    if (status==XR_XIR_OK) {
        XrXirValueStatus admitted=xdap_current_rules_replace(s->launch->control,rules,locations);
        if (admitted==XR_XIR_VALUE_UNSUPPORTED) {
            status=xdap_dispatch_response(s,r,false,NULL,"Native instruction breakpoints are unsupported");
            goto done;
        }
        if (admitted!=XR_XIR_VALUE_OK) status=XR_XIR_BUDGET;
    }
    if (status==XR_XIR_OK) {
        for (uint32_t i=0;i<s->function_breakpoint_count;++i) {
            xr_compile_resources_free(s->function_breakpoints[i]);xdap_predicate_free(&s->function_predicates[i]);
        }
        memset(s->function_breakpoints,0,sizeof(s->function_breakpoints));memset(s->function_rules,0,sizeof(s->function_rules));
        memset(s->function_predicates,0,sizeof(s->function_predicates));
        memcpy(s->function_breakpoints,names,(size_t)count*sizeof(*names));
        memcpy(s->function_rules,saved,(size_t)count*sizeof(*saved));
        memcpy(s->function_predicates,predicates,(size_t)count*sizeof(*predicates));s->function_breakpoint_count=count;
        memset(names,0,sizeof(names));memset(predicates,0,sizeof(predicates));status=xdap_dispatch_send_prepared(s,&response);
    }
done:
    for (uint32_t i=0;i<32;++i) { xr_compile_resources_free(names[i]);xdap_predicate_free(&predicates[i]); }
    xdap_json_writer_free(&response);xdap_json_writer_free(&body);
    return error ? xdap_dispatch_response(s,r,false,NULL,error) : status;
}
