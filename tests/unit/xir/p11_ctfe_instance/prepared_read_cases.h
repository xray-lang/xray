/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * prepared_read_cases.h - Current wire equality on reused physical capacity
 */
#include "xir/xxir_read_attempt.h"

static XrXirStatus prepared_packet_attempt(const XrXirCompileContext *context,
    const XrXirArtifact *artifact,const XrXirCheckedPacket *packet,XirReadOperation *operation) {
    XrXirStatus status=xir_read_begin(context,operation);if(status!=XR_XIR_OK)return status;
    for(XirReadScratchBlock *b=operation->capacity.memory;b;b=b->next) memset(b+1,0xD7,b->bytes);
    XirReadAttempt attempt={true};const XirReadRequest request={.operation=operation,.attempt=&attempt};
    size_t allocations=source_fixture_compile_attempts;
    uint64_t held=stats(context).held_bytes;
    status=xir_checked_match_prepared(context,artifact,packet,&request);
    CHECK(source_fixture_compile_attempts==allocations && stats(context).held_bytes==held && !operation->capacity.active);
    return status;
}
static void prepared_packet_mode(bool evaluation) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *artifact=NULL;
    if(evaluation) CHECK(evaluation_source(&context,"scalar",&artifact)==XR_XIR_OK && artifact);
    else artifact=instance_source(&context,"scalar");
    XrXirCheckedPacket packet={0};
    CHECK((evaluation?xir_evaluation_write(artifact,&packet,NULL,NULL):
        xr_xir_compile_checked_write(artifact,&packet,NULL))==XR_XIR_OK && packet.length>76);
    CHECK(!memcmp(packet.bytes,evaluation?"XRCEV\0\0\0":"XRCHK\0\0\0",8));
    uint64_t held=stats(&context).held_bytes;
    XirReadOperation operation={.capacity={.resources=context.resources}};
    CHECK(prepared_packet_attempt(&context,artifact,&packet,&operation)==XR_XIR_BUDGET && operation.capacity.required);
    CHECK(xir_read_prepare(&context,&operation,NULL)==XR_XIR_OK && operation.capacity.blocks==1);
    CHECK(prepared_packet_attempt(&context,artifact,&packet,&operation)==XR_XIR_OK);
    CHECK(!memcmp(operation.capacity.memory+1,packet.bytes,packet.length));
    CHECK(prepared_packet_attempt(&context,artifact,&packet,&operation)==XR_XIR_OK);
    CHECK(!memcmp(operation.capacity.memory+1,packet.bytes,packet.length));
    XrXirCompileContext narrow=context;narrow.limits.functions=artifact->module.function_count-1;
    CHECK(prepared_packet_attempt(&narrow,artifact,&packet,&operation)==XR_XIR_BUDGET && !operation.capacity.required);
    CHECK(prepared_packet_attempt(&context,artifact,&packet,&operation)==XR_XIR_OK);
    uint8_t old=packet.bytes[32];packet.bytes[32]^=1;
    CHECK(prepared_packet_attempt(&context,artifact,&packet,&operation)==XR_XIR_BAD_STRUCTURE);
    packet.bytes[32]=old;
    CHECK(prepared_packet_attempt(&context,artifact,&packet,&operation)==XR_XIR_OK);
    XrXirCheckedPacket overlap={(uint8_t *)(operation.capacity.memory+1),packet.length};
    CHECK(prepared_packet_attempt(&context,artifact,&overlap,&operation)==XR_XIR_BAD_STRUCTURE);
    CHECK(prepared_packet_attempt(&context,artifact,&packet,&operation)==XR_XIR_OK);
    printf("prepared packet evaluation%u: %zu complete wire bytes, poison overwrite, zero provider callbacks, current limits, digest drift, overlap, recovery\n",
        evaluation,packet.length);
    xir_read_operation_free(&operation);CHECK(stats(&context).held_bytes==held);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(artifact);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void prepared_current_views(void) {
    XrXirCompileContext context;context_new(&context);XirCtfeInstanceBatch *owner=literal_batch(&context,"scalar");
    XirCtfeInstanceView views[2]={0};
    CHECK(xr_xir_compile_ctfe_instance_batch_views(owner,views,2,NULL)==XR_XIR_CTFE_OK);
    uint32_t functions=views[0].checked->module.function_count;
    memset(views,0,sizeof(views));uint64_t held=stats(&context).held_bytes;
    XirReadOperation operation={.capacity={.resources=context.resources}};
    const XirReadRequest request={.operation=&operation};
    XrXirCompileContext narrow=context;narrow.limits.functions=functions-1;
    CHECK(xir_ctfe_instance_batch_views_read(&narrow,owner,views,2,&request)==XR_XIR_CTFE_COMPLEXITY_LIMIT);
    CHECK(!views[0].checked && !views[1].checked);
    CHECK(xir_ctfe_instance_batch_views_read(&context,owner,views,2,&request)==XR_XIR_CTFE_OK);
    CHECK(views[0].value->payload==42 && views[1].value->payload==42);
    xir_read_operation_free(&operation);CHECK(stats(&context).held_bytes==held);
    xr_xir_compile_ctfe_instance_batch_free(owner);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("prepared receiver: same ledger, narrower current limits rejected, no partial views, original policy recovery, physical0");
}
