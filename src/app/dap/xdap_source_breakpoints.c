/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_source_breakpoints.c - Source and function rules share one VM admission
 */
#include "xdap_dispatch_internal.h"
#include "xdap_hit.h"
#include <limits.h>
#include <string.h>

static unsigned char source_path_byte(unsigned char byte) {
    if (byte=='\\') byte='/';
#if XR_OS_WINDOWS
    if (byte>='A' && byte<='Z') byte=(unsigned char)(byte+'a'-'A');
#endif
    return byte;
}
static XrXirStatus source_module(const XdapLaunch *launch,const char *text,size_t length,bool identity,uint32_t *output) {
    uint32_t found=UINT32_MAX;
    for (uint32_t m=0;m<launch->module_count;++m) {
        const XdapCodeModule *module=&launch->modules[m];
        const char *candidate=identity ? module->identity : module->path;
        size_t bytes=identity ? module->identity_length : module->path_length;
        if (!candidate || bytes!=length) continue;
        bool same=true;
        for (size_t i=0;i<length;++i) {
            if (identity ? candidate[i]!=text[i] : source_path_byte((unsigned char)candidate[i])!=source_path_byte((unsigned char)text[i])) {
                same=false;break;
            }
        }
        if (same) { if (found!=UINT32_MAX) return XR_XIR_UNRESOLVED;found=m; }
    }
    if (found==UINT32_MAX) return XR_XIR_UNRESOLVED;
    *output=found;return XR_XIR_OK;
}
typedef struct SourceSelection { uint32_t module,line,column;XrXirVmDebugRule rule; } SourceSelection;
static bool source_point_after(const XrXirSourceDebugPoint *point,const SourceSelection *selection) {
    return (uint32_t)point->range.line>selection->line ||
        ((uint32_t)point->range.line==selection->line && (uint32_t)point->range.column>=selection->column);
}
static XrXirStatus source_rules(const XdapLaunch *launch,const SourceSelection *selection,
    XrXirVmDebugRule *output,uint32_t capacity,uint32_t *count,XrXirSourceDebugPoint *resolved) {
    XrXirSourceDebugPoint best={0};XrXirVmDebugRule fresh[32]={{0}};uint32_t matches=0;
    if (capacity>32 || !count || !resolved || (capacity && !output)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<launch->function_count;++f) {
        const XdapCodeFunction *function=&launch->functions[f];
        if (function->module!=selection->module) continue;
        for (uint32_t i=0;i<function->instructions;++i) {
            XrXirSourceDebugPoint point={0};XrXirStatus status=xdap_launch_source_point(launch,f,i,&point);
            if (status==XR_XIR_UNRESOLVED) continue;
            if (status!=XR_XIR_OK) return status;
            if (!(point.flags & XR_XIR_SOURCE_DEBUG_ENTRY) || !source_point_after(&point,selection)) continue;
            if (!best.statement || point.range.line<best.range.line ||
                (point.range.line==best.range.line && point.range.column<best.range.column)) best=point;
        }
    }
    if (best.statement) for (uint32_t f=0;f<launch->function_count;++f) {
        const XdapCodeFunction *function=&launch->functions[f];uint32_t at=UINT32_MAX;
        if (function->module!=selection->module) continue;
        for (uint32_t i=0;i<function->instructions;++i) {
            XrXirSourceDebugPoint point={0};XrXirStatus status=xdap_launch_source_point(launch,f,i,&point);
            if (status==XR_XIR_UNRESOLVED) continue;
            if (status!=XR_XIR_OK) return status;
            if (!(point.flags & XR_XIR_SOURCE_DEBUG_ENTRY) || point.statement!=best.statement ||
                point.range.line!=best.range.line || point.range.column!=best.range.column) continue;
            at=i;break;
        }
        if (at==UINT32_MAX) continue;
        if (matches==capacity) return XR_XIR_BUDGET;
        fresh[matches]=selection->rule;fresh[matches++].location=(XrXirVmDebugLocation){f,at};
    }
    if (matches) { memcpy(output,fresh,(size_t)matches*sizeof(*output));*resolved=best; }
    *count=matches;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xdap_breakpoints_collect(const XdapLaunch *launch,const XdapBreakpointSet *set,
    XrXirVmDebugRule *output,uint32_t *count) {
    if (!launch || !set || !output || !count || set->function_count>32 || set->source_count>32-set->function_count ||
        (set->function_count && (!set->functions || !set->function_rules)) || (set->source_count && !set->sources))
        return XR_XIR_BAD_STRUCTURE;
    XrXirVmDebugRule fresh[32]={{0}};uint32_t used=0;
    for (uint32_t i=0;i<set->function_count;++i) {
        XdapFunctionSelection selection={set->functions[i],strlen(set->functions[i]),set->function_rules[i]};
        selection.rule.group=i+1;uint32_t found=0;
        XrXirStatus status=xdap_launch_function_rules(launch,&selection,fresh+used,32-used,&found);
        if (status!=XR_XIR_OK) return status;
        used+=found;
    }
    for (uint32_t i=0;i<set->source_count;++i) {
        const XdapSourceBreakpoint *entry=&set->sources[i];uint32_t module=0,found=0;
        XrXirStatus status=source_module(launch,entry->identity,entry->identity_length,true,&module);
        if (status==XR_XIR_UNRESOLVED) continue;
        if (status!=XR_XIR_OK) return status;
        SourceSelection selection={module,entry->line,entry->column,entry->rule};
        selection.rule.group=set->function_count+i+1;XrXirSourceDebugPoint point={0};
        status=source_rules(launch,&selection,fresh+used,32-used,&found,&point);
        if (status!=XR_XIR_OK) return status;
        used+=found;
    }
    if (used) memcpy(output,fresh,(size_t)used*sizeof(*output));*count=used;return XR_XIR_OK;
}
static XrXirStatus source_coordinate(const XdapRequest *request,uint32_t node,uint64_t fallback,uint32_t *output) {
    uint64_t value=fallback;
    XrXirStatus status=node ? xdap_json_u64(&request->document,node,&value) : XR_XIR_OK;
    if (status!=XR_XIR_OK) return status;
    if (!value || value>INT_MAX) return XR_XIR_BAD_STRUCTURE;
    *output=(uint32_t)value;return XR_XIR_OK;
}
static XrXirStatus source_identity_copy(XdapAdapter *s,uint32_t module,char **output) {
    const XdapCodeModule *source=&s->launch->modules[module];char *identity=NULL;
    XrCompileResourceStatus allocated=xr_compile_resources_alloc(s->resources,source->identity_length+1,(void **)&identity);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        allocated==XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        allocated==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_OVERFLOW :
        allocated==XR_COMPILE_RESOURCE_CANCELLED ? XR_XIR_CANCELLED :
        allocated==XR_COMPILE_RESOURCE_DEADLINE ? XR_XIR_DEADLINE :
        allocated==XR_COMPILE_RESOURCE_CLOCK_FAILURE ? XR_XIR_CLOCK_FAILURE : XR_XIR_BAD_STRUCTURE;
    memcpy(identity,source->identity,source->identity_length+1);*output=identity;return XR_XIR_OK;
}
static XrXirStatus source_request_point(XdapAdapter *s,const XdapRequest *request,uint32_t item,uint32_t module,
    XdapSourceBreakpoint *output,XrXirSourceDebugPoint *resolved) {
    uint32_t line=0,column=0,hits=0,condition=0,log=0;
    XrXirStatus status=xdap_json_member(&request->document,item,"line",&line);
    if (status==XR_XIR_OK) status=xdap_json_member(&request->document,item,"column",&column);
    if (status==XR_XIR_OK) status=xdap_json_member(&request->document,item,"hitCondition",&hits);
    if (status==XR_XIR_OK) status=xdap_json_member(&request->document,item,"condition",&condition);
    if (status==XR_XIR_OK) status=xdap_json_member(&request->document,item,"logMessage",&log);
    uint32_t row=0,col=1;
    if (status==XR_XIR_OK) status=source_coordinate(request,line,0,&row);
    if (status==XR_XIR_OK) status=source_coordinate(request,column,1,&col);
    if (status!=XR_XIR_OK) return status;
    uint32_t predicates[3]={hits,condition,log};
    for (uint32_t i=0;i<3;++i) if (predicates[i] && request->document.nodes[predicates[i]].kind!=XDAP_JSON_STRING)
        return XR_XIR_BAD_STRUCTURE;
    SourceSelection selection={module,row,col,{0}};
    if (hits) status=xdap_hit_parse(s->resources,request->document.nodes[hits].text,request->document.nodes[hits].length,&selection.rule);
    if (status!=XR_XIR_OK) return status;
    if (module==UINT32_MAX) return XR_XIR_UNRESOLVED;
    XrXirVmDebugRule rules[32]={{0}};uint32_t matches=0;
    status=source_rules(s->launch,&selection,rules,32,&matches,resolved);
    if (status!=XR_XIR_OK || !matches) return status==XR_XIR_OK ? XR_XIR_UNRESOLVED : status;
    XdapBreakpointPredicate predicate={0};status=xdap_predicate_prepare(s,request,condition,log,&predicate);
    if (status!=XR_XIR_OK) return status;
    const XdapCodeModule *source=&s->launch->modules[module];char *identity=NULL;
    status=source_identity_copy(s,module,&identity);
    if (status!=XR_XIR_OK) { xdap_predicate_free(&predicate);return status; }
    *output=(XdapSourceBreakpoint){identity,source->identity_length,row,col,selection.rule,predicate};return XR_XIR_OK;
}
static void source_breakpoint_free(XdapSourceBreakpoint *entry) {
    xr_compile_resources_free(entry->identity);xdap_predicate_free(&entry->predicate);*entry=(XdapSourceBreakpoint){0};
}
static bool source_same_module(const XdapSourceBreakpoint *entry,const XdapCodeModule *module) {
    return module && entry->identity_length==module->identity_length &&
        !memcmp(entry->identity,module->identity,module->identity_length);
}
XR_FUNC XrXirStatus xdap_dispatch_source_breakpoints(XdapAdapter *s,const XdapRequest *request) {
    if (!s->launch || s->running) return xdap_dispatch_response(s,request,false,NULL,"A stopped or unstarted launch is required");
    uint32_t source=0,path=0,list=0,modified=0;
    XrXirStatus status=xdap_dispatch_argument(request,"source",&source);
    if (status==XR_XIR_OK) status=xdap_json_member(&request->document,source,"path",&path);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(request,"breakpoints",&list);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(request,"sourceModified",&modified);
    if (status!=XR_XIR_OK || !path || request->document.nodes[path].kind!=XDAP_JSON_STRING || !list ||
        request->document.nodes[list].kind!=XDAP_JSON_ARRAY || request->document.nodes[list].count>32)
        return xdap_dispatch_response(s,request,false,NULL,"Invalid source breakpoint request");
    if (modified) {
        if (request->document.nodes[modified].kind!=XDAP_JSON_BOOL) return XR_XIR_BAD_STRUCTURE;
        if (request->document.nodes[modified].boolean)
            return xdap_dispatch_response(s,request,false,NULL,"Modified source requires a fresh restart");
    }
    uint32_t module=UINT32_MAX;
    status=source_module(s->launch,request->document.nodes[path].text,request->document.nodes[path].length,false,&module);
    if (status!=XR_XIR_OK && status!=XR_XIR_UNRESOLVED) return status;
    const XdapCodeModule *matched=module==UINT32_MAX ? NULL : &s->launch->modules[module];
    XdapSourceBreakpoint fresh[32]={{0}};uint32_t kept=0,used=0;bool replaced[32]={0};
    for (uint32_t i=0;i<s->source_breakpoint_count;++i) {
        replaced[i]=source_same_module(&s->source_breakpoints[i],matched);
        if (!replaced[i]) fresh[kept++]=s->source_breakpoints[i];
    }
    used=kept;
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"breakpoints\":[");
    uint32_t number=0;
    for (uint32_t item=request->document.nodes[list].first;item;item=request->document.nodes[item].next,++number) {
        XdapSourceBreakpoint entry={0};XrXirSourceDebugPoint point={0};
        status=source_request_point(s,request,item,module,&entry,&point);
        if (status!=XR_XIR_OK && status!=XR_XIR_UNRESOLVED) {
            if (status==XR_XIR_BAD_TYPE) status=xdap_dispatch_response(s,request,false,NULL,"Invalid source breakpoint expression or log template");
            goto done;
        }
        if (number) XDAP_TEXT(&body,",");
        if (status==XR_XIR_OK) {
            if (used==32-s->function_breakpoint_count) { source_breakpoint_free(&entry);status=XR_XIR_BUDGET;goto done; }
            fresh[used++]=entry;XDAP_TEXT(&body,"{\"verified\":true,\"line\":");xdap_json_unsigned(&body,(uint32_t)point.range.line);
            XDAP_TEXT(&body,",\"column\":");xdap_json_unsigned(&body,(uint32_t)point.range.column);XDAP_TEXT(&body,"}");
        } else XDAP_TEXT(&body,"{\"verified\":false,\"message\":\"No admitted executable source site or predicate\"}");
    }
    XDAP_TEXT(&body,"]}");status=body.status;
    if (status==XR_XIR_OK && !matched) { status=xdap_dispatch_response(s,request,true,&body,NULL);goto done; }
    XrXirVmDebugRule rules[32]={{0}};uint32_t count=0;
    XdapBreakpointSet set={s->function_breakpoints,s->function_rules,s->function_breakpoint_count,fresh,used};
    if (status==XR_XIR_OK) status=xdap_breakpoints_collect(s->launch,&set,rules,&count);
    XdapJsonWriter response={0};
    if (status==XR_XIR_OK) status=xdap_dispatch_prepare_response(s,request,true,&body,NULL,&response);
    if (status==XR_XIR_OK) {
        XrXirValueStatus admitted=xdap_current_rules_replace(s->launch->control,rules,count);
        if (admitted==XR_XIR_VALUE_UNSUPPORTED) {
            status=xdap_dispatch_response(s,request,false,NULL,"Native instruction breakpoints are unsupported");
            xdap_json_writer_free(&response);goto done;
        }
        if (admitted!=XR_XIR_VALUE_OK) status=admitted==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
            admitted==XR_XIR_VALUE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
            admitted==XR_XIR_VALUE_OVERFLOW ? XR_XIR_OVERFLOW : XR_XIR_BUDGET;
    }
    if (status==XR_XIR_OK) {
        for (uint32_t i=0;i<s->source_breakpoint_count;++i)
            if (replaced[i]) source_breakpoint_free(&s->source_breakpoints[i]);
        memcpy(s->source_breakpoints,fresh,(size_t)used*sizeof(*fresh));s->source_breakpoint_count=used;kept=used;
        status=xdap_dispatch_send_prepared(s,&response);
    }
    xdap_json_writer_free(&response);
done:
    for (uint32_t i=kept;i<used;++i) source_breakpoint_free(&fresh[i]);
    xdap_json_writer_free(&body);return status;
}
