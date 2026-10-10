/* Private shared Catalog/Source admission subset; common Checked verification runs first. */
#ifndef XXIR_LIBRARY_IMPORT_SHAPE_INC
#define XXIR_LIBRARY_IMPORT_SHAPE_INC
#include "xxir_nominal.h"
#include "xxir_interface.h"
#include "xxir_types.h"
#include "xxir_construction.h"
#include "xxir_library_governed.inc.c"
/* This is an import subset, not a replacement for Checked verification.
 * Member and witness metadata below enter the same common verifiers. Complex
 * body operations and native state remain closed. Nominal metadata retains its kind. */
/* These operations carry no artifact-global ordinal outside their type.
 * Values, blocks, field ordinals and generic argument slices remain local to
 * the copied body. Calls, references, literals and requirements map separately. */
static bool library_catalog_local_instruction(XrXirOp op) {
    switch (op) {
    case XR_XIR_ATOMIC_NEW:
    case XR_XIR_ATOMIC_LOAD:
    case XR_XIR_ATOMIC_STORE:
    case XR_XIR_ATOMIC_FETCH_ADD:
    /* Suspension has only local body state; common verification retains
     * control obligations and the copied function preserves its effects. */
    case XR_XIR_SUSPEND:
    case XR_XIR_TASK_AWAIT:
    case XR_XIR_INVOKE_RESULT:
    case XR_XIR_INVOKE_ERROR:
    case XR_XIR_ERROR_ERASE:
    case XR_XIR_THROW:
    case XR_XIR_RETURN:
    case XR_XIR_CONST_INT:
    case XR_XIR_ADD_INT:
    case XR_XIR_CONST_BOOL:
    case XR_XIR_CONCAT_STRING:
    case XR_XIR_WRITE_STREAM:
    case XR_XIR_ENUM_NEW:
    case XR_XIR_ENUM_TAG:
    case XR_XIR_ENUM_GET:
    case XR_XIR_MATCH_FAIL:
    case XR_XIR_CLASS_NEW:
    case XR_XIR_CLASS_GET:
    case XR_XIR_CLASS_SET:
    case XR_XIR_STRUCT_NEW:
    case XR_XIR_STRUCT_GET:
    case XR_XIR_STRUCT_SET:
    case XR_XIR_COPY:
    case XR_XIR_LOCAL_NEW:
    case XR_XIR_LOCAL_READ:
    case XR_XIR_LOCAL_WRITE:
    case XR_XIR_LOCAL_UNINIT:
    case XR_XIR_CELL_NEW:
    case XR_XIR_CELL_READ:
    case XR_XIR_CELL_WRITE:
    case XR_XIR_CELL_LOCAL_WRITE:
    case XR_XIR_CELL_PLACE:
    case XR_XIR_CELL_PROJECT:
    case XR_XIR_FIELD_PLACE:
    case XR_XIR_INDEX_PLACE:
    case XR_XIR_PLACE_READ:
    case XR_XIR_PLACE_WRITE:
    case XR_XIR_OBJECT_PLACE:
    case XR_XIR_ARRAY_NEW:
    case XR_XIR_ARRAY_GET:
    case XR_XIR_ARRAY_SET:
    case XR_XIR_ARRAY_PUSH:
    case XR_XIR_ARRAY_LEN:
    case XR_XIR_ARRAY_REPEAT:
    case XR_XIR_ARRAY_CAPACITY:
    case XR_XIR_ARRAY_WITH_CAPACITY:
    case XR_XIR_ARRAY_RESERVE:
    case XR_XIR_TUPLE_NEW:
    case XR_XIR_TUPLE_FIELD:
    case XR_XIR_NULLABLE_NONE:
    case XR_XIR_NULLABLE_SOME:
    case XR_XIR_NULLABLE_IS_SOME:
    case XR_XIR_NULLABLE_UNWRAP:
    case XR_XIR_CALL_INDIRECT:
    case XR_XIR_FUNCTION_WEAKEN:
    case XR_XIR_JUMP:
    case XR_XIR_BRANCH:
    case XR_XIR_PHI:
    case XR_XIR_EQ_INT:
    case XR_XIR_NE_INT:
    case XR_XIR_LT_INT:
    case XR_XIR_LE_INT:
    case XR_XIR_GT_INT:
    case XR_XIR_GE_INT:
    case XR_XIR_SUB_INT:
    case XR_XIR_MUL_INT:
    case XR_XIR_DIV_INT:
    case XR_XIR_REM_INT:
    case XR_XIR_STRING_LEN:
    case XR_XIR_EQ_STRING:
    case XR_XIR_NE_STRING:
    case XR_XIR_EQUAL:
        return true;
    default: return false;
    }
}
static bool library_catalog_builtin(XrXirType type) {
    return type == XR_XIR_I64 || type == XR_XIR_BOOL || type == XR_XIR_STRING;
}
/* Common verification validates access, ownership and every recursive type first.
 * Import admits only the ordinary value families whose full type and body
 * closure below is relocated. Atomic and Task state admit I64 handles only;
 * direct Cell state stays closed. Task boundaries retain their common proof. */
