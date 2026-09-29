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
static void cell_checked_cases(void) {
    for (unsigned mode = 0; mode < 9; ++mode) {
        XrXirArtifact *base = checked_fixture(), *checked = NULL, *decoded = NULL;
        XrXirModule built = *xr_xir_artifact_module(base); built.stage = XR_XIR_BUILT;
        XrXirFunction functions[9]; memcpy(functions, built.functions, sizeof(functions));
        XrXirType parameters[] = {XR_XIR_I64, XR_XIR_STRING};
        XrXirTypeNode nodes[] = {{XR_XIR_TYPE_CELL,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0, {0}},
            {XR_XIR_TYPE_CELL,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0, {0}}};
        XrXirTypes types = {nodes,2, NULL}; built.types = &types;
        XrXirInstruction ops[] = {
            {XR_XIR_CELL_NEW, (XrXirType)256, {1}, {0}, 0, {0}},
            {XR_XIR_CELL_READ, XR_XIR_STRING, {2}, {0}, 0, {0}},
            {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {2, 3}, {0}, 0, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
        };
        XrXirBlock block = {0, 4, 0};
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
        XrXirStatus status = xr_xir_check(&built, NULL, &checked, NULL);
        xr_xir_artifact_free(base);
        if (mode) {
            CHECK(!checked && status == (mode == 4 ? XR_XIR_BAD_DOMINANCE : XR_XIR_BAD_TYPE));
            continue;
        }
        CHECK(status == XR_XIR_OK && checked);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(checked);
        CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(decoded); decoded = NULL;
        uint8_t record[32] = {0};
        put32(record, 51); put32(record + 4, 256); put32(record + 8, 1);
        size_t found = 0; unsigned matches = 0;
        for (size_t i = 64; i + sizeof(record) <= packet.length; ++i)
            if (!memcmp(packet.bytes + i, record, sizeof(record))) { found = i; ++matches; }
        CHECK(matches == 1); put32(packet.bytes + found + 4, 257); digest_packet(&packet);
        CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_TYPE && !decoded);
        xr_xir_checked_packet_free(&packet);
    }
    puts("Shared cells: eight typed capability rejections and re-signed hostile packet rejected");
}
#endif
