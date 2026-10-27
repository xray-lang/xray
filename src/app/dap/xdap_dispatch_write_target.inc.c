/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch_write_target.inc.c - Match displayed children to typed Source paths
 */
static bool inspect_write_index(const XdapJsonNode *name,uint64_t *output) {
    if (name->length<3 || name->length>12 || name->text[0]!='[' || name->text[name->length-1]!=']' ||
        (name->length>3 && name->text[1]=='0')) return false;
    uint64_t value=0;
    for (size_t i=1;i<name->length-1;++i) {
        unsigned char byte=(unsigned char)name->text[i];if (byte<'0' || byte>'9') return false;
        uint32_t digit=(uint32_t)(byte-'0');if (value>(UINT32_MAX-digit)/10u) return false;
        value=value*10u+digit;
    }
    *output=value;return true;
}
static XrXirStatus inspect_write_child(XdapAdapter *s,const XdapValueRef *parent,const XdapJsonNode *name,
    XrXirValuePathStep steps[128],XdapValueOrigin *output,XrXirValue *expected) {
    if (!parent->origin.epoch || parent->origin.epoch!=s->stop_epoch) return XR_XIR_BAD_TYPE;
    uint64_t first=0,end=parent->shape.children;
    if (parent->shape.indexed) {
        if (!inspect_write_index(name,&first) || first>=end) return XR_XIR_UNRESOLVED;
        end=first+1;
    }
    for (uint64_t i=first;i<end;++i) {
        XdapValueView view={&parent->value,s->config.observer_metadata,s->snapshot,parent->frame};
        XrXirValue child={0};XdapValueChildInfo info={0};
        XrXirStatus status=inspect_write_status(xdap_value_child(&view,i,&child,&info));
        if (status!=XR_XIR_OK) return status;
        char index[32];if (!info.name) {
            int bytes=snprintf(index,sizeof(index),"[%llu]",(unsigned long long)i);
            info.name=index;info.name_length=(size_t)bytes;
        }
        bool matches=info.name_length==name->length && !memcmp(info.name,name->text,name->length);
        if (!matches) { xr_xir_value_drop(&child);continue; }
        if (!info.selected) { xr_xir_value_drop(&child);return XR_XIR_BAD_TYPE; }
        status=inspect_origin_child(s,parent,&info,steps,output);
        if (status==XR_XIR_OK) *expected=child;else xr_xir_value_drop(&child);
        return status;
    }
    return XR_XIR_UNRESOLVED;
}
