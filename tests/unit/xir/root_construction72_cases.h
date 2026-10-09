/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * root_construction72_cases.h - Independent recursive construction wire ownership
 */
#ifndef ROOT_CONSTRUCTION72_CASES_H
#define ROOT_CONSTRUCTION72_CASES_H
#include "xir/xxir_internal.h"
#include "xir/xxir_nominal.h"
#include "base/xsha256.h"
#include "root_construction72_goldens.h"
_Static_assert(XR_XIR_CONST_INT == 2 && XR_XIR_ADD_INT == 25 && XR_XIR_CALL == 28 &&
    XR_XIR_RETURN == 33 && XR_XIR_STRUCT_NEW == 82 && XR_XIR_STRUCT_GET == 83,
    "Independent construction vector opcode identities changed");

static XrXirStatus root_construction72_build(const XrXirCompileContext *context,
    XrXirArtifact **output) {
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction entry[] = {
        {XR_XIR_CALL,(XrXirType)256,{0},{0},2,{0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{1,2},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{3},{0},0,{0}}};
    XrXirInstruction make[] = {
        {XR_XIR_CALL,XR_XIR_I64,{0},{0},3,{0}},
        {XR_XIR_CALL,XR_XIR_I64,{0},{0},4,{0}},
        {XR_XIR_STRUCT_NEW,(XrXirType)256,{0,2},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    XrXirInstruction a[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},19,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction b[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},23,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock blocks[] = {{0,1,0,0},{0,5,0,0},{0,4,0,0},{0,2,0,0}};
    uint32_t operands[] = {0,1};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,&init,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&blocks[1],1,entry,5,NULL,0},
        {"make",4,NULL,0,(XrXirType)256,&blocks[2],1,make,4,operands,2},
        {"a",1,NULL,0,XR_XIR_I64,&blocks[3],1,a,2,NULL,0},
        {"b",1,NULL,0,XR_XIR_I64,&blocks[3],1,b,2,NULL,0}};
    XrXirSourceModule module = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[] = {
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER,0,0},
        {0,1,0,0,0,0,XR_XIR_NON_MEMBER,0,0},
        {0,1,1,0,0,0,XR_XIR_CONSTRUCTOR,0,0},
        {0,1,1,0,0,0,XR_XIR_MEMBER_HELPER,0,0},
        {0,1,1,0,0,0,XR_XIR_MEMBER_HELPER,0,0}};
    XrXirDeclarations declarations = {&module,1,identities,NULL,0,NULL,0,0,1,NULL};
    XrXirNominalField fields[] = {{{"a",1},XR_XIR_I64,0},{{"b",1},XR_XIR_I64,0}};
    XrXirNominalDeclaration nominal = {{"alpha",5},{"Pair",4},1,NULL,0,fields,2,
        XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}};
    XrXirNominalTable table = {&nominal,1,NULL};
    XrXirTypeNode node = {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,
        {0,NULL,0,NULL,0}};
    XrXirTypes types = {&node,1,&table,NULL};
    XrXirGeneric generics[5] = {{0}};
    XrXirModule built = {XR_XIR_BUILT,functions,5,&declarations,generics,&types,
        NULL,XR_XIR_PROGRAM,NULL};
    uint32_t helpers[] = {4,5};
    XrXirConstructionRow row = {3,helpers,2};
    XrXirConstruction *facts = NULL;
    XrXirStatus status = xr_xir_compile_construction_new(context,&types,&row,1,&facts);
    if (status == XR_XIR_OK) status = xr_xir_compile_check_v2(context,&built,facts,output,NULL);
    xr_xir_compile_construction_free(facts);
    memset(helpers,0xCC,sizeof(helpers)); memset(fields,0xCC,sizeof(fields));
    memset(functions,0xCC,sizeof(functions)); memset(identities,0xCC,sizeof(identities));
    return status;
}

static void root_construction72_rows(const XrXirArtifact *artifact, bool original) {
    const XrXirConstruction *facts = xr_xir_compile_artifact_construction(artifact);
    CHECK(facts && xr_xir_compile_construction_count(facts) == 1);
    const XrXirConstructionRow *row = xr_xir_compile_construction_row(facts,0);
    CHECK(row && row->field_count == 2 && row->field_initializers);
    CHECK(row->default_initializer == (original ? 3u : 0u));
    CHECK(row->field_initializers[0] == (original ? 4u : 0u));
    CHECK(row->field_initializers[1] == (original ? 5u : 0u));
}

