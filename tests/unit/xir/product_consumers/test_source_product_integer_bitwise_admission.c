/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_integer_bitwise_admission.c - Exact bitwise admission
 *
 * KEY CONCEPT:
 *   Every integer width retains all six bitwise signatures. Complement uses
 *   a typed all-bit mask and XOR. Borrowed inputs die before owned inspection.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "integer_bitwise_cases.inc.c"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_I8 == 5 && XR_XIR_U8 == 8 && XR_XIR_I16 == 6 && XR_XIR_U16 == 9 &&
    XR_XIR_I32 == 7 && XR_XIR_U32 == 10 && XR_XIR_I64 == 2 && XR_XIR_U64 == 11 && XR_XIR_BOOL == 1 &&
    XR_XIR_CONST_INT == 2 && XR_XIR_AND_INT == 51 && XR_XIR_OR_INT == 52 && XR_XIR_XOR_INT == 53 &&
    XR_XIR_SHL_INT == 54 && XR_XIR_SHR_INT == 55 && XR_XIR_RETURN == 33,
    "Independent scalar and bitwise instruction identities");
_Static_assert(sizeof(integer_bitwise_functions)/sizeof(integer_bitwise_functions[0]) == 48,
    "Complete integer bitwise signature table");

static void exact_owner(const XrXirArtifact *owner) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(owner);
    CHECK(module && module->stage == XR_XIR_CHECKED && module->function_count == 48);
    CHECK(!module->types && !module->declarations && !module->generics);
    for (unsigned i = 0; i < 48; ++i) {
        const IntegerBitwiseFunction *expected = &integer_bitwise_functions[i];
        const XrXirFunction *function = &module->functions[i];
        unsigned operation_index = expected->complement ? 1u : 0u, count = operation_index + 2;
        CHECK(function->name_length == strlen(expected->name) && !memcmp(function->name, expected->name, function->name_length));
        CHECK(function->parameter_count == 2 && function->parameters[0] == expected->type &&
            function->parameters[1] == expected->type && function->result == expected->type);
        CHECK(function->block_count == 1 && function->blocks[0].first == 0 && function->blocks[0].count == count &&
            !function->blocks[0].panic && !function->blocks[0].frontier && function->instruction_count == count);
        CHECK(!function->operands && !function->operand_count);
        if (expected->complement) {
            const XrXirInstruction *mask = &function->instructions[0];
            CHECK(mask->op == XR_XIR_CONST_INT && mask->type == expected->type && mask->immediate == expected->mask &&
                !mask->args[0] && !mask->args[1] && !mask->targets[0] && !mask->targets[1] &&
                !mask->type_arguments[0] && !mask->type_arguments[1]);
        }
        const XrXirInstruction *operation = &function->instructions[operation_index], *ret = &function->instructions[count - 1];
        CHECK(operation->op == expected->operation && operation->type == expected->type &&
            operation->args[0] == (expected->complement ? 2u : 0u) && operation->args[1] == (expected->complement ? 0u : 1u) &&
            !operation->targets[0] && !operation->targets[1] && !operation->immediate &&
            !operation->type_arguments[0] && !operation->type_arguments[1]);
        CHECK(ret->op == XR_XIR_RETURN && ret->type == XR_XIR_UNIT && ret->args[0] == operation_index + 2 &&
            !ret->args[1] && !ret->targets[0] && !ret->targets[1] && !ret->immediate &&
            !ret->type_arguments[0] && !ret->type_arguments[1]);
        CHECK(xr_xir_type_is_integer(expected->type) && !xr_xir_type_is_owned(module->types, expected->type));
    }
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == integer_bitwise_cases[0].length && !memcmp(packet.bytes, integer_bitwise_cases[0].bytes, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
}

