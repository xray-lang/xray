/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * evaluation_controls.h - A compiler helper cannot become a runtime Program
 */
#include "xir/xxir_emit_c.h"
#include "base/xsha256.h"
static void evaluation_u32(uint8_t *bytes,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[i]=(uint8_t)(value>>(8*i));
}
static void evaluation_digest(XrXirCheckedPacket *packet) {
    XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,packet->bytes,32);
    xr_sha256_update(&hash,packet->bytes+64,packet->length-64);xr_sha256_final(&hash,packet->bytes+32);
}
static void evaluation_wire_controls(XrXirCompileContext *context,const XrXirArtifact *checked,XrXirCheckedPacket *packet) {
    uint8_t prefix[32]={0};memcpy(prefix,"XRCEV\0\0\0",8);
    evaluation_u32(prefix+8,1);evaluation_u32(prefix+12,XR_XIR_CHECKED_CONTRACT);evaluation_u32(prefix+16,2);
    uint64_t payload=packet->length-64;for (unsigned i=0;i<8;++i) prefix[24+i]=(uint8_t)(payload>>(8*i));
    CHECK(!memcmp(prefix,packet->bytes,32));
    uint8_t root[12];const XirEvaluationRoot *selection=&checked->module.provenance->evaluation;
    evaluation_u32(root,selection->caller);evaluation_u32(root+4,selection->instruction);evaluation_u32(root+8,selection->callee);
    CHECK(!memcmp(root,packet->bytes+64,12));
    uint8_t digest[32];memcpy(digest,packet->bytes+32,32);evaluation_digest(packet);CHECK(!memcmp(digest,packet->bytes+32,32));
    XrXirArtifact *out=NULL;evaluation_u32(packet->bytes+64,selection->callee);evaluation_digest(packet);
    CHECK(xir_evaluation_read(context,packet->bytes,packet->length,&out,NULL)==XR_XIR_BAD_STRUCTURE && !out);
    memcpy(packet->bytes+64,root,12);evaluation_digest(packet);
    XrXirStatus status=XR_XIR_OK;XrXirCheckedPacket ordinary={0};ordinary.length=packet->length-12;
    ordinary.bytes=xir_compile_calloc(context,ordinary.length,1,&status);CHECK(ordinary.bytes && status==XR_XIR_OK);
    memcpy(ordinary.bytes,packet->bytes,32);memcpy(ordinary.bytes,"XRCHK\0\0\0",8);evaluation_u32(ordinary.bytes+8,30);
    payload=ordinary.length-64;for (unsigned i=0;i<8;++i) ordinary.bytes[24+i]=(uint8_t)(payload>>(8*i));
    memcpy(ordinary.bytes+64,packet->bytes+76,packet->length-76);evaluation_digest(&ordinary);
    XrCompileResourceStats before=stats(context);
    CHECK(xr_xir_compile_checked_read(context,ordinary.bytes,ordinary.length,&out,NULL)==XR_XIR_BAD_STRUCTURE && !out);
    CHECK(stats(context).held_bytes==before.held_bytes);xr_xir_compile_checked_packet_free(&ordinary);
}
static void evaluation_controls(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *checked=NULL,*lowered=NULL,*out=NULL;
    CHECK(evaluation_source(&context,"scalar",&checked)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_specialize(checked,&out,NULL)==XR_XIR_BAD_STAGE && !out);
    XirComptimePending *pending=NULL;CHECK(xr_xir_compile_comptime_pending(checked,&pending,NULL)==XR_XIR_BAD_STAGE && !pending);
    CHECK(xr_xir_compile_lower(checked,&target, NULL,&lowered,NULL)==XR_XIR_OK && lowered->evaluation_only);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_BAD_STAGE && lowered && !program);
    XrXirVmBinding binding={0};XrXirCallEntry entry={0};
    CHECK(xr_xir_compile_vm_bind(lowered,lowered->module.provenance->evaluation.callee,&binding,&entry)==XR_XIR_BAD_STAGE);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"eval",65536,&c)==XR_XIR_BAD_STAGE && !c.text && !c.length);
    CHECK(xr_xir_compile_emit_leaf_c(lowered,"eval",65536,&c)==XR_XIR_BAD_STAGE && !c.text && !c.length);
    XrXirCtfeCallRequest call=import_request(lowered,"run",NULL);XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_calls(&call,&value)==XR_XIR_CTFE_BAD_ARGUMENT && !value.type && !value.payload);
    XrXirProvenance *proof=(XrXirProvenance *)lowered->module.provenance;XirEvaluationRoot root=proof->evaluation;
    ++proof->evaluation.instruction;CHECK(xr_xir_compile_artifact_verify(lowered,NULL)!=XR_XIR_OK);proof->evaluation=root;
    uint32_t kind=proof->kind;proof->kind=XR_XIR_EVIDENCE_INSTANCE;
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)!=XR_XIR_OK);proof->kind=kind;
    checked->evaluation_only=0;CHECK(xr_xir_compile_artifact_verify(checked,NULL)!=XR_XIR_OK);checked->evaluation_only=1;
    XrXirProvenance *original=(XrXirProvenance *)checked->module.provenance;XirEvaluationRoot original_root=original->evaluation;
    original->kind=XR_XIR_EVIDENCE_INSTANCE;original->evaluation=(XirEvaluationRoot){0};checked->evaluation_only=0;
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_BAD_TYPE);
    original->kind=XR_XIR_EVIDENCE_EVALUATION;original->evaluation=original_root;checked->evaluation_only=1;
    ++original->evaluation.instruction;CHECK(xr_xir_compile_artifact_verify(checked,NULL)!=XR_XIR_OK);original->evaluation=original_root;
    original->source->evaluation_only=1;CHECK(xr_xir_compile_artifact_verify(checked,NULL)!=XR_XIR_OK);original->source->evaluation_only=0;
    XrXirCheckedPacket packet={0};CHECK(xir_evaluation_write(checked,&packet,NULL)==XR_XIR_OK);
    evaluation_wire_controls(&context,checked,&packet);
    packet.bytes[64]^=1;CHECK(xir_evaluation_read(&context,packet.bytes,packet.length,&out,NULL)!=XR_XIR_OK && !out);packet.bytes[64]^=1;
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK && xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Evaluation purpose/transport/root/identity tamper, non-root execution, VM/native Program reject physical0\n");
}
