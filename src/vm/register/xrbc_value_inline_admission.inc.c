/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_inline_admission.inc.c - Sealed field and Nullable descriptions
 */
static bool rbc_value_inline_operand(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,uint32_t value,uint32_t type,bool destination) {
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!type || !image || !image->types || type>image->type_count) return false;
    const XrLirValueHome *home=rbc_value_home(code,value);
    if (home && home->type) {
        if (home->type!=type) return false;
        if (rbc_value_owner(module,code,value,destination)) return true;
        return !home->owner_count && rbc_value_location_valid(function,code->locations[value]) &&
            code->locations[value].kind==XR_RBC_VALUE_AGGREGATE;
    }
    const XrtValueImageType *row=&image->types[type-1];
    return !row->owned_count && row->size<=8 && row->kind<XRT_VALUE_STRING &&
        rbc_value_scalar(code,function,value,XR_RBC_BITS);
}
static bool rbc_value_inline_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess a) {
    bool class_new=a.operation==XR_RBC_VALUE_CLASS_NEW, class_field=a.operation==XR_RBC_VALUE_CLASS_FIELD;
    bool class_push=a.operation==XR_RBC_VALUE_CLASS_ARRAY_PUSH, class_write=a.operation==XR_RBC_VALUE_CLASS_ARRAY_WRITE;
    bool scalar_write=a.operation==XR_RBC_VALUE_CLASS_SCALAR_WRITE;
    bool sum_new=a.operation==XR_RBC_VALUE_SUM_NEW, sum_tag=a.operation==XR_RBC_VALUE_SUM_TAG;
    bool sum_get=a.operation==XR_RBC_VALUE_SUM_GET;
    bool create=class_new || a.operation==XR_RBC_VALUE_RECORD_NEW || sum_new ||
        a.operation==XR_RBC_VALUE_NULLABLE_NONE || a.operation==XR_RBC_VALUE_NULLABLE_SOME;
    uint32_t value=create ? a.result : a.source;
    if (!rbc_value_inline(module,code,value,create)) return false;
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    const XrtValueImageType *type=&image->types[home->type-1];
    if (sum_new || sum_tag || sum_get) {
        if (type->kind!=XRT_VALUE_SUM || !image->variants ||
            type->first_variant>image->variant_count ||
            type->variant_count>image->variant_count-type->first_variant) return false;
        if (sum_new) {
            if (a.index>=type->variant_count || a.other || a.window) return false;
            const XrtValueImageVariant *variant=&image->variants[type->first_variant+a.index];
            if (variant->field_count!=a.count || a.first>code->argument_count ||
                a.count>code->argument_count-a.first || (a.count && !code->arguments)) return false;
            for (uint32_t p=0;p<a.count;++p) {
                uint32_t at=type->first_field+variant->first_field+p;
                if (at>=image->slot_count) return false;
                if (!rbc_value_inline_operand(module,code,function,code->arguments[a.first+p],
                    image->slots[at].type,false)) return false;
            }
            return true;
        }
        if (sum_tag) return !a.other && !a.index && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
        if (a.other || a.index>=type->field_count || type->first_field>image->slot_count ||
            type->field_count>image->slot_count-type->first_field) return false;
        return rbc_value_inline_operand(module,code,function,a.result,
            image->slots[type->first_field+a.index].type,true);
    }
    if (class_new || class_field || class_push || class_write || scalar_write) {
        if (!xr_rbc_module_code_owner(module) || type->kind!=XRT_VALUE_CLASS ||
            type->class_storage || !type->element || type->element>=home->type) return false;
        type=&image->types[type->element-1];
        if (type->kind!=XRT_VALUE_RECORD) return false;
    }
    if (class_push || class_write || scalar_write) {
        if (a.result || a.first || a.count || a.index>=type->field_count || !image->slots ||
            type->first_field>image->slot_count || type->field_count>image->slot_count-type->first_field) return false;
        const XrtValueImageSlot *field=&image->slots[type->first_field+a.index];
        if (field->flags!=XRT_VALUE_FIELD_MUTABLE || !field->type || field->type>image->type_count) return false;
        XrtValueKind kind=image->types[field->type-1].kind;
        if (scalar_write) return !a.window && xrt_class_scalar_field_supported(kind) &&
            rbc_value_inline_operand(module,code,function,a.other,field->type,false);
        if (kind!=XRT_VALUE_ARRAY_I64 && kind!=XRT_VALUE_ARRAY_U8) return false;
        if (class_write) return !a.window && rbc_value_inline_operand(module,code,function,a.other,field->type,false);
        if (!a.window || a.window>code->homes->workspace_count || !code->homes->workspaces ||
            !rbc_value_scalar(code,function,a.other,XR_RBC_BITS)) return false;
        const XrLirHomeWorkspace *workspace=&code->homes->workspaces[a.window-1];
        const XrtManagedFrameSpec *frame=&code->homes->frame;
        return workspace->kind==XR_LIR_HOME_WORKSPACE_CLASS_ARRAY_PUSH && workspace->type==field->type &&
            workspace->value==a.source && workspace->block && workspace->instruction &&
            frame->owned_offsets && workspace->first_owner<frame->slot_count &&
            frame->owned_offsets[workspace->first_owner]==workspace->offset;
    }
    if (class_new || a.operation==XR_RBC_VALUE_RECORD_NEW) {
        if (a.source || a.other || a.window || !xrt_value_kind_has_fields(type->kind) || a.count!=type->field_count ||
            a.first>code->argument_count || a.count>code->argument_count-a.first ||
            (a.count && (!code->arguments || !image->slots)) || type->first_field>image->slot_count ||
            type->field_count>image->slot_count-type->first_field) return false;
        for (uint32_t p=0;p<a.count;++p)
            if (!rbc_value_inline_operand(module,code,function,code->arguments[a.first+p],image->slots[type->first_field+p].type,false)) return false;
        return true;
    }
    if (class_field || a.operation==XR_RBC_VALUE_RECORD_FIELD)
        return !a.other && xrt_value_kind_has_fields(type->kind) && a.index<type->field_count && image->slots &&
            type->first_field<=image->slot_count && type->field_count<=image->slot_count-type->first_field &&
            rbc_value_inline_operand(module,code,function,a.result,image->slots[type->first_field+a.index].type,true);
    if (type->kind!=XRT_VALUE_NULLABLE || !type->nullable_element || type->nullable_element>=home->type ||
        type->nullable_tag>=type->size || type->nullable_payload>type->size) return false;
    if (a.operation==XR_RBC_VALUE_NULLABLE_NONE) return !a.source && !a.other;
    if (a.operation==XR_RBC_VALUE_NULLABLE_SOME)
        return !a.other && rbc_value_inline_operand(module,code,function,a.source,type->nullable_element,false);
    if (a.operation==XR_RBC_VALUE_NULLABLE_IS_SOME)
        return !a.other && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    return !a.other && rbc_value_inline_operand(module,code,function,a.result,type->nullable_element,true);
}
