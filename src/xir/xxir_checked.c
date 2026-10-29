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

void xr_xir_compile_checked_packet_free(XrXirCheckedPacket *packet) {
    if (packet) { xr_compile_resources_free(packet->bytes); *packet = (XrXirCheckedPacket) {0}; }
}
XrXirStatus xr_xir_compile_checked_write(const XrXirArtifact *artifact, XrXirCheckedPacket *output, XrXirDiagnostic *diagnostic) {
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    if (!output || output->bytes || output->length) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);

    if (!artifact || artifact->module.stage != XR_XIR_CHECKED)
        return checked_error(XR_XIR_BAD_STAGE, diagnostic);
    if (xir_artifact_is_evaluation(artifact)) return checked_error(XR_XIR_BAD_STAGE,diagnostic);
    XrXirStatus status = xr_xir_compile_artifact_verify(artifact, diagnostic);
    if (status != XR_XIR_OK) return status;
    return checked_encode(artifact, output, diagnostic, NULL);
}
static XrXirStatus checked_read_mode(const XrXirCompileContext *compile_context,const void *bytes,size_t length,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic,bool evaluation) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!output || *output) return checked_error(XR_XIR_BAD_STRUCTURE, diagnostic);

    XrXirCompileContext limits = *budget;
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
    /* Hash reads length-32 bytes; comparing the stored digest reads another 32. */
    uint8_t digest[32]; checked_digest(bytes,length,digest,NULL);
    if (memcmp(digest,(const uint8_t *)bytes+32,32)) { c.status=XR_XIR_BAD_STRUCTURE; goto decode_failure; }
    artifact=xir_compile_calloc(compile_context, 1,sizeof(*artifact), &allocation_status);
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

    c.status=xir_source_facts_prepare_decoded(artifact,diagnostic);
    if (c.status==XR_XIR_OK) c.status=xr_xir_compile_artifact_verify(artifact,diagnostic);

    if (c.status!=XR_XIR_OK) { xr_xir_compile_artifact_free(artifact); return c.status; }
    *output=artifact;
    return XR_XIR_OK;
decode_failure:
    xr_xir_compile_artifact_free(artifact);
    return checked_error(c.status,diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_checked_read(const XrXirCompileContext *context,const void *bytes,size_t length,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    return checked_read_mode(context,bytes,length,output,diagnostic,false);
}
XR_FUNC XrXirStatus xir_evaluation_read(const XrXirCompileContext *context,const void *bytes,size_t length,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    return checked_read_mode(context,bytes,length,output,diagnostic,true);
}
XR_FUNC XrXirStatus xir_evaluation_write(const XrXirArtifact *artifact,XrXirCheckedPacket *output,
    XrXirDiagnostic *diagnostic) {
    if (!artifact || !output || output->bytes || output->length) return checked_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    XrXirStatus status=xr_xir_compile_artifact_verify(artifact,diagnostic);
    return status==XR_XIR_OK?checked_evaluation_encode(artifact,output,diagnostic,NULL):status;
}
