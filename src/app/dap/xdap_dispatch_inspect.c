/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch_inspect.c - DAP observations through owned stop snapshots
 */
#include "xdap_dispatch_internal.h"
#include "xdap_hit.h"
#include <stdio.h>
#include <string.h>
static XrXirStatus inspect_finish(XdapAdapter *s,const XdapRequest *r,XdapJsonWriter *body) {
    XrXirStatus status=xdap_dispatch_response(s,r,true,body,NULL);xdap_json_writer_free(body);return status;
}
static uint32_t inspect_frame(const XdapAdapter *s,uint64_t handle,bool scope) {
    for (uint32_t i=0;i<s->depth;++i)
        if ((scope ? s->frames[i].scope : s->frames[i].handle)==handle) return i;
    return UINT32_MAX;
}
static bool inspect_position(XdapAdapter *s,XdapJsonWriter *w,uint32_t function,uint32_t instruction) {
    if (!s->launch || function>=s->launch->function_count || instruction>=s->launch->functions[function].instructions) {
        w->status=XR_XIR_BAD_STRUCTURE;return false;
    }
    char position[32];uint64_t address=(uint64_t)s->launch->functions[function].first+instruction;
    if (xr_compile_resources_work(w->resources,sizeof(position))!=XR_COMPILE_RESOURCE_OK) { w->status=XR_XIR_BUDGET;return false; }
    int count=snprintf(position,sizeof(position),"%llu",(unsigned long long)address);
    return count>0 && (size_t)count<sizeof(position) && xdap_json_quote(w,position,(size_t)count);
}
static bool inspect_source(XdapAdapter *s,XdapJsonWriter *body,uint32_t function,bool location) {
    if (!s->launch || function>=s->launch->function_count) { body->status=XR_XIR_BAD_STRUCTURE;return false; }
    uint32_t index=s->launch->functions[function].module;
    if (index>=s->launch->module_count) { body->status=XR_XIR_BAD_STRUCTURE;return false; }
    const XdapCodeModule *module=&s->launch->modules[index];
    if (!module->path || !module->path_length) return true;
    if (module->name_offset>module->path_length) { body->status=XR_XIR_BAD_STRUCTURE;return false; }
    if (location) XDAP_TEXT(body,",\"location\":{\"name\":");else XDAP_TEXT(body,",\"source\":{\"name\":");
    xdap_json_quote(body,module->path+module->name_offset,module->path_length-module->name_offset);
    XDAP_TEXT(body,",\"path\":");xdap_json_quote(body,module->path,module->path_length);XDAP_TEXT(body,"}");
    return body->status==XR_XIR_OK;
}
static bool inspect_source_position(XdapAdapter *s,XdapJsonWriter *body,uint32_t function,uint32_t instruction,bool required) {
    XrXirSourceDebugPoint point={0};
    XrXirStatus status=instruction==UINT32_MAX ? XR_XIR_UNRESOLVED :
        xdap_launch_source_point(s->launch,function,instruction,&point);
    if (status!=XR_XIR_OK && status!=XR_XIR_UNRESOLVED) { body->status=status;return false; }
    if (status==XR_XIR_OK || required) {
        XDAP_TEXT(body,",\"line\":");xdap_json_unsigned(body,(uint32_t)point.range.line);
        XDAP_TEXT(body,",\"column\":");xdap_json_unsigned(body,(uint32_t)point.range.column);
    }
    return body->status==XR_XIR_OK;
}
static XrXirStatus inspect_threads(XdapAdapter *s,const XdapRequest *r) {
    uint64_t ids[65]={0};uint32_t count=0;
    if (s->launch && s->configured && !s->terminal) {
        XrXirValueStatus status=xdap_current_threads(s->launch->control,ids,65,&count);
        if (status==XR_XIR_VALUE_UNSUPPORTED)
            return xdap_dispatch_response(s,r,false,NULL,"Complete thread observation requires admitted VM ingress");
        if (status!=XR_XIR_VALUE_OK) return status==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    }
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    XDAP_TEXT(&body,"{\"threads\":[");
    for (uint32_t i=0;i<count;++i) {
        if (i) XDAP_TEXT(&body,",");XDAP_TEXT(&body,"{\"id\":");xdap_json_unsigned(&body,ids[i]);XDAP_TEXT(&body,",\"name\":");
        char label[64];int length=snprintf(label,sizeof(label),"%s %llu",ids[i]==1 ? "Root" : "Activation",(unsigned long long)ids[i]);
        xdap_json_quote(&body,label,(size_t)length);XDAP_TEXT(&body,"}");
    }
    XDAP_TEXT(&body,"]}");
    return inspect_finish(s,r,&body);
}
static XrXirStatus inspect_stack(XdapAdapter *s,const XdapRequest *r) {
    uint64_t thread=0,start=0,levels=0;XrXirStatus status=xdap_dispatch_number(r,"threadId",0,&thread);
    if (status==XR_XIR_OK) status=xdap_dispatch_number(r,"startFrame",0,&start);
    if (status==XR_XIR_OK) status=xdap_dispatch_number(r,"levels",0,&levels);
    if (status!=XR_XIR_OK) return status;
    if (!thread || !s->snapshot) return xdap_dispatch_response(s,r,false,NULL,"notStopped");
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"stackFrames\":[");
    uint32_t shown=0,total=0;
    for (uint32_t i=0;i<s->depth;++i) {
        XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
        XrXirValueStatus observed=xr_xir_vm_debug_snapshot_frame(s->snapshot,i,&info,&name);
        if (observed!=XR_XIR_VALUE_OK) { body.status=observed==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;break; }
        if (info.thread!=thread) { xr_xir_value_drop(&name);continue; }
        if (total++<start || (levels && shown>=levels)) { xr_xir_value_drop(&name);continue; }
        const char *text=NULL;size_t length=0;
        if (!xr_xir_string_view(&name,&text,&length)) body.status=XR_XIR_BAD_STRUCTURE;
        if (shown++) XDAP_TEXT(&body,",");XDAP_TEXT(&body,"{\"id\":");xdap_json_unsigned(&body,s->frames[i].handle);
        XDAP_TEXT(&body,",\"name\":");xdap_json_quote(&body,text,length);
        if (!inspect_source_position(s,&body,info.function,
            info.code==XR_XIR_VM_DEBUG_VM ? info.instruction : UINT32_MAX,true)) { xr_xir_value_drop(&name);break; }
        if (!inspect_source(s,&body,info.function,false)) { xr_xir_value_drop(&name);break; }
        if (info.code==XR_XIR_VM_DEBUG_VM && info.instruction!=UINT32_MAX) {
            XDAP_TEXT(&body,",\"instructionPointerReference\":");inspect_position(s,&body,info.function,info.instruction);
        } else XDAP_TEXT(&body,",\"presentationHint\":\"subtle\"");
        XDAP_TEXT(&body,"}");xr_xir_value_drop(&name);
    }
    if (!total && body.status==XR_XIR_OK) { xdap_json_writer_free(&body);return xdap_dispatch_response(s,r,false,NULL,"Unknown stopped thread"); }
    XDAP_TEXT(&body,"],\"totalFrames\":");xdap_json_unsigned(&body,total);XDAP_TEXT(&body,"}");
    return inspect_finish(s,r,&body);
}
static XrXirStatus inspect_scopes(XdapAdapter *s,const XdapRequest *r) {
    uint64_t handle=0;XrXirStatus status=xdap_dispatch_number(r,"frameId",0,&handle);if (status!=XR_XIR_OK) return status;
    uint32_t frame=inspect_frame(s,handle,false);
    if (!s->snapshot || frame==UINT32_MAX) return xdap_dispatch_response(s,r,false,NULL,"Invalid or retired frame");
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
    if (xr_xir_vm_debug_snapshot_frame(s->snapshot,frame,&info,&name)!=XR_XIR_VALUE_OK) return XR_XIR_BUDGET;
    xr_xir_value_drop(&name);
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    if (!info.value_count) { XDAP_TEXT(&body,"{\"scopes\":[]}");return inspect_finish(s,r,&body); }
    XDAP_TEXT(&body,"{\"scopes\":[{\"name\":\"VM values\",\"presentationHint\":\"registers\",\"variablesReference\":");
    xdap_json_unsigned(&body,s->frames[frame].scope);XDAP_TEXT(&body,",\"expensive\":false}]}");
    return inspect_finish(s,r,&body);
}
static bool inspect_value(XdapJsonWriter *body,const XrXirValue *value) {
    if (value->type==XR_XIR_UNIT) return xdap_json_quote(body,"()",2);
    if (value->type==XR_XIR_STRING) {
        const char *bytes=NULL;size_t length=0;
        return xr_xir_string_view(value,&bytes,&length) && xdap_json_quote(body,bytes,length);
    }
    char buffer[XR_XIR_SCALAR_TEXT_BYTES];const char *bytes=NULL;size_t length=0;
    if (xr_xir_scalar_text(value,buffer,&bytes,&length)) return xdap_json_quote(body,bytes,length);
    /* An owned handle is observed as opaque until its typed child projection is admitted. */
    return xdap_json_quote(body,"<owned value>",13);
}
static XrXirStatus inspect_reference(XdapAdapter *s,const XrXirValue *value,uint32_t frame,uint32_t *handle,XdapValueShape *shape) {
    XdapValueView view={value,s->config.observer_metadata,s->snapshot,frame};XrXirValueStatus status=xdap_value_shape(&view,shape);
    if (status!=XR_XIR_VALUE_OK) return status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    *handle=0;if (!shape->children) return XR_XIR_OK;
    for (uint32_t i=0;i<s->value_count;++i) {
        const XdapValueRef *ref=&s->values[i];
        if (ref->frame==frame && ref->value.type==value->type && ref->value.payload==value->payload) { *handle=ref->handle;return XR_XIR_OK; }
    }
    if (s->value_count==128 || s->next_handle==INT32_MAX) return XR_XIR_BUDGET;
    XdapValueRef *ref=&s->values[s->value_count];status=xr_xir_value_copy(value,&ref->value);
    if (status!=XR_XIR_VALUE_OK) return status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    ref->handle=s->next_handle++;ref->frame=frame;ref->shape=*shape;++s->value_count;*handle=ref->handle;return XR_XIR_OK;
}
static bool inspect_variable(XdapAdapter *s,XdapJsonWriter *body,const char *name,size_t length,const XrXirValue *value,uint32_t frame) {
    uint32_t handle=0;XdapValueShape shape={0};XrXirStatus status=inspect_reference(s,value,frame,&handle,&shape);
    if (status!=XR_XIR_OK) { body->status=status;return false; }
    XDAP_TEXT(body,"{\"name\":");xdap_json_quote(body,name,length);XDAP_TEXT(body,",\"value\":");
    if (!inspect_value(body,value) && body->status==XR_XIR_OK) body->status=XR_XIR_BAD_STRUCTURE;
    if (s->variable_types) {
        XdapValueView view={value,s->config.observer_metadata,s->snapshot,frame};XdapTypeText type={0};
        XrXirValueStatus formatted=xdap_value_type_text(&view,&type);
        if (formatted!=XR_XIR_VALUE_OK) { body->status=formatted==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;return false; }
        XDAP_TEXT(body,",\"type\":");xdap_json_quote(body,type.bytes,type.length);
    }
    XDAP_TEXT(body,",\"variablesReference\":");xdap_json_unsigned(body,handle);
    if (shape.children) { XDAP_TEXT(body,",");
        const char *key=shape.indexed ? "indexedVariables" : "namedVariables";
        xdap_json_quote(body,key,strlen(key));XDAP_TEXT(body,":");xdap_json_unsigned(body,shape.children); }
    XDAP_TEXT(body,"}");return body->status==XR_XIR_OK;
}
static void inspect_rollback_values(XdapAdapter *s,uint32_t before) {
    while (s->value_count>before) {
        XdapValueRef *ref=&s->values[--s->value_count];xr_xir_value_drop(&ref->value);*ref=(XdapValueRef){0};
    }
}
static XrXirStatus inspect_children(XdapAdapter *s,const XdapRequest *r,const XdapValueRef *ref,uint64_t start,uint64_t count) {
    uint32_t before=s->value_count;XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    XDAP_TEXT(&body,"{\"variables\":[");uint32_t shown=0;
    uint64_t available=start<ref->shape.children ? ref->shape.children-start : 0;
    if (count && available>count) available=count;
    if (available>256) { xdap_json_writer_free(&body);return xdap_dispatch_response(s,r,false,NULL,"Request a bounded child page"); }
    for (uint64_t i=0;i<available;++i) {
        XrXirValue value={0};const char *name=NULL;size_t length=0;
        XdapValueView view={&ref->value,s->config.observer_metadata,s->snapshot,ref->frame};
        XrXirValueStatus status=xdap_value_child(&view,start+i,&value,&name,&length);
        if (status!=XR_XIR_VALUE_OK) {
            if (status==XR_XIR_VALUE_BAD_ARGUMENT || status==XR_XIR_VALUE_UNSUPPORTED || status==XR_XIR_VALUE_BOUNDS) {
                xdap_json_writer_free(&body);inspect_rollback_values(s,before);
                return xdap_dispatch_response(s,r,false,NULL,"Typed child observation is unavailable");
            }
            body.status=status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;break;
        }
        char index[32];if (!name) { int bytes=snprintf(index,sizeof(index),"[%llu]",(unsigned long long)(start+i));name=index;length=(size_t)bytes; }
        if (shown++) XDAP_TEXT(&body,",");bool emitted=inspect_variable(s,&body,name,length,&value,ref->frame);xr_xir_value_drop(&value);
        if (!emitted) break;
    }
    XDAP_TEXT(&body,"]}");XrXirStatus status=inspect_finish(s,r,&body);
    if (status!=XR_XIR_OK) inspect_rollback_values(s,before);return status;
}
static XrXirStatus inspect_variables(XdapAdapter *s,const XdapRequest *r) {
    uint64_t handle=0,start=0,count=0;XrXirStatus status=xdap_dispatch_number(r,"variablesReference",0,&handle);
    if (status==XR_XIR_OK && s->variable_paging) status=xdap_dispatch_number(r,"start",0,&start);
    if (status==XR_XIR_OK && s->variable_paging) status=xdap_dispatch_number(r,"count",0,&count);
    if (status!=XR_XIR_OK) return status;
    uint32_t filter=0;bool indexed=false,named=false;status=xdap_dispatch_argument(r,"filter",&filter);
    if (status==XR_XIR_OK && filter) status=xdap_json_equal(&r->document,filter,"indexed",&indexed);
    if (status==XR_XIR_OK && filter && !indexed) status=xdap_json_equal(&r->document,filter,"named",&named);
    if (status!=XR_XIR_OK) return status;
    if (filter && !indexed && !named) return xdap_dispatch_response(s,r,false,NULL,"Invalid child filter");
    for (uint32_t i=0;i<s->value_count;++i) {
        if (s->values[i].handle!=handle || !s->snapshot) continue;
        if ((named && s->values[i].shape.indexed) || (indexed && !s->values[i].shape.indexed))
            return inspect_children(s,r,&s->values[i],s->values[i].shape.children,0);
        return inspect_children(s,r,&s->values[i],start,count);
    }
    uint32_t frame=inspect_frame(s,handle,true);
    if (!s->snapshot || frame==UINT32_MAX) return xdap_dispatch_response(s,r,false,NULL,"Invalid or retired variable reference");
    if (indexed) {
        XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"variables\":[]}");
        return inspect_finish(s,r,&body);
    }
    XrXirVmDebugFrameInfo info={0};XrXirValue function_name={0};
    if (xr_xir_vm_debug_snapshot_frame(s->snapshot,frame,&info,&function_name)!=XR_XIR_VALUE_OK) return XR_XIR_BUDGET;
    xr_xir_value_drop(&function_name);
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"variables\":[");
    uint32_t ordinal=0,shown=0,before=s->value_count;
    for (uint32_t value_id=0;value_id<info.value_count;++value_id) {
        XrXirValue value={0};XrXirValueStatus copied=xr_xir_vm_debug_snapshot_value(s->snapshot,frame,value_id,&value);
        if (copied==XR_XIR_VALUE_BAD_ARGUMENT) continue;
        if (copied!=XR_XIR_VALUE_OK) { body.status=copied==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;break; }
        if (ordinal++<start || (count && shown>=count)) { xr_xir_value_drop(&value);continue; }
        XrXirValue cell={0};copied=xr_xir_vm_debug_snapshot_cell_value(s->snapshot,frame,value_id,&cell);
        if (copied==XR_XIR_VALUE_OK) { xr_xir_value_drop(&value);value=cell; }
        else if (copied!=XR_XIR_VALUE_UNSUPPORTED) { xr_xir_value_drop(&value);body.status=XR_XIR_BUDGET;break; }
        char name[32];int name_length=snprintf(name,sizeof(name),"%%v%u",value_id);
        if (shown++) XDAP_TEXT(&body,",");bool emitted=inspect_variable(s,&body,name,(size_t)name_length,&value,frame);xr_xir_value_drop(&value);
        if (!emitted) break;
    }
    XDAP_TEXT(&body,"]}");status=inspect_finish(s,r,&body);
    if (status!=XR_XIR_OK) inspect_rollback_values(s,before);return status;
}
static XrXirStatus inspect_function_breakpoints(XdapAdapter *s,const XdapRequest *r) {
    if (!s->launch || s->running || s->terminal) return xdap_dispatch_response(s,r,false,NULL,"No configurable program");
    uint32_t list=0;XrXirStatus status=xdap_dispatch_argument(r,"breakpoints",&list);if (status!=XR_XIR_OK) return status;
    if (!list || r->document.nodes[list].kind!=XDAP_JSON_ARRAY || r->document.nodes[list].count>32)
        return xdap_dispatch_response(s,r,false,NULL,"Invalid function breakpoint list");
    XrXirVmDebugRule rules[32]={{0}},saved[32]={{0}};uint32_t location_count=0,logical_count=0;
    bool verified[32]={0};uint32_t item_number=0;
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"breakpoints\":[");
    for (uint32_t item=r->document.nodes[list].first;item;item=r->document.nodes[item].next,++item_number) {
        uint32_t name=0,condition=0,hits=0;
        status=xdap_json_member(&r->document,item,"name",&name);
        if (status==XR_XIR_OK) status=xdap_json_member(&r->document,item,"condition",&condition);
        if (status==XR_XIR_OK) status=xdap_json_member(&r->document,item,"hitCondition",&hits);
        if (status!=XR_XIR_OK || !name || r->document.nodes[name].kind!=XDAP_JSON_STRING) {
            xdap_json_writer_free(&body);return xdap_dispatch_response(s,r,false,NULL,"Invalid function breakpoint"); }
        if ((condition && r->document.nodes[condition].kind!=XDAP_JSON_STRING) ||
            (hits && r->document.nodes[hits].kind!=XDAP_JSON_STRING)) {
            xdap_json_writer_free(&body);return xdap_dispatch_response(s,r,false,NULL,"Invalid function breakpoint predicate");
        }
        XrXirVmDebugRule rule={0};
        if (hits) {
            status=xdap_hit_parse(s->resources,r->document.nodes[hits].text,r->document.nodes[hits].length,&rule);
            if (status!=XR_XIR_OK) { xdap_json_writer_free(&body);
                return status==XR_XIR_BAD_STRUCTURE ? xdap_dispatch_response(s,r,false,NULL,"Invalid hit count predicate") : status; }
        }
        uint32_t matches=0;rule.group=logical_count+1;
        if (!condition || !r->document.nodes[condition].length) {
            XdapFunctionSelection selection={r->document.nodes[name].text,r->document.nodes[name].length,rule};
            status=xdap_launch_function_rules(s->launch,&selection,rules+location_count,32-location_count,&matches);
            if (status!=XR_XIR_OK && status!=XR_XIR_UNRESOLVED) { xdap_json_writer_free(&body);return status; }
            if (status==XR_XIR_UNRESOLVED) matches=0;
        }
        if (matches) {
            verified[item_number]=true;saved[logical_count++]=rules[location_count];location_count+=matches;
        }
        if (item_number) XDAP_TEXT(&body,",");
        if (verified[item_number]) XDAP_TEXT(&body,"{\"verified\":true}");
        else XDAP_TEXT(&body,"{\"verified\":false,\"message\":\"Unknown, ambiguous, or conditional function breakpoint\"}");
    }
    XDAP_TEXT(&body,"]}");
    if (body.status!=XR_XIR_OK) { status=body.status;xdap_json_writer_free(&body);return status; }
    char *names[32]={0};
    for (uint32_t i=0;i<logical_count;++i) {
        const char *source=s->launch->functions[saved[i].location.function].source_name;size_t length=strlen(source);
        XrCompileResourceStatus allocated=xr_compile_resources_alloc(s->resources,length+1,(void **)&names[i]);
        if (allocated!=XR_COMPILE_RESOURCE_OK || xr_compile_resources_work(s->resources,length+1)!=XR_COMPILE_RESOURCE_OK) {
            for (uint32_t j=0;j<=i;++j) xr_compile_resources_free(names[j]);xdap_json_writer_free(&body);
            return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        }
        memcpy(names[i],source,length+1);
    }
    XdapBreakpointSet combined={names,saved,logical_count,s->source_breakpoints,s->source_breakpoint_count};
    status=xdap_breakpoints_collect(s->launch,&combined,rules,&location_count);
    if (status!=XR_XIR_OK) {
        for (uint32_t i=0;i<logical_count;++i) xr_compile_resources_free(names[i]);xdap_json_writer_free(&body);return status;
    }
    XdapJsonWriter response={0};status=xdap_dispatch_prepare_response(s,r,true,&body,NULL,&response);
    xdap_json_writer_free(&body);
    if (status!=XR_XIR_OK) { for (uint32_t i=0;i<logical_count;++i) xr_compile_resources_free(names[i]);return status; }
    XrXirValueStatus installed=xdap_current_rules_replace(s->launch->control,rules,location_count);
    if (installed!=XR_XIR_VALUE_OK) {
        for (uint32_t i=0;i<logical_count;++i) xr_compile_resources_free(names[i]);xdap_json_writer_free(&response);
        return xdap_dispatch_response(s,r,false,NULL,"Breakpoint admission failed");
    }
    for (uint32_t i=0;i<s->function_breakpoint_count;++i) xr_compile_resources_free(s->function_breakpoints[i]);
    memset(s->function_breakpoints,0,sizeof(s->function_breakpoints));
    memset(s->function_rules,0,sizeof(s->function_rules));
    for (uint32_t i=0;i<logical_count;++i) s->function_breakpoints[i]=names[i];
    memcpy(s->function_rules,saved,(size_t)logical_count*sizeof(*saved));
    s->function_breakpoint_count=logical_count;status=xdap_dispatch_send_prepared(s,&response);
    xdap_json_writer_free(&response);return status;
}
/* Instruction addresses are indices in the owned flattened VM display code,
 * not host pointers or byte offsets into a native code lease. */
