/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_checked.c - Bounded canonical Checked serialization and re-admission
 *
 * KEY CONCEPT:
 *   One field traversal owns the wire schema; decoding publishes only after
 *   independent semantic verification of fully owned data.
 */
#include "xxir_checked.h"
#include "xxir_compile_memory.h"
#include "xxir_nominal.h"
#include "xxir_interface.h"
#include "xxir_implementation.h"
#include "xxir_internal.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"

#include "xxir_checked_codec.inc.c"

XR_FUNC XrXirStatus xir_checked_match_prepared(const XrXirCompileContext *context,const XrXirArtifact *artifact,
    const XrXirCheckedPacket *expected,const XirReadRequest *request) {
    if(!artifact || !expected || !expected->bytes || !request || !request->operation ||
        !request->attempt || !request->attempt->active ||
        !xir_read_request_matches(context,request) || context->resources!=artifact->context.resources) return XR_XIR_BAD_STRUCTURE;
    bool evaluation=artifact->evaluation_only!=0;size_t size=0;
    XrXirStatus status=checked_encode_measure(context,artifact,&size,NULL,evaluation,NULL);
    if(status!=XR_XIR_OK) return status;
    if(size!=expected->length) return XR_XIR_BAD_STRUCTURE;
    XirReadClaim claim={0};
    uint8_t *bytes=xir_read_scratch_alloc(context,&request->operation->capacity,
        size,false,&claim,&status);
    if(!bytes) return status;
    uintptr_t a=(uintptr_t)bytes,b=(uintptr_t)expected->bytes;
    if(a<=b?b-a<size:a-b<expected->length) status=XR_XIR_BAD_STRUCTURE;
    if(status==XR_XIR_OK) status=checked_encode_buffer(context,artifact,bytes,size,NULL,evaluation,NULL);
    if(status==XR_XIR_OK && memcmp(bytes,expected->bytes,size)) status=XR_XIR_BAD_STRUCTURE;
    xir_read_scratch_release(&request->operation->capacity,bytes,&claim);return status;
}

