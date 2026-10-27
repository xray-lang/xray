/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch_write.inc.c - Prepared responses around live paused writes
 */
static XrXirStatus inspect_write_status(XrXirValueStatus status) {
    return status==XR_XIR_VALUE_OK ? XR_XIR_OK : status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
        status==XR_XIR_VALUE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
        status==XR_XIR_VALUE_OVERFLOW ? XR_XIR_OVERFLOW :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
}
static XrXirStatus inspect_write_binding(const XdapLexicalView *lexical,const XdapJsonNode *name,uint32_t *operand,XrXirValue *expected) {
    for (uint32_t b=lexical->environment;b;b=lexical->function->bindings[b-1].parent) {
        const XrXirSourceDebugBinding *binding=&lexical->function->bindings[b-1];
        const char *text=lexical->source->declarations[binding->declaration-1].name;size_t bytes=strlen(text);
        if (bytes!=name->length || memcmp(text,name->text,bytes)) continue;
        if (!(binding->flags&XR_XIR_SOURCE_DEBUG_CELL)) return XR_XIR_BAD_TYPE;
        const char *read_name=NULL;size_t length=0;
        XrXirStatus status=inspect_write_status(xdap_lexical_read(lexical,b,expected,&read_name,&length));
        if (status==XR_XIR_OK) *operand=binding->value;return status;
    }
    return XR_XIR_UNRESOLVED;
}
static XrXirStatus inspect_write_global(const XdapLexicalView *view,const XdapJsonNode *name,uint32_t *slot,XrXirValue *expected) {
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(view->launch->source);
    if (!debug) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t g=0;g<debug->global_count;++g) {
        XrXirValue value={0};const char *text=NULL;size_t length=0;
        XrXirValueStatus read=xdap_global_read(view,g,&value,&text,&length);
        if (read==XR_XIR_VALUE_BAD_ARGUMENT || read==XR_XIR_VALUE_UNSUPPORTED) continue;
        if (read!=XR_XIR_VALUE_OK) return inspect_write_status(read);
        bool matches=length==name->length && !memcmp(text,name->text,length);
        if (!matches) { xr_xir_value_drop(&value);continue; }
        const XrXirSourceDeclaration *declaration=&view->source->declarations[debug->globals[g].declaration-1];
        if (!declaration->mutable) { xr_xir_value_drop(&value);return XR_XIR_BAD_TYPE; }
        *expected=value;*slot=debug->globals[g].slot;return XR_XIR_OK;
    }
    return XR_XIR_UNRESOLVED;
}
#include "xdap_dispatch_write_target.inc.c"
static XrXirStatus inspect_write_body(XdapAdapter *s,const XrXirValue *value,uint32_t frame,const XdapValueOrigin *origin,uint32_t *reference,XdapJsonWriter *body) {
    XdapValueShape shape={0};body->status=inspect_reference(s,value,frame,origin,reference,&shape);
    XDAP_TEXT(body,"{\"value\":");if (!inspect_value(body,value) && body->status==XR_XIR_OK) body->status=XR_XIR_BAD_STRUCTURE;
    if (s->variable_types && body->status==XR_XIR_OK) {
        XdapValueView view={value,s->config.observer_metadata,s->snapshot,frame};XdapTypeText type={0};
        body->status=inspect_write_status(xdap_value_type_text(&view,&type));
        if (body->status==XR_XIR_OK) { XDAP_TEXT(body,",\"type\":");xdap_json_quote(body,type.bytes,type.length); }
    }
    XDAP_TEXT(body,",\"variablesReference\":");xdap_json_unsigned(body,*reference);
    if (shape.children) {
        XDAP_TEXT(body,",");const char *key=shape.indexed ? "indexedVariables" : "namedVariables";
        xdap_json_quote(body,key,strlen(key));XDAP_TEXT(body,":");xdap_json_unsigned(body,shape.children);
    }
    XDAP_TEXT(body,"}");return body->status;
}
static void inspect_write_retire_values(XdapAdapter *s,uint32_t reference,uint64_t epoch) {
    XdapValueRef kept={0};
    for (uint32_t i=0;i<s->value_count;++i) {
        if (s->values[i].handle==reference) { kept=s->values[i];s->values[i]=(XdapValueRef){0}; }
        xdap_value_ref_drop(&s->values[i]);
    }
    memset(s->values,0,sizeof(s->values));s->value_count=kept.handle ? 1 : 0;
    if (kept.handle) { if (kept.origin.epoch) kept.origin.epoch=epoch;s->values[0]=kept; }
}
static XrXirStatus inspect_set_variable(XdapAdapter *s,const XdapRequest *r) {
    uint64_t scope=0;uint32_t name=0,text=0;XrXirStatus status=xdap_dispatch_number(r,"variablesReference",0,&scope);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"name",&name);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"value",&text);
    if (status!=XR_XIR_OK) return status;
    uint32_t frame=inspect_frame(s,scope,true);const XdapValueRef *parent=NULL;bool global=false;
    if (frame==UINT32_MAX) { frame=inspect_frame(s,scope,2);global=frame!=UINT32_MAX; }
    if (frame==UINT32_MAX) for (uint32_t i=0;i<s->value_count;++i) if (s->values[i].handle==scope) { parent=&s->values[i];frame=parent->frame;break; }
    if (!s->snapshot || s->running || frame>=s->depth || !name || !text || r->document.nodes[name].kind!=XDAP_JSON_STRING ||
        r->document.nodes[text].kind!=XDAP_JSON_STRING || !r->document.nodes[name].length || !r->document.nodes[text].length)
        return xdap_dispatch_response(s,r,false,NULL,"A current variable scope or child reference, writable name and complete value expression are required");
    if (s->stop_epoch==UINT64_MAX) return XR_XIR_BUDGET;
    XdapLexicalView lexical={0};status=xdap_lexical_open(s->launch,s->snapshot,frame,&lexical);
    XrXirValuePathStep steps[128];XdapValueOrigin target={0};XrXirValue value={0},expected={0};
    if (status==XR_XIR_OK && parent) status=inspect_write_child(s,parent,&r->document.nodes[name],steps,&target,&expected);
    else if (status==XR_XIR_OK) {
        uint32_t operand=0;status=global ? inspect_write_global(&lexical,&r->document.nodes[name],&operand,&expected) :
            inspect_write_binding(&lexical,&r->document.nodes[name],&operand,&expected);
        if (status==XR_XIR_OK) target=(XdapValueOrigin){lexical.epoch,lexical.frame_id,operand,{0},global};
    }
    if (status==XR_XIR_OK && (target.epoch!=s->stop_epoch || target.frame!=lexical.frame_id)) status=XR_XIR_BAD_TYPE;
    if (status==XR_XIR_OK) {
        const XdapJsonNode *expression=&r->document.nodes[text];
        XdapExpressionRequest request={s->compiler,&lexical,expression->text,expression->length,s->config.message_limit,&expected};
        status=xdap_expression_evaluate(&request,&value);
    }
    xr_xir_value_drop(&expected);
    if (status!=XR_XIR_OK) {
        if (status==XR_XIR_OUT_OF_MEMORY || status==XR_XIR_BUDGET) return status;
        return xdap_dispatch_response(s,r,false,NULL,"The selected binding or value expression is not writable");
    }
    uint32_t before=s->value_count,next=s->next_handle,reference=0;bool applied=false,armed=false;
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit),success={0},failure={0};
    XdapValueOrigin returned=target;returned.epoch=s->stop_epoch+1;
    status=inspect_write_body(s,&value,frame,&returned,&reference,&body);
    if (status==XR_XIR_OK) status=xdap_dispatch_stage_response(s,r,true,&body,NULL,&success);
    if (status==XR_XIR_OK) status=xdap_dispatch_stage_response(s,r,false,NULL,"The paused write was not admitted",&failure);
    if (status==XR_XIR_OK) {
        XrXirValueStatus begun=target.slot ? xdap_current_write_slot_begin(s->launch->control,s->stop_epoch,target.frame,target.operand,&target.path,&value) :
            xdap_current_write_begin(s->launch->control,s->stop_epoch,target.frame,target.operand,&target.path,&value);
        armed=begun==XR_XIR_VALUE_OK;status=inspect_write_status(begun);
    }
    if (armed) {
        XdapCurrentResult result={0};XrXirValueStatus driver=xdap_current_poll(s->launch->control,1,&result);
        bool visited=false;XrXirVmDebugSnapshot *fresh=NULL;
        XrXirValueStatus written=xdap_current_write_finish(s->launch->control,&visited,&fresh);
        applied=written==XR_XIR_VALUE_OK && fresh!=NULL;
        if (applied) {
            uint64_t epoch=xdap_current_stopped_epoch(s->launch->control);inspect_write_retire_values(s,reference,epoch);
            xr_xir_vm_debug_snapshot_drop(s->snapshot);s->snapshot=fresh;s->stop_epoch=epoch;
            status=xdap_dispatch_send_staged(s,&success);
        } else {
            xr_xir_vm_debug_snapshot_drop(fresh);status=inspect_write_status(written);
            if (status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET) status=xdap_dispatch_send_staged(s,&failure);
        }
        if (driver!=XR_XIR_VALUE_OK) {
            if (!xdap_current_stopped_epoch(s->launch->control)) { xdap_dispatch_clear_stop(s);s->running=true; }
            if (status==XR_XIR_OK) status=inspect_write_status(driver);
        }
        xdap_current_result_drop(&result);
    } else if (status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET && failure.bytes) status=xdap_dispatch_send_staged(s,&failure);
    if (!applied) { inspect_rollback_values(s,before);s->next_handle=next; }
    xr_xir_value_drop(&value);xdap_json_writer_free(&body);xdap_json_writer_free(&success);xdap_json_writer_free(&failure);return status;
}
