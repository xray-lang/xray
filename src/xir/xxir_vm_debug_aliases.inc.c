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
typedef struct DebugAliasWrite {
    XrXirVmDebug *debug;
    const XrXirTypes *types;
    const XrXirValue *cell,*replacement;
    XrXirValuePath target;
    XrXirValueAdmission *admission;
    bool identity;
} DebugAliasWrite;
static XrXirValueStatus debug_alias_update(const DebugAliasWrite *write,
    const XrXirValue *value,XrXirValue *payload,unsigned char *known) {
    if (!(*known&1) || !(*known&6) || !xr_xir_type_is_cell(write->types,(XrXirType)value->type)) return XR_XIR_VALUE_OK;
    if (write->identity) {
        const XrXirValue *root=xr_xir_cell_canonical_root(value);
        if (!root || root->type!=write->cell->type || root->payload!=write->cell->payload) return XR_XIR_VALUE_OK;
    } else if (!xr_xir_cell_same_owner(write->cell,value)) return XR_XIR_VALUE_OK;
    XrXirValuePath alias={0};XrXirValueStatus status=xir_cell_path_view(value,&alias);
    if (status!=XR_XIR_VALUE_OK || (!(*known&2) && alias.count<=write->target.count)) return status;
    XrXirValue owned={0};bool affected=false;
    status=debug_alias_value(write->debug,&write->target,&alias,payload,write->replacement,write->admission,&affected,&owned);
    if (status==XR_XIR_VALUE_BOUNDS) {
        *known=(unsigned char)((*known&~2u)|4u);xr_xir_value_drop(payload);return XR_XIR_VALUE_OK;
    }
    if (status!=XR_XIR_VALUE_OK) { xr_xir_value_drop(&owned);return status; }
    if (!affected) return XR_XIR_VALUE_OK;
    XrXirType expected=xr_xir_cell_element(write->types,(XrXirType)value->type);
    if (expected==XR_XIR_UNIT ? !debug_unit_value(&owned) : !xr_xir_value_argument(&owned,write->admission->arena,expected)) {
        xr_xir_value_drop(&owned);return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    xr_xir_value_drop(payload);*payload=owned;*known=(unsigned char)((*known|2u)&~4u);return XR_XIR_VALUE_OK;
}
static XrXirValueStatus debug_write_aliases(XrXirVmDebug *debug,XrXirVmDebugSnapshot *snapshot,
    const XrXirValue *cell,const XrXirValuePath *identity_path,const XrXirValue *replacement,XrXirValueAdmission *admission) {
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    DebugAliasWrite write={debug,module->types,cell,replacement,{0},admission,identity_path!=NULL};
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (identity_path) write.target=*identity_path;else status=xir_cell_path_view(cell,&write.target);
    if (status!=XR_XIR_VALUE_OK) return status;
    for (uint32_t f=0;f<snapshot->count;++f) {
        SnapshotFrame *frame=&snapshot->frames[f];
        if (!xr_xir_domain_work(debug->domain,(uint64_t)frame->info.value_count+frame->global_count)) return XR_XIR_VALUE_LIMIT;
        for (uint32_t v=0;v<frame->info.value_count;++v) {
            status=debug_alias_update(&write,&frame->values[v],&frame->cell_values[v],&frame->known[v]);
            if (status!=XR_XIR_VALUE_OK) return status;
        }
        for (uint32_t g=0;g<frame->global_count;++g) {
            SnapshotGlobal *global=&frame->globals[g];
            status=debug_alias_update(&write,&global->value,&global->cell_value,&global->known);
            if (status!=XR_XIR_VALUE_OK) return status;
        }
    }
    return XR_XIR_VALUE_OK;
}