static XrXirStatus root_construction72_pipeline(XrCompileResources *resources) {
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    XrXirArtifact *checked = NULL, *instance = NULL, *decoded = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    XrXirStatus status = root_construction72_build(&context,&checked);
    if (status == XR_XIR_OK) {
        root_construction72_rows(checked,true);
        status = xr_xir_compile_checked_write(checked,&packet,NULL);
    }
    if (status == XR_XIR_OK) {
        CHECK(packet.length == sizeof(root_construction72_checked));
        CHECK(!memcmp(packet.bytes,root_construction72_checked,packet.length));
        status = xr_xir_compile_specialize(checked,&instance,NULL);
    }
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked); checked = NULL;
    if (status == XR_XIR_OK) {
        root_construction72_rows(instance,false);
        const XrXirModule *m = xr_xir_compile_artifact_module(instance);
        CHECK(m->provenance && m->provenance->source && m->provenance->count == 5);
        root_construction72_rows(m->provenance->source,true);
        status = xr_xir_compile_checked_write(instance,&packet,NULL);
    }
    if (status == XR_XIR_OK) {
        CHECK(packet.length == sizeof(root_construction72_instance));
        CHECK(!memcmp(packet.bytes,root_construction72_instance,packet.length));
        status = xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,NULL);
    }
    xr_xir_compile_artifact_free(instance); instance = NULL;
    if (packet.bytes) memset(packet.bytes,0xCC,packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    if (status == XR_XIR_OK) {
        root_construction72_rows(decoded,false);
        const XrXirModule *m = xr_xir_compile_artifact_module(decoded);
        root_construction72_rows(m->provenance->source,true);
        CHECK(xr_xir_compile_artifact_construction(decoded) !=
            xr_xir_compile_artifact_construction(m->provenance->source));
        const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        status = xr_xir_compile_lower(decoded,&target,&lowered,NULL);
    }
    xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_artifact_free(lowered);
    return status;
}

static void root_construction72_put(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
}
static void root_construction72_attacks(void) {
    reset(SIZE_MAX);
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    XrXirArtifact *decoded = NULL;
    size_t before = attempts;
    CHECK(xr_xir_compile_checked_read(&context,root_construction71_previous,
        sizeof(root_construction71_previous),&decoded,NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!decoded && attempts == before && live_count == 1);
    decoded = (XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&context,root_construction71_previous,
        sizeof(root_construction71_previous),&decoded,NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(decoded == (XrXirArtifact *)(uintptr_t)1 && attempts == before);
    /* A valid digest cannot hide a wrong dense denominator or helper identity. */
    static const unsigned offsets[] = {0,4,8,16};
    static const uint32_t values[] = {0,1,1,4};
    for (unsigned recursive = 0; recursive < 2; ++recursive)
        for (unsigned attack = 0; attack < 4; ++attack) {
            uint8_t bytes[sizeof(root_construction72_instance)];
            size_t length = recursive ? sizeof(root_construction72_instance) : sizeof(root_construction72_checked);
            memcpy(bytes,recursive ? root_construction72_instance : root_construction72_checked,length);
            size_t owner = recursive ? ROOT_CONSTRUCTION72_RECURSIVE_OWNER : ROOT_CONSTRUCTION72_CHECKED_OWNER;
            root_construction72_put(bytes + owner + offsets[attack],values[attack]);
            XrSHA256Context hash;
            xr_sha256_init(&hash); xr_sha256_update(&hash,bytes,32);
            xr_sha256_update(&hash,bytes + 64,length - 64); xr_sha256_final(&hash,bytes + 32);
            decoded = NULL;
            CHECK(xr_xir_compile_checked_read(&context,bytes,length,&decoded,NULL) == XR_XIR_BAD_STRUCTURE);
            CHECK(!decoded && live_count == 1);
        }
    xr_compile_resources_release(resources);
    CHECK(!live && !live_count);
}

static void root_construction72_wire_cases(void) {
    root_construction72_attacks();
    reset(SIZE_MAX);
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(root_construction72_pipeline(resources) == XR_XIR_OK);
    XrCompileResourceStats measured = stats(resources);
    size_t denominator = attempts;
    CHECK(live_count == 1 && measured.allocation_count == denominator);
    xr_compile_resources_release(resources);
    CHECK(!live && !live_count);
    for (size_t point = 1; point < denominator; ++point) {
        reset(point); resources = NULL;
        CHECK(xr_compile_resources_new(&unlimited,&resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(root_construction72_pipeline(resources) == XR_XIR_OUT_OF_MEMORY);
        CHECK(live_count == 1);
        xr_compile_resources_release(resources);
        CHECK(!live && !live_count);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        reset(SIZE_MAX);
        XrCompileResourceLimits limits = {measured.allocated_bytes,measured.peak_bytes,measured.work};
        if (axis == 0) limits.allocated_bytes -= minus;
        else if (axis == 1) limits.live_bytes -= minus;
        else limits.work -= minus;
        resources = NULL;
        CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(root_construction72_pipeline(resources) == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(live_count == 1);
        xr_compile_resources_release(resources);
        CHECK(!live && !live_count);
    }
    printf("Independent nonzero construction and recursive INSTANCE bytes; fresh allocation faults=%zu; three axes; physical zero\n",
        denominator - 1);
}
#endif // ROOT_CONSTRUCTION72_CASES_H
