/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch_lexical.inc.c - Visible names with ordinary snapshot authority
 */
static XrXirStatus inspect_lexical(XdapAdapter *s,const XdapRequest *r,uint32_t frame,uint64_t start,uint64_t count) {
    XdapLexicalView view={0};XrXirStatus status=xdap_lexical_open(s->launch,s->snapshot,frame,&view);
    if (status!=XR_XIR_OK && status!=XR_XIR_UNRESOLVED) return status;
    uint32_t size=0,*bindings=NULL;
    for (uint32_t b=view.environment;b;b=view.function->bindings[b-1].parent) {
        if (xr_compile_resources_work(s->resources,1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
        ++size;
    }
    if (size) {
        if (xr_compile_resources_work(s->resources,(uint64_t)size*2*sizeof(*bindings))!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
        XrCompileResourceStatus allocated=xr_compile_resources_alloc(s->resources,(size_t)size*sizeof(*bindings),(void **)&bindings);
        if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    }
    uint32_t at=0;
    for (uint32_t b=view.environment;b;b=view.function->bindings[b-1].parent) bindings[at++]=b;
    XdapJsonWriter body=xdap_json_writer(s->resources,s->config.message_limit);XDAP_TEXT(&body,"{\"variables\":[");
    uint32_t shown=0,before=s->value_count;uint64_t ordinal=0;
    for (uint32_t i=size;i && body.status==XR_XIR_OK;--i) {
        const XrXirSourceDebugBinding *binding=&view.function->bindings[bindings[i-1]-1];
        const char *text=view.source->declarations[binding->declaration-1].name;size_t bytes=strlen(text);
        bool hidden=false;
        for (uint32_t newer=0;newer<i-1;++newer) {
            const XrXirSourceDebugBinding *candidate=&view.function->bindings[bindings[newer]-1];
            const char *name=view.source->declarations[candidate->declaration-1].name;
            if (xr_compile_resources_work(s->resources,(uint64_t)bytes+2)!=XR_COMPILE_RESOURCE_OK) { body.status=XR_XIR_BUDGET;break; }
            if (!strcmp(text,name)) { hidden=true;break; }
        }
        if (hidden || body.status!=XR_XIR_OK) continue;
        XrXirValue value={0};const char *name=NULL;size_t length=0;
        XrXirValueStatus copied=xdap_lexical_read(&view,bindings[i-1],&value,&name,&length);
        if (copied==XR_XIR_VALUE_BAD_ARGUMENT || copied==XR_XIR_VALUE_UNSUPPORTED) continue;
        if (copied!=XR_XIR_VALUE_OK) { body.status=copied==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;break; }
        if (ordinal++<start || (count && shown>=count)) { xr_xir_value_drop(&value);continue; }
        if (shown++) XDAP_TEXT(&body,",");
        XdapCellOrigin origin={0};
        if (binding->flags&XR_XIR_SOURCE_DEBUG_CELL) origin=(XdapCellOrigin){view.epoch,view.frame_id,binding->value,{0}};
        bool emitted=inspect_variable(s,&body,name,length,&value,frame,&origin);xr_xir_value_drop(&value);
        if (!emitted) break;
    }
    xr_compile_resources_free(bindings);
    XDAP_TEXT(&body,"]}");status=inspect_finish(s,r,&body);
    if (status!=XR_XIR_OK) inspect_rollback_values(s,before);return status;
}
