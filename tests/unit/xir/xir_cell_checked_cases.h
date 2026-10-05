/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_checked_cases.h - Typed storage admission and hostile cell packets
 *
 * KEY CONCEPT:
 *   Shared storage capabilities cannot masquerade as their copied contents.
 */
#ifndef XR_XIR_CELL_CHECKED_CASES_H
#define XR_XIR_CELL_CHECKED_CASES_H
static void cell_generic_nominal_cases(void) {
    for (unsigned mode = 0; mode < 8; ++mode) {
        NominalFixture nominal; nominal_fixture(&nominal); nominal.constraint.markers = 0;
        NominalIdentityFixture identity; nominal_identity_fixture(&identity);
        XrXirType argument = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, closed = XR_XIR_I64;
        XrXirTypeNode nodes[] = {
            {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,1,{0,&argument,1,NULL,0}},
            {XR_XIR_TYPE_CELL,(XrXirType)256,NULL,0,XR_XIR_UNIT,0,1,{0}},
            {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{1,&closed,1,NULL,0}}
        };
        XrXirTypes types = {nodes,3,&nominal.table, NULL};
        if (mode == 1) nodes[1].parameter_span = 0;
        if (mode == 2) nodes[1].element = (XrXirType)257;
        if (mode == 3) nodes[1].element = (XrXirType)258;
        if (mode == 4) nodes[1].element = XR_XIR_UNIT;
        if (mode == 5) nodes[0].parameter_span = 0;
        if (mode == 6) types.nominals = &identity.table;
        if (mode == 7) nodes[2] = (XrXirTypeNode){XR_XIR_TYPE_CELL,(XrXirType)257,NULL,0,XR_XIR_UNIT,0,1,{0}};
        XrXirCompileContext budget = consumer_context_default();
        XrXirStatus status = xr_xir_compile_types_structure_verify(&budget, &types);
        CHECK(mode ? status != XR_XIR_OK : status == XR_XIR_OK);
    }
}
static void cell_local_checked_cases(void) {
    for (unsigned mode = 0; mode < 7; ++mode) {
        XrXirArtifact *base = checked_fixture(suite_context), *checked = NULL, *decoded = NULL;
        XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
        XrXirFunction functions[9]; memcpy(functions, built.functions, sizeof(functions));
        XrXirType parameters[] = {XR_XIR_I64, XR_XIR_STRING};
        XrXirTypeNode node = {XR_XIR_TYPE_CELL, XR_XIR_STRING, NULL, 0, XR_XIR_UNIT, 0, 0, {0}};
        XrXirTypes types = {&node, 1, NULL, NULL}; built.types = &types;
        XrXirInstruction ops[] = {
            {XR_XIR_LOCAL_UNINIT, (XrXirType)256, {0}, {0}, 0, {0}},
            {XR_XIR_CELL_LOCAL_WRITE, XR_XIR_UNIT, {2,1}, {0}, 0, {0}},
            {XR_XIR_LOCAL_READ, (XrXirType)256, {2}, {0}, 0, {0}},
            {XR_XIR_CELL_READ, XR_XIR_STRING, {4}, {0}, 0, {0}},
            {XR_XIR_CELL_LOCAL_WRITE, XR_XIR_UNIT, {2,5}, {0}, 0, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
        XrXirBlock block = {0, 6, 0, 0};
        functions[8].instructions = ops; functions[8].instruction_count = 6;
        functions[8].blocks = &block; functions[8].block_count = 1;
        functions[8].parameters = parameters; functions[8].operands = NULL; functions[8].operand_count = 0;
        built.functions = functions;
        if (mode == 1) ops[0].type = XR_XIR_I64;
        if (mode == 2) ops[1].args[1] = 0;
        if (mode == 3) ops[1].args[0] = 1;
        if (mode == 4) ops[1] = (XrXirInstruction){XR_XIR_LOCAL_READ,(XrXirType)256,{2},{0},0,{0}};
        if (mode == 5) ops[0].immediate = 1;
        if (mode == 6) ops[0] = (XrXirInstruction){XR_XIR_CELL_NEW,(XrXirType)256,{1},{0},0,{0}};
        XrXirStatus status = xr_xir_compile_check(suite_context, &built, &checked, NULL);
        xr_xir_compile_artifact_free(base);base=NULL;
        if (mode) { CHECK(status != XR_XIR_OK && !checked); continue; }
        CHECK(status == XR_XIR_OK && checked);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);checked=NULL;
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(decoded); decoded = NULL;
        uint8_t record[32] = {0}; put32(record, 103); put32(record + 8, 2); put32(record + 12, 1);
        size_t at = 0; unsigned matches = 0;
        for (size_t i = 64; i + sizeof(record) <= packet.length; ++i)
            if (!memcmp(packet.bytes + i, record, sizeof(record))) { at = i; ++matches; }
        CHECK(matches == 1); put32(packet.bytes + at + 12, 0); digest_packet(&packet);
        rejected(packet.bytes, packet.length);
        xr_xir_compile_checked_packet_free(&packet);
    }
}
static void cell_checked_cases(void) {
    cell_generic_nominal_cases();
    cell_local_checked_cases();
    for (unsigned mode = 0; mode < 9; ++mode) {
        XrXirArtifact *base = checked_fixture(suite_context), *checked = NULL, *decoded = NULL;
        XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
        XrXirFunction functions[9]; memcpy(functions, built.functions, sizeof(functions));
        XrXirType parameters[] = {XR_XIR_I64, XR_XIR_STRING};
        XrXirTypeNode nodes[] = {{XR_XIR_TYPE_CELL,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0, {0}},
            {XR_XIR_TYPE_CELL,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0, {0}}};
        XrXirTypes types = {nodes,2, NULL, NULL}; built.types = &types;
        XrXirInstruction ops[] = {
            {XR_XIR_CELL_NEW, (XrXirType)256, {1}, {0}, 0, {0}},
            {XR_XIR_CELL_READ, XR_XIR_STRING, {2}, {0}, 0, {0}},
            {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {2, 3}, {0}, 0, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
        };
        XrXirBlock block = {0, 4, 0, 0};
        functions[8].instructions = ops; functions[8].instruction_count = 4;
        functions[8].blocks = &block; functions[8].block_count = 1;
        functions[8].parameters = parameters; functions[8].operands = NULL; functions[8].operand_count = 0;
        XrXirFunctionIdentity identities[9]; memcpy(identities, built.declarations->functions, sizeof(identities));
        XrXirDeclarations declarations = *built.declarations; declarations.functions = identities;
        built.functions = functions; built.declarations = &declarations;
        if (mode == 1) ops[0].type = XR_XIR_STRING;
        if (mode == 2) ops[1].type = XR_XIR_I64;
        if (mode == 3) ops[2].args[1] = 0;
        if (mode == 4) ops[0].args[0] = 3;
        if (mode == 5) ops[2].args[0] = 0;
        if (mode == 6) functions[8].result = (XrXirType)256;
        if (mode == 7) { parameters[0] = (XrXirType)257; identities[8].exported = 1; }
        if (mode == 8) nodes[0].element = XR_XIR_UNIT;
        XrXirStatus status = xr_xir_compile_check(suite_context, &built, &checked, NULL);
        xr_xir_compile_artifact_free(base);base=NULL;
        if (mode) {
            CHECK(!checked && status == (mode == 4 ? XR_XIR_BAD_DOMINANCE : XR_XIR_BAD_TYPE));
            continue;
        }
        CHECK(status == XR_XIR_OK && checked);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);checked=NULL;
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(decoded); decoded = NULL;
        uint8_t record[32] = {0};
        put32(record, 51); put32(record + 4, 256); put32(record + 8, 1);
        size_t found = 0; unsigned matches = 0;
        for (size_t i = 64; i + sizeof(record) <= packet.length; ++i)
            if (!memcmp(packet.bytes + i, record, sizeof(record))) { found = i; ++matches; }
        CHECK(matches == 1); put32(packet.bytes + found + 4, 257); digest_packet(&packet);
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_TYPE && !decoded);
        xr_xir_compile_checked_packet_free(&packet);
    }
    puts("Shared cells: eight typed capability rejections and re-signed hostile packet rejected");
}
#endif
