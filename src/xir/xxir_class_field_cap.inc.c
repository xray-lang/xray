/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_field_cap.inc.c - Bounded closed class-field storage capability
 *
 * KEY CONCEPT:
 *   Identity handles pin their field storage, type arena and physical accounting domain.
 */
typedef struct ClassFieldFrame { uint32_t index, next, count; bool declaration; } ClassFieldFrame;
static XrXirStatus class_field_leaf(const XrXirTypes *types, XrXirType type, bool *leaf) {
    *leaf=true;
    if (type==XR_XIR_BOOL || xr_xir_type_is_number(type) || type==XR_XIR_STRING) return XR_XIR_OK;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if (!node || node->parameter_span) return XR_XIR_BAD_TYPE;
    if (node->kind==XR_XIR_TYPE_NULLABLE) { *leaf=false; return XR_XIR_OK; }
    /* An Array field is a handle whose backing store is its own admitted array; any element the
     * array family admits (scalars, strings, nominal values, nullable values, arrays) is stored there. */
    if (node->kind==XR_XIR_TYPE_ARRAY) {
        if (node->element==XR_XIR_BOOL || xr_xir_type_is_number(node->element) || node->element==XR_XIR_STRING) return XR_XIR_OK;
        const XrXirTypeNode *element=xr_xir_type_node(types,node->element);
        return element && !element->parameter_span && (element->kind==XR_XIR_TYPE_NOMINAL ||
            element->kind==XR_XIR_TYPE_NULLABLE || element->kind==XR_XIR_TYPE_ARRAY) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    }
    if (node->kind!=XR_XIR_TYPE_NOMINAL || !types->nominals ||
        (types->nominals->declarations!=NULL)==(types->nominals->identities!=NULL) ||
        node->nominal.declaration>=types->nominals->count) return XR_XIR_BAD_TYPE;
    uint32_t kind=types->nominals->declarations ? types->nominals->declarations[node->nominal.declaration].kind :
        types->nominals->identities[node->nominal.declaration].kind;
    /* A class field is an identity handle, never an inline body, so it ends the traversal. */
    if (kind==XR_XIR_NOMINAL_CLASS) return XR_XIR_OK;
    if (kind!=XR_XIR_NOMINAL_STRUCT && kind!=XR_XIR_NOMINAL_ENUM) return XR_XIR_BAD_TYPE;
    *leaf=false;return XR_XIR_OK;
}
static XrXirStatus class_field_frame(const XrXirTypes *types, uint32_t index, ClassFieldFrame *frame) {
    const XrXirTypeNode *node=&types->nodes[index];
    if (node->kind==XR_XIR_TYPE_NULLABLE) {
        *frame=(ClassFieldFrame){index,0,1,false}; return XR_XIR_OK;
    }
    uint32_t count=types->nominals->declarations ?
        types->nominals->declarations[node->nominal.declaration].field_count :
        types->nominals->identities[node->nominal.declaration].field_count;
    bool declaration=false;
    if (node->nominal.field_count!=count) {
        if (node->nominal.field_count || !types->nominals->declarations ||
            types->nominals->declarations[node->nominal.declaration].parameter_count) return XR_XIR_BAD_TYPE;
        declaration=true;
    }
    if (count && (declaration ? !types->nominals->declarations[node->nominal.declaration].fields :
        !node->nominal.fields)) return XR_XIR_BAD_STRUCTURE;
    *frame=(ClassFieldFrame){index,0,count,declaration};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_class_field_verify(const XrXirCompileContext *compile_context, const XrXirTypes *types, XrXirType type) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(budget, 1)) return XR_XIR_BUDGET;
    bool leaf=false;XrXirStatus status=class_field_leaf(types,type,&leaf);
    if (status!=XR_XIR_OK || leaf) return status;
    uint32_t count=types->count;
    uint64_t bytes=(uint64_t)count*(sizeof(ClassFieldFrame)+sizeof(unsigned char));
    if (bytes>SIZE_MAX ||(bytes > SIZE_MAX)) return XR_XIR_BUDGET;

    ClassFieldFrame *frames=xir_compile_calloc(compile_context, 1,(size_t)bytes, &allocation_status);
    if (!frames) {return allocation_status;}
    unsigned char *states=(unsigned char *)(frames+count);uint32_t depth=1;
    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    status=class_field_frame(types,index,&frames[0]);states[index]=1;
    while(status==XR_XIR_OK && depth) {
        if (!xir_compile_work(budget, 1)) {status=XR_XIR_BUDGET;break;}
        ClassFieldFrame *frame=&frames[depth-1];
        if (frame->next==frame->count) {states[frame->index]=2;--depth;continue;}
        const XrXirTypeNode *node=&types->nodes[frame->index];uint32_t field=frame->next++;
        XrXirType child=node->kind==XR_XIR_TYPE_NULLABLE ? node->element : frame->declaration ?
            types->nominals->declarations[node->nominal.declaration].fields[field].type : node->nominal.fields[field];
        status=class_field_leaf(types,child,&leaf);
        if (status!=XR_XIR_OK || leaf) continue;
        index=(uint32_t)child-XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (states[index]==1) {status=XR_XIR_BAD_TYPE;break;}
        if (states[index]==2) continue;
        if (depth==count) {status=XR_XIR_BAD_TYPE;break;}
        status=class_field_frame(types,index,&frames[depth]);
        if (status==XR_XIR_OK) {states[index]=1;++depth;}
    }
    xr_compile_resources_free(frames);return status;
}
