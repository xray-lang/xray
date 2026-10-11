/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * reader_lower_cases.h - Immediate owned reader transition and fresh public checks
 */
#include "../xir_construction_fixture.h"
#include "base/xsha256.h"

static const XrCompileResourceLimits reader_lower_caps = {67108864,8388608,128000000};
static XrXirStatus reader_lower_packet(const XrXirCompileContext *context, unsigned kind,
    XrXirCheckedPacket *packet) {
    XrXirArtifact *checked = NULL, *closed = NULL;
    XrXirStatus status = kind ? generic_built(context,&checked) : xir_fixture_check(context, &module, &checked, NULL);
    if (status == XR_XIR_OK && kind) status = xr_xir_compile_specialize(checked,&closed,NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_write(kind ? closed : checked,packet,NULL);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(checked);
    return status;
}
static XrXirStatus reader_lower_pipeline(XrCompileResources *owner, unsigned kind) {
    XrXirCompileContext context = {owner,xr_xir_compile_default_limits()};
    XrXirCheckedPacket packet = {0};
    XrXirArtifact *lowered = NULL;
    XrXirStatus status = reader_lower_packet(&context,kind,&packet);
    if (status == XR_XIR_OK)
        status = xr_xir_compile_checked_read_lower(&context,packet.bytes,packet.length,&target,&lowered,NULL);
    if (status != XR_XIR_OK) CHECK(!lowered);
    else {
        CHECK(lowered && lowered->context.resources == owner);
        CHECK(lowered->module.stage == XR_XIR_LOWERED);
        CHECK(lowered->checked_packet.length == packet.length);
        CHECK(!memcmp(lowered->checked_packet.bytes,packet.bytes,packet.length));
        /* The decoded owner must survive the original packet producer. */
        xr_xir_compile_checked_packet_free(&packet);
        if (!kind) CHECK(lowered->module.functions[0].instructions[0].immediate == 42);
        else CHECK(lowered->module.provenance && lowered->module.provenance->source->module.function_count == 2);
        status = xr_xir_compile_artifact_verify(lowered,NULL);
    }
    xr_xir_compile_artifact_free(lowered);
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
/* Every occupied slot holds a live Artifact, not an invalid pointer sentinel.
 * Rejection precedes allocation, work charging, and any change to that owner. */
static void reader_lower_occupied(const XrXirCompileContext *context,
    const XrXirCheckedPacket *packet, XrXirArtifact *checked) {
    const XrXirConstruction *facts=xr_xir_compile_artifact_construction(checked);
    const XrXirModule *view=xr_xir_compile_artifact_module(checked);
    CHECK(facts && !xr_xir_compile_construction_count(facts));
    for (unsigned operation=0;operation<7;++operation) {
        XrXirArtifact *output=checked;
        XrCompileResourceStats before=stats(context->resources);
        size_t calls=attempts,blocks=live_count,bytes=live;
        XrXirDiagnostic diagnostic={0};XrXirStatus status;
        if (!operation) status=xr_xir_compile_check_v2(context,&module,facts,&output,&diagnostic);
        else if (operation==1) status=xr_xir_compile_recheck_v2(context,view,facts,&output,&diagnostic);
        else if (operation==2) status=xr_xir_compile_lower(checked,&target,&output,&diagnostic);
        else if (operation==3) status=xr_xir_compile_checked_read_lower(context,packet->bytes,packet->length,&target,&output,&diagnostic);
        else if (operation==4) {
            status=xr_xir_compile_checked_read(context,packet->bytes,packet->length,&output,&diagnostic);
            CHECK(status==XR_XIR_BAD_STRUCTURE && output==checked);
            status=xr_xir_compile_checked_read(context,(const void *)(uintptr_t)1,SIZE_MAX,&output,&diagnostic);
        }
        else if (operation==5) status=xr_xir_compile_specialize(checked,&output,&diagnostic);
        else {
            status=XR_XIR_BAD_STRUCTURE;
            for (unsigned fields=0;fields<3;++fields) {
                XrXirCheckedPacket occupied={fields==1?NULL:packet->bytes,fields==2?0:packet->length};
                XrXirCheckedPacket saved=occupied;
                status=xr_xir_compile_checked_write(checked,&occupied,&diagnostic);
                CHECK(status==XR_XIR_BAD_STRUCTURE && occupied.bytes==saved.bytes && occupied.length==saved.length);
            }
        }
        XrCompileResourceStats after=stats(context->resources);
        CHECK(status==XR_XIR_BAD_STRUCTURE && diagnostic.status==XR_XIR_BAD_STRUCTURE && output==checked);
        CHECK(attempts==calls && live_count==blocks && live==bytes);
        CHECK(after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes &&
            after.peak_bytes==before.peak_bytes && after.work==before.work);
    }
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
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
        reader_lower_occupied(&c,&packet,decoded);
        CHECK(xr_xir_compile_lower(decoded,&target,&b,NULL) == XR_XIR_OK); reader_lower_equal(a,b);
        CHECK(b->module.functions != decoded->module.functions &&
            b->module.functions[0].instructions != decoded->module.functions[0].instructions);
        if (kind) {
            CHECK(b->module.provenance != decoded->module.provenance &&
                b->module.provenance->origins != decoded->module.provenance->origins);
            CHECK(b->module.provenance->source != decoded->module.provenance->source &&
                b->module.provenance->source->module.functions != decoded->module.provenance->source->module.functions);
        }
        CHECK(a->checked_packet.length == packet.length && !memcmp(a->checked_packet.bytes,packet.bytes,packet.length));
        XrXirInstruction *instruction = (XrXirInstruction *)decoded->module.functions[0].instructions;
        XrXirOp saved = instruction->op; instruction->op = XR_XIR_INVALID;
        XrXirArtifact *out = NULL; uint64_t before = stats(owner).live_bytes;
        CHECK(xr_xir_compile_lower(decoded,&target,&out,NULL) == XR_XIR_BAD_STRUCTURE && !out);
        XrXirCheckedPacket refused = {0};
        CHECK(xr_xir_compile_checked_write(decoded,&refused,NULL) == XR_XIR_BAD_STRUCTURE);
        CHECK(!refused.bytes && !refused.length);
        CHECK(stats(owner).live_bytes == before); instruction->op = saved;
        if (kind) {
            XrXirArtifact *source = decoded->module.provenance->source;
            instruction = (XrXirInstruction *)source->module.functions[0].instructions;
            saved = instruction->op; instruction->op = XR_XIR_INVALID;
            CHECK(xr_xir_compile_lower(decoded,&target,&out,NULL) == XR_XIR_BAD_STRUCTURE && !out);
            CHECK(xr_xir_compile_checked_write(decoded,&refused,NULL) == XR_XIR_BAD_STRUCTURE);
            CHECK(!refused.bytes && !refused.length);
            CHECK(stats(owner).live_bytes == before); instruction->op = saved;
        }
        decoded->target.architecture = target.architecture;
        CHECK(xr_xir_compile_checked_write(decoded,&refused,NULL) == XR_XIR_BAD_LAYOUT);
        CHECK(!refused.bytes && !refused.length);
        CHECK(stats(owner).live_bytes == before); decoded->target.architecture = 0;
        out = NULL; CHECK(xr_xir_compile_lower(decoded,&target,&out,NULL) == XR_XIR_OK);
        reader_lower_equal(a,out); xr_xir_compile_artifact_free(out);
        xr_xir_compile_artifact_free(decoded); decoded = NULL;
        CHECK(xr_xir_compile_artifact_verify(b,NULL) == XR_XIR_OK);
        reader_lower_equal(a,b);
        xr_xir_compile_artifact_free(a); xr_xir_compile_artifact_free(b);
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
    return xir_fixture_check(context, &library, output, NULL);
}
static void reader_lower_rejections(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&reader_lower_caps,&owner) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext c = {owner,xr_xir_compile_default_limits()};
    XrXirArtifact *out = NULL;
    XrXirCheckedPacket packet = {0}; CHECK(reader_lower_packet(&c,0,&packet) == XR_XIR_OK);
    uint64_t baseline = stats(owner).live_bytes; XrXirTarget invalid = {0};
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_checked_read_lower(NULL,packet.bytes,packet.length,&target,&out,NULL) == XR_XIR_BAD_STRUCTURE && !out);
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&target,NULL,NULL) == XR_XIR_BAD_STRUCTURE);
    packet.bytes[0] ^= 1;
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&invalid,&out,NULL) == XR_XIR_BAD_STRUCTURE && !out);
    packet.bytes[0] ^= 1;
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&invalid,&out,NULL) == XR_XIR_BAD_LAYOUT && !out);
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,NULL,&out,NULL) == XR_XIR_BAD_LAYOUT && !out);
    /* Scalar schema: 12 module bytes, 8 name bytes, 3 counts, 16 block bytes and instruction count. */
    const size_t opcode = 64 + 12 + 8 + 12 + 16 + 4;
    CHECK(packet.length > opcode + 40 && packet.bytes[opcode] == XR_XIR_CONST_INT);
    uint8_t saved[4]; memcpy(saved,packet.bytes+opcode,4); memset(packet.bytes+opcode,0,4); reader_lower_digest(&packet);
    CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&invalid,&out,&diagnostic) == XR_XIR_BAD_STRUCTURE && !out);
    CHECK(diagnostic.function == 0 && diagnostic.instruction == 0);
    memcpy(packet.bytes+opcode,saved,4); reader_lower_digest(&packet);
    CHECK(stats(owner).live_bytes == baseline); xr_xir_compile_checked_packet_free(&packet);
    for (unsigned kind = 0; kind < 2; ++kind) {
        XrXirArtifact *checked = NULL, *decoded = NULL;
        CHECK((kind ? generic_built(&c,&checked) : reader_lower_library(&c,&checked)) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(checked,&packet,NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(&c,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
        baseline = stats(owner).live_bytes;
        CHECK(xr_xir_compile_checked_read_lower(&c,packet.bytes,packet.length,&target,&out,NULL) == XR_XIR_BAD_STAGE && !out);
        CHECK(stats(owner).live_bytes == baseline);
        xr_xir_compile_artifact_free(decoded); xr_xir_compile_artifact_free(checked); xr_xir_compile_checked_packet_free(&packet);
    }
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
    puts("reader/lower: valid Library and open generic refuse; malformed/semantic precede target; public tamper reverified");
}
/* The transferred owner retains its function storage; canonical bytes and
 * complete receiving layout must equal the independent borrowed transition. */
static XrXirStatus reader_lower_take_closed(const XrXirCompileContext *context,
    unsigned kind, XrXirArtifact **output) {
    if (!kind) return xir_fixture_check(context,&module,output,NULL);
    XrXirArtifact *template = NULL;
    XrXirStatus status = kind == 1 ? generic_built(context,&template) : nominal_built(context,&template);
    if (status == XR_XIR_OK) status = xr_xir_compile_specialize(template,output,NULL);
    xr_xir_compile_artifact_free(template); return status;
}
/* Both independent public admissions reject metadata mutations. The
 * complete artifact checker also compares every receiving physical row. */
static void reader_lower_layout_admission(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&reader_lower_caps,&owner)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={owner,xr_xir_compile_default_limits()};
    XrXirArtifact *closed=NULL, *lowered=NULL;
    CHECK(reader_lower_take_closed(&context,2,&closed)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed); closed=NULL;
    CHECK(lowered->module.types && lowered->module.types->count && lowered->module.declarations);
    uint64_t baseline=stats(owner).live_bytes;
    CHECK(xr_xir_compile_layout_verify(lowered)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    XrXirTypeNode *node=(XrXirTypeNode *)lowered->module.types->nodes;
    CHECK(node->kind==XR_XIR_TYPE_NOMINAL && node->result==XR_XIR_UNIT);
    node->result=XR_XIR_I64;
    CHECK(xr_xir_compile_layout_verify(lowered)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_BAD_STRUCTURE);
    node->result=XR_XIR_UNIT;
    XrXirFunctionIdentity *identity=(XrXirFunctionIdentity *)lowered->module.declarations->functions;
    uint32_t module_id=identity->module; identity->module=lowered->module.declarations->module_count;
    CHECK(xr_xir_compile_layout_verify(lowered)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_BAD_STRUCTURE);
    identity->module=module_id;
    XrXirFunctionLayout *layout=&lowered->layouts[0];
    uint32_t *fields[]={ (uint32_t *)layout->offsets, &layout->owned_count,
        &layout->outgoing_count, &layout->path_count, &layout->result.size, &layout->result.alignment };
    for (unsigned field=0;field<sizeof(fields)/sizeof(fields[0]);++field) {
        uint32_t saved=*fields[field]; *fields[field]^=1;
        CHECK(xr_xir_compile_layout_verify(lowered)==XR_XIR_BAD_LAYOUT);
        CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_BAD_LAYOUT);
        *fields[field]=saved;
        CHECK(stats(owner).live_bytes==baseline);
    }
    CHECK(xr_xir_compile_layout_verify(lowered)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    CHECK(stats(owner).live_bytes==baseline);
    xr_xir_compile_artifact_free(lowered); CHECK(live_count==1);
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
}

static void reader_lower_take_equal(const XrXirArtifact *a, const XrXirArtifact *b) {
    reader_lower_equal(a,b);
    CHECK(a->module.stage == b->module.stage && a->module.linkage_kind == b->module.linkage_kind);
    for (uint32_t f=0;f<a->module.function_count;++f) {
        const XrXirFunction *x=&a->module.functions[f], *y=&b->module.functions[f];
        CHECK(x->block_count==y->block_count && x->operand_count==y->operand_count);
        if (x->parameter_count) CHECK(!memcmp(x->parameters,y->parameters,
            (size_t)x->parameter_count*sizeof(*x->parameters)));
        CHECK(!memcmp(x->blocks,y->blocks,(size_t)x->block_count*sizeof(*x->blocks)));
        if (x->operand_count) CHECK(!memcmp(x->operands,y->operands,
            (size_t)x->operand_count*sizeof(*x->operands)));
        if (x->parameter_count) CHECK(!memcmp(a->layouts[f].parameters,b->layouts[f].parameters,
            (size_t)x->parameter_count*sizeof(*a->layouts[f].parameters)));
    }
}
static void reader_lower_take_requests(const XrXirCompileContext *context, XrXirArtifact *checked) {
    XrXirArtifact *other=NULL;
    CHECK(reader_lower_take_closed(context,0,&other)==XR_XIR_OK);
    for (unsigned request=0;request<7;++request) {
        XrXirArtifact *input=checked, *output=request==4?checked:request==5?other:NULL, *empty=NULL;
        XrCompileResourceStats before=stats(context->resources);
        size_t sites=attempts, blocks=live_count, bytes=live;
        XrXirDiagnostic diagnostic={0}; XrXirStatus status;
        if (!request) status=xr_xir_compile_lower_take(NULL,&target,&output,&diagnostic);
        else if (request==1) status=xr_xir_compile_lower_take(&empty,&target,&output,&diagnostic);
        else if (request==2) status=xr_xir_compile_lower_take(&input,&target,NULL,&diagnostic);
        else if (request==3) status=xr_xir_compile_lower_take(&input,&target,&input,&diagnostic);
        else if (request==6) {
            XrCompileResources *saved=checked->context.resources; checked->context.resources=NULL;
            status=xr_xir_compile_lower_take(&input,&target,&output,&diagnostic);
            checked->context.resources=saved;
        } else status=xr_xir_compile_lower_take(&input,&target,&output,&diagnostic);
        CHECK(status==XR_XIR_BAD_STRUCTURE && diagnostic.status==status && input==checked && !empty);
        CHECK(output==(request==4?checked:request==5?other:NULL));
        XrCompileResourceStats after=stats(context->resources);
        CHECK(attempts==sites && live_count==blocks && live==bytes);
        CHECK(after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes &&
            after.peak_bytes==before.peak_bytes && after.work==before.work);
    }
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(other,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(other);
}
static void reader_lower_take_canonical(unsigned kind) {
    reset(SIZE_MAX); XrCompileResources *owner=NULL;
    CHECK(xr_compile_resources_new(&reader_lower_caps,&owner)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={owner,xr_xir_compile_default_limits()};
    XrXirArtifact *closed=NULL, *borrowed=NULL, *taken=NULL;
    CHECK(reader_lower_take_closed(&context,kind,&closed)==XR_XIR_OK);
    reader_lower_take_requests(&context,closed);
    CHECK(xr_xir_compile_lower(closed,&target,&borrowed,NULL)==XR_XIR_OK);
    XrXirArtifact *identity=closed;
    const XrXirFunction *functions=closed->module.functions;
    CHECK(xr_xir_compile_lower_take(&closed,&target,&taken,NULL)==XR_XIR_OK);
    CHECK(!closed && taken==identity && taken!=borrowed && taken->module.functions==functions);
    CHECK(taken->context.resources==owner && borrowed->context.resources==owner);
    reader_lower_take_equal(borrowed,taken);
    xr_xir_compile_artifact_free(borrowed);
    CHECK(xr_xir_compile_artifact_verify(taken,NULL)==XR_XIR_OK);
    XrXirInstruction *op=(XrXirInstruction *)taken->module.functions[0].instructions;
    XrXirOp saved=op->op; op->op=XR_XIR_INVALID;
    CHECK(xr_xir_compile_artifact_verify(taken,NULL)==XR_XIR_BAD_STRUCTURE); op->op=saved;
    uint32_t *offsets=(uint32_t *)taken->layouts[0].offsets; uint32_t saved_offset=offsets[0];
    offsets[0]=UINT32_MAX;
    CHECK(xr_xir_compile_artifact_verify(taken,NULL)==XR_XIR_BAD_LAYOUT); offsets[0]=saved_offset;
    taken->checked_identity[0]^=1;
    CHECK(xr_xir_compile_artifact_verify(taken,NULL)==XR_XIR_BAD_STRUCTURE); taken->checked_identity[0]^=1;
    CHECK(xr_xir_compile_artifact_verify(taken,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(taken); CHECK(live_count==1);
    xr_compile_resources_release(owner); CHECK(!live && !live_count);
}
/* Task<U8> is a valid definition but its executable await result is outside
 * the current receiving Task ABI. Only final Lowered verification rejects it. */
static XrXirStatus reader_lower_take_unsupported(const XrXirCompileContext *context, XrXirArtifact **output) {
    const XrXirTypeNode node={.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_U8};
    const XrXirTypes types={&node,1,NULL,NULL}; const XrXirType parameter=(XrXirType)256;
    const XrXirInstruction body[]={
        {.op=XR_XIR_TASK_AWAIT,.args={0},.targets={1,2}},
        {.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_U8}, {.op=XR_XIR_RETURN,.args={2}},
        {.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR}, {.op=XR_XIR_THROW,.args={4}}};
    const XrXirBlock blocks[]={{0,1,0,0},{1,2,0,0},{3,2,0,0}};
    const XrXirFunction entry={"awaitByte",9,&parameter,1,XR_XIR_U8,blocks,3,body,5,NULL,0};
    const XrXirModule built={XR_XIR_BUILT,&entry,1,NULL,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    return xir_fixture_check(context,&built,output,NULL);
}
static void reader_lower_take_rejections(void) {
    for (unsigned refusal=0;refusal<10;++refusal) {
        reset(SIZE_MAX); XrCompileResources *owner=NULL;
        CHECK(xr_compile_resources_new(&reader_lower_caps,&owner)==XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context={owner,xr_xir_compile_default_limits()};
        XrXirArtifact *closed=NULL, *output=NULL;
        XrXirStatus prepared=refusal==3?reader_lower_library(&context,&closed):
            refusal==4?generic_built(&context,&closed):refusal==9?reader_lower_take_unsupported(&context,&closed):
            reader_lower_take_closed(&context,0,&closed);
        CHECK(prepared==XR_XIR_OK && closed);
        XrXirTarget invalid={0}; const XrXirTarget *receiving=refusal==0?&invalid:refusal==1?NULL:&target;
        XrXirStatus expected=refusal<2?XR_XIR_BAD_LAYOUT:refusal<5?XR_XIR_BAD_STAGE:
            refusal==6?XR_XIR_BAD_TYPE:refusal==7?XR_XIR_BAD_LAYOUT:refusal==9?XR_XIR_UNSUPPORTED:XR_XIR_BAD_STRUCTURE;
        if (refusal==2) closed->module.stage=XR_XIR_BUILT;
        if (refusal==5) ((XrXirInstruction *)closed->module.functions[0].instructions)[0].op=XR_XIR_INVALID;
        if (refusal==6) ((XrXirFunction *)closed->module.functions)[0].result=XR_XIR_BOOL;
        if (refusal==7) closed->target=target;
        if (refusal==8) CHECK(xr_xir_compile_checked_write(closed,&closed->checked_packet,NULL)==XR_XIR_OK);
        XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_lower_take(&closed,receiving,&output,&diagnostic);
        if (status!=expected) fprintf(stderr,"take refusal=%u actual=%u expected=%u f=%u b=%u i=%u\n",
            refusal,(unsigned)status,(unsigned)expected,diagnostic.function,diagnostic.block,diagnostic.instruction);
        CHECK(status==expected && !closed && !output && diagnostic.status==status);
        CHECK(live_count==1); (void)stats(owner);
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
}
static XrXirStatus reader_lower_take_pipeline(XrCompileResources *owner, unsigned kind) {
    XrXirCompileContext context={owner,xr_xir_compile_default_limits()};
    XrXirArtifact *closed=NULL, *output=NULL;
    XrXirStatus status=reader_lower_take_closed(&context,kind,&closed);
    if (status==XR_XIR_OK) {
        status=xr_xir_compile_lower_take(&closed,&target,&output,NULL); CHECK(!closed);
        if (status==XR_XIR_OK) {
            CHECK(output && output->context.resources==owner && output->module.stage==XR_XIR_LOWERED);
            status=xr_xir_compile_artifact_verify(output,NULL);
        } else CHECK(!output);
    } else CHECK(!closed && !output);
    xr_xir_compile_artifact_free(output); xr_xir_compile_artifact_free(closed);
    CHECK(live_count==1); (void)stats(owner); return status;
}
static void reader_lower_take_boundaries(unsigned kind) {
    reset(0); XrCompileResources *bootstrap=NULL;
    CHECK(xr_compile_resources_new(&reader_lower_caps,&bootstrap)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    CHECK(!bootstrap && attempts==1 && !live && !live_count);
    reset(SIZE_MAX); XrCompileResources *owner=NULL;
    CHECK(xr_compile_resources_new(&reader_lower_caps,&owner)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial=stats(owner);
    CHECK(reader_lower_take_pipeline(owner,kind)==XR_XIR_OK);
    size_t count=attempts; XrCompileResourceStats measured=stats(owner);
    CHECK(count>1 && count<4096); xr_compile_resources_release(owner); CHECK(!live && !live_count);
    for (size_t failure=1;failure<count;++failure) {
        reset(failure); owner=NULL;
        CHECK(xr_compile_resources_new(&reader_lower_caps,&owner)==XR_COMPILE_RESOURCE_OK);
        CHECK(reader_lower_take_pipeline(owner,kind)==XR_XIR_OUT_OF_MEMORY && attempts==failure+1);
        XrCompileResourceStats failed=stats(owner);
        size_t failed_sites=attempts;
        fail_at=SIZE_MAX; CHECK(reader_lower_take_pipeline(owner,kind)==XR_XIR_OK);
        XrCompileResourceStats retried=stats(owner);
        CHECK(retried.allocated_bytes-failed.allocated_bytes==measured.allocated_bytes-initial.allocated_bytes &&
            retried.work-failed.work==measured.work-initial.work && attempts-failed_sites==count-1);
        CHECK(live_count==1); xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
    uint64_t live_boundary=measured.peak_bytes;
    for (;;) {
        CHECK(live_boundary>initial.live_bytes);
        reset(SIZE_MAX); owner=NULL; XrCompileResourceLimits limits=reader_lower_caps;
        limits.live_bytes=live_boundary-1;
        CHECK(xr_compile_resources_new(&limits,&owner)==XR_COMPILE_RESOURCE_OK);
        XrXirStatus status=reader_lower_take_pipeline(owner,kind);
        XrCompileResourceStats actual=stats(owner);
        CHECK(status==XR_XIR_OK || status==XR_XIR_BUDGET);
        CHECK(actual.peak_bytes<=limits.live_bytes && actual.allocated_bytes<limits.allocated_bytes &&
            actual.work<limits.work);
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
        if (status==XR_XIR_BUDGET) break;
        CHECK(actual.peak_bytes<live_boundary); live_boundary=actual.peak_bytes;
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        reset(SIZE_MAX); owner=NULL; XrCompileResourceLimits limits=reader_lower_caps;
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=live_boundary-minus;
        else limits.work=measured.work-minus;
        CHECK(xr_compile_resources_new(&limits,&owner)==XR_COMPILE_RESOURCE_OK);
        CHECK(reader_lower_take_pipeline(owner,kind)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        xr_compile_resources_release(owner); CHECK(!live && !live_count);
    }
    printf("take/lower kind=%u actualOOM=%zu allocated=%llu peak=%llu work=%llu allsites retry sameLedger axes6 physical0\n",
        kind,count-1,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,
        (unsigned long long)measured.work);
}
static void reader_lower_take_cases(void) {
    reader_lower_take_rejections();
    for (unsigned kind=0;kind<3;++kind) {
        reader_lower_take_canonical(kind); reader_lower_take_boundaries(kind);
    }
}
static void reader_lower_cases(void) {
    reader_lower_rejections(); reader_lower_canonical();
    for (unsigned kind = 0; kind < 2; ++kind) reader_lower_boundaries(kind);
    reader_lower_take_cases(); reader_lower_layout_admission();
}
