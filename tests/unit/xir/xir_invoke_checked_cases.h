/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invoke_checked_cases.h - Call results are available only on their own edges
 */
static void invoke_generic_cases(void) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        const XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
        XrXirType parameter = mode ? t : XR_XIR_I64;
        XrXirConstraint caller_constraint = {.markers = mode == 2 ? XR_XIR_CONSTRAINT_SENDABLE : 0};
        XrXirConstraint callee_constraint = {.markers = XR_XIR_CONSTRAINT_SENDABLE};
        uint32_t argument = 0;
        XrXirInstruction ops[] = {
            {XR_XIR_INVOKE,parameter,{0,1},{1,2},1,{0,1}},
            {XR_XIR_INVOKE_RESULT,parameter,{0},{0},0,{0}},
            {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}},
            {XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},0,{0}},
            {XR_XIR_THROW,XR_XIR_UNIT,{4},{0},0,{0}}};
        XrXirInstruction ret = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
        XrXirBlock blocks[] = {{0,1, 0, 0},{1,2, 0, 0},{3,2, 0, 0}}, leaf = {0,1, 0, 0};
        XrXirFunction functions[] = {
            {"guard",5,&parameter,1,parameter,blocks,3,ops,5,&argument,1},
            {"leaf",4,&t,1,t,&leaf,1,&ret,1,NULL,0}};
        XrXirGeneric generics[] = {
            {mode ? &caller_constraint : NULL,mode ? 1u : 0u,&parameter,1},
            {&callee_constraint,1,NULL,0}};
        XrXirModule module = {XR_XIR_BUILT,functions,2,NULL,generics,NULL,NULL, XR_XIR_PROGRAM, NULL};
        XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
        XrXirStatus status = xr_xir_check(&module,NULL,&checked,NULL);
        /* An unused caller must prove the callee's bound at its definition. */
        if (mode == 1) { CHECK(status != XR_XIR_OK && !checked); continue; }
        CHECK(status == XR_XIR_OK);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
        if (!mode) {
            CHECK(xr_xir_specialize(decoded,NULL,&closed,NULL) == XR_XIR_OK);
            const XrXirModule *specialized = xr_xir_artifact_module(closed);
            const XrXirInstruction *call = specialized->functions[0].instructions;
            CHECK(!specialized->generics && specialized->function_count == 2);
            CHECK(call->op == XR_XIR_INVOKE && call->targets[0] == 1 && call->targets[1] == 2);
            CHECK(!call->type_arguments[0] && !call->type_arguments[1]);
            CHECK(call->immediate == 1 && specialized->functions[1].parameters[0] == XR_XIR_I64);
            XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
            CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK);
        }
        xr_xir_artifact_free(lowered); xr_xir_artifact_free(closed);
        xr_xir_artifact_free(decoded); xr_xir_artifact_free(checked);
        xr_xir_checked_packet_free(&packet);
    }
}

static void invoke_checked_cases(void) {
    invoke_generic_cases();
    for (unsigned attack = 0; attack < 14; ++attack) {
        XrXirType parameter = XR_XIR_I64;
        XrXirInstruction ops[] = {
            {XR_XIR_INVOKE,XR_XIR_I64,{0,1},{1,2},1,{0}},
            {XR_XIR_INVOKE_RESULT,XR_XIR_I64,{0},{0},0,{0}},
            {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}},
            {XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},0,{0}},
            {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
            {XR_XIR_RETURN,XR_XIR_UNIT,{5},{0},0,{0}}};
        XrXirInstruction ret = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
        XrXirBlock blocks[] = {{0,1, 0, 0},{1,2, 0, 0},{3,3, 0, 0}}, leaf = {0,1, 0, 0};
        uint32_t argument = 0;
        XrXirFunction functions[] = {
            {"guard",5,&parameter,1,XR_XIR_I64,blocks,3,ops,6,&argument,1},
            {"leaf",4,&parameter,1,XR_XIR_I64,&leaf,1,&ret,1,NULL,0}};
        XrXirModule module = {XR_XIR_BUILT,functions,2,NULL,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};
        if (attack == 1) ops[0].targets[1] = 1;
        if (attack == 2) ops[1].immediate = 1;
        if (attack == 3) ops[3].op = XR_XIR_INVOKE_RESULT;
        if (attack == 4) ops[2].args[0] = 1;
        if (attack == 5) ops[5].args[0] = 2;
        if (attack == 6) ops[1].args[0] = 1;
        if (attack == 7) ops[3].type = XR_XIR_I64;
        if (attack == 8) ops[0].type_arguments[1] = 1;
        if (attack == 9) ops[3].immediate = INT64_MAX;
        if (attack == 10) ops[4] = (XrXirInstruction){XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},0,{0}};
        if (attack == 11) ops[2] = (XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{2,0},0,{0}};
        if (attack == 12) ops[0].targets[0] = 0;
        if (attack == 13) {
            functions[1].result = XR_XIR_UNIT;
            ops[0].type = XR_XIR_UNIT; ops[1].type = XR_XIR_UNIT;
        }
        XrXirArtifact *checked = NULL, *decoded = NULL;
        XrXirStatus status = xr_xir_check(&module,NULL,&checked,NULL);
        if (attack) { CHECK(status != XR_XIR_OK && !checked); continue; }
        CHECK(status == XR_XIR_OK);
        XrXirArtifact *lowered = NULL;
        XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_lower(checked,&target,NULL,&lowered,NULL) == XR_XIR_OK);
        const XrXirFunctionLayout *layout = xr_xir_artifact_layout(lowered,0);
        CHECK(layout->offsets[1] == UINT32_MAX && layout->offsets[2] != UINT32_MAX &&
            layout->offsets[4] != UINT32_MAX && layout->outgoing_count == 1 && layout->owned_count == 1);
        xr_xir_artifact_free(lowered);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
        xr_xir_artifact_free(decoded); decoded = NULL;
        /* Locate the complete call record independently of producer offsets. */
        uint8_t pattern[40] = {0};
        put32(pattern,XR_XIR_INVOKE); put32(pattern+4,XR_XIR_I64);
        put32(pattern+12,1); put32(pattern+16,1); put32(pattern+20,2); put32(pattern+24,1);
        size_t found = 0; unsigned matches = 0;
        for (size_t at=64;at+sizeof(pattern)<=packet.length;++at)
            if (!memcmp(packet.bytes+at,pattern,sizeof(pattern))) { found=at; ++matches; }
        CHECK(matches == 1);
        put32(packet.bytes+found+20,1); digest_packet(&packet);
        rejected(packet.bytes,packet.length);
        xr_xir_checked_packet_free(&packet); xr_xir_artifact_free(checked);
    }
}
