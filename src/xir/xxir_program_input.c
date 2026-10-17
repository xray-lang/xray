/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_program_input.c - One owned input for packet and internal reception
 *
 * KEY CONCEPT:
 *   Transport creates ownership, never a permission or a saved verification.
 */
#include "xxir_program_input_internal.h"
#include "xxir_program_packet_internal.h"
#include "xxir_compile_memory.h"
#include "../base/xsha256.h"

XR_FUNC XrXirProgramPacket xir_program_packet_borrow(const XrXirArtifact *artifact) {
    if (!artifact || artifact->module.stage!=XR_XIR_LOWERED) return (XrXirProgramPacket){0};
    return (XrXirProgramPacket){artifact->checked_packet.bytes,artifact->checked_packet.length,
        artifact->checked_identity};
}

static XrXirStatus input_storage(const XrXirCompileContext *context,
    const void *memory,size_t bytes,size_t alignment) {
    if (!memory || (uintptr_t)memory%alignment) return XR_XIR_BAD_STRUCTURE;
    XrCompileResourceStatus status=xr_compile_resources_allocation(context->resources,memory,bytes);
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status==XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus input_artifact_storage(const XrXirCompileContext *context,
    const XrXirArtifact *artifact) {
    XrXirStatus status=input_storage(context,artifact,sizeof(*artifact),_Alignof(XrXirArtifact));
    if (status!=XR_XIR_OK) return status;
    if (artifact->context.resources!=context->resources || artifact->module.stage!=XR_XIR_LOWERED ||
        !artifact->program_final || !artifact->layouts) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}
/* A carrier owns physical input without granting execution authority. Body
 * and layout correspondence keep later binding reads bounded; the receiver
 * independently proves semantics before preparation and after its callbacks. */
static XrXirStatus input_artifact_shape(const XrXirCompileContext *context,
    const XrXirArtifact *artifact) {
    XrXirStatus status=input_artifact_storage(context,artifact);
    if (status==XR_XIR_OK) status=xir_effects_program_body_match(context,&artifact->module,artifact->program_final);
    if (status==XR_XIR_OK) status=xir_effects_program_layout_match(context,artifact->program_final,
        &artifact->target,artifact->layouts);
    return status==XR_XIR_BAD_LAYOUT?XR_XIR_BAD_STRUCTURE:status;
}
static XrXirStatus input_artifact_verify(const XrXirCompileContext *context,
    const XrXirArtifact *artifact) {
    XrXirStatus status=input_artifact_storage(context,artifact);
    if (status==XR_XIR_OK) status=xir_effects_program_final_verify(context,&artifact->module,artifact->program_final);
    if (status==XR_XIR_OK) status=xir_compile_verify_program_effects(context,&artifact->module,
        artifact->construction,artifact->program_final,NULL);
    if (status==XR_XIR_OK) status=xir_effects_program_layout_match(context,artifact->program_final,
        &artifact->target,artifact->layouts);
    if (status==XR_XIR_BAD_LAYOUT) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xir_effects_program_body_match(context,&artifact->module,artifact->program_final);
    if (status==XR_XIR_OK) status=xir_effects_program_root_current(context,&artifact->module,artifact->program_final);
    return status;
}
XR_FUNC XrXirStatus xir_program_input_verify(const XrXirCompileContext *context,
    const XrXirProgramInput *input) {
    if (!xir_compile_context_valid(context) || !input) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=input_storage(context,input,sizeof(*input),_Alignof(XrXirProgramInput));
    if (status!=XR_XIR_OK) return status;
    if (input->context.resources!=context->resources || !input->artifact) return XR_XIR_BAD_STRUCTURE;
    return input_artifact_verify(context,input->artifact);
}
XR_FUNC void xir_program_input_free(XrXirProgramInput *input) {
    if (!input) return;
    xr_xir_compile_artifact_free(input->artifact);
    xr_compile_resources_free(input);
}
XR_FUNC XrXirStatus xir_program_input_take(const XrXirCompileContext *context,
    XrXirArtifact **artifact,XrXirProgramInput **output) {
    if (!xir_compile_context_valid(context) || !artifact || !*artifact || !output || *output ||
        (void *)artifact==(void *)output) return XR_XIR_BAD_STRUCTURE;
    XrXirArtifact *original=*artifact;
    XrXirStatus status=XR_XIR_OK;
    XrXirProgramInput *input=xir_compile_calloc(context,1,sizeof(*input),&status);
    if (!input) return status;
    status=input_artifact_shape(context,original);
    if (status==XR_XIR_OK && (*artifact!=original || *output)) status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(input);return status; }
    input->context=*context;input->artifact=original;
    *artifact=NULL;*output=input;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_program_input_read(const XrXirCompileContext *context,
    const XrXirProgramPacket *packet,const XrXirTarget *target,XrXirProgramInput **output) {
    if (!xir_compile_context_valid(context) || !packet || !packet->bytes || packet->length<64 ||
        !packet->identity || !target || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirProgramPacket physical=*packet;
    uint8_t identity[32];
    if (physical.length>UINT64_MAX-64) return XR_XIR_BUDGET;
    if (!xir_compile_work(context,physical.length+32)) return XR_XIR_BUDGET;
    xr_sha256(physical.bytes,physical.length,identity);
    if (memcmp(identity,physical.identity,sizeof(identity))) return XR_XIR_BAD_STRUCTURE;
    XrXirArtifact *artifact=NULL;XrXirProgramInput *input=NULL;
    XrXirStatus status=xr_xir_compile_checked_read_lower(context,physical.bytes,physical.length,
        target,&artifact,NULL);
    if (status==XR_XIR_OK) status=xir_program_input_take(context,&artifact,&input);
    xr_xir_compile_artifact_free(artifact);
    if (status==XR_XIR_OK && (*output || packet->bytes!=physical.bytes || packet->length!=physical.length ||
        packet->identity!=physical.identity)) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) {
        if (!xir_compile_work(context,physical.length+64)) status=XR_XIR_BUDGET;
        else {
            uint8_t current[32];xr_sha256(physical.bytes,physical.length,current);
            if (memcmp(current,identity,sizeof(current)) || memcmp(current,physical.identity,sizeof(current)))
                status=XR_XIR_BAD_STRUCTURE;
        }
    }
    if (status!=XR_XIR_OK) { xir_program_input_free(input);return status; }
    *output=input;return XR_XIR_OK;
}
