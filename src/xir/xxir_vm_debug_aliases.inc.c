/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_aliases.inc.c - Prepare already-authorized Cell alias payloads
 */
static XrXirValueStatus debug_alias_value(XrXirVmDebug *debug,const XrXirValuePath *target,
    const XrXirValuePath *alias,const XrXirValue *before,const XrXirValue *replacement,
    XrXirValueAdmission *admission,bool *affected,XrXirValue *output) {
    uint32_t common=target->count<alias->count ? target->count : alias->count;
    if (!xr_xir_domain_work(debug->domain,(uint64_t)common+1)) return XR_XIR_VALUE_LIMIT;
    for (uint32_t i=0;i<common;++i) {
        const XrXirValuePathStep *a=&target->steps[i],*b=&alias->steps[i];
        if (a->kind!=b->kind || a->container!=b->container || a->selector!=b->selector) return XR_XIR_VALUE_OK;
    }
    *affected=true;
    if (target->count==alias->count) return xr_xir_value_copy(replacement,output);
    XrXirFaultDetail fault={0};
    if (alias->count<target->count) {
        XrXirValuePath relative={target->steps+alias->count,target->count-alias->count};
        return xir_value_path_replacement(before,&relative,replacement,admission,output,&fault);
    }
    XrXirValuePath relative={alias->steps+target->count,alias->count-target->count};
    XrXirValue borrowed=*replacement;XrXirValuePlace place={(XrXirType)replacement->type,&borrowed.payload};
    return xr_xir_value_path_read(&place,&relative,admission,output,&fault);
}
/* Known payloads were admitted by the fresh real Call. Bit 4 records a read
 * that passed its real authority and failed only at bounds; a replacement may
 * restore that descendant. Permission-denied roots never become readable. */
static XrXirValueStatus debug_write_aliases(XrXirVmDebug *debug,XrXirVmDebugSnapshot *snapshot,
    const XrXirValue *cell,const XrXirValue *replacement,XrXirValueAdmission *admission) {
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);XrXirValuePath target={0};
    XrXirValueStatus status=xir_cell_path_view(cell,&target);if (status!=XR_XIR_VALUE_OK) return status;
    for (uint32_t f=0;f<snapshot->count;++f) {
        SnapshotFrame *frame=&snapshot->frames[f];
        if (!xr_xir_domain_work(debug->domain,frame->info.value_count)) return XR_XIR_VALUE_LIMIT;
        for (uint32_t v=0;v<frame->info.value_count;++v) {
            if (!(frame->known[v]&1) || !(frame->known[v]&6) || !xr_xir_type_is_cell(module->types,(XrXirType)frame->values[v].type) ||
                !xr_xir_cell_same_owner(cell,&frame->values[v])) continue;
            XrXirValuePath alias={0};status=xir_cell_path_view(&frame->values[v],&alias);
            if (status!=XR_XIR_VALUE_OK) return status;
            if (!(frame->known[v]&2) && alias.count<=target.count) continue;
            XrXirValue owned={0};bool affected=false;
            status=debug_alias_value(debug,&target,&alias,&frame->cell_values[v],replacement,admission,&affected,&owned);
            if (status==XR_XIR_VALUE_BOUNDS) {
                frame->known[v]=(unsigned char)((frame->known[v]&~2u)|4u);xr_xir_value_drop(&frame->cell_values[v]);continue;
            }
            if (status!=XR_XIR_VALUE_OK) { xr_xir_value_drop(&owned);return status; }
            if (!affected) continue;
            XrXirType expected=xr_xir_cell_element(module->types,(XrXirType)frame->values[v].type);
            if (!xr_xir_value_argument(&owned,admission->arena,expected)) { xr_xir_value_drop(&owned);return XR_XIR_VALUE_BAD_ARGUMENT; }
            xr_xir_value_drop(&frame->cell_values[v]);frame->cell_values[v]=owned;frame->known[v]=(unsigned char)((frame->known[v]|2u)&~4u);
        }
    }
    return XR_XIR_VALUE_OK;
}