static bool library_catalog_state_type(const XrXirTypes *types,XrXirType type) {
    if (xr_xir_type_is_cell(types,type)) type=xr_xir_cell_element(types,type);
    if (type==XR_XIR_UNIT || library_catalog_builtin(type)) return true;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if (!node) return false;
    switch (node->kind) {
    case XR_XIR_TYPE_ATOMIC:
    case XR_XIR_TYPE_TASK:return node->element==XR_XIR_I64;
    case XR_XIR_TYPE_NOMINAL:
    case XR_XIR_TYPE_CALLABLE:
    case XR_XIR_TYPE_ARRAY:
    case XR_XIR_TYPE_TUPLE:
    case XR_XIR_TYPE_NULLABLE:
        return true;
    default:return false;
    }
}
static XrXirStatus library_catalog_closed_types(const XrXirModule *m,
    const XrXirCompileContext *context) {
    const XrXirTypes *types = m->types;
    if (!types) return XR_XIR_OK;
    const XrXirNominalTable *nominals = types->nominals;
    if (nominals && nominals->count && !nominals->declarations) return XR_XIR_BAD_STAGE;
    for (uint32_t d=0; nominals && d<nominals->count; ++d) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirNominalDeclaration *record=&nominals->declarations[d];
        if ((record->kind!=XR_XIR_NOMINAL_STRUCT && record->kind!=XR_XIR_NOMINAL_ENUM &&
             record->kind!=XR_XIR_NOMINAL_CLASS) ||
            (record->native.native_id && record->native.native_id!=XR_NATIVE_DECLARATION_ORDERING))
            return XR_XIR_BAD_STAGE;
        for (uint32_t f=0; f<record->field_count; ++f) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            uint32_t parameter=(uint32_t)record->fields[f].type-XR_XIR_TYPE_PARAMETER_BASE;
            if (!library_catalog_builtin(record->fields[f].type) &&
                !xr_xir_type_node(types,record->fields[f].type) &&
                !((uint32_t)record->fields[f].type>=XR_XIR_TYPE_PARAMETER_BASE &&
                  (uint32_t)record->fields[f].type<XR_XIR_TYPE_PARAMETER_LIMIT &&
                  parameter<record->parameter_count)) return XR_XIR_BAD_STAGE;
        }
    }
    const XrXirInterfaceTable *interfaces=types->interfaces;
    if (interfaces && interfaces->count && !interfaces->declarations) return XR_XIR_BAD_STAGE;
    for (uint32_t d=0; interfaces && d<interfaces->count; ++d) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirInterfaceDeclaration *record=&interfaces->declarations[d];
        /* Parent applications and both binder scopes are deeply remapped. */
        for (uint32_t member=0; member<record->method_count; ++member) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            if (!xr_xir_callable_signature(types,record->methods[member].signature)) return XR_XIR_BAD_STAGE;
        }
    }
    for (uint32_t t=0; t<types->count; ++t) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirTypeNode *node=&types->nodes[t];
        if (node->kind!=XR_XIR_TYPE_NOMINAL && node->kind!=XR_XIR_TYPE_CALLABLE &&
            node->kind!=XR_XIR_TYPE_ARRAY && node->kind!=XR_XIR_TYPE_TUPLE &&
            node->kind!=XR_XIR_TYPE_NULLABLE && node->kind!=XR_XIR_TYPE_CELL &&
            !((node->kind==XR_XIR_TYPE_ATOMIC || node->kind==XR_XIR_TYPE_TASK) &&
              node->element==XR_XIR_I64))
            return XR_XIR_BAD_STAGE;
    }
    return XR_XIR_OK;
}
static bool library_catalog_type(const XrXirModule *module, const XrXirGeneric *generic, XrXirType type) {
    if (type == XR_XIR_I64 || type == XR_XIR_STRING || type == XR_XIR_BOOL) return true;
    if (xr_xir_type_node(module->types,type)) return true;
    uint32_t ordinal = (uint32_t)type - XR_XIR_TYPE_PARAMETER_BASE;
    return generic && (uint32_t)type >= XR_XIR_TYPE_PARAMETER_BASE &&
        (uint32_t)type < XR_XIR_TYPE_PARAMETER_LIMIT && ordinal < generic->parameter_count;
}
static XrXirStatus library_catalog_generic(const XrXirModule *module, const XrXirGeneric *generic,
    const XrXirCompileContext *context) {
    if (!generic) return XR_XIR_OK;
    for (uint32_t p = 0; p < generic->parameter_count; ++p) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirConstraint *constraint = &generic->constraints[p];
        if (xr_xir_binder_kind(generic,p) != XR_XIR_BINDER_TYPE ||
            (constraint->markers & ~XR_XIR_CONSTRAINT_MASK)) return XR_XIR_BAD_STAGE;
    }
    for (uint32_t a = 0; a < generic->argument_count; ++a) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (!library_catalog_type(module,generic,generic->arguments[a])) return XR_XIR_BAD_STAGE;
    }
    return XR_XIR_OK;
}
static XrXirStatus library_catalog_shape(const XrXirModule *m,
    const XrXirConstruction *construction,const XrXirCompileContext *context) {
    const XrXirDeclarations *d=m->declarations;
    uint32_t nominal_count=m->types && m->types->nominals ? m->types->nominals->count : 0;
    if (!construction || xr_xir_compile_construction_count(construction)!=nominal_count) return XR_XIR_BAD_STRUCTURE;
    if(m->linkage_kind!=XR_XIR_LIBRARY || m->stage!=XR_XIR_CHECKED) return XR_XIR_BAD_STAGE;
    if(!d || !d->module_count || !d->modules) return XR_XIR_BAD_STAGE;
    /* State remains lexically private. The shared checker has verified each
     * initializer and access; import bounds the owned relocation surface. */
    for (uint32_t s=0; s<d->slot_count; ++s) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirSlot *slot=&d->slots[s];
        if (slot->module>=d->module_count || slot->mutable>1 ||
            !library_catalog_state_type(m->types,slot->type))
            return XR_XIR_BAD_STAGE;
    }
    XrXirStatus typed_status = library_catalog_closed_types(m,context);
    if (typed_status != XR_XIR_OK) return typed_status;
    /* The reader has completely verified this evidence. Instances cannot
     * authorize declaration import, even when their body is scalar. */
    if (m->provenance) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (m->provenance->kind != XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_BAD_STAGE;
    }
    for(uint32_t f=0;f<m->function_count;++f){
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirFunction *fn=&m->functions[f];
        const XrXirGeneric *generic = m->generics ? &m->generics[f] : NULL;
        XrXirStatus status = library_catalog_generic(m,generic,context);
        if (status != XR_XIR_OK) return status;
        if((fn->result!=XR_XIR_UNIT&&!library_catalog_type(m,generic,fn->result)) ||
            d->functions[f].cleanup_owner ||
            (!d->functions[f].nominal_owner && d->functions[f].method_kind!=XR_XIR_NON_MEMBER) ||
            (d->functions[f].nominal_owner && d->functions[f].method_kind!=XR_XIR_CONSTRUCTOR &&
             d->functions[f].method_kind!=XR_XIR_MEMBER_HELPER &&
             d->functions[f].method_kind!=XR_XIR_READ_METHOD &&
             d->functions[f].method_kind!=XR_XIR_STATIC_METHOD))
            return XR_XIR_BAD_STAGE;
        if (d->functions[f].method_kind==XR_XIR_MEMBER_HELPER) {
            bool bound=false;
            uint32_t owner=d->functions[f].nominal_owner-1;
            const XrXirNominalDeclaration *record=&m->types->nominals->declarations[owner];
            const XrXirConstructionRow *row=xr_xir_compile_construction_row(construction,owner);
            if (!row || row->field_count!=record->field_count || (row->field_count && !row->field_initializers))
                return XR_XIR_BAD_STRUCTURE;
            for (uint32_t field=0; field<record->field_count; ++field) {
                if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
                bound |= row->field_initializers[field]==f+1;
            }
            for (uint32_t a=0; m->defaults && a<m->defaults->count; ++a) {
                if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
                bound |= m->defaults->records[a].function==f;
            }
            if (!bound) return XR_XIR_BAD_STAGE;
        }
        for (uint32_t p = 0; p < fn->parameter_count; ++p) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            if (!library_catalog_type(m,generic,fn->parameters[p]))
                return XR_XIR_BAD_STAGE;
        }
        for(uint32_t i=0;i<fn->instruction_count;++i){
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op=&fn->instructions[i];
            if (!library_catalog_local_instruction(op->op) &&
                op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_GO &&
                op->op!=XR_XIR_FUNCTION_REF &&
                op->op!=XR_XIR_CALL_REQUIREMENT && op->op!=XR_XIR_CALL_DEFAULT &&
                op->op!=XR_XIR_CONST_STRING && op->op!=XR_XIR_SLOT_LOAD &&
                op->op!=XR_XIR_SLOT_INIT && op->op!=XR_XIR_SLOT_STORE &&
                op->op!=XR_XIR_SLOT_PLACE && op->op!=XR_XIR_SLOT_GROUP_INIT)
                return XR_XIR_BAD_STAGE;
        }
    }
    return XR_XIR_OK;
}

#endif /* XXIR_LIBRARY_IMPORT_SHAPE_INC */
