/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_value.c - Owned aggregate children without execution or write permission
 */
#include "xdap_value.h"
#include "../../xir/xxir_array.h"
#include "../../xir/xxir_tuple.h"
#include "../../xir/xxir_nullable.h"
#include "../../xir/xxir_enum.h"
#include "../../xir/xxir_struct.h"
#include "../../xir/xxir_class.h"
#include "../../xir/xxir_type_arena.h"
#include "../../xir/xxir_types.h"
#include "../../xir/xxir_value_internal.h"
static XrXirValueAdmission value_admission(const XrXirValue *value,XrXirDomain *domain) {
    return (XrXirValueAdmission){.arena=xr_xir_value_arena(value),.domain=domain,.work=65536,.scratch_bytes=65536};
}
static const XrXirNominalIdentity *value_nominal(const XrXirValue *value,const XrXirTypes *types) {
    const XrXirTypeNode *node=xr_xir_type_node(types,(XrXirType)value->type);
    return node && (xr_xir_type_is_struct(types,(XrXirType)value->type) || xr_xir_type_is_class(types,(XrXirType)value->type)) ?
        &types->nominals->identities[node->nominal.declaration] : NULL;
}
XR_FUNC XrXirValueStatus xdap_value_shape(const XdapValueView *view,XdapValueShape *output) {
    if (!view) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirValue *value=view->value;XrXirDomain *domain=view->domain;
    if (!value || !output || !xr_xir_domain_current(domain) || !xr_xir_value_valid(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(domain,1)) return XR_XIR_VALUE_LIMIT;
    XdapValueShape shape={0};const XrXirTypeArena *arena=xr_xir_value_arena(value);
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(types,(XrXirType)value->type);
    if (xr_xir_type_is_array(types,(XrXirType)value->type)) {
        XrXirValueAdmission admission=value_admission(value,domain);int64_t length=0;
        XrXirValueStatus status=xr_xir_array_len(value,&admission,&length);if (status!=XR_XIR_VALUE_OK) return status;
        if (length<0) return XR_XIR_VALUE_BAD_ARGUMENT;
        shape=(XdapValueShape){(uint64_t)length,true};
    } else if (tuple) shape=(XdapValueShape){tuple->parameter_count,true};
    else if (value_nominal(value,types)) {
        const XrXirNominalIdentity *identity=value_nominal(value,types);
        if (!xr_xir_domain_work(domain,identity->field_count)) return XR_XIR_VALUE_LIMIT;
        for (uint32_t i=0;i<identity->field_count;++i) {
            bool allowed=false;XrXirValueStatus status=xr_xir_vm_debug_snapshot_member(view->snapshot,view->frame,value,i,&allowed);
            if (status!=XR_XIR_VALUE_OK) return status;
            if (allowed) ++shape.children;
        }
    }
    else if (xr_xir_type_is_nullable(types,(XrXirType)value->type)) {
        bool some=false;const XrXirValue *payload=NULL;
        if (!xr_xir_nullable_view(value,&some,&payload)) return XR_XIR_VALUE_BAD_ARGUMENT;
        shape.children=some ? 1 : 0;
    } else if (xr_xir_type_is_enum(types,(XrXirType)value->type)) {
        XrXirEnumBorrow borrow={0};XrXirValueStatus status=xr_xir_enum_borrow(value,&borrow);
        if (status!=XR_XIR_VALUE_OK) return status;shape=(XdapValueShape){borrow.field_count,true};
    }
    *output=shape;return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xdap_value_child(const XdapValueView *view,uint64_t index,
    XrXirValue *output,XdapValueChildInfo *info) {
    if (!view || !output || output->type || output->reserved || output->payload || !info) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirValue *value=view->value;XrXirDomain *domain=view->domain;
    XdapValueShape shape={0};XrXirValueStatus status=xdap_value_shape(view,&shape);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (index>=shape.children || index>UINT32_MAX) return XR_XIR_VALUE_BOUNDS;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(value));
    XdapValueChildInfo child={0};
    if (xr_xir_type_is_array(types,(XrXirType)value->type)) {
        XrXirValueAdmission admission=value_admission(value,domain);XrXirFaultDetail fault={0};
        status=xr_xir_array_get(value,(int64_t)index,&admission,output,&fault);
        child.selector=(XrXirValuePathStep){XR_XIR_PATH_INDEX,(XrXirType)value->type,(int64_t)index};child.selected=true;
    } else if (xr_xir_tuple_signature(types,(XrXirType)value->type)) status=xr_xir_tuple_get(value,(uint32_t)index,output);
    else if (value_nominal(value,types)) {
        const XrXirNominalIdentity *identity=value_nominal(value,types);uint64_t ordinal=0;uint32_t field=UINT32_MAX;
        if (!xr_xir_domain_work(domain,identity->field_count)) return XR_XIR_VALUE_LIMIT;
        for (uint32_t i=0;i<identity->field_count;++i) {
            bool allowed=false;status=xr_xir_vm_debug_snapshot_member(view->snapshot,view->frame,value,i,&allowed);
            if (status!=XR_XIR_VALUE_OK) return status;
            if (!allowed) continue;
            if (ordinal++==index) { field=i;break; }
        }
        if (field==UINT32_MAX) return XR_XIR_VALUE_BOUNDS;
        XrXirValueAdmission admission=value_admission(value,domain);
        status=xr_xir_type_is_class(types,(XrXirType)value->type) ? xr_xir_class_get(value,field,&admission,output) :
            xr_xir_struct_get(value,field,&admission,output);
        child.name=identity->fields[field].name.bytes;child.name_length=identity->fields[field].name.length;
        child.selector=(XrXirValuePathStep){XR_XIR_PATH_FIELD,(XrXirType)value->type,field};child.selected=true;
    } else if (xr_xir_type_is_nullable(types,(XrXirType)value->type)) {
        bool some=false;const XrXirValue *payload=NULL;
        if (!xr_xir_nullable_view(value,&some,&payload) || !some) return XR_XIR_VALUE_BAD_ARGUMENT;
        status=xr_xir_value_copy(payload,output);child.name="value";child.name_length=5;
    } else {
        XrXirEnumBorrow borrow={0};status=xr_xir_enum_borrow(value,&borrow);
        if (status==XR_XIR_VALUE_OK) status=xr_xir_value_copy(&borrow.fields[index],output);
    }
    if (status==XR_XIR_VALUE_OK) *info=child;
    return status;
}
