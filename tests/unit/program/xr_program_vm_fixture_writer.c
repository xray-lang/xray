/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Build-time producer for the runtime-only Task 299 embedding fixture.
 */

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

#include <stdio.h>
#include <string.h>

static XrCoreIrKey fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT_HEADER\n", argv[0]);
        return 2;
    }
    XrCoreIrConstantInput constant = {
        .key = fixture_key("task-299:embedded:constant:42"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey value = fixture_key("task-299:embedded:value:42");
    XrCoreIrKey returned[] = {value};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = value,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("task-299:embedded:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("task-299:embedded:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = fixture_key("task-299:embedded:module"),
        .constants = &constant,
        .constant_count = 1,
        .functions = &function,
        .function_count = 1,
    };
    XrCoreIrKey semantic_profile = fixture_key("task-299:embedded:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic_profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1,
        .modules = &module,
        .module_count = 1,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    if (xr_core_ir_program_build(&input, &program, diagnostic, sizeof(diagnostic)) !=
            XR_PROGRAM_BUILD_OK ||
        xr_program_write(program, &artifact, diagnostic, sizeof(diagnostic)) !=
            XR_PROGRAM_BUILD_OK) {
        fprintf(stderr, "fixture build failed: %s\n", diagnostic);
        xr_program_artifact_free(&artifact);
        xr_core_ir_program_free(program);
        return 1;
    }
    FILE *output = fopen(argv[1], "wb");
    if (!output) {
        perror(argv[1]);
        xr_program_artifact_free(&artifact);
        xr_core_ir_program_free(program);
        return 1;
    }
    fprintf(output, "#ifndef XR_PROGRAM_VM_EMBEDDED_FIXTURE_H\n");
    fprintf(output, "#define XR_PROGRAM_VM_EMBEDDED_FIXTURE_H\n\n");
    fprintf(output, "static const unsigned char xr_program_vm_embedded_fixture[] = {\n");
    for (size_t index = 0; index < artifact.size; ++index) {
        if (index % 12u == 0u)
            fprintf(output, "    ");
        fprintf(output, "0x%02x%s", (unsigned) artifact.bytes[index],
                index + 1u == artifact.size ? "" : ", ");
        if (index % 12u == 11u || index + 1u == artifact.size)
            fputc('\n', output);
    }
    fprintf(output, "};\n");
    fprintf(output, "static const unsigned long xr_program_vm_embedded_fixture_size = %luUL;\n\n",
            (unsigned long) artifact.size);
    fprintf(output, "#endif\n");
    int failed = ferror(output) != 0;
    if (fclose(output) != 0)
        failed = 1;
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(program);
    return failed ? 1 : 0;
}
