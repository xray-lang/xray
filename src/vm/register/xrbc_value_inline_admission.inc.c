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
static bool rbc_writable_class_scalar(const XrtValueImage *image, const XrtValueImageType *type) {
    return xrt_class_scalar_field_supported(type->kind) ||
        (type->kind == XRT_VALUE_SUM && !type->owned_count && !type->field_count &&
            !type->sum_tag && type->sum_payload == type->sum_tag_size &&
            xrt_payloadless_sum_layout(type->variant_count, type->sum_tag_size, type->size, type->alignment)) ||
        (type->kind==XRT_VALUE_NULLABLE && !type->owned_count && type->nullable_element &&
            type->nullable_element<=image->type_count &&
            image->types[type->nullable_element-1].kind==XRT_VALUE_I64);
}
static bool rbc_writable_class_array(XrtValueKind kind) {
    return kind==XRT_VALUE_ARRAY_I64 || kind==XRT_VALUE_ARRAY_U8;
}
static bool rbc_writable_class_string(const XrtValueImage *image, const XrtValueImageType *type) {
    if (!image->owned_offsets) return false;
    if (type->kind==XRT_VALUE_NULLABLE) {
        const size_t payload=offsetof(XrtHostNullableOwner, owner);
        if (!type->nullable_element || type->nullable_element>image->type_count ||
            type->size!=sizeof(XrtHostNullableOwner) || type->alignment!=_Alignof(XrtHostNullableOwner) ||
            type->nullable_tag!=offsetof(XrtHostNullableOwner, present) || type->nullable_payload!=payload ||
            type->owned_count!=1 || type->first_owned>=image->owned_count ||
            image->owned_offsets[type->first_owned]!=payload) return false;
        type=&image->types[type->nullable_element-1];
    }
    return type->kind==XRT_VALUE_STRING && type->size==sizeof(XrtManagedHeader *) &&
        type->alignment==_Alignof(XrtManagedHeader *) && type->owned_count==1 &&
        type->first_owned<image->owned_count && !image->owned_offsets[type->first_owned];
}
static bool rbc_class_string_operand(const XrRbcModule *module, const XrRbcValueCode *code,
    uint32_t value, uint32_t type, XrtValueKind kind, bool destination) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    if (!home || home->type!=type) return false;
    if (kind==XRT_VALUE_STRING)
        return destination ? rbc_value_string_destination(module,code,value,false) :
            rbc_value_string_borrow_operand(module,code,value);
    return destination ? rbc_value_host_nullable_backing(module,code,value,XRT_VALUE_STRING) :
        rbc_value_inline(module,code,value,false);
}
static bool rbc_value_inline_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess a) {
    bool class_new=a.operation==XR_RBC_VALUE_CLASS_NEW, class_field=a.operation==XR_RBC_VALUE_CLASS_FIELD;
    bool class_push=a.operation==XR_RBC_VALUE_CLASS_ARRAY_PUSH, class_write=a.operation==XR_RBC_VALUE_CLASS_ARRAY_WRITE;
    bool scalar_write=a.operation==XR_RBC_VALUE_CLASS_SCALAR_WRITE, string_write=a.operation==XR_RBC_VALUE_CLASS_STRING_WRITE;
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
    if (class_new || class_field || class_push || class_write || scalar_write || string_write) {
        if (!xr_rbc_module_code_owner(module) || type->kind!=XRT_VALUE_CLASS ||
            (type->class_storage && !(class_field && type->class_storage == XR_NATIVE_DECLARATION_FILE_STAT)) ||
            !type->element || type->element>=home->type) return false;
        type=&image->types[type->element-1];
        if (type->kind!=XRT_VALUE_RECORD) return false;
    }
    if (class_push || class_write || scalar_write || string_write) {
        if (a.result || a.first || a.count || a.index>=type->field_count || !image->slots ||
            type->first_field>image->slot_count || type->field_count>image->slot_count-type->first_field) return false;
        const XrtValueImageSlot *field=&image->slots[type->first_field+a.index];
        if (field->flags!=XRT_VALUE_FIELD_MUTABLE || !field->type || field->type>image->type_count) return false;
        const XrtValueImageType *member=&image->types[field->type-1];
        if (scalar_write) return !a.window && rbc_writable_class_scalar(image,member) &&
            rbc_value_inline_operand(module,code,function,a.other,field->type,false);
        if (string_write) return !a.window && rbc_writable_class_string(image,member) &&
            rbc_class_string_operand(module,code,a.other,field->type,member->kind,false);
        if (!rbc_writable_class_array(member->kind)) return false;
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
    if (class_field || a.operation==XR_RBC_VALUE_RECORD_FIELD) {
        if (a.other || !xrt_value_kind_has_fields(type->kind) || a.index>=type->field_count || !image->slots ||
            type->first_field>image->slot_count || type->field_count>image->slot_count-type->first_field) return false;
        const XrtValueImageSlot *field=&image->slots[type->first_field+a.index];
        if (class_field && (field->flags & XRT_VALUE_FIELD_MUTABLE)) {
            if (field->flags!=XRT_VALUE_FIELD_MUTABLE || !field->type || field->type>image->type_count) return false;
            const XrtValueImageType *member=&image->types[field->type-1];
            if (rbc_writable_class_string(image,member))
                return rbc_class_string_operand(module,code,a.result,field->type,member->kind,true);
            if (!rbc_writable_class_array(member->kind) && !rbc_writable_class_scalar(image,member)) return false;
        }
        return rbc_value_inline_operand(module,code,function,a.result,field->type,true);
    }
    if (type->kind!=XRT_VALUE_NULLABLE || !type->nullable_element || type->nullable_element>=home->type ||
        type->nullable_tag>=type->size || type->nullable_payload>type->size) return false;
    if (a.operation==XR_RBC_VALUE_NULLABLE_NONE) return !a.source && !a.other;
    if (a.operation==XR_RBC_VALUE_NULLABLE_SOME)
        return !a.other && rbc_value_inline_operand(module,code,function,a.source,type->nullable_element,false);
    if (a.operation==XR_RBC_VALUE_NULLABLE_IS_SOME)
        return !a.other && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    return !a.other && rbc_value_inline_operand(module,code,function,a.result,type->nullable_element,true);
}
