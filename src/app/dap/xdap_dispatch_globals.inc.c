/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch_globals.inc.c - Module names over admitted owned slot snapshots
 */
static XrXirStatus inspect_globals(XdapAdapter *s,const XdapRequest *r,uint32_t frame,uint64_t start,uint64_t count) {
    XdapLexicalView view={0};XrXirStatus status=xdap_lexical_open(s->launch,s->snapshot,frame,&view);
    if (status!=XR_XIR_OK) return status;
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(s->launch->source);
    if (!debug) return XR_XIR_BAD_STRUCTURE;
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"variables\":[");
    uint32_t before=s->value_count,shown=0;uint64_t ordinal=0;
    for (uint32_t g=0;g<debug->global_count && body.status==XR_XIR_OK;++g) {
        XrXirValue value={0};const char *name=NULL;size_t length=0;
        XrXirValueStatus copied=xdap_global_read(&view,g,&value,&name,&length);
        if (copied==XR_XIR_VALUE_BAD_ARGUMENT || copied==XR_XIR_VALUE_UNSUPPORTED) continue;
        if (copied!=XR_XIR_VALUE_OK) { body.status=copied==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;break; }
        if (ordinal++<start || (count && shown>=count)) { xr_xir_value_drop(&value);continue; }
        if (shown++) XDAP_TEXT(&body,",");
        XdapValueOrigin origin={view.epoch,view.frame_id,debug->globals[g].slot,{0},true};
        bool emitted=inspect_variable(s,&body,name,length,&value,frame,&origin);xr_xir_value_drop(&value);
        if (!emitted) break;
    }
    XDAP_TEXT(&body,"]}");status=inspect_finish(s,r,&body);
    if (status!=XR_XIR_OK) inspect_rollback_values(s,before);return status;
}
