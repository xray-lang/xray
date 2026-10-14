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
static XrXirStatus exception_configure(XdapAdapter *s,const XdapRequest *r) {
    uint32_t filters=0,options=0,exceptions=0;
    XrXirStatus status=xdap_dispatch_argument(r,"filters",&filters);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"filterOptions",&options);
    if (status==XR_XIR_OK) status=xdap_dispatch_argument(r,"exceptionOptions",&exceptions);
    if (status!=XR_XIR_OK) return status;
    if (!filters || r->document.nodes[filters].kind!=XDAP_JSON_ARRAY || r->document.nodes[filters].count>1 ||
        (options && (r->document.nodes[options].kind!=XDAP_JSON_ARRAY || r->document.nodes[options].count)) ||
        (exceptions && (r->document.nodes[exceptions].kind!=XDAP_JSON_ARRAY || r->document.nodes[exceptions].count)))
        return xdap_dispatch_response(s,r,false,NULL,"Unsupported exception filter configuration");
    bool enabled=false;
    if (r->document.nodes[filters].count) {
        uint32_t filter=r->document.nodes[filters].first;
        if (r->document.nodes[filter].kind!=XDAP_JSON_STRING)
            return xdap_dispatch_response(s,r,false,NULL,"Invalid exception filter");
        status=xdap_json_equal(&r->document,filter,"all",&enabled);if (status!=XR_XIR_OK) return status;
        if (!enabled) return xdap_dispatch_response(s,r,false,NULL,"Unknown exception filter");
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
    XdapJsonWriter identity=xdap_json_writer(s->resources,s->config.message_limit);
    xdap_json_text(&identity,borrowed.name.bytes,borrowed.name.length);XDAP_TEXT(&identity,".");
    xdap_json_text(&identity,borrowed.member.bytes,borrowed.member.length);
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);
    if (identity.status==XR_XIR_OK) {
        XDAP_TEXT(&body,"{\"exceptionId\":");xdap_json_quote(&body,identity.bytes,identity.length);
        XDAP_TEXT(&body,",\"breakMode\":\"always\",\"details\":{\"typeName\":");
        xdap_json_quote(&body,borrowed.name.bytes,borrowed.name.length);XDAP_TEXT(&body,"}}");
        status=xdap_dispatch_response(s,r,true,&body,NULL);
    } else status=identity.status;
    xdap_json_writer_free(&body);xdap_json_writer_free(&identity);xr_xir_value_drop(&value);return status;
}
XR_FUNC XrXirStatus xdap_dispatch_exception(XdapAdapter *s,const XdapRequest *r,bool configure) {
    if (!s->initialized) return xdap_dispatch_response(s,r,false,NULL,"Initialize first");
    return configure ? exception_configure(s,r) : dap_exception_info(s,r);
}
