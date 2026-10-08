/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_integer_conversions.c - Complete integer admission and detached owners
 *
 * KEY CONCEPT:
 *   Each integer source has all eight integer destinations. Borrowed construction
 *   inputs and wire bytes die before owned tables and their exact encoding are checked.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "integer_conversion_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_I8 == 5 && XR_XIR_U8 == 8 && XR_XIR_I16 == 6 && XR_XIR_U16 == 9 &&
    XR_XIR_I32 == 7 && XR_XIR_U32 == 10 && XR_XIR_I64 == 2 && XR_XIR_U64 == 11 &&
    XR_XIR_BOOL == 1 && XR_XIR_CONVERT_NUMBER == 62 && XR_XIR_RETURN == 33,
    "Independent integer and instruction identities");
_Static_assert(sizeof(integer_conversion_pairs)/sizeof(integer_conversion_pairs[0]) == 64,
    "All original integer source and destination pairs");

static void exact_owner(const XrXirArtifact *owner) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(owner);
    CHECK(module && module->stage == XR_XIR_CHECKED && module->function_count == 64);
    CHECK(!module->types && !module->declarations && !module->generics);
    for (unsigned i = 0; i < 64; ++i) {
        const IntegerConversionPair *pair = &integer_conversion_pairs[i];
        const XrXirFunction *function = &module->functions[i];
        CHECK(function->name_length == strlen(pair->name) && !memcmp(function->name, pair->name, function->name_length));
        CHECK(function->parameter_count == 1 && function->parameters[0] == pair->source && function->result == pair->target);
        CHECK(function->block_count == 1 && function->blocks[0].first == 0 && function->blocks[0].count == 2 &&
            !function->blocks[0].panic && !function->blocks[0].frontier && function->instruction_count == 2);
        CHECK(!function->operands && !function->operand_count);
        const XrXirInstruction *convert = &function->instructions[0], *ret = &function->instructions[1];
        CHECK(convert->op == XR_XIR_CONVERT_NUMBER && convert->type == pair->target && !convert->args[0] &&
            !convert->args[1] && !convert->targets[0] && !convert->targets[1] && !convert->immediate &&
            !convert->type_arguments[0] && !convert->type_arguments[1]);
        CHECK(ret->op == XR_XIR_RETURN && ret->type == XR_XIR_UNIT && ret->args[0] == 1 &&
            !ret->args[1] && !ret->targets[0] && !ret->targets[1] && !ret->immediate &&
            !ret->type_arguments[0] && !ret->type_arguments[1]);
        CHECK(xr_xir_type_is_integer(pair->source) && xr_xir_type_is_integer(pair->target));
        CHECK(!xr_xir_type_is_owned(module->types, pair->source) && !xr_xir_type_is_owned(module->types, pair->target));
    }
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == integer_conversion_cases[0].length &&
        !memcmp(packet.bytes, integer_conversion_cases[0].bytes, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
}

static XrXirStatus build_case(const XrXirCompileContext *context, unsigned mutation,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirType parameters[64]; XrXirBlock blocks[64] = {0};
    XrXirInstruction instructions[64][2] = {0}; XrXirFunction functions[64] = {0};
    char names[64][32] = {0};
    for (unsigned i = 0; i < 64; ++i) {
        const IntegerConversionPair *pair = &integer_conversion_pairs[i];
        CHECK(strlen(pair->name) < sizeof(names[i])); memcpy(names[i], pair->name, strlen(pair->name));
        parameters[i] = pair->source; blocks[i].count = 2;
        instructions[i][0] = (XrXirInstruction){.op = XR_XIR_CONVERT_NUMBER, .type = pair->target};
        instructions[i][1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {1, 0}};
        functions[i] = (XrXirFunction){.name = names[i], .name_length = (uint32_t)strlen(pair->name),
            .parameters = &parameters[i], .parameter_count = 1, .result = pair->target,
            .blocks = &blocks[i], .block_count = 1, .instructions = instructions[i], .instruction_count = 2};
    }
    switch (mutation) {
    case 1: parameters[0] = XR_XIR_BOOL; break;
    case 2: functions[0].result = instructions[0][0].type = XR_XIR_BOOL; break;
    case 3: instructions[0][0].immediate = 1; break;
    case 4: instructions[0][0].args[1] = 1; break;
    case 5: instructions[0][0].targets[0] = 1; break;
    case 6: instructions[0][0].type_arguments[0] = 1; break;
    case 7: functions[0].result = XR_XIR_BOOL; break;
    default: CHECK(!mutation); break;
    }
    XrXirModule built = {.stage = XR_XIR_BUILT, .functions = functions, .function_count = 64};
    XrXirType saved_parameters[64]; XrXirInstruction saved_instructions[64][2]; XrXirFunction saved_functions[64];
    memcpy(saved_parameters, parameters, sizeof(parameters)); memcpy(saved_instructions, instructions, sizeof(instructions));
    memcpy(saved_functions, functions, sizeof(functions));
    XrXirStatus status = xr_xir_compile_check(context, &built, output, diagnostic);
    CHECK(!memcmp(saved_parameters, parameters, sizeof(parameters)) &&
        !memcmp(saved_instructions, instructions, sizeof(instructions)) && !memcmp(saved_functions, functions, sizeof(functions)));
    for (unsigned i = 0; i < 64; ++i) CHECK(!strcmp(names[i], integer_conversion_pairs[i].name));
    memset(parameters, 0xa5, sizeof(parameters)); memset(blocks, 0xa5, sizeof(blocks));
    memset(instructions, 0xa5, sizeof(instructions)); memset(functions, 0xa5, sizeof(functions));
    memset(names, 0xa5, sizeof(names)); memset(&built, 0xa5, sizeof(built));
    return status;
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL;
    CHECK(build_case(context, 0, &owner, NULL) == XR_XIR_OK); exact_owner(owner);
    unsigned transitions = 0, mismatches = 0;
    for (size_t c = 0; c < sizeof(integer_conversion_cases)/sizeof(integer_conversion_cases[0]); ++c)
        for (unsigned wire = 0; wire < 2; ++wire) for (unsigned occupied = 0; occupied < 2; ++occupied) {
            const IntegerConversionCase *test = &integer_conversion_cases[c];
            size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
            XrXirArtifact *output = occupied ? owner : NULL, *before = output;
            XrXirDiagnostic diagnostic = {0}; XrXirStatus status;
            if (wire) {
                uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
                status = xr_xir_compile_checked_read(context, input, test->length, &output, &diagnostic);
                CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
            } else status = build_case(context, test->mutation, &output, &diagnostic);
            if (status == XR_XIR_OK) { CHECK(output && output != owner); exact_owner(output); xr_xir_compile_artifact_free(output); }
            else CHECK(output == before && diagnostic.status == status);
            CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes); exact_owner(owner);
            bool match = status == test->expected; mismatches += match ? 0u : 1u; ++transitions;
            printf("integer-conversions case=%s wire=%u occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
                test->name, wire, occupied, test->expected, status, match ? "PASS" : "FAIL");
        }
    xr_xir_compile_artifact_free(owner); source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity && transitions == 32);
    printf("integer-conversions pairs=64 transitions=%u mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        transitions, mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
