/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_hierarchy.inc.c - Public-prefix ancestry in one owned nominal pool
 *
 * KEY CONCEPT:
 *   A Base view preserves the actual object and all Derived field obligations.
 */
static XrXirStatus class_hierarchy_prefix(const XrXirNominalTable *table,
    uint32_t child,uint32_t parent,XrCompileRequestControl *control,uint32_t *steps) {
    XrXirNominalIdentity derived=nominal_identity_header(table,child);
    XrXirNominalIdentity base=nominal_identity_header(table,parent);
    if (derived.kind!=XR_XIR_NOMINAL_CLASS || base.kind!=XR_XIR_NOMINAL_CLASS ||
        derived.arity || base.arity || derived.native.native_id || base.native.native_id ||
        (base.flags&(XR_XIR_NOMINAL_FINAL|XR_XIR_NOMINAL_SENDABLE)) ||
        derived.field_count<base.field_count) return XR_XIR_BAD_TYPE;
    for(uint32_t f=0;f<base.field_count;++f) {
        XrXirStatus status=xir_receive_step(control,steps);if(status!=XR_XIR_OK)return status;
        XrXirNominalFieldIdentity a=nominal_identity_field(table,child,f);
        XrXirNominalFieldIdentity b=nominal_identity_field(table,parent,f);
        if (b.flags&(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED)) return XR_XIR_BAD_TYPE;
        if (a.flags!=b.flags || a.name.length!=b.name.length ||
            memcmp(a.name.bytes,b.name.bytes,a.name.length)) return XR_XIR_BAD_STRUCTURE;
        if(table->declarations && table->declarations[child].fields[f].type!=
            table->declarations[parent].fields[f].type) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static XrXirStatus class_hierarchy_instances(const XrXirTypes *types,uint32_t declaration,
    uint32_t parent,XrCompileRequestControl *control,uint32_t *steps) {
    for(uint32_t i=0;i<types->count;++i) {
        XrXirStatus status=xir_receive_step(control,steps);if(status!=XR_XIR_OK)return status;
        const XrXirTypeNode *child=&types->nodes[i];
        if(child->kind!=XR_XIR_TYPE_NOMINAL || child->nominal.declaration!=declaration)continue;
        bool found=false;
        for(uint32_t p=0;p<types->count;++p) {
            status=xir_receive_step(control,steps);if(status!=XR_XIR_OK)return status;
            const XrXirTypeNode *base=&types->nodes[p];
            if(base->kind!=XR_XIR_TYPE_NOMINAL || base->nominal.declaration!=parent)continue;
            if(child->nominal.argument_count || base->nominal.argument_count ||
                child->nominal.field_count<base->nominal.field_count)return XR_XIR_BAD_TYPE;
            for(uint32_t f=0;f<base->nominal.field_count;++f) {
                status=xir_receive_step(control,steps);if(status!=XR_XIR_OK)return status;
                if(child->nominal.fields[f]!=base->nominal.fields[f])return XR_XIR_BAD_TYPE;
            }
            found=true;
        }
        if(!found)return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_class_hierarchy_verify(const XrXirCompileContext *context,
    const XrXirTypes *types,XrCompileRequestControl *control) {
    if(!xir_compile_context_valid(context))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xir_receive_poll(control);if(status!=XR_XIR_OK)return status;
    const XrXirNominalTable *table=types?types->nominals:NULL;
    if(!table)return XR_XIR_OK;
    if((!!table->declarations==!!table->identities) || !table->count ||
        (types->count && !types->nodes))return XR_XIR_BAD_STRUCTURE;
    uint32_t steps=0;
    for(uint32_t d=0;d<table->count;++d) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        XrXirNominalIdentity child=nominal_identity_header(table,d);
        if(!child.parent)continue;
        if(child.parent>table->count || child.parent==d+1)return XR_XIR_BAD_STRUCTURE;
        status=class_hierarchy_prefix(table,d,child.parent-1,control,&steps);
        if(status!=XR_XIR_OK)return status;
        uint32_t parent=child.parent;
        for(uint32_t depth=0;parent;++depth) {
            status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            if(depth>=table->count || parent>table->count || parent==d+1)return XR_XIR_BAD_STRUCTURE;
            parent=nominal_identity_header(table,parent-1).parent;
        }
        status=class_hierarchy_instances(types,d,child.parent-1,control,&steps);
        if(status!=XR_XIR_OK)return status;
    }
    return XR_XIR_OK;
}
XR_FUNC bool xr_xir_class_is_subtype(const XrXirTypes *types,XrXirType actual,XrXirType expected) {
    const XrXirTypeNode *child=xr_xir_type_node(types,actual),*base=xr_xir_type_node(types,expected);
    if(!child || !base || !xr_xir_type_is_class(types,actual) || !xr_xir_type_is_class(types,expected))return false;
    if(actual==expected)return true;
    const XrXirNominalTable *table=types->nominals;
    if(child->nominal.argument_count || base->nominal.argument_count ||
        child->nominal.field_count<base->nominal.field_count)return false;
    for(uint32_t f=0;f<base->nominal.field_count;++f)
        if(child->nominal.fields[f]!=base->nominal.fields[f])return false;
    uint32_t declaration=child->nominal.declaration;
    for(uint32_t depth=0;depth<table->count;++depth) {
        if(declaration>=table->count)return false;
        uint32_t parent=nominal_identity_header(table,declaration).parent;
        if(!parent)return false;
        declaration=parent-1;
        if(declaration==base->nominal.declaration)return true;
    }
    return false;
}

XR_FUNC XrXirStatus xir_compile_class_common_base(const XrXirTypes *types,XrXirType left,
    XrXirType right,XrXirType *output,XrCompileRequestControl *control) {
    if(!output)return XR_XIR_BAD_STRUCTURE;
    *output=XR_XIR_UNIT;
    XrXirStatus status=xir_receive_poll(control);if(status!=XR_XIR_OK)return status;
    if(!xr_xir_type_is_class(types,left) || !xr_xir_type_is_class(types,right))return XR_XIR_BAD_TYPE;
    uint32_t steps=0;XrXirType candidate=left;
    for(uint32_t depth=0;depth<types->nominals->count;++depth) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        if(xr_xir_class_is_subtype(types,right,candidate)) {*output=candidate;return XR_XIR_OK;}
        const XrXirTypeNode *node=xr_xir_type_node(types,candidate);
        uint32_t parent=nominal_identity_header(types->nominals,node->nominal.declaration).parent;
        if(!parent || parent>types->nominals->count)return XR_XIR_BAD_TYPE;
        candidate=XR_XIR_UNIT;
        for(uint32_t t=0;t<types->count;++t) {
            status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            const XrXirTypeNode *base=&types->nodes[t];
            if(base->kind!=XR_XIR_TYPE_NOMINAL || base->nominal.declaration!=parent-1 ||
                base->nominal.argument_count || base->parameter_span)continue;
            if(candidate!=XR_XIR_UNIT)return XR_XIR_BAD_STRUCTURE;
            candidate=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        }
        if(candidate==XR_XIR_UNIT)return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_BAD_STRUCTURE;
}
