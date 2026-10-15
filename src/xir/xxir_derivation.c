/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_derivation.c - Owned declaration grants and independent receiving
 *
 * KEY CONCEPT:
 *   Copying a grant preserves data lifetime, not private operation authority.
 */
#include "xxir_derivation_internal.h"
#include "xxir_compile_memory.h"
#include "../shared/xr_utf8_core.h"

static void derivation_origin_free(XrXirGeneratedOrigin *origin) {
    xr_compile_resources_free((void *)origin->module.bytes);
    xr_compile_resources_free((void *)origin->file.bytes);
    xr_compile_resources_free((void *)origin->field_path.bytes);
}
static void derivation_policy_free(XrXirDerivationPolicy *row) {
    xr_compile_resources_free((void *)row->protocol.arguments);
    xr_compile_resources_free((void *)row->members);
    derivation_origin_free(&row->origin);
}
static void derivation_row_free(XrXirDerivationGrant *row) {
    derivation_origin_free(&row->origins.entry);
    for (uint32_t n=0; row->origins.instructions && n<row->origins.instruction_count; ++n)
        derivation_origin_free(&((XrXirGeneratedOrigin *)row->origins.instructions)[n]);
    xr_compile_resources_free((void *)row->origins.instructions);
}
XR_FUNC void xir_derivations_free(XrXirDerivationTable *table) {
    if (!table) return;
    for (uint32_t i=0; table->records && i<table->count; ++i)
        derivation_row_free(&((XrXirDerivationGrant *)table->records)[i]);
    for (uint32_t i=0; table->policies && i<table->policy_count; ++i)
        derivation_policy_free(&((XrXirDerivationPolicy *)table->policies)[i]);
    xr_compile_resources_free((void *)table->policies);
    xr_compile_resources_free((void *)table->records);
    xr_compile_resources_free(table);
}
static XrXirStatus derivation_span(const XrXirCompileContext *context,const void *data,
    uint32_t count,size_t width,uint32_t *remaining) {
    if ((count!=0)!=(data!=NULL)) return XR_XIR_BAD_STRUCTURE;
    if (count>*remaining || (uint64_t)count*width>SIZE_MAX ||
        !xir_compile_work(context,(uint64_t)count+1)) return XR_XIR_BUDGET;
    *remaining-=count;
    return XR_XIR_OK;
}
static XrXirStatus derivation_text_shape(const XrXirCompileContext *context,
    XrXirGeneratedText text,bool required) {
    if ((text.length!=0)!=(text.bytes!=NULL) || (required && !text.length))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,(uint64_t)text.length+1)) return XR_XIR_BUDGET;
    if (text.length && (memchr(text.bytes,0,text.length) ||
        xr_utf8_core_scan_strict((const uint8_t *)text.bytes,text.length).error!=XR_UTF8_OK))
        return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}
