/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_generated.inc.c - Independent generated input lifetime and checking
 *
 * KEY CONCEPT:
 *   Only the common verifier can turn an owned Built body into Checked output.
 */
#include "xxir_generated.h"
#include "xxir_input_owned.h"
#include "xxir_implementation_verify.h"
#include "../shared/xr_utf8_core.h"

struct XrXirGeneratedBuilt {
    XrXirArtifact *storage;
    XrXirGeneratedFunctionOrigins *origins;
    uint32_t function_count;
};

static void generated_origin_free(XrXirGeneratedOrigin *origin) {
    xr_compile_resources_free((void *)origin->module.bytes);
    xr_compile_resources_free((void *)origin->file.bytes);
    xr_compile_resources_free((void *)origin->field_path.bytes);
}
XR_FUNC void xr_xir_compile_generated_free(XrXirGeneratedBuilt *owner) {
    if (!owner) return;
    for (uint32_t f=0; owner->origins && f<owner->function_count; ++f) {
        XrXirGeneratedFunctionOrigins *row=&owner->origins[f];
        generated_origin_free(&row->entry);
        for (uint32_t i=0; row->instructions && i<row->instruction_count; ++i)
            generated_origin_free(&((XrXirGeneratedOrigin *)row->instructions)[i]);
        xr_compile_resources_free((void *)row->instructions);
    }
    xr_compile_resources_free(owner->origins);
    xr_xir_compile_artifact_free(owner->storage);
    xr_compile_resources_free(owner);
}
static XrXirStatus generated_text_copy(const XrXirCompileContext *context,
    XrXirGeneratedText from, XrXirGeneratedText *to, bool required) {
    if ((from.length!=0)!=(from.bytes!=NULL) || (required && !from.length))
        return XR_XIR_BAD_STRUCTURE;

    if (from.length && (memchr(from.bytes,0,from.length) ||
        xr_utf8_core_scan_strict((const uint8_t *)from.bytes,from.length).error!=XR_UTF8_OK))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    to->bytes=copy_bytes(context,from.bytes,from.length,&status);
    if (status==XR_XIR_OK) to->length=from.length;
    return status;
}
static XrXirStatus generated_origin_copy(const XrXirCompileContext *context,
    const XrXirGeneratedOrigin *from, XrXirGeneratedOrigin *to) {
    if (!from->line || !from->column) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=generated_text_copy(context,from->module,&to->module,true);
    if (status==XR_XIR_OK) status=generated_text_copy(context,from->file,&to->file,true);
    if (status==XR_XIR_OK) status=generated_text_copy(context,from->field_path,&to->field_path,false);
    if (status==XR_XIR_OK) { to->line=from->line; to->column=from->column; }
    return status;
}
/* Extents and metadata must be readable before copying. These common metadata
 * checks carry no instruction, borrow, effect or private-operation permission. */
