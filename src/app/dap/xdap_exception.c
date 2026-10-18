/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_exception.c - First-chance VM language throw configuration and queries
 */
#include "xdap_dispatch_internal.h"
#include "../../xir/xxir_enum.h"
#include "../../xir/xxir_vm_debug_internal.h"
#include <string.h>
static XrXirStatus exception_configure(XdapAdapter *s,const XdapRequest *r) {
    uint32_t filters=0,options=0,exceptions=0;
    XrXirStatus status=xdap_dispatch_argument(r,"filters",&filters);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"filterOptions",&options);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"exceptionOptions",&exceptions);
    if (status!=XR_XIR_OK) return status;
    if (!filters || r->document.nodes[filters].kind!=XDAP_JSON_ARRAY || r->document.nodes[filters].count>3 ||
        (options && (r->document.nodes[options].kind!=XDAP_JSON_ARRAY || r->document.nodes[options].count)) ||
        (exceptions && (r->document.nodes[exceptions].kind!=XDAP_JSON_ARRAY || r->document.nodes[exceptions].count)))
        return xdap_dispatch_response(s,r,false,NULL,"Unsupported exception filter configuration");
    uint32_t enabled=0;
    for (uint32_t filter=r->document.nodes[filters].first;filter;filter=r->document.nodes[filter].next) {
        if (r->document.nodes[filter].kind!=XDAP_JSON_STRING) return xdap_dispatch_response(s,r,false,NULL,"Invalid exception filter");
        static const char *const names[]={"all","caught","uncaught"};uint32_t selected=0;
        for (uint32_t i=0;i<3;++i) {
            bool equal=false;status=xdap_json_equal(&r->document,filter,names[i],&equal);if (status!=XR_XIR_OK) return status;
            if (equal) selected=1u<<i;
        }
        if (!selected || (enabled&selected)) return xdap_dispatch_response(s,r,false,NULL,"Unknown or duplicate exception filter");
        enabled|=selected;
    }
    if (s->running) return xdap_dispatch_response(s,r,false,NULL,"notStopped");
    XdapJsonWriter response={0};status=xdap_dispatch_prepare_response(s,r,true,NULL,NULL,&response);
    if (status!=XR_XIR_OK) return status;
    if (s->launch && !s->terminal && xdap_current_break_on_throw(s->launch->control,enabled)!=XR_XIR_VALUE_OK) {
        xdap_json_writer_free(&response);return xdap_dispatch_response(s,r,false,NULL,"Exception filter admission failed");
    }
    s->break_throw=enabled;status=xdap_dispatch_send_prepared(s,&response);xdap_json_writer_free(&response);return status;
}
static XrXirStatus dap_exception_info(XdapAdapter *s,const XdapRequest *r) {
    uint64_t thread=0;XrXirStatus status=xdap_dispatch_number(r,"threadId",0,&thread);
    if (status!=XR_XIR_OK) return status;
    if (!s->snapshot || s->running || !thread || thread!=xr_xir_vm_debug_snapshot_stopped_thread(s->snapshot))
        return xdap_dispatch_response(s,r,false,NULL,"No stopped exception thread");
    XrXirValue value={0};XrXirValueStatus copied=xr_xir_vm_debug_snapshot_exception(s->snapshot,&value);
    if (copied!=XR_XIR_VALUE_OK)
        return copied==XR_XIR_VALUE_UNSUPPORTED ? xdap_dispatch_response(s,r,false,NULL,"Stop has no language exception") :
            copied==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    XrXirEnumBorrow borrowed={0};copied=xr_xir_enum_borrow(&value,&borrowed);
    if (copied!=XR_XIR_VALUE_OK) { xr_xir_value_drop(&value);return XR_XIR_BAD_STRUCTURE; }
    uint32_t filter=0;copied=xir_vm_debug_snapshot_exception_filter(s->snapshot,&filter);
    if (copied!=XR_XIR_VALUE_OK) { xr_xir_value_drop(&value);return copied==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE; }
    bool visible=false;copied=xr_xir_vm_debug_snapshot_type_visible(s->snapshot,0,&value,(XrXirType)value.type,&visible);
    if (copied!=XR_XIR_VALUE_OK) { xr_xir_value_drop(&value);return copied==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE; }
    XdapJsonWriter identity=xdap_json_writer(s->resources,s->config.message_limit);
    if (visible) {
        xdap_json_text(&identity,borrowed.name.bytes,borrowed.name.length);XDAP_TEXT(&identity,".");
        xdap_json_text(&identity,borrowed.member.bytes,borrowed.member.length);
    } else XDAP_TEXT(&identity,"<private exception>");
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    if (identity.status==XR_XIR_OK) {
        XDAP_TEXT(&body,"{\"exceptionId\":");xdap_json_quote(&body,identity.bytes,identity.length);
        XDAP_TEXT(&body,",\"breakMode\":");
        const char *mode=filter==XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT ? "unhandled" : "always";
        xdap_json_quote(&body,mode,strlen(mode));XDAP_TEXT(&body,",\"details\":{\"typeName\":");
        if (visible) xdap_json_quote(&body,borrowed.name.bytes,borrowed.name.length);
        else xdap_json_quote(&body,"<private type>",14);
        XDAP_TEXT(&body,"}}");
        status=xdap_dispatch_response(s,r,true,&body,NULL);
    } else status=identity.status;
    xdap_json_writer_free(&body);xdap_json_writer_free(&identity);xr_xir_value_drop(&value);return status;
}
XR_FUNC XrXirStatus xdap_dispatch_exception(XdapAdapter *s,const XdapRequest *r,bool configure) {
    if (!s->initialized) return xdap_dispatch_response(s,r,false,NULL,"Initialize first");
    return configure ? exception_configure(s,r) : dap_exception_info(s,r);
}
