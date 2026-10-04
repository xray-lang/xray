/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * reader_lower_cases.h - Immediate owned reader transition and fresh public checks
 */
#include "base/xsha256.h"

static const XrCompileResourceLimits reader_lower_caps = {67108864,8388608,128000000};
static XrXirStatus reader_lower_packet(const XrXirCompileContext *context, unsigned kind,
    XrXirCheckedPacket *packet) {
    XrXirArtifact *checked = NULL, *closed = NULL;
    XrXirStatus status = kind ? generic_built(context,&checked) : xr_xir_compile_check(context,&module,&checked,NULL);
    if (status == XR_XIR_OK && kind) status = xr_xir_compile_specialize(checked,&closed,NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_write(kind ? closed : checked,packet,NULL);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(checked);
    return status;
}
static XrXirStatus reader_lower_pipeline(XrCompileResources *owner, unsigned kind) {
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    XrXirCheckedPacket packet = {0};
    XrXirArtifact *sentinel = (XrXirArtifact *)&context, *lowered = sentinel;
    XrXirStatus status = reader_lower_packet(&context,kind,&packet);
    if (status == XR_XIR_OK)
        status = xr_xir_compile_checked_read_lower(&context,packet.bytes,packet.length,&target,&lowered,NULL);
    if (status != XR_XIR_OK) CHECK(lowered == sentinel);
    else {
        CHECK(lowered && lowered != sentinel && lowered->context.resources == owner);
        CHECK(lowered->module.stage == XR_XIR_LOWERED);
        CHECK(lowered->checked_packet.length == packet.length);
        CHECK(!memcmp(lowered->checked_packet.bytes,packet.bytes,packet.length));
        if (!kind) CHECK(lowered->module.functions[0].instructions[0].immediate == 42);
        else CHECK(lowered->module.provenance && lowered->module.provenance->source->module.function_count == 2);
        status = xr_xir_compile_artifact_verify(lowered,NULL);
    }
    if (lowered != sentinel) xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(live_count == 1); (void)stats(owner);
    return status;
}
static void reader_lower_boundaries(unsigned kind) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&reader_lower_caps,&owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(reader_lower_pipeline(owner,kind) == XR_XIR_OK);
    size_t count = attempts; XrCompileResourceStats measured = stats(owner);
    CHECK(count > 1 && count < 4096); xr_compile_resources_release(owner);
    CHECK(!live && !live_count);
    for (size_t failure = 1; failure < count; ++failure) {
        reset(failure); owner = NULL;
        CHECK(xr_compile_resources_new(&reader_lower_caps,&owner) == XR_COMPILE_RESOURCE_OK);
        CHECK(reader_lower_pipeline(owner,kind) == XR_XIR_OUT_OF_MEMORY && attempts == failure + 1);
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
    for (unsigned metric = 0; metric < 3; ++metric) for (unsigned below = 0; below < 2; ++below) {
        reset(SIZE_MAX); owner = NULL; XrCompileResourceLimits limits = reader_lower_caps;
        if (!metric) limits.allocated_bytes = measured.allocated_bytes - below;
        else if (metric == 1) limits.live_bytes = measured.peak_bytes - below;
        else limits.work = measured.work - below;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
        CHECK(reader_lower_pipeline(owner,kind) == (below ? XR_XIR_BUDGET : XR_XIR_OK));
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
    uint64_t step = measured.work / 128 + 1;
    for (uint64_t work = 1; work < measured.work; work += step) {
        reset(SIZE_MAX); owner = NULL; XrCompileResourceLimits limits = reader_lower_caps; limits.work = work;
        CHECK(xr_compile_resources_new(&limits,&owner) == XR_COMPILE_RESOURCE_OK);
        CHECK(reader_lower_pipeline(owner,kind) == XR_XIR_BUDGET);
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
    printf("reader/lower kind=%u actual OOM=%zu work=%llu; finite axes and physical zero\n",
        kind,count-1,(unsigned long long)measured.work);
}
static void reader_lower_equal(const XrXirArtifact *a, const XrXirArtifact *b) {
    CHECK(a->module.function_count == b->module.function_count);
    CHECK(a->target.architecture == b->target.architecture && a->target.abi_version == b->target.abi_version);
    CHECK(a->checked_packet.length == b->checked_packet.length);
    CHECK(!memcmp(a->checked_packet.bytes,b->checked_packet.bytes,a->checked_packet.length));
    CHECK(!memcmp(a->checked_identity,b->checked_identity,32));
    for (uint32_t f = 0; f < a->module.function_count; ++f) {
        const XrXirFunction *x = &a->module.functions[f], *y = &b->module.functions[f];
        CHECK(x->name_length == y->name_length && !memcmp(x->name,y->name,x->name_length));
        CHECK(x->result == y->result && x->parameter_count == y->parameter_count);
        CHECK(x->instruction_count == y->instruction_count);
        CHECK(!memcmp(x->instructions,y->instructions,(size_t)x->instruction_count * sizeof(*x->instructions)));
        const XrXirFunctionLayout *u = &a->layouts[f], *v = &b->layouts[f];
        CHECK(u->slot_count == v->slot_count && u->frame_bytes == v->frame_bytes);
        CHECK(u->result.size == v->result.size && u->result.alignment == v->result.alignment);
        CHECK(u->owned_count == v->owned_count && u->outgoing_count == v->outgoing_count && u->path_count == v->path_count);
        if (u->slot_count) CHECK(!memcmp(u->offsets,v->offsets,(size_t)u->slot_count * sizeof(*u->offsets)));
        if (u->owned_count) CHECK(!memcmp(u->owned_offsets,v->owned_offsets,(size_t)u->owned_count * sizeof(*u->owned_offsets)));
    }
}
static void reader_lower_canonical(void) {
    for (unsigned kind = 0; kind < 2; ++kind) {
        reset(SIZE_MAX); XrCompileResources *owner = NULL;
        CHECK(xr_compile_resources_new(&reader_lower_caps,&owner) == XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext c = {owner,xr_xir_compile_default_limits()};
        XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *a = NULL, *b = NULL;
        CHECK(reader_lower_packet(&c,kind,&packet) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&target,&a,NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(&c,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_lower(decoded,&target,&b,NULL) == XR_XIR_OK); reader_lower_equal(a,b);
        CHECK(a->checked_packet.length == packet.length && !memcmp(a->checked_packet.bytes,packet.bytes,packet.length));
        XrXirInstruction *instruction = (XrXirInstruction *)decoded->module.functions[0].instructions;
        XrXirOp saved = instruction->op; instruction->op = XR_XIR_INVALID;
        XrXirArtifact *out = decoded; uint64_t before = stats(owner).live_bytes;
        CHECK(xr_xir_compile_lower(decoded,&target,&out,NULL) == XR_XIR_BAD_STRUCTURE && out == decoded);
        XrXirCheckedPacket refused = {packet.bytes,packet.length};
        CHECK(xr_xir_compile_checked_write(decoded,&refused,NULL) == XR_XIR_BAD_STRUCTURE);
        CHECK(refused.bytes == packet.bytes && refused.length == packet.length);
        CHECK(stats(owner).live_bytes == before); instruction->op = saved;
        if (kind) {
            XrXirArtifact *source = decoded->module.provenance->source;
            instruction = (XrXirInstruction *)source->module.functions[0].instructions;
            saved = instruction->op; instruction->op = XR_XIR_INVALID;
            CHECK(xr_xir_compile_lower(decoded,&target,&out,NULL) == XR_XIR_BAD_STRUCTURE && out == decoded);
            CHECK(xr_xir_compile_checked_write(decoded,&refused,NULL) == XR_XIR_BAD_STRUCTURE);
            CHECK(refused.bytes == packet.bytes && refused.length == packet.length);
            CHECK(stats(owner).live_bytes == before); instruction->op = saved;
        }
        decoded->target.architecture = target.architecture;
        CHECK(xr_xir_compile_checked_write(decoded,&refused,NULL) == XR_XIR_BAD_LAYOUT);
        CHECK(refused.bytes == packet.bytes && refused.length == packet.length);
        CHECK(stats(owner).live_bytes == before); decoded->target.architecture = 0;
        out = NULL; CHECK(xr_xir_compile_lower(decoded,&target,&out,NULL) == XR_XIR_OK);
        reader_lower_equal(a,out); xr_xir_compile_artifact_free(out);
        xr_xir_compile_artifact_free(a); xr_xir_compile_artifact_free(b); xr_xir_compile_artifact_free(decoded);
        xr_compile_resources_release(owner);
        CHECK(packet.length >= 64 && !memcmp(packet.bytes,"XRCHK",5));
        xr_xir_compile_checked_packet_free(&packet); CHECK(!live && !live_count);
    }
}
static void reader_lower_digest(XrXirCheckedPacket *packet) {
    XrSHA256Context sha; xr_sha256_init(&sha);
    xr_sha256_update(&sha,packet->bytes,32);
    xr_sha256_update(&sha,packet->bytes+64,packet->length-64);
    xr_sha256_final(&sha,packet->bytes+32);
}
static XrXirStatus reader_lower_library(const XrXirCompileContext *context, XrXirArtifact **output) {
    const XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirBlock block = {0,1,0,0};
    XrXirFunction functions[] = {{"init",4,NULL,0,XR_XIR_UNIT,&block,1,&init,1,NULL,0},function};
    const XrXirSourceModule source = {"reader/library",14,NULL,0,0};
    const XrXirFunctionIdentity identities[] = {{0},{.exported=1}};
    const XrXirDeclarations declarations = {&source,1,identities,NULL,0,NULL,0,UINT32_MAX,UINT32_MAX,NULL};
    const XrXirModule library = {XR_XIR_BUILT,functions,2,&declarations,NULL,NULL,NULL,XR_XIR_LIBRARY,NULL};
    return xr_xir_compile_check(context,&library,output,NULL);
}
static void reader_lower_rejections(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&reader_lower_caps,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext c = {owner,xr_xir_compile_default_limits()};
    XrXirArtifact *sentinel = (XrXirArtifact *)&c, *out = sentinel;
    XrXirCheckedPacket packet = {0}; CHECK(reader_lower_packet(&c,0,&packet) == XR_XIR_OK);
    uint64_t baseline = stats(owner).live_bytes; XrXirTarget invalid = {0};
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_checked_read_lower(NULL,packet.bytes,packet.length,&target,&out,NULL) == XR_XIR_BAD_STRUCTURE && out == sentinel);
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&target,NULL,NULL) == XR_XIR_BAD_STRUCTURE);
    packet.bytes[0] ^= 1;
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&invalid,&out,NULL) == XR_XIR_BAD_STRUCTURE && out == sentinel);
    packet.bytes[0] ^= 1;
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&invalid,&out,NULL) == XR_XIR_BAD_LAYOUT && out == sentinel);
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,NULL,&out,NULL) == XR_XIR_BAD_LAYOUT && out == sentinel);
    /* Scalar schema: 12 module bytes, 8 name bytes, 3 counts, 16 block bytes and instruction count. */
    const size_t opcode = 64 + 12 + 8 + 12 + 16 + 4;
    CHECK(packet.length > opcode + 40 && packet.bytes[opcode] == XR_XIR_CONST_INT);
    uint8_t saved[4]; memcpy(saved,packet.bytes+opcode,4); memset(packet.bytes+opcode,0,4); reader_lower_digest(&packet);
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&invalid,&out,&diagnostic) == XR_XIR_BAD_STRUCTURE && out == sentinel);
    CHECK(diagnostic.function == 0 && diagnostic.instruction == 0);
    memcpy(packet.bytes+opcode,saved,4); reader_lower_digest(&packet);
    CHECK(stats(owner).live_bytes == baseline); xr_xir_compile_checked_packet_free(&packet);
    for (unsigned kind = 0; kind < 2; ++kind) {
        XrXirArtifact *checked = NULL, *decoded = NULL;
        CHECK((kind ? generic_built(&c,&checked) : reader_lower_library(&c,&checked)) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(checked,&packet,NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(&c,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
        baseline = stats(owner).live_bytes;
        CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&target,&out,NULL) == XR_XIR_BAD_STAGE && out == sentinel);
        CHECK(stats(owner).live_bytes == baseline);
        xr_xir_compile_artifact_free(decoded); xr_xir_compile_artifact_free(checked); xr_xir_compile_checked_packet_free(&packet);
    }
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
    puts("reader/lower: valid Library and open generic refuse; malformed/semantic precede target; public tamper reverified");
}
static void reader_lower_cases(void) {
    reader_lower_rejections(); reader_lower_canonical();
    for (unsigned kind = 0; kind < 2; ++kind) reader_lower_boundaries(kind);
}
