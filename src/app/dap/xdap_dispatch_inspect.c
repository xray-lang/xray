/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch_inspect.c - DAP observations through owned stop snapshots
 */
#include "xdap_dispatch_internal.h"
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
static bool inspect_position(XdapJsonWriter *w,uint32_t function,uint32_t instruction) {
    char position[64];int count=snprintf(position,sizeof(position),"f%u:i%u",function,instruction);
    return count>0 && xdap_json_quote(w,position,(size_t)count);
}
static XrXirStatus inspect_threads(XdapAdapter *s,const XdapRequest *r) {
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    if (s->snapshot) {
        uint64_t ids[64]={0};uint32_t count=0;XDAP_TEXT(&body,"{\"threads\":[");
        for (uint32_t i=0;i<s->depth;++i) {
            XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
            if (xr_xir_vm_debug_snapshot_frame(s->snapshot,i,&info,&name)!=XR_XIR_VALUE_OK) { body.status=XR_XIR_BUDGET;break; }
            xr_xir_value_drop(&name);bool duplicate=false;
            for (uint32_t j=0;j<count;++j) if (ids[j]==info.thread) duplicate=true;
            if (duplicate) continue;
            ids[count++]=info.thread;if (count>1) XDAP_TEXT(&body,",");XDAP_TEXT(&body,"{\"id\":");
            xdap_json_unsigned(&body,info.thread);XDAP_TEXT(&body,",\"name\":");
            char label[64];int length=snprintf(label,sizeof(label),"%s %llu",info.thread==1 ? "Root" : "Activation",(unsigned long long)info.thread);
            xdap_json_quote(&body,label,(size_t)length);XDAP_TEXT(&body,"}");
        }
        XDAP_TEXT(&body,"]}");
    } else if (s->launch && s->configured && !s->terminal) XDAP_TEXT(&body,"{\"threads\":[{\"id\":1,\"name\":\"Root\"}]}");
    else XDAP_TEXT(&body,"{\"threads\":[]}");
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
        XDAP_TEXT(&body,",\"line\":0,\"column\":0");
        if (info.code==XR_XIR_VM_DEBUG_VM && info.instruction!=UINT32_MAX) {
            XDAP_TEXT(&body,",\"instructionPointerReference\":");inspect_position(&body,info.function,info.instruction);
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
static const char *inspect_type(XrXirType type) {
    switch (type) {
    case XR_XIR_UNIT:return "unit";case XR_XIR_BOOL:return "bool";case XR_XIR_STRING:return "string";
    case XR_XIR_I8:return "i8";case XR_XIR_I16:return "i16";case XR_XIR_I32:return "i32";case XR_XIR_I64:return "i64";
    case XR_XIR_U8:return "u8";case XR_XIR_U16:return "u16";case XR_XIR_U32:return "u32";case XR_XIR_U64:return "u64";
    case XR_XIR_F32:return "f32";case XR_XIR_F64:return "f64";case XR_XIR_RUNE:return "Rune";
    case XR_XIR_ERROR:return "Error";default:return "owned";
    }
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
static XrXirStatus inspect_reference(XdapAdapter *s,const XrXirValue *value,uint32_t *handle,XdapValueShape *shape) {
    XrXirValueStatus status=xdap_value_shape(value,s->config.observer_metadata,shape);
    if (status!=XR_XIR_VALUE_OK) return status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    *handle=0;if (!shape->children) return XR_XIR_OK;
    for (uint32_t i=0;i<s->value_count;++i) {
        const XdapValueRef *ref=&s->values[i];
        if (ref->value.type==value->type && ref->value.payload==value->payload) { *handle=ref->handle;return XR_XIR_OK; }
    }
    if (s->value_count==128 || s->next_handle==INT32_MAX) return XR_XIR_BUDGET;
    XdapValueRef *ref=&s->values[s->value_count];status=xr_xir_value_copy(value,&ref->value);
    if (status!=XR_XIR_VALUE_OK) return status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    ref->handle=s->next_handle++;ref->shape=*shape;++s->value_count;*handle=ref->handle;return XR_XIR_OK;
}
static bool inspect_variable(XdapAdapter *s,XdapJsonWriter *body,const char *name,size_t length,const XrXirValue *value) {
    uint32_t handle=0;XdapValueShape shape={0};XrXirStatus status=inspect_reference(s,value,&handle,&shape);
    if (status!=XR_XIR_OK) { body->status=status;return false; }
    XDAP_TEXT(body,"{\"name\":");xdap_json_quote(body,name,length);XDAP_TEXT(body,",\"value\":");
    if (!inspect_value(body,value) && body->status==XR_XIR_OK) body->status=XR_XIR_BAD_STRUCTURE;
    if (s->variable_types) { const char *type=inspect_type((XrXirType)value->type);
        XDAP_TEXT(body,",\"type\":");xdap_json_quote(body,type,strlen(type)); }
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
        XrXirValueStatus status=xdap_value_child(&ref->value,s->config.observer_metadata,start+i,&value,&name,&length);
        if (status!=XR_XIR_VALUE_OK) {
            if (status==XR_XIR_VALUE_BAD_ARGUMENT || status==XR_XIR_VALUE_UNSUPPORTED || status==XR_XIR_VALUE_BOUNDS) {
                xdap_json_writer_free(&body);inspect_rollback_values(s,before);
                return xdap_dispatch_response(s,r,false,NULL,"Typed child observation is unavailable");
            }
            body.status=status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;break;
        }
        char index[32];if (!name) { int bytes=snprintf(index,sizeof(index),"[%llu]",(unsigned long long)(start+i));name=index;length=(size_t)bytes; }
        if (shown++) XDAP_TEXT(&body,",");bool emitted=inspect_variable(s,&body,name,length,&value);xr_xir_value_drop(&value);
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
        if (shown++) XDAP_TEXT(&body,",");bool emitted=inspect_variable(s,&body,name,(size_t)name_length,&value);xr_xir_value_drop(&value);
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
    XrXirVmDebugLocation locations[32]={{0}};uint32_t location_count=0;
    bool verified[32]={0};uint32_t item_number=0;
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"breakpoints\":[");
    for (uint32_t item=r->document.nodes[list].first;item;item=r->document.nodes[item].next,++item_number) {
        uint32_t name=0,condition=0,hits=0;
        status=xdap_json_member(&r->document,item,"name",&name);
        if (status==XR_XIR_OK) status=xdap_json_member(&r->document,item,"condition",&condition);
        if (status==XR_XIR_OK) status=xdap_json_member(&r->document,item,"hitCondition",&hits);
        if (status!=XR_XIR_OK || !name || r->document.nodes[name].kind!=XDAP_JSON_STRING) {
            xdap_json_writer_free(&body);return xdap_dispatch_response(s,r,false,NULL,"Invalid function breakpoint"); }
        uint32_t match=UINT32_MAX,matches=0;
        if (!condition && !hits) for (uint32_t f=0;f<s->launch->function_count;++f) {
            bool equal=false;status=xdap_json_equal(&r->document,name,s->launch->functions[f].name,&equal);
            if (status!=XR_XIR_OK) { xdap_json_writer_free(&body);return status; }
            if (equal) { match=f;++matches; }
        }
        if (matches==1) { verified[item_number]=true;locations[location_count++]=(XrXirVmDebugLocation){match,0}; }
        if (item_number) XDAP_TEXT(&body,",");
        if (verified[item_number]) XDAP_TEXT(&body,"{\"verified\":true}");
        else XDAP_TEXT(&body,"{\"verified\":false,\"message\":\"Unknown, ambiguous, or conditional function breakpoint\"}");
    }
    XDAP_TEXT(&body,"]}");
    if (body.status!=XR_XIR_OK) { status=body.status;xdap_json_writer_free(&body);return status; }
    char *names[32]={0};
    for (uint32_t i=0;i<location_count;++i) {
        const char *source=s->launch->functions[locations[i].function].name;size_t length=strlen(source);
        XrCompileResourceStatus allocated=xr_compile_resources_alloc(s->resources,length+1,(void **)&names[i]);
        if (allocated!=XR_COMPILE_RESOURCE_OK || xr_compile_resources_work(s->resources,length+1)!=XR_COMPILE_RESOURCE_OK) {
            for (uint32_t j=0;j<=i;++j) xr_compile_resources_free(names[j]);xdap_json_writer_free(&body);
            return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        }
        memcpy(names[i],source,length+1);
    }
    XdapJsonWriter response={0};status=xdap_dispatch_prepare_response(s,r,true,&body,NULL,&response);
    xdap_json_writer_free(&body);
    if (status!=XR_XIR_OK) { for (uint32_t i=0;i<location_count;++i) xr_compile_resources_free(names[i]);return status; }
    XrXirValueStatus installed=xdap_current_breakpoints_replace(s->launch->control,locations,location_count);
    if (installed!=XR_XIR_VALUE_OK) {
        for (uint32_t i=0;i<location_count;++i) xr_compile_resources_free(names[i]);xdap_json_writer_free(&response);
        return xdap_dispatch_response(s,r,false,NULL,"Breakpoint admission failed");
    }
    for (uint32_t i=0;i<s->function_breakpoint_count;++i) xr_compile_resources_free(s->function_breakpoints[i]);
    memset(s->function_breakpoints,0,sizeof(s->function_breakpoints));
    for (uint32_t i=0;i<location_count;++i) s->function_breakpoints[i]=names[i];
    s->function_breakpoint_count=location_count;status=xdap_dispatch_send_prepared(s,&response);
    xdap_json_writer_free(&response);return status;
}
static XrXirStatus inspect_disassemble(XdapAdapter *s,const XdapRequest *r) {
    if (!s->launch) return xdap_dispatch_response(s,r,false,NULL,"No admitted program");
    uint32_t reference=0,offset=0;uint64_t count=0;
    XrXirStatus status=xdap_dispatch_argument(r,"memoryReference",&reference);
    if (status==XR_XIR_OK) status=xdap_dispatch_number(r,"instructionCount",0,&count);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"instructionOffset",&offset);
    if (status!=XR_XIR_OK) return status;
    if (!reference || r->document.nodes[reference].kind!=XDAP_JSON_STRING || count>256)
        return xdap_dispatch_response(s,r,false,NULL,"Invalid instruction request");
    uint32_t function=0,instruction=0;char extra=0;
    if (sscanf(r->document.nodes[reference].text,"f%u:i%u%c",&function,&instruction,&extra)!=2 ||
        function>=s->launch->function_count)
        return xdap_dispatch_response(s,r,false,NULL,"Unknown instruction reference");
    int64_t displacement=0;
    if (offset) { status=xdap_json_i64(&r->document,offset,&displacement);if (status!=XR_XIR_OK) return status; }
    uint64_t bytes=0;status=xdap_dispatch_number(r,"offset",0,&bytes);
    if (status!=XR_XIR_OK) return status;
    if (bytes || displacement<-256 || displacement>256)
        return xdap_dispatch_response(s,r,false,NULL,"IR positions accept bounded instruction offsets");
    XdapCodeFunction *code=&s->launch->functions[function];
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"instructions\":[");
    for (uint64_t i=0;i<count;++i) {
        int64_t at=(int64_t)instruction+displacement+(int64_t)i;
        char address[64],description[160];
        int address_size=snprintf(address,sizeof(address),"f%u:i%lld",function,(long long)at);
        bool valid=at>=0 && (uint64_t)at<code->instructions;int text_size=0;
        if (valid) {
            const XrXirInstruction *op=&s->launch->instructions[code->first+(uint32_t)at];
            text_size=snprintf(description,sizeof(description),"%s type=%u args=(%u,%u) immediate=%lld",
                xr_xir_op_name(op->op),op->type,op->args[0],op->args[1],(long long)op->immediate);
        } else { memcpy(description,"invalid",8);text_size=7; }
        if (i) XDAP_TEXT(&body,",");XDAP_TEXT(&body,"{\"address\":");
        xdap_json_quote(&body,address,(size_t)address_size);XDAP_TEXT(&body,",\"instruction\":");
        xdap_json_quote(&body,description,(size_t)text_size);
        if (!valid) XDAP_TEXT(&body,",\"presentationHint\":\"invalid\"");
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
    if (!strcmp(command,"setBreakpoints")) return xdap_dispatch_response(s,r,false,NULL,"Owned source instruction mapping is not yet available");
    if (!strcmp(command,"evaluate") || !strcmp(command,"setVariable"))
        return xdap_dispatch_response(s,r,false,NULL,"Paused lexical expression admission is not yet available");
    if (!strcmp(command,"attach")) return xdap_dispatch_response(s,r,false,NULL,"Authenticated target attachment is not yet available");
    if (!strcmp(command,"restart")) return xdap_dispatch_response(s,r,false,NULL,"Restart construction is not yet available");
    if (!strcmp(command,"exceptionInfo") || !strcmp(command,"setExceptionBreakpoints"))
        return xdap_dispatch_response(s,r,false,NULL,"Current exception stop observation is not yet available");
    if (!strcmp(command,"disassemble")) return inspect_disassemble(s,r);
    return xdap_dispatch_response(s,r,false,NULL,"Unknown request");
}