static XrXirStatus derivation_origin_shape(const XrXirCompileContext *context,
    const XrXirGeneratedOrigin *origin) {
    if (!origin->line || !origin->column) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=derivation_text_shape(context,origin->module,true);
    if (status==XR_XIR_OK) status=derivation_text_shape(context,origin->file,true);
    if (status==XR_XIR_OK) status=derivation_text_shape(context,origin->field_path,false);
    return status;
}
static XrXirStatus derivation_policy_shape(const XrXirCompileContext *context,
    const XrXirDerivationPolicy *row,XrXirCompileLimits *remaining) {
    XrXirStatus status=derivation_span(context,row->protocol.arguments,row->protocol.argument_count,
        sizeof(*row->protocol.arguments),&remaining->parameters);
    if (status==XR_XIR_OK) status=derivation_span(context,row->members,row->member_count,
        sizeof(*row->members),&remaining->parameters);
    if (status==XR_XIR_OK) status=derivation_origin_shape(context,&row->origin);
    for (uint32_t f=0; status==XR_XIR_OK && f<row->member_count; ++f) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirDerivationMember *member=&row->members[f];
        if (!member->operations || (member->operations&~XR_XIR_DERIVE_MEMBER_MASK) ||
            (f && row->members[f-1].field>=member->field)) return XR_XIR_BAD_STRUCTURE;
    }
    return status;
}
static XrXirStatus derivation_binding_shape(const XrXirCompileContext *context,
    const XrXirDerivationGrant *row,XrXirCompileLimits *remaining) {
    XrXirStatus status=derivation_origin_shape(context,&row->origins.entry);
    if (status==XR_XIR_OK && !row->origins.instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=derivation_span(context,row->origins.instructions,
        row->origins.instruction_count,sizeof(*row->origins.instructions),&remaining->instructions);
    for (uint32_t n=0; status==XR_XIR_OK && n<row->origins.instruction_count; ++n)
        status=derivation_origin_shape(context,&row->origins.instructions[n]);
    return status;
}
/* Only initialized readable extents are admitted here. Module identities,
 * protocol entailment, actual constructors and current body correspondence
 * remain independent ordinary verifier responsibilities. */
XR_FUNC XrXirStatus xir_derivations_structure_verify(const XrXirCompileContext *context,
    const XrXirDerivationTable *table) {
    if (!xir_compile_context_valid(context)) return XR_XIR_BAD_STRUCTURE;
    if (!table) return XR_XIR_OK;
    if (!table->count && !table->policy_count) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileLimits remaining=context->limits;
    XrXirStatus status=derivation_span(context,table->policies,table->policy_count,
        sizeof(*table->policies),&remaining.parameters);
    for (uint32_t i=0; status==XR_XIR_OK && i<table->policy_count; ++i)
        status=derivation_policy_shape(context,&table->policies[i],&remaining);
    if (status==XR_XIR_OK) status=derivation_span(context,table->records,table->count,
        sizeof(*table->records),&remaining.functions);
    for (uint32_t i=0; status==XR_XIR_OK && i<table->count; ++i) {
        const XrXirDerivationGrant *row=&table->records[i];
        if (i && table->records[i-1].function>=row->function) return XR_XIR_BAD_STRUCTURE;
        status=derivation_binding_shape(context,row,&remaining);
    }
    return status;
}
static XrXirStatus derivation_origin_copy(const XrXirCompileContext *context,
    const XrXirGeneratedOrigin *source,XrXirGeneratedOrigin *output) {
    XrXirStatus status=XR_XIR_OK;
    output->module.bytes=xir_compile_copy(context,source->module.bytes,source->module.length,&status);
    output->file.bytes=xir_compile_copy(context,source->file.bytes,source->file.length,&status);
    output->field_path.bytes=xir_compile_copy(context,source->field_path.bytes,source->field_path.length,&status);
    if (status==XR_XIR_OK) {
        output->module.length=source->module.length; output->file.length=source->file.length;
        output->field_path.length=source->field_path.length;
        output->line=source->line; output->column=source->column;
    }
    return status;
}
static XrXirStatus derivation_policy_copy(const XrXirCompileContext *context,
    const XrXirDerivationPolicy *source,XrXirDerivationPolicy *output) {
    XrXirStatus status=XR_XIR_OK;
    output->grantor_module=source->grantor_module; output->target=source->target;
    output->protocol.declaration=source->protocol.declaration;
    output->protocol.arguments=xir_compile_copy(context,source->protocol.arguments,
        (size_t)source->protocol.argument_count*sizeof(*source->protocol.arguments),&status);
    output->protocol.argument_count=source->protocol.argument_count;
    output->generator=source->generator; output->constructor=source->constructor;
    output->members=xir_compile_copy(context,source->members,
        (size_t)source->member_count*sizeof(*source->members),&status);
    output->member_count=source->member_count;
    if (status!=XR_XIR_OK) return status;
    return derivation_origin_copy(context,&source->origin,&output->origin);
}
static XrXirStatus derivation_row_copy(const XrXirCompileContext *context,
    const XrXirDerivationGrant *source,XrXirDerivationGrant *output) {
    output->function=source->function; output->policy=source->policy;
    output->source_row=source->source_row; output->root=source->root; output->target=source->target;
    XrXirStatus status=XR_XIR_OK;
    status=derivation_origin_copy(context,&source->origins.entry,&output->origins.entry);
    XrXirGeneratedOrigin *origins=xir_compile_calloc(context,source->origins.instruction_count,
        sizeof(*origins),&status);
    output->origins.instructions=origins;
    output->origins.instruction_count=source->origins.instruction_count;
    for (uint32_t i=0; status==XR_XIR_OK && i<source->origins.instruction_count; ++i)
        status=derivation_origin_copy(context,&source->origins.instructions[i],&origins[i]);
    return status;
}
static XrXirStatus derivation_table_copy(const XrXirCompileContext *context,
    const XrXirDerivationTable *source,XrXirDerivationTable *table) {
    XrXirStatus status=XR_XIR_OK;
    XrXirDerivationPolicy *policies=xir_compile_calloc(context,source->policy_count,sizeof(*policies),&status);
    table->policies=policies; table->policy_count=source->policy_count;
    XrXirDerivationGrant *records=xir_compile_calloc(context,source->count,sizeof(*records),&status);
    table->records=records; table->count=source->count;
    for (uint32_t i=0; status==XR_XIR_OK && i<source->policy_count; ++i)
        status=derivation_policy_copy(context,&source->policies[i],&policies[i]);
    for (uint32_t i=0; status==XR_XIR_OK && i<source->count; ++i)
        status=derivation_row_copy(context,&source->records[i],&records[i]);
    return status;
}
XR_FUNC XrXirStatus xir_derivations_copy(const XrXirCompileContext *context,
    const XrXirDerivationTable *source,XrXirDerivationTable **output) {
    if (!xir_compile_context_valid(context) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xir_derivations_structure_verify(context,source);
    if (status!=XR_XIR_OK || !source) return status;
    XrXirDerivationTable *table=xir_compile_calloc(context,1,sizeof(*table),&status);
    if (!table) return status;
    status=derivation_table_copy(context,source,table);
    if (status!=XR_XIR_OK) { xir_derivations_free(table); return status; }
    *output=table;
    return XR_XIR_OK;
}