static XrXirStatus build_case(const XrXirCompileContext *context, unsigned mutation,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirType parameters[48][2]; XrXirBlock blocks[48] = {0};
    XrXirInstruction instructions[48][3] = {0}; XrXirFunction functions[48] = {0}; char names[48][32] = {0};
    for (unsigned i = 0; i < 48; ++i) {
        const IntegerBitwiseFunction *expected = &integer_bitwise_functions[i];
        unsigned operation_index = expected->complement ? 1u : 0u, count = operation_index + 2;
        CHECK(strlen(expected->name) < sizeof(names[i])); memcpy(names[i], expected->name, strlen(expected->name));
        parameters[i][0] = parameters[i][1] = expected->type; blocks[i].count = count;
        if (expected->complement) instructions[i][0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = expected->type, .immediate = expected->mask};
        instructions[i][operation_index] = (XrXirInstruction){.op = expected->operation, .type = expected->type,
            .args = {expected->complement ? 2u : 0u, expected->complement ? 0u : 1u}};
        instructions[i][count - 1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {operation_index + 2, 0}};
        functions[i] = (XrXirFunction){.name = names[i], .name_length = (uint32_t)strlen(expected->name),
            .parameters = parameters[i], .parameter_count = 2, .result = expected->type,
            .blocks = &blocks[i], .block_count = 1, .instructions = instructions[i], .instruction_count = count};
    }
    switch (mutation) {
    case 1: parameters[0][0] = XR_XIR_BOOL; break;
    case 2: parameters[0][1] = XR_XIR_BOOL; break;
    case 3: functions[0].result = instructions[0][0].type = XR_XIR_BOOL; break;
    case 4: instructions[0][0].immediate = 6; break;
    case 5: instructions[3][0].type = XR_XIR_BOOL; break;
    case 6: instructions[0][0].args[1] = UINT32_MAX; break;
    case 7: functions[0].result = XR_XIR_BOOL; break;
    case 8: instructions[0][0].type_arguments[0] = 1; break;
    case 9: instructions[0][0].targets[0] = 1; break;
    default: CHECK(!mutation); break;
    }
    XrXirModule built = {.stage = XR_XIR_BUILT, .functions = functions, .function_count = 48}, saved_module = built;
    XrXirType saved_parameters[48][2]; XrXirBlock saved_blocks[48];
    XrXirInstruction saved_instructions[48][3]; XrXirFunction saved_functions[48];
    memcpy(saved_parameters, parameters, sizeof(parameters)); memcpy(saved_blocks, blocks, sizeof(blocks));
    memcpy(saved_instructions, instructions, sizeof(instructions)); memcpy(saved_functions, functions, sizeof(functions));
    XrXirStatus status = xr_xir_compile_check(context, &built, output, diagnostic);
    CHECK(!memcmp(saved_parameters, parameters, sizeof(parameters)) && !memcmp(saved_blocks, blocks, sizeof(blocks)) &&
        !memcmp(saved_instructions, instructions, sizeof(instructions)) && !memcmp(saved_functions, functions, sizeof(functions)) &&
        !memcmp(&saved_module, &built, sizeof(built)));
    for (unsigned i = 0; i < 48; ++i) CHECK(!strcmp(names[i], integer_bitwise_functions[i].name));
    memset(parameters, 0xa5, sizeof(parameters)); memset(blocks, 0xa5, sizeof(blocks)); memset(instructions, 0xa5, sizeof(instructions));
    memset(functions, 0xa5, sizeof(functions)); memset(names, 0xa5, sizeof(names)); memset(&built, 0xa5, sizeof(built));
    return status;
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL; CHECK(build_case(context, 0, &owner, NULL) == XR_XIR_OK); exact_owner(owner);
    unsigned transitions = 0, mismatches = 0;
    for (size_t c = 0; c < sizeof(integer_bitwise_cases)/sizeof(integer_bitwise_cases[0]); ++c)
        for (unsigned wire = 0; wire < 2; ++wire) for (unsigned occupied = 0; occupied < 2; ++occupied) {
            const IntegerBitwiseCase *test = &integer_bitwise_cases[c];
            size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
            XrXirArtifact *output = occupied ? owner : NULL, *before = output; XrXirDiagnostic diagnostic = {0}; XrXirStatus status;
            if (wire) {
                uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
                status = xr_xir_compile_checked_read(context, input, test->length, &output, &diagnostic);
                CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
            } else status = build_case(context, test->mutation, &output, &diagnostic);
            if (status == XR_XIR_OK) { CHECK(output && output != owner); exact_owner(output); xr_xir_compile_artifact_free(output); }
            else CHECK(output == before && diagnostic.status == status);
            CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes); exact_owner(owner);
            bool match = status == test->expected; mismatches += match ? 0u : 1u; ++transitions;
            printf("integer-bitwise case=%s wire=%u occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
                test->name, wire, occupied, test->expected, status, match ? "PASS" : "FAIL");
        }
    xr_xir_compile_artifact_free(owner); source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity && transitions == 40);
    printf("integer-bitwise functions=48 transitions=%u mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        transitions, mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
