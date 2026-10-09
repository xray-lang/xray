/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_path_checked_cases.h - Logical projection authority and access timing
 */
#ifndef XIR_PATH_CHECKED_CASES_H
#define XIR_PATH_CHECKED_CASES_H
#include "xir_construction_fixture.h"
#include "xir/xxir_value_place.h"
static void path_array_packet_attacks(XrXirCheckedPacket *packet) {
    CHECK(packet32(packet,64) == 0 && packet32(packet,68) == 1);
    size_t at = 76;
    at += 4 + packet32(packet,at);
    at += 4 + (size_t)packet32(packet,at) * 4;
    at += 4;
    at += 4 + (size_t)packet32(packet,at) * 16;
    CHECK(packet32(packet,at) == 9); at += 4;
    CHECK(packet32(packet,at + 3 * 40) == XR_XIR_INDEX_PLACE);
    const uint32_t fields[] = {3*40+8,3*40+12,3*40+4,3*40+8,7*40+8,6*40+12,24,8*40+8,3*40+32};
    const uint32_t values[] = {0,0,XR_XIR_I64,4,0,0,1,5,1};
    for (unsigned i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i) {
        uint8_t saved[4]; memcpy(saved,packet->bytes + at + fields[i],4);
        put32(packet->bytes + at + fields[i],values[i]); digest_packet(packet);
        rejected(packet->bytes,packet->length);
        memcpy(packet->bytes + at + fields[i],saved,4); digest_packet(packet);
    }
}
static void path_layout_cases(XrXirArtifact *closed) {
    CHECK(closed->module.stage == XR_XIR_LOWERED);
    XrXirFunctionLayout *layout = &closed->layouts[0];
    CHECK(layout->path_count == 2 && layout->frame_bytes == 40 && layout->owned_count == 2);
    CHECK(layout->offsets[4] == UINT32_MAX && layout->offsets[5] == UINT32_MAX);
    CHECK(!layout->outgoing_count && xr_xir_compile_layout_verify(closed) == XR_XIR_OK);
    for (unsigned minus=0;minus<2;++minus) {
        XrXirCompileContext budget=consumer_context_default();
        budget.limits.frame_bytes=40+2*sizeof(XrXirValuePathStep)-minus;
        XrXirArtifact *read=NULL,*special=NULL,*lowered=NULL;
        CHECK(xr_xir_compile_checked_read(&budget,closed->checked_packet.bytes,closed->checked_packet.length,&read,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_specialize(read,&special,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_lower(special,&closed->target,&lowered,NULL)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK((lowered!=NULL)==!minus);
        xr_xir_compile_artifact_free(lowered);lowered=NULL;xr_xir_compile_artifact_free(special);special=NULL;xr_xir_compile_artifact_free(read);read=NULL;
    }
    ++layout->path_count;
    CHECK(xr_xir_compile_layout_verify(closed) == XR_XIR_BAD_LAYOUT);
    --layout->path_count;
    CHECK(xr_xir_compile_layout_verify(closed) == XR_XIR_OK);
}
static void path_array_checked_cases(void) {
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)256}};
    XrXirTypes types = {nodes,2,NULL, NULL}; XrXirType parameter = (XrXirType)257;
    const XrXirInstruction original[] = {
        {XR_XIR_LOCAL_UNINIT,(XrXirType)257,{0},{0},0,{0}},
        {XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{1,0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,(XrXirType)256,{1,3},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,XR_XIR_I64,{4,3},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},99,{0}},
        {XR_XIR_PLACE_WRITE,XR_XIR_UNIT,{5,6},{0},0,{0}},
        {XR_XIR_PLACE_READ,XR_XIR_I64,{5},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{8},{0},0,{0}}};
    XrXirInstruction ops[9]; XrXirBlock block = {0,9,0,0};
    XrXirFunction function = {"paths",5,&parameter,1,XR_XIR_I64,&block,1,ops,9,NULL,0};
    XrXirModule module = {XR_XIR_BUILT,&function,1,NULL,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
    for (unsigned mode = 0; mode < 8; ++mode) {
        memcpy(ops,original,sizeof(ops));
        if (mode == 1) ops[0].immediate = 1;
        if (mode == 2) ops[1] = (XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}};
        if (mode == 3) ops[3].type = XR_XIR_I64;
        if (mode == 4) ops[3].args[0] = 0;
        if (mode == 5) ops[4].args[1] = 0;
        if (mode == 6) ops[8].args[0] = 5;
        if (mode == 7) {
            ops[1] = original[5]; ops[5] = original[1]; ops[6].args[1] = 2;
        }
        XrXirArtifact *checked = NULL; XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xir_fixture_check(suite_context, &module, &checked, &diagnostic);
        if (mode && mode != 7) {
            CHECK(status != XR_XIR_OK && !checked);
            if (mode == 1) CHECK(diagnostic.reason == XR_XIR_DIAGNOSTIC_READONLY_WRITE && diagnostic.instruction == 6);
            if (mode == 2) CHECK(diagnostic.reason == XR_XIR_DIAGNOSTIC_UNINITIALIZED_READ && diagnostic.instruction == 6);
        } else {
            CHECK(status == XR_XIR_OK && checked);
            XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *closed = NULL;
            CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
            if (!mode) path_array_packet_attacks(&packet);
            CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
            CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
            CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
            XrXirArtifact *lowered = NULL;
            XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
            CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK && lowered);
            path_layout_cases(lowered);
            xr_xir_compile_artifact_free(lowered);lowered=NULL;
            xr_xir_compile_artifact_free(closed);closed=NULL; xr_xir_compile_artifact_free(decoded);decoded=NULL;
            xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_artifact_free(checked);checked=NULL;
        }
    }
}
static void path_field_checked_cases(void) {
    XrXirArtifact *checked = struct_set_checked(suite_context, 0);
    XrXirModule *module = &checked->module;
    XrXirInstruction *ops = (XrXirInstruction *)module->functions[1].instructions;
    ops[4] = (XrXirInstruction){XR_XIR_FIELD_PLACE,XR_XIR_I64,{1},{0},0,{0}};
    ops[6] = (XrXirInstruction){XR_XIR_PLACE_READ,XR_XIR_I64,{4},{0},0,{0}};
    ops[7] = (XrXirInstruction){XR_XIR_PLACE_WRITE,XR_XIR_UNIT,{4,3},{0},0,{0}};
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    XrXirNominalField *fields = (XrXirNominalField *)module->types->nominals->declarations[0].fields;
    uint32_t flags = fields[0].flags; XrXirDiagnostic diagnostic = {0};
    fields[0].flags = 0;
    CHECK(xr_xir_compile_artifact_verify(checked, &diagnostic) != XR_XIR_OK);
    CHECK(diagnostic.function == 1 && diagnostic.instruction == 7);
    fields[0].flags = flags | XR_XIR_FIELD_PRIVATE;
    CHECK(xr_xir_compile_artifact_verify(checked, &diagnostic) != XR_XIR_OK);
    /* This fixture constructs the type in its module initializer, so private
     * construction must already fail before its later projection is reached. */
    CHECK(diagnostic.function == 0 && diagnostic.instruction == 2);
    fields[0].flags = flags;
    ops[4].type = XR_XIR_STRING;
    CHECK(xr_xir_compile_artifact_verify(checked, &diagnostic) == XR_XIR_BAD_TYPE);
    CHECK(diagnostic.function == 1 && diagnostic.instruction == 4);
    ops[4].type = XR_XIR_I64;
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
}
static void path_ancestor_checked_cases(void) {
    const XrXirType array = (XrXirType)256, pair = (XrXirType)257;
    NominalFixture nominal; nominal_fixture(&nominal); nominal.table.count = 1;
    XrXirType argument = array;
    XrXirTypeNode nodes[] = {{.kind = XR_XIR_TYPE_ARRAY,.element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_NOMINAL,.nominal = {0,&argument,1,NULL,0}}};
    XrXirTypes types = {nodes,2,&nominal.table, NULL};
    XrXirInstruction init[] = {{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction main[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction path[] = {
        {XR_XIR_LOCAL_NEW,pair,{0},{0},0,{0}},
        {XR_XIR_FIELD_PLACE,array,{1},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,XR_XIR_I64,{2,3},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},99,{0}},
        {XR_XIR_PLACE_WRITE,XR_XIR_UNIT,{4,5},{0},0,{0}},
        {XR_XIR_PLACE_READ,XR_XIR_I64,{4},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{7},{0},0,{0}}};
    XrXirBlock blocks[] = {{0,1,0,0},{0,2,0,0},{0,8,0,0}};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&blocks[1],1,main,2,NULL,0},
        {"path",4,&pair,1,XR_XIR_I64,&blocks[2],1,path,8,NULL,0}};
    XrXirSourceModule source = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[3] = {{0}};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,NULL,0,0,1, NULL};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
    for (unsigned mode = 0; mode < 4; ++mode) {
        nominal.fields[0].flags = mode == 1 ? 0 : XR_XIR_FIELD_MUTABLE;
        if (mode >= 2) nominal.fields[0].flags |= XR_XIR_FIELD_PRIVATE;
        identities[2].nominal_owner = mode == 3 ? 1 : 0;
        identities[2].method_kind = mode == 3 ? XR_XIR_MEMBER_HELPER : XR_XIR_NON_MEMBER;
        XrXirArtifact *checked = NULL; XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xir_fixture_check(suite_context, &built, &checked, &diagnostic);
        if (mode == 1 || mode == 2) {
            CHECK(status != XR_XIR_OK && !checked && diagnostic.function == 2);
            CHECK(diagnostic.instruction == (mode == 1 ? 5u : 1u));
        } else {
            CHECK(status == XR_XIR_OK && checked);
            XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *closed = NULL, *lowered = NULL;
            CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
            CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
            CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
            const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
            CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
            xr_xir_compile_artifact_free(lowered);lowered=NULL; xr_xir_compile_artifact_free(closed);closed=NULL; xr_xir_compile_artifact_free(decoded);decoded=NULL;
            xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_artifact_free(checked);checked=NULL;
        }
    }
}
#endif // XIR_PATH_CHECKED_CASES_H