void xr_xir_compile_checked_packet_free(XrXirCheckedPacket *packet) {
    if (packet) { xr_compile_resources_free(packet->bytes); *packet = (XrXirCheckedPacket) {0}; }
}
static XrXirStatus checked_packet_final(const XrXirCompileContext *context,const XrXirArtifact *artifact,
    const XrXirCheckedPacket *packet,XrXirDiagnostic *diagnostic,const XirReadRequest *read_request) {
    if(!read_request || !xir_read_request_matches(context,read_request))
        return checked_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    bool outer=!read_request->attempt;
    XirReadAttempt attempt={true};XirReadRequest request=*read_request;
    if(outer) request.attempt=&attempt;
    for(;;) {
        XrXirStatus status=outer?xir_read_begin(context,request.operation):XR_XIR_OK;
        if(status==XR_XIR_OK) status=xir_artifact_verify(context,artifact,diagnostic,request.control,&request);
        if(status==XR_XIR_OK) status=xir_receive_poll(request.control);
        XirReadRequest fresh=request;fresh.control=NULL;
        if(status==XR_XIR_OK) status=xir_artifact_verify(context,artifact,diagnostic,NULL,&fresh);
        if(status==XR_XIR_OK) status=xir_checked_match_prepared(context,artifact,packet,&fresh);
        XirReadStep step=xir_read_step(request.operation,status);
        if(step.kind!=XIR_READ_NEED_PREPARE || !outer) return status;
        status=xir_read_prepare(context,request.operation,request.control);
        if(status!=XR_XIR_OK) return checked_error(status,diagnostic);
    }
}
XR_FUNC XrXirStatus xir_checked_write_borrowed(const XrXirCompileContext *context,const XrXirArtifact *artifact,
    bool evaluation,XrXirCheckedPacket *output,XrXirDiagnostic *diagnostic,const XirReadRequest *request) {
    if(!artifact || !output || output->bytes || output->length || !request ||
        !xir_read_request_matches(context,request)) return checked_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    XrXirStatus status=checked_encode_shape(artifact,evaluation,diagnostic);
    if(status==XR_XIR_OK) status=xir_artifact_verify(context,artifact,diagnostic,request->control,request);
    XrXirCheckedPacket packet={0};
    if(status==XR_XIR_OK) status=checked_encode_mode(context,artifact,&packet,diagnostic,evaluation,request->control);
    if(status==XR_XIR_OK) status=checked_packet_final(context,artifact,&packet,diagnostic,request);
    if(status==XR_XIR_OK && (output->bytes || output->length)) status=checked_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    if(status==XR_XIR_OK) {*output=packet;packet=(XrXirCheckedPacket){0};}
    xr_xir_compile_checked_packet_free(&packet);return status;
}
static XrXirStatus checked_write_mode(const XrXirArtifact *artifact,XrXirCheckedPacket *output,
    XrXirDiagnostic *diagnostic,bool evaluation,XrCompileRequestControl *control) {
    if(!artifact) return checked_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    XirReadOperation operation={.capacity={.resources=artifact->context.resources}};
    const XirReadRequest request={.operation=&operation,.control=control};
    XrXirStatus status=xir_checked_write_borrowed(&artifact->context,artifact,evaluation,output,diagnostic,&request);
    xir_read_operation_free(&operation);return status;
}
XR_FUNC XrXirStatus xir_checked_write(const XrXirArtifact *artifact,XrXirCheckedPacket *output,
    XrXirDiagnostic *diagnostic,XrCompileRequestControl *control) {
    return checked_write_mode(artifact,output,diagnostic,false,control);
}
XR_FUNC XrXirStatus xr_xir_compile_checked_write(const XrXirArtifact *artifact,
    XrXirCheckedPacket *output, XrXirDiagnostic *diagnostic) {
    return xir_checked_write(artifact,output,diagnostic,NULL);
}
static XrXirStatus checked_read_mode(const XrXirCompileContext *compile_context,const void *bytes,size_t length,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic,bool evaluation,XrCompileRequestControl *control) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!output || *output) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);

    XrXirCompileContext limits = *budget;
    XirReadOperation operation={.capacity={.resources=compile_context->resources}};
    const XirReadRequest request={.operation=&operation,.control=control};
    if (!bytes || length < 64) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);

    CheckedCursor c = {bytes, NULL, 8, length, limits.limits, limits, XR_XIR_OK, true, NULL, 0};
    XrXirArtifact *artifact = NULL;
    if (memcmp(bytes,evaluation?"XRCEV\0\0\0":"XRCHK\0\0\0",8)) { c.status=XR_XIR_BAD_STRUCTURE; goto decode_failure; }
    uint32_t schema = checked_u32(&c,0), contract = checked_u32(&c,0);
    uint32_t stage = checked_u32(&c,0), reserved = checked_u32(&c,0);
    uint64_t payload = checked_integer(&c,0,8);
    if (c.status != XR_XIR_OK) goto decode_failure;
    if (schema != (evaluation?1u:XR_XIR_CHECKED_SCHEMA) || contract != XR_XIR_CHECKED_CONTRACT || reserved || payload != length-64) {
        c.status=XR_XIR_BAD_STRUCTURE; goto decode_failure;
    }
    if (stage != XR_XIR_CHECKED) { c.status=XR_XIR_BAD_STAGE; goto decode_failure; }
    c.status=xir_receive_poll(control);
    if (c.status!=XR_XIR_OK) goto decode_failure;
    c.control=control;
    /* Hash reads length-32 bytes; comparing the stored digest reads another 32. */
    uint8_t digest[32];c.status=checked_digest(bytes,length,digest,control);
    if (c.status!=XR_XIR_OK) goto decode_failure;
    if (memcmp(digest,(const uint8_t *)bytes+32,32)) { c.status=XR_XIR_BAD_STRUCTURE; goto decode_failure; }
    artifact=xir_type_copy_zero(compile_context,1,sizeof(*artifact),&allocation_status,control);
    if (!artifact) { c.status=allocation_status; goto decode_failure; }
    artifact->module.stage=XR_XIR_CHECKED; artifact->context=limits;artifact->evaluation_only=evaluation?1u:0u;
    c.position=64;
    XirEvaluationRoot root={0};
    if (evaluation) { root.caller=checked_u32(&c,0);root.instruction=checked_u32(&c,0);root.callee=checked_u32(&c,0); }
    checked_module(&c,&artifact->module,&artifact->construction,evaluation?XR_XIR_EVIDENCE_EVALUATION:XR_XIR_EVIDENCE_INSTANCE);
    if (evaluation && c.status==XR_XIR_OK) {
        if (!xir_effect_evidence_is_evaluation(&artifact->module)) c.status=XR_XIR_BAD_STAGE;
        else ((XrXirProvenance *)artifact->module.provenance)->evaluation=root;
    }
    if (c.status==XR_XIR_OK && c.position!=length) c.status=XR_XIR_BAD_STRUCTURE;
    if (c.status!=XR_XIR_OK) goto decode_failure;
    /* Decoding and semantic verification share the allocation owner. */

    c.status=xir_source_facts_prepare_decoded(artifact,diagnostic,control,&request);
    XrXirCheckedPacket packet={(uint8_t *)bytes,length};
    if(c.status==XR_XIR_OK) c.status=checked_packet_final(compile_context,artifact,&packet,diagnostic,&request);
    if(c.status==XR_XIR_OK && *output) c.status=XR_XIR_BAD_STRUCTURE;
    if(c.status!=XR_XIR_OK) goto decode_failure;
    *output=artifact;
    xir_read_operation_free(&operation);return XR_XIR_OK;
decode_failure:
    xr_xir_compile_artifact_free(artifact);xir_read_operation_free(&operation);
    return checked_error(c.status,diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_checked_read(const XrXirCompileContext *context,const void *bytes,size_t length,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    return checked_read_mode(context,bytes,length,output,diagnostic,false,NULL);
}
XR_FUNC XrXirStatus xir_checked_read(const XrXirCompileContext *context,const void *bytes,size_t length,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic,XrCompileRequestControl *control) {
    return checked_read_mode(context,bytes,length,output,diagnostic,false,control);
}
XR_FUNC XrXirStatus xir_evaluation_read(const XrXirCompileContext *context,const void *bytes,size_t length,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic, XrCompileRequestControl *control) {
    return checked_read_mode(context,bytes,length,output,diagnostic,true,control);
}
XR_FUNC XrXirStatus xir_evaluation_write(const XrXirArtifact *artifact,XrXirCheckedPacket *output,
    XrXirDiagnostic *diagnostic,XrCompileRequestControl *control) {
    return checked_write_mode(artifact,output,diagnostic,true,control);
}
