/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_observe.inc.c - Read immutable aggregate storage without owner admission
 */
static bool value_observe_work(XrXirValueAdmission *admission,uint64_t units) {
    if (units>admission->work) return false;
    admission->work-=units;return xr_xir_domain_work(admission->domain,units);
}
static uint64_t value_observe_header_work(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    uint64_t fields=0;
    if (node && node->kind==XR_XIR_TYPE_TUPLE) fields=node->parameter_count;
    else if (node && node->kind==XR_XIR_TYPE_NULLABLE) fields=1;
    else if (node && node->kind==XR_XIR_TYPE_NOMINAL && !xr_xir_type_is_class(types,type)) fields=node->nominal.field_count;
    /* Admission and the owned copy each validate their immediate headers. */
    return 4+2*fields;
}
static XrXirValueStatus value_observe_child_operation(const XrXirValue *value,const XrXirValuePathStep *step,
    XrXirValueAdmission *admission,XrXirValue *output,XrXirFaultDetail *fault) {
    if (!value || !step || !admission || !admission->arena || !admission->domain || !fault || !unit_value(output) ||
        !owned_carrier_type((XrXirType)value->type) || !value_header_valid(value) || step->container!=(XrXirType)value->type ||
        object_pointer(value)->arena!=admission->arena) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission->arena);
    bool indexed=step->kind==XR_XIR_PATH_INDEX && xr_xir_type_is_array(types,(XrXirType)value->type);
    bool field=step->kind==XR_XIR_PATH_FIELD && xr_xir_type_is_struct(types,(XrXirType)value->type);
    bool tuple=step->kind==XR_XIR_PATH_TUPLE_FIELD && xr_xir_tuple_signature(types,(XrXirType)value->type);
    bool nullable=step->kind==XR_XIR_PATH_NULLABLE_VALUE && xr_xir_type_is_nullable(types,(XrXirType)value->type);
    bool enumeration=step->kind==XR_XIR_PATH_ENUM_FIELD && xr_xir_type_is_enum(types,(XrXirType)value->type);
    if (!indexed && !field && !tuple && !nullable && !enumeration) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!value_observe_work(admission,value_observe_header_work(types,(XrXirType)value->type))) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_value_argument(value,admission->arena,(XrXirType)value->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirArray *array=indexed ? (XirArray *)object_pointer(value) : NULL;
    XirNominalValue *record=field || enumeration ? (XirNominalValue *)object_pointer(value) : NULL;
    XirTuple *ordered=tuple ? (XirTuple *)object_pointer(value) : NULL;
    const XrXirValue *payload=NULL;bool some=false;
    if (nullable && !xr_xir_nullable_view(value,&some,&payload)) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t count=indexed ? array->length : tuple ? ordered->count : nullable ? (uint64_t)some : record->count;
    if (step->selector<0 || (uint64_t)step->selector>=count) {
        *fault=(XrXirFaultDetail){XR_XIR_PANIC_BOUNDS,0,step->selector,(int64_t)count};return XR_XIR_VALUE_BOUNDS;
    }
    if (indexed) {
        if (!value_observe_work(admission,value_observe_header_work(types,array->element))) return XR_XIR_VALUE_LIMIT;
        StorageSpan span={array->element,array_slot(array,(size_t)step->selector)};
        return storage_unpack_operation_mode(span,admission,output,false,true);
    }
    const XrXirValue *child=tuple ? &ordered->fields[step->selector] : nullable ? payload : &record->fields[step->selector];
    if (!value_observe_work(admission,value_observe_header_work(types,(XrXirType)child->type))) return XR_XIR_VALUE_LIMIT;
    /* Unit is valid stored data but is deliberately not a call argument. */
    if (child->type==XR_XIR_UNIT ? !unit_value(child) :
        !xr_xir_value_argument(child,admission->arena,(XrXirType)child->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    return xr_xir_value_copy(child,output);
}
XR_FUNC XrXirValueStatus xir_value_observe_child(const XrXirValue *value,const XrXirValuePathStep *step,
    XrXirValueAdmission *admission,XrXirValue *output,XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=value_observe_child_operation(value,step,admission,output,fault);
    xr_xir_value_graph_end();return status;
}
