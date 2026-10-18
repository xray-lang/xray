/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_type.c - Bounded static type displays from retained verified metadata
 */
#include "xdap_value.h"
#include "../../shared/xr_type_names_core.h"
#include "../../xir/xxir_type_arena.h"
#include "../../xir/xxir_types.h"
#include "../../xir/xxir_value_internal.h"
#include <string.h>
typedef struct TypeDisplay {
    const XdapValueView *view;
    const XrXirTypes *types;
    XdapTypeText text;
    XrXirValueStatus status;
} TypeDisplay;
static const char *type_scalar(XrXirType type) {
    switch (type) {
    case XR_XIR_UNIT:return TYPE_NAME_UNIT;case XR_XIR_BOOL:return TYPE_NAME_BOOL;case XR_XIR_STRING:return TYPE_NAME_STRING;
    case XR_XIR_I8:return TYPE_NAME_I8;case XR_XIR_I16:return TYPE_NAME_I16;case XR_XIR_I32:return TYPE_NAME_I32;case XR_XIR_I64:return TYPE_NAME_I64;
    case XR_XIR_U8:return TYPE_NAME_U8;case XR_XIR_U16:return TYPE_NAME_U16;case XR_XIR_U32:return TYPE_NAME_U32;case XR_XIR_U64:return TYPE_NAME_U64;
    case XR_XIR_F32:return TYPE_NAME_F32;case XR_XIR_F64:return TYPE_NAME_F64;case XR_XIR_RUNE:return TYPE_NAME_RUNE;
    case XR_XIR_ERROR:return "Error";case XR_XIR_PANIC_INFO:return TYPE_NAME_PANIC_INFO;default:return NULL;
    }
}
static bool type_append(TypeDisplay *display,const char *bytes,size_t length) {
    if (display->status!=XR_XIR_VALUE_OK) return false;
    if (length>XDAP_TYPE_TEXT_BYTES-display->text.length || !xr_xir_domain_work(display->view->domain,length)) {
        display->status=XR_XIR_VALUE_LIMIT;return false;
    }
    memcpy(display->text.bytes+display->text.length,bytes,length);display->text.length+=length;return true;
}
static bool type_literal(TypeDisplay *display,const char *bytes) { return type_append(display,bytes,strlen(bytes)); }
static bool type_collect(TypeDisplay *display,XrXirType type,uint32_t depth) {
    if (display->status!=XR_XIR_VALUE_OK) return false;
    if (depth>32 || !xr_xir_domain_work(display->view->domain,1)) { display->status=XR_XIR_VALUE_LIMIT;return false; }
    const char *scalar=type_scalar(type);if (scalar) return type_literal(display,scalar);
    const XrXirTypeNode *node=xr_xir_type_node(display->types,type);
    if (!node || node->parameter_span) { display->status=XR_XIR_VALUE_BAD_ARGUMENT;return false; }
    const char *wrapper=NULL;
    switch (node->kind) {
    case XR_XIR_TYPE_ARRAY:wrapper=TYPE_NAME_ARRAY;break;
    case XR_XIR_TYPE_ATOMIC:wrapper=TYPE_NAME_ATOMIC;break;
    case XR_XIR_TYPE_TASK:wrapper=TYPE_NAME_TASK;break;
    case XR_XIR_TYPE_CELL:wrapper="Cell";break;
    case XR_XIR_TYPE_NULLABLE:return type_collect(display,node->element,depth+1) && type_literal(display,"?");
    case XR_XIR_TYPE_CALLABLE:return type_literal(display,TYPE_NAME_FUNCTION);
    case XR_XIR_TYPE_TUPLE:
        if (!type_literal(display,"(")) return false;
        for (uint32_t i=0;i<node->parameter_count;++i) {
            if (i && !type_literal(display,", ")) return false;
            if (!type_collect(display,node->parameters[i].type,depth+1)) return false;
        }
        return (node->parameter_count!=1 || type_literal(display,",")) && type_literal(display,")");
    case XR_XIR_TYPE_NOMINAL: {
        bool visible=false;display->status=xr_xir_vm_debug_snapshot_type_visible(display->view->snapshot,
            display->view->frame,display->view->value,type,&visible);
        if (display->status!=XR_XIR_VALUE_OK) return false;
        if (!visible) return type_literal(display,"<private type>");
        const XrXirNominalIdentity *identity=&display->types->nominals->identities[node->nominal.declaration];
        if (!type_append(display,identity->name.bytes,identity->name.length)) return false;
        if (!node->nominal.argument_count) return true;
        if (!type_literal(display,"<")) return false;
        for (uint32_t i=0;i<node->nominal.argument_count;++i) {
            if (i && !type_literal(display,", ")) return false;
            if (!type_collect(display,node->nominal.arguments[i],depth+1)) return false;
        }
        return type_literal(display,">");
    }
    default:display->status=XR_XIR_VALUE_UNSUPPORTED;return false;
    }
    return type_literal(display,wrapper) && type_literal(display,"<") &&
        type_collect(display,node->element,depth+1) && type_literal(display,">");
}
XR_FUNC XrXirValueStatus xdap_value_type_text(const XdapValueView *view,XdapTypeText *output) {
    if (!view || !output || !xr_xir_domain_current(view->domain) || !xr_xir_value_valid(view->value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(view->domain,sizeof(TypeDisplay)+sizeof(*output))) return XR_XIR_VALUE_LIMIT;
    TypeDisplay display={.view=view,.types=xr_xir_compile_type_arena_types(xr_xir_value_arena(view->value)),.status=XR_XIR_VALUE_OK};
    if (!type_collect(&display,(XrXirType)view->value->type,0)) return display.status;
    *output=display.text;return XR_XIR_VALUE_OK;
}