static XrXirStatus generated_receive_shape(const XrXirCompileContext *context,
    const XrXirGeneratedReceive *request) {
    const XrXirModule *module=request->built;
    if (!module || module->stage!=XR_XIR_BUILT) return XR_XIR_BAD_STAGE;
    if (!module->functions || !module->function_count || module->provenance ||
        !request->origins || request->function_count!=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (module->function_count>context->limits.functions) return XR_XIR_BUDGET;
    XrXirCompileLimits remaining=context->limits;
    for (uint32_t f=0; f<module->function_count; ++f) {

        const XrXirFunction *function=&module->functions[f];
        if (function->parameter_count>remaining.parameters || function->block_count>remaining.blocks ||
            function->instruction_count>remaining.instructions) return XR_XIR_BUDGET;
        if ((uint64_t)function->parameter_count*sizeof(*function->parameters)>SIZE_MAX ||
            (uint64_t)function->block_count*sizeof(*function->blocks)>SIZE_MAX ||
            (uint64_t)function->instruction_count*sizeof(*function->instructions)>SIZE_MAX ||
            (uint64_t)function->operand_count*sizeof(*function->operands)>SIZE_MAX)
            return XR_XIR_BUDGET;
        remaining.parameters-=function->parameter_count; remaining.blocks-=function->block_count;
        remaining.instructions-=function->instruction_count;
        if (!function->name || !function->name_length || !function->blocks ||
            !function->instructions || !function->instruction_count ||
            request->origins[f].instruction_count!=function->instruction_count ||
            !request->origins[f].instructions) return XR_XIR_BAD_STRUCTURE;
        XirInputFunctionHead head={function->name,function->name_length,function->parameters,
            function->parameter_count,function->result,function->blocks,function->block_count,
            function->instruction_count,function->operands,function->operand_count,function->error_type};
        XrXirStatus status=xir_input_function_head_shape(&head);
        if (status!=XR_XIR_OK) return status;
    }
    XrXirStatus status=xr_xir_compile_types_structure_verify(context,module->types);
    if (status==XR_XIR_OK) status=xr_xir_compile_declarations_verify(context,module->declarations,
        module->types,module->function_count,module->linkage_kind);
    if (status==XR_XIR_OK) status=xr_xir_compile_generics_structure_verify(context,module);
    if (status==XR_XIR_OK) status=xr_xir_compile_implementations_verify(context,module);
    if (status==XR_XIR_OK) status=xr_xir_compile_defaults_verify(context,module);
    if (status==XR_XIR_OK) status=xir_construction_verify(context,module,request->construction);
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_generated_receive(const XrXirCompileContext *context,
    const XrXirGeneratedReceive *request,XrXirGeneratedBuilt **output) {
    if (!xir_compile_context_valid(context) || !request || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=generated_receive_shape(context,request);
    if (status!=XR_XIR_OK) return status;
    XrXirGeneratedBuilt *owner=xir_compile_calloc(context,1,sizeof(*owner),&status);
    if (!owner) return status;
    owner->function_count=request->function_count;
    owner->origins=xir_compile_calloc(context,owner->function_count,sizeof(*owner->origins),&status);
    for (uint32_t f=0; status==XR_XIR_OK && f<owner->function_count; ++f) {
        const XrXirGeneratedFunctionOrigins *from=&request->origins[f];
        XrXirGeneratedFunctionOrigins *to=&owner->origins[f];
        status=generated_origin_copy(context,&from->entry,&to->entry);
        if (status!=XR_XIR_OK) break;
        to->instruction_count=from->instruction_count;
        XrXirGeneratedOrigin *rows=xir_compile_calloc(context,to->instruction_count,sizeof(*rows),&status);
        to->instructions=rows;
        for (uint32_t i=0; status==XR_XIR_OK && i<to->instruction_count; ++i)
            status=generated_origin_copy(context,&from->instructions[i],&rows[i]);
    }
    if (status==XR_XIR_OK) owner->storage=clone_module_receiving(context,request->built,request->construction,&status,NULL,NULL);
    if (status!=XR_XIR_OK) { xr_xir_compile_generated_free(owner); return status; }
    *output=owner;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_generated_check(const XrXirGeneratedBuilt *owner,
    XrXirArtifact **output,XrXirGeneratedDiagnostic *diagnostic) {
    XrXirDiagnostic cause={XR_XIR_BAD_STRUCTURE,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    if (!owner || !owner->storage || !output || *output) {
        if (diagnostic) *diagnostic=(XrXirGeneratedDiagnostic){cause,NULL};
        return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=xr_xir_compile_check_v2(&owner->storage->context,
        &owner->storage->module,owner->storage->construction,output,&cause);
    if (diagnostic) {
        const XrXirGeneratedOrigin *origin=NULL;
        if (status!=XR_XIR_OK) {
            const XrXirDeclarations *declarations=owner->storage->module.declarations;
            uint32_t entry=declarations && declarations->entry_function<owner->function_count ?
                declarations->entry_function : 0;
            origin=&owner->origins[entry].entry;
        }
        if (status!=XR_XIR_OK && cause.function<owner->function_count) {
            const XrXirGeneratedFunctionOrigins *row=&owner->origins[cause.function];
            origin=cause.instruction<row->instruction_count ? &row->instructions[cause.instruction] : &row->entry;
        }
        *diagnostic=(XrXirGeneratedDiagnostic){cause,origin};
    }
    return status;
}
