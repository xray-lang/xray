/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal.inc.c - Declaration scope validation and owned metadata cloning
 *
 * KEY CONCEPT:
 *   Every failed construction releases its initialized ownership prefix.
 */
#include "xxir_lower_control.h"
static XrXirStatus nominal_control_poll(XrCompileRequestControl *control) {
    return control ? xir_compile_resource_status(xr_compile_request_control_poll(control)) : XR_XIR_OK;
}
static XrXirStatus nominal_name(XrXirLiteral name, XrCompileRequestControl *control) {
    if (!name.bytes || !name.length) return XR_XIR_BAD_STRUCTURE;
    if (!(((uint64_t) name.length + 1) <= SIZE_MAX)) return XR_XIR_BUDGET;
    if (!control) return memchr(name.bytes, 0, name.length) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
    for (size_t offset=0;offset<name.length;) {
        XrXirStatus status=nominal_control_poll(control);
        if (status!=XR_XIR_OK) return status;
        size_t count=name.length-offset;if (count>4096) count=4096;
        if (memchr(name.bytes+offset,0,count)) return XR_XIR_BAD_STRUCTURE;
        offset+=count;
    }
    return nominal_control_poll(control);
}
static XrXirStatus nominal_same_name(XrXirLiteral a, XrXirLiteral b, bool *same) {
    *same = false;

    *same = a.length == b.length && memcmp(a.bytes, b.bytes, a.length) == 0;
    return XR_XIR_OK;
}
static XrXirStatus nominal_variants(uint32_t kind, const XrXirNominalVariant *variants,
    uint32_t count, uint32_t fields, XrCompileRequestControl *control) {
    if (kind > XR_XIR_NOMINAL_CLASS || (count != 0) != (variants != NULL) ||
        (kind == XR_XIR_NOMINAL_ENUM) != (count != 0)) return XR_XIR_BAD_STRUCTURE;
    if (!(((uint64_t) count * sizeof(*variants)) <= SIZE_MAX)) return XR_XIR_BUDGET;
    uint32_t end = 0;
    for (uint32_t i = 0; i < count; ++i) {
        XrXirStatus polled=nominal_control_poll(control);if (polled!=XR_XIR_OK) return polled;
        const XrXirNominalVariant *v = &variants[i];
        if (v->field_begin != end || v->field_count > fields - end) return XR_XIR_BAD_STRUCTURE;
        end += v->field_count;
        XrXirStatus status = nominal_name(v->name,control);
        if (status != XR_XIR_OK) return status;
        for (uint32_t j = 0; j < i; ++j) {
            status=nominal_control_poll(control);if (status!=XR_XIR_OK) return status;
            XrXirLiteral prior=variants[j].name;
            bool same=v->name.length==prior.length;
            for (size_t offset=0;same && offset<v->name.length;) {
                status=nominal_control_poll(control);if (status!=XR_XIR_OK) return status;
                size_t chunk=v->name.length-offset;
                if (control && chunk>4096) chunk=4096;
                same=!memcmp(v->name.bytes+offset,prior.bytes+offset,chunk);offset+=chunk;
            }
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return kind == XR_XIR_NOMINAL_ENUM && end != fields ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
static bool nominal_flags_valid(uint32_t kind, uint32_t flags) {
    if (kind == XR_XIR_NOMINAL_CLASS)
        return !(flags & ~(XR_XIR_NOMINAL_FINAL | XR_XIR_NOMINAL_SENDABLE));
    if (kind == XR_XIR_NOMINAL_STRUCT) return flags == XR_XIR_NOMINAL_MODULE_LITERAL || flags == XR_XIR_NOMINAL_RECORD || flags == 0;
    return flags == 0;
}
static XrXirStatus nominal_field_begin(uint32_t kind, const XrXirNominalVariant *variants,
    uint32_t count, uint32_t field, uint32_t *begin) {
    *begin = 0;
    if (kind == XR_XIR_NOMINAL_STRUCT || kind == XR_XIR_NOMINAL_CLASS) return XR_XIR_OK;
    for (uint32_t i = 0; i < count; ++i) {

        if (field >= variants[i].field_begin && field - variants[i].field_begin < variants[i].field_count) {
            *begin = variants[i].field_begin; return XR_XIR_OK;
        }
    }
    return XR_XIR_BAD_STRUCTURE;
}
static void nominal_variants_free(const XrXirNominalVariant *variants, uint32_t count) {
    for (uint32_t i = 0; variants && i < count; ++i) xr_compile_resources_free((void *) variants[i].name.bytes);
    xr_compile_resources_free((void *) variants);
}
static bool nominal_field_type(const XrXirTypes *types, XrXirType type, uint32_t parameters, bool unit_payload) {
    if (type==XR_XIR_UNIT) return unit_payload;
    if (type == XR_XIR_BOOL || type == XR_XIR_RUNE || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
        type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO) return true;
    uint32_t id = (uint32_t) type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE < parameters;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CALLABLE ||
        node->kind == XR_XIR_TYPE_NOMINAL || node->kind == XR_XIR_TYPE_NULLABLE || node->kind == XR_XIR_TYPE_TUPLE ||
        node->kind == XR_XIR_TYPE_ATOMIC || node->kind == XR_XIR_TYPE_MAP || node->kind == XR_XIR_TYPE_SET || node->kind == XR_XIR_TYPE_TASK || node->kind == XR_XIR_TYPE_ITERATOR || node->kind == XR_XIR_TYPE_CHANNEL) &&
        node->parameter_span <= parameters;
}
static int nominal_record_order(XrXirLiteral a,XrXirLiteral b) {
    uint32_t n=a.length<b.length?a.length:b.length;
    int order=memcmp(a.bytes,b.bytes,n);
    return order?order:a.length<b.length?-1:a.length>b.length?1:0;
}
static XrXirStatus nominal_declaration(const XrXirNominalDeclaration *d,
    const XrXirTypes *types, XrXirCompileContext *b) {
    if (d->exported > 1 || (d->parameter_count != 0) != (d->constraints != NULL) ||
        (d->field_count != 0) != (d->fields != NULL)) return XR_XIR_BAD_STRUCTURE;
    if (d->parameter_count > 65536 || d->parameter_count > b->limits.parameters) return XR_XIR_BUDGET;
    uint64_t bytes = (uint64_t) d->parameter_count * sizeof(*d->constraints) +
        (uint64_t) d->field_count * sizeof(*d->fields);
    if (!((bytes) <= SIZE_MAX)) return XR_XIR_BUDGET;
    b->limits.parameters -= d->parameter_count;
    XrXirStatus status = nominal_name(d->module,NULL);
    if (status == XR_XIR_OK) status = nominal_name(d->name,NULL);
    if (status != XR_XIR_OK) return status;
    if (!nominal_flags_valid(d->kind,d->flags)) return XR_XIR_BAD_STRUCTURE;
    if (d->flags & XR_XIR_NOMINAL_RECORD) {
        if (d->kind!=XR_XIR_NOMINAL_STRUCT || d->exported!=1 || d->parameter_count!=d->field_count ||
            d->native.native_id || d->variant_count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t f=0;f<d->field_count;++f) {
            const XrXirConstraint *constraint=&d->constraints[f];
            if (constraint->markers || constraint->interfaces || constraint->interface_count ||
                d->fields[f].type!=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+f) ||
                d->fields[f].flags!=XR_XIR_FIELD_MUTABLE) return XR_XIR_BAD_TYPE;
        }
    }
    XrXirNominalIdentity native_identity = {d->module,d->name,d->exported,d->parameter_count,
        NULL,d->field_count,d->kind,d->variants,d->variant_count,d->flags,d->native};
    status = nominal_native_verify( &native_identity);
    if (status != XR_XIR_OK) return status;
    if (d->native.native_id==XR_NATIVE_DECLARATION_RECV) {
        if (d->fields[0].type!=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE || d->constraints[0].markers ||
            d->constraints[0].interface_count) return XR_XIR_BAD_TYPE;
        status=nominal_native_name(d->fields[0].name,"value");
        if (status!=XR_XIR_OK) return status;
    }
    if (d->native.native_id==XR_NATIVE_DECLARATION_TASK_RESULT) {
        if (d->fields[0].type!=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE ||
            d->fields[1].type!=XR_XIR_ERROR || d->fields[2].type!=XR_XIR_PANIC_INFO ||
            d->constraints[0].markers || d->constraints[0].interface_count ||
            d->constraints[1].markers!=XR_XIR_CONSTRAINT_ERROR || d->constraints[1].interface_count)
            return XR_XIR_BAD_TYPE;
        const char *names[]={"value","error","info"};
        for (uint32_t f=0;f<3;++f) {
            status=nominal_native_name(d->fields[f].name,names[f]);
            if (status!=XR_XIR_OK) return status;
        }
    }
    status = nominal_variants(d->kind, d->variants, d->variant_count, d->field_count,NULL);
    if (status != XR_XIR_OK) return status;
    for (uint32_t p = 0; p < d->parameter_count; ++p) {
        status = xr_xir_compile_constraint_structure(b, types, d->constraints[p], d->parameter_count);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t f = 0; f < d->field_count; ++f) {
        const XrXirNominalField *field = &d->fields[f];
        if (d->kind == XR_XIR_NOMINAL_CLASS && (d->flags & XR_XIR_NOMINAL_SENDABLE) &&
            (field->flags & XR_XIR_FIELD_MUTABLE))
            return XR_XIR_BAD_TYPE;
        if (d->kind == XR_XIR_NOMINAL_ENUM && field->flags) return XR_XIR_BAD_STRUCTURE;
        if (d->kind == XR_XIR_NOMINAL_CLASS && !xr_xir_type_span(types,field->type)) {
            status=xr_xir_compile_class_field_verify(b, types, field->type);
            if (status!=XR_XIR_OK) return status;
        }
        uint32_t visibility = field->flags & (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED);
        if ((field->flags & ~(XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED | XR_XIR_FIELD_MUTABLE)) ||
            visibility == (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED)) return XR_XIR_BAD_STRUCTURE;
        status = nominal_name(field->name,NULL);
        if (status != XR_XIR_OK) return status;
        if ((d->flags&XR_XIR_NOMINAL_RECORD) && f && nominal_record_order(d->fields[f-1].name,field->name)>=0) return XR_XIR_BAD_STRUCTURE;
        if (!nominal_field_type(types, field->type, d->parameter_count,d->native.native_id==XR_NATIVE_DECLARATION_RECV && !f)) return XR_XIR_BAD_TYPE;
        status = xr_xir_compile_type_expression_shape(b, types, field->type, d->parameter_count);
        if (status != XR_XIR_OK) return status;
        uint32_t begin = 0;
        status = nominal_field_begin(d->kind, d->variants, d->variant_count, f, &begin);
        if (status != XR_XIR_OK) return status;
        for (uint32_t j = begin; j < f; ++j) {
            bool same = false;
            status = nominal_same_name(field->name, d->fields[j].name, &same);
            if (status != XR_XIR_OK) return status;
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus nominal_table_verify(const XrXirNominalTable *table,
    const XrXirTypes *types, XrXirCompileContext *budget) {
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    if (!table) return XR_XIR_OK;
    if (!table->count || (!!table->declarations == !!table->identities)) return XR_XIR_BAD_STRUCTURE;
    if (table->identities) return nominal_identities_verify(table, budget);
    XrXirCompileContext remaining = *budget;
    XrXirStatus status = XR_XIR_OK;
    if (!((sizeof(*table) +
        (uint64_t) table->count * sizeof(*table->declarations)) <= SIZE_MAX)) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirNominalDeclaration *d = &table->declarations[i];
        status = nominal_declaration(d, types, &remaining);
        if (status != XR_XIR_OK) return status;
        for (uint32_t j = 0; j < i; ++j) {
            bool same = false;
            const XrXirNominalDeclaration *previous=&table->declarations[j];
            if((d->flags&XR_XIR_NOMINAL_RECORD)&&(previous->flags&XR_XIR_NOMINAL_RECORD)&&d->field_count==previous->field_count) {
                bool equal=true;
                for(uint32_t f=0;f<d->field_count&&equal;++f)equal=nominal_record_order(d->fields[f].name,previous->fields[f].name)==0;
                if(equal)return XR_XIR_BAD_STRUCTURE;
            }
            status = nominal_same_name(d->module, table->declarations[j].module, &same);
            if (status != XR_XIR_OK) return status;
            if (!same) continue;
            status = nominal_same_name(d->name, table->declarations[j].name, &same);
            if (status != XR_XIR_OK) return status;
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    *budget = remaining; return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_nominal_structure_verify(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, const XrXirTypes *types) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext remaining = *budget;
    XrXirStatus status = xr_xir_compile_types_structure_verify(&remaining, types);
    if (status == XR_XIR_OK && (!types || table != types->nominals))
        status = nominal_table_verify(table, types, &remaining);
    if (status == XR_XIR_OK) *budget = remaining;
    return status;
}
XR_FUNC void xr_xir_compile_nominal_free(XrXirNominalTable *table) {
    if (!table) return;
    if (table->identities) { nominal_free_identities(table); return; }
    for (uint32_t i = 0; table->declarations && i < table->count; ++i) {
        const XrXirNominalDeclaration *d = &table->declarations[i];
        xr_compile_resources_free((void *) d->module.bytes); xr_compile_resources_free((void *) d->name.bytes);
        xr_xir_compile_constraint_array_free((XrXirConstraint *)d->constraints, d->parameter_count);
        nominal_variants_free(d->variants, d->variant_count);
        for (uint32_t f = 0; d->fields && f < d->field_count; ++f)
            xr_compile_resources_free((void *) d->fields[f].name.bytes);
        xr_compile_resources_free((void *) d->fields);
    }
    xr_compile_resources_free((void *) table->declarations); xr_compile_resources_free(table);
}
static bool nominal_copy_name(const XrXirCompileContext *compile_context, XrXirLiteral source, XrXirLiteral *output, XrXirStatus *allocation_status, XrCompileRequestControl *control) {
    if ((uint64_t)source.length + 1 > SIZE_MAX) {
        *allocation_status = XR_XIR_BUDGET; return false;
    }
    char *bytes = xir_compile_alloc(compile_context, (size_t) source.length + 1, allocation_status);
    if (!bytes) return false;
    *allocation_status=xir_compile_resource_status(xir_lower_copy_span(control,bytes,source.bytes,source.length));
    if (*allocation_status!=XR_XIR_OK) { xr_compile_resources_free(bytes);return false; }
    bytes[source.length] = 0;
    *output = (XrXirLiteral) {bytes, source.length}; return true;
}
static bool nominal_variants_copy(const XrXirCompileContext *compile_context, const XrXirNominalVariant *source, uint32_t count,
    const XrXirNominalVariant **output, XrXirStatus *allocation_status, XrCompileRequestControl *control) {

    if (!count) return true;
    XrXirNominalVariant *copy = xir_type_copy_zero(compile_context, count, sizeof(*copy), allocation_status,control);
    if (!copy) return false;
    *output = copy;
    for (uint32_t i = 0; i < count; ++i) {
        *allocation_status=xir_type_copy_rows(control,i);
        if (*allocation_status!=XR_XIR_OK) return false;
        copy[i].field_begin = source[i].field_begin; copy[i].field_count = source[i].field_count;
        if (!nominal_copy_name(compile_context, source[i].name, &copy[i].name, allocation_status,control)) return false;
    }
    return true;
}

static bool nominal_copy_declaration(const XrXirCompileContext *compile_context, const XrXirNominalDeclaration *source,
                                     XrXirNominalDeclaration *d, XrCompileRequestControl *control, XrXirStatus *allocation_status) {
    d->exported = source->exported; d->kind = source->kind;
    d->variant_count = source->variant_count; d->flags = source->flags;

    d->native = source->native;
    if (!nominal_variants_copy(compile_context, source->variants, source->variant_count, &d->variants, allocation_status,control)) return false;
    if (!nominal_copy_name(compile_context, source->module, &d->module, allocation_status,control) ||
        !nominal_copy_name(compile_context, source->name, &d->name, allocation_status,control)) return false;
    XrXirConstraint *constraints = NULL;
    if ((*allocation_status = xr_xir_compile_constraint_array_copy_verified(compile_context, source->constraints, source->parameter_count, control, &constraints)) != XR_XIR_OK)
        return false;
    d->constraints = constraints; d->parameter_count = source->parameter_count;
    if (source->field_count) {
        XrXirNominalField *fields = xir_type_copy_zero(compile_context, source->field_count, sizeof(*fields), allocation_status,control);
        if (!fields) return false;
        d->fields = fields; d->field_count = source->field_count;
        for (uint32_t f = 0; f < source->field_count; ++f) {
            *allocation_status=xir_type_copy_rows(control,f);if(*allocation_status!=XR_XIR_OK)return false;
            fields[f].type = source->fields[f].type; fields[f].flags = source->fields[f].flags;
            if (!nominal_copy_name(compile_context, source->fields[f].name, &fields[f].name, allocation_status,control)) return false;
        }
    }
    return true;
}
XR_FUNC XrXirStatus xr_xir_compile_nominal_clone(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, const XrXirTypes *types, XrXirNominalTable **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!output) return XR_XIR_BAD_STRUCTURE;

    if (!budget) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext remaining = *budget;
    XrXirStatus status = xr_xir_compile_nominal_structure_verify(&remaining, table, types);
    if (status != XR_XIR_OK || !table) return status;
    return nominal_copy_table(compile_context, table, NULL, output);
}

static XrXirStatus nominal_copy_table(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, XrCompileRequestControl *control,
                                     XrXirNominalTable **output) {
    XrXirStatus allocation_status = XR_XIR_OK;

    allocation_status=xir_type_copy_poll(control);if(allocation_status!=XR_XIR_OK)return allocation_status;
    if (!table) { *output = NULL; return XR_XIR_OK; }
    if (table->identities) return nominal_copy_identities(compile_context, table, control, output);

    XrXirNominalTable *copy = xir_compile_calloc(compile_context, 1, sizeof(*copy), &allocation_status);
    if (!copy) return allocation_status;
    XrXirNominalDeclaration *declarations = xir_type_copy_zero(compile_context, table->count, sizeof(*declarations), &allocation_status,control);
    if (!declarations) { xr_compile_resources_free(copy); return allocation_status; }
    copy->declarations = declarations; copy->count = table->count;
    for (uint32_t i = 0; i < table->count; ++i) {
        allocation_status=xir_type_copy_rows(control,i);
        if(allocation_status!=XR_XIR_OK) {xr_xir_compile_nominal_free(copy);return allocation_status;}
        if (!nominal_copy_declaration(compile_context, &table->declarations[i], &declarations[i], control, &allocation_status)) {
            xr_xir_compile_nominal_free(copy); return allocation_status;
        }
    }
    allocation_status=xir_type_copy_poll(control);
    if(allocation_status!=XR_XIR_OK) {xr_xir_compile_nominal_free(copy);return allocation_status;}
    *output = copy; return XR_XIR_OK;
}

static XrXirStatus nominal_nodes_verify(const XrXirTypes *types, XrXirCompileContext *budget) {
    if (!types->nominals) return XR_XIR_OK;
    for (uint32_t i = 0; i < types->count; ++i) {

        const XrXirTypeNode *node = &types->nodes[i];
        if (node->kind != XR_XIR_TYPE_NOMINAL) continue;
        if (!types->nominals || node->nominal.declaration >= types->nominals->count) return XR_XIR_BAD_TYPE;
        if (types->nominals->identities) {
            const XrXirNominalIdentity *identity = &types->nominals->identities[node->nominal.declaration];
            if (node->nominal.argument_count != identity->arity || node->nominal.field_count != identity->field_count)
                return XR_XIR_BAD_TYPE;
            if (identity->native.native_id==XR_NATIVE_DECLARATION_RECV &&
                node->nominal.fields[0]!=node->nominal.arguments[0]) return XR_XIR_BAD_TYPE;
            if (identity->native.native_id==XR_NATIVE_DECLARATION_TASK_RESULT) {
                XrXirTaskResultType result={0};
                if (!xr_xir_task_result_type_info(types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i),&result))
                    return XR_XIR_BAD_TYPE;
            }
            for (uint32_t f = 0; f < node->nominal.field_count; ++f) {

                if ((identity->flags&XR_XIR_NOMINAL_RECORD) && node->nominal.fields[f]!=node->nominal.arguments[f])
                    return XR_XIR_BAD_TYPE;
                if (!nominal_field_type(types, node->nominal.fields[f], 0,!f &&
                    (identity->native.native_id==XR_NATIVE_DECLARATION_RECV ||
                     identity->native.native_id==XR_NATIVE_DECLARATION_TASK_RESULT))) return XR_XIR_BAD_TYPE;
            }
            continue;
        }
        const XrXirNominalDeclaration *d = &types->nominals->declarations[node->nominal.declaration];
        if (node->nominal.argument_count != d->parameter_count) return XR_XIR_BAD_TYPE;
        if (node->nominal.field_count && node->nominal.field_count != d->field_count) return XR_XIR_BAD_TYPE;
        for (uint32_t f = 0; f < node->nominal.field_count; ++f) {
            if (!nominal_field_type(types, node->nominal.fields[f], node->parameter_span,!f &&
                (d->native.native_id==XR_NATIVE_DECLARATION_RECV ||
                 d->native.native_id==XR_NATIVE_DECLARATION_TASK_RESULT))) return XR_XIR_BAD_TYPE;
            XrXirStatus status = xr_xir_compile_type_substitution_matches(budget, types, node->nominal.arguments, node->nominal.argument_count, d->fields[f].type, node->nominal.fields[f]);
            if (status != XR_XIR_OK) return status;
        }

    }
    return XR_XIR_OK;
}
