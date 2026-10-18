/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_vm_fixture_writer.c - Opt-in runtime fixture refresh tool
 */

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static XrCoreIrKey fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static int write_fixture(FILE *output, const XrProgramArtifact *artifact) {
    if (fprintf(output,
                "/*\n"
                " * xray - Lightweight typed scripting with native concurrency\n"
                " * https://www.xray-lang.org\n"
                " *\n"
                " * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>\n"
                " * Licensed under the MIT License\n"
                " *\n"
                " * xr_program_vm_embedded_fixture.h - Runtime-only canonical XrProgram fixture\n"
                " *\n"
                " * Canonical XrProgram artifact consumed by the runtime-only VM test.\n"
                " *\n"
                " * Keeping this fixture in source is intentional: the runtime target must not\n"
                " * link or execute a compiler-side fixture producer during its build.  Schema\n"
                " * or CoreSpec drift fails closed in xr_program_validate(), while the runtime\n"
                " * result assertion below the decode boundary pins the payload to i64(42). */\n"
                "#ifndef XR_PROGRAM_VM_EMBEDDED_FIXTURE_H\n"
                "#define XR_PROGRAM_VM_EMBEDDED_FIXTURE_H\n\n"
                "static const unsigned char xr_program_vm_embedded_fixture[] = {\n") < 0)
        return 1;
    for (size_t index = 0u; index < artifact->size; ++index) {
        if (index % 16u == 0u && fprintf(output, "    ") < 0)
            return 1;
        if (fprintf(output, "0x%02x,", (unsigned) artifact->bytes[index]) < 0)
            return 1;
        bool line_end = index % 16u == 15u || index + 1u == artifact->size;
        if (fputc(line_end ? '\n' : ' ', output) == EOF)
            return 1;
    }
    return fprintf(output,
                   "};\n"
                   "static const unsigned long xr_program_vm_embedded_fixture_size = %luUL;\n\n"
                   "#endif  // XR_PROGRAM_VM_EMBEDDED_FIXTURE_H\n",
                   (unsigned long) artifact->size) < 0;
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
         .operand_count = 1u,
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
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = fixture_key("task-299:embedded:module"),
        .constants = &constant,
        .constant_count = 1u,
        .functions = &function,
        .function_count = 1u,
    };
    XrCoreIrKey semantic_profile = fixture_key("task-299:embedded:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic_profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .modules = &module,
        .module_count = 1u,
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
    int failed = write_fixture(output, &artifact);
    if (fclose(output) != 0)
        failed = 1;
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(program);
    return failed;
}