static XrXirStatus inspect_address(const XdapRequest *r,uint32_t reference,uint32_t *output) {
    const XdapJsonNode *node=&r->document.nodes[reference];
    if (node->kind!=XDAP_JSON_STRING || !node->length || node->length>10) return XR_XIR_BAD_STRUCTURE;
    if (xr_compile_resources_work(r->document.resources,node->length+sizeof(*output))!=XR_COMPILE_RESOURCE_OK)
        return XR_XIR_BUDGET;
    size_t at=0;uint32_t base=10,value=0;
    if (node->length>=2 && node->text[0]=='0' && node->text[1]=='x') { base=16;at=2; }
    if (at==node->length) return XR_XIR_BAD_STRUCTURE;
    for (;at<node->length;++at) {
        unsigned char byte=(unsigned char)node->text[at];uint32_t digit=UINT32_MAX;
        if (byte>='0' && byte<='9') digit=(uint32_t)(byte-'0');
        else if (base==16 && byte>='a' && byte<='f') digit=(uint32_t)(byte-'a')+10;
        else if (base==16 && byte>='A' && byte<='F') digit=(uint32_t)(byte-'A')+10;
        if (digit>=base || value>(UINT32_MAX-digit)/base) return XR_XIR_BAD_STRUCTURE;
        value=value*base+digit;
    }
    *output=value;return XR_XIR_OK;
}
static bool inspect_symbol(XdapJsonWriter *body,const XdapCodeFunction *code) {
    size_t length=0;
    for (;;) {
        if (xr_compile_resources_work(body->resources,1)!=XR_COMPILE_RESOURCE_OK) { body->status=XR_XIR_BUDGET;return false; }
        if (!code->name[length]) break;
        ++length;
    }
    XDAP_TEXT(body,",\"symbol\":");return xdap_json_quote(body,code->name,length);
}
static XrXirStatus inspect_disassemble(XdapAdapter *s,const XdapRequest *r) {
    if (!s->launch) return xdap_dispatch_response(s,r,false,NULL,"No admitted program");
    uint32_t reference=0,offset=0,count_node=0,byte_offset=0,symbols=0;uint64_t count=0;
    XrXirStatus status=xdap_dispatch_argument(r,"memoryReference",&reference);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"instructionCount",&count_node);
    if (status==XR_XIR_OK && count_node) status=xdap_json_u64(&r->document,count_node,&count);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"instructionOffset",&offset);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"offset",&byte_offset);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"resolveSymbols",&symbols);
    if (status!=XR_XIR_OK) return status;
    if (!reference || !count_node || count>256 || (symbols && r->document.nodes[symbols].kind!=XDAP_JSON_BOOL))
        return xdap_dispatch_response(s,r,false,NULL,"Invalid instruction request");
    uint32_t instruction=0;status=inspect_address(r,reference,&instruction);
    if (status==XR_XIR_BUDGET) return status;
    if (status!=XR_XIR_OK || instruction>=s->launch->instruction_count)
        return xdap_dispatch_response(s,r,false,NULL,"Unknown instruction reference");
    int64_t displacement=0,bytes=0;
    if (offset) { status=xdap_json_i64(&r->document,offset,&displacement);if (status!=XR_XIR_OK) return status; }
    if (byte_offset) status=xdap_json_i64(&r->document,byte_offset,&bytes);
    if (status!=XR_XIR_OK) return status;
    if (bytes || displacement<-256 || displacement>256)
        return xdap_dispatch_response(s,r,false,NULL,"IR positions accept bounded instruction offsets");
    bool resolve=symbols && r->document.nodes[symbols].boolean;
    uint32_t function=0,previous_module=UINT32_MAX;
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"instructions\":[");
    for (uint64_t i=0;i<count;++i) {
        if (xr_compile_resources_work(s->resources,384)!=XR_COMPILE_RESOURCE_OK) { body.status=XR_XIR_BUDGET;break; }
        int64_t at=(int64_t)instruction+displacement+(int64_t)i;
        char address[32],description[384];int address_size=snprintf(address,sizeof(address),"%lld",(long long)at);
        bool valid=at>=0 && (uint64_t)at<s->launch->instruction_count;int text_size=0;
        if (valid) {
            while (function<s->launch->function_count) {
                if (xr_compile_resources_work(s->resources,1)!=XR_COMPILE_RESOURCE_OK) { body.status=XR_XIR_BUDGET;break; }
                const XdapCodeFunction *code=&s->launch->functions[function];
                if ((uint64_t)at>=code->first && (uint64_t)at-code->first<code->instructions) break;
                ++function;
            }
            if (body.status!=XR_XIR_OK) break;
            if (function==s->launch->function_count) { body.status=XR_XIR_BAD_STRUCTURE;break; }
            const XrXirInstruction *op=&s->launch->instructions[(uint32_t)at];
            text_size=snprintf(description,sizeof(description),"%s type=%u args=(%u,%u) targets=(%u,%u) immediate=%lld type_arguments=(%u,%u)",
                xr_xir_op_name(op->op),op->type,op->args[0],op->args[1],op->targets[0],op->targets[1],
                (long long)op->immediate,op->type_arguments[0],op->type_arguments[1]);
        } else { memcpy(description,"invalid",8);text_size=7; }
        if (address_size<0 || (size_t)address_size>=sizeof(address) || text_size<0 || (size_t)text_size>=sizeof(description)) {
            body.status=XR_XIR_BAD_STRUCTURE;break;
        }
        if (i) XDAP_TEXT(&body,",");XDAP_TEXT(&body,"{\"address\":");
        xdap_json_quote(&body,address,(size_t)address_size);XDAP_TEXT(&body,",\"instruction\":");
        xdap_json_quote(&body,description,(size_t)text_size);
        if (!valid) XDAP_TEXT(&body,",\"presentationHint\":\"invalid\"");
        else {
            const XdapCodeFunction *code=&s->launch->functions[function];
            if (resolve && !inspect_symbol(&body,code)) break;
            if (previous_module!=code->module) { if (!inspect_source(s,&body,function,true)) break;previous_module=code->module; }
            if (!inspect_source_position(s,&body,function,(uint32_t)at-code->first,false)) break;
        }
        XDAP_TEXT(&body,"}");
    }
    XDAP_TEXT(&body,"]}");return inspect_finish(s,r,&body);
}
XR_FUNC XrXirStatus xdap_dispatch_inspect(XdapAdapter *s,const XdapRequest *r,const char *command) {
    if (!s->initialized) return xdap_dispatch_response(s,r,false,NULL,"Initialize first");
    if (!strcmp(command,"threads")) return inspect_threads(s,r);
    if (!strcmp(command,"stackTrace")) return inspect_stack(s,r);
    if (!strcmp(command,"scopes")) return inspect_scopes(s,r);
    if (!strcmp(command,"variables")) return inspect_variables(s,r);
    if (!strcmp(command,"setFunctionBreakpoints")) return inspect_function_breakpoints(s,r);
    if (!strcmp(command,"setBreakpoints")) return xdap_dispatch_source_breakpoints(s,r);
    if (!strcmp(command,"evaluate") || !strcmp(command,"setVariable"))
        return xdap_dispatch_response(s,r,false,NULL,"Paused lexical expression admission is not yet available");
    if (!strcmp(command,"restart")) return xdap_dispatch_response(s,r,false,NULL,"Restart construction is not yet available");
    if (!strcmp(command,"disassemble")) return inspect_disassemble(s,r);
    return xdap_dispatch_response(s,r,false,NULL,"Unknown request");
}
