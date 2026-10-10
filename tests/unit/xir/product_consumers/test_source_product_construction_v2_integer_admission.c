/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_construction_v2_integer_admission.c - Owned integer construction admission
 *
 * KEY CONCEPT:
 *   Every original integer pair receives a real construction owner. Independent
 *   type and operand oracles survive destruction of all borrowed input storage.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "integer_conversion_cases.inc.c"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 27u && XR_XIR_CHECKED_CONTRACT == 72u, "Exact Checked identity");
_Static_assert(XR_XIR_I8 == 5 && XR_XIR_U8 == 8 && XR_XIR_I16 == 6 && XR_XIR_U16 == 9 &&
    XR_XIR_I32 == 7 && XR_XIR_U32 == 10 && XR_XIR_I64 == 2 && XR_XIR_U64 == 11 &&
    XR_XIR_BOOL == 1 && XR_XIR_CONVERT_NUMBER == 62 && XR_XIR_RETURN == 33,
    "Independent integer and instruction identities");
_Static_assert(XR_XIR_OK == 0 && XR_XIR_BAD_STRUCTURE == 1 && XR_XIR_BAD_TYPE == 3,
    "Independent admission result identities");
_Static_assert(sizeof(integer_conversion_pairs) / sizeof(integer_conversion_pairs[0]) == 64,
    "Complete original integer source and destination pairs");
_Static_assert(sizeof(integer_conversion_cases) / sizeof(integer_conversion_cases[0]) == 8,
    "Complete original normal and malformed Built inputs");

typedef XrXirStatus (*ConstructionPrototype)(const XrXirCompileContext *, const XrXirTypes *,
    const XrXirConstructionRow *, uint32_t, XrXirConstruction **);
typedef XrXirStatus (*CheckPrototype)(const XrXirCompileContext *, const XrXirModule *,
    const XrXirConstruction *, XrXirArtifact **, XrXirDiagnostic *);
typedef XrXirStatus (*VerifyPrototype)(const XrXirCompileContext *, const XrXirModule *,
    const XrXirConstruction *, XrXirDiagnostic *);
_Static_assert(_Generic(&xr_xir_compile_construction_new, ConstructionPrototype: 1, default: 0),
    "Public construction signature");
_Static_assert(_Generic(&xr_xir_compile_check_v2, CheckPrototype: 1, default: 0), "Public owned check signature");
_Static_assert(_Generic(&xr_xir_compile_verify_v2, VerifyPrototype: 1, default: 0), "Public owned verify signature");

typedef struct IntegerInput {
    XrXirType parameters[64];
    XrXirBlock blocks[64];
    XrXirInstruction instructions[64][2];
    XrXirFunction functions[64];
    char names[64][32];
    XrXirModule built;
} IntegerInput;

static XrCompileResourceStats integer_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result = {0};
    CHECK(context && context->resources);
    CHECK(xr_compile_resources_stats(context->resources, &result) == XR_COMPILE_RESOURCE_OK);
    return result;
}

static void integer_input(IntegerInput *input, unsigned mutation) {
    CHECK(input && mutation < 8);
    memset(input, 0, sizeof(*input));
    static const XrXirType kinds[8] = {5, 8, 6, 9, 7, 10, 2, 11};
    for (unsigned i = 0; i < 64; ++i) {
        const IntegerConversionPair *pair = &integer_conversion_pairs[i];
        CHECK(pair->source == kinds[i / 8] && pair->target == kinds[i % 8]);
        CHECK(strlen(pair->name) < sizeof(input->names[i]));
        memcpy(input->names[i], pair->name, strlen(pair->name));
        input->parameters[i] = pair->source;
        input->blocks[i].count = 2;
        input->instructions[i][0] = (XrXirInstruction){.op = XR_XIR_CONVERT_NUMBER, .type = pair->target};
        input->instructions[i][1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {1, 0}};
        input->functions[i] = (XrXirFunction){.name = input->names[i],
            .name_length = (uint32_t)strlen(pair->name), .parameters = &input->parameters[i],
            .parameter_count = 1, .result = pair->target, .blocks = &input->blocks[i],
            .block_count = 1, .instructions = input->instructions[i], .instruction_count = 2};
    }
    switch (mutation) {
    case 0: break;
    case 1: input->parameters[0] = XR_XIR_BOOL; break;
    case 2: input->functions[0].result = input->instructions[0][0].type = XR_XIR_BOOL; break;
    case 3: input->instructions[0][0].immediate = 1; break;
    case 4: input->instructions[0][0].args[1] = 1; break;
    case 5: input->instructions[0][0].targets[0] = 1; break;
    case 6: input->instructions[0][0].type_arguments[0] = 1; break;
    case 7: input->functions[0].result = XR_XIR_BOOL; break;
    default: CHECK(false); break;
    }
    input->built = (XrXirModule){.stage = XR_XIR_BUILT, .functions = input->functions, .function_count = 64};
    CHECK(!input->built.types && !input->built.declarations && !input->built.generics);
}

static void integer_owner(const XrXirArtifact *owner) {
    CHECK(owner);
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(owner);
    const XrXirModule *module = xr_xir_compile_artifact_module(owner);
    const XrXirConstruction *facts = xr_xir_compile_artifact_construction(owner);
    CHECK(context && module && facts && xr_xir_compile_construction_count(facts) == 0);
    CHECK(!xr_xir_compile_construction_row(facts, 0));
    size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
    CHECK(xr_xir_compile_verify_v2(context, module, facts, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes);
    CHECK(module->stage == XR_XIR_CHECKED && module->function_count == 64);
    CHECK(!module->types && !module->declarations && !module->generics);
    for (unsigned i = 0; i < 64; ++i) {
        const IntegerConversionPair *pair = &integer_conversion_pairs[i];
        const XrXirFunction *function = &module->functions[i];
        CHECK(function->name_length == strlen(pair->name) &&
            !memcmp(function->name, pair->name, function->name_length));
        CHECK(function->parameter_count == 1 && function->parameters[0] == pair->source &&
            function->result == pair->target);
        CHECK(function->block_count == 1 && !function->blocks[0].first && function->blocks[0].count == 2 &&
            !function->blocks[0].panic && !function->blocks[0].frontier && function->instruction_count == 2);
        CHECK(!function->operands && !function->operand_count);
        const XrXirInstruction *convert = &function->instructions[0], *ret = &function->instructions[1];
        CHECK(convert->op == XR_XIR_CONVERT_NUMBER && convert->type == pair->target && !convert->args[0] &&
            !convert->args[1] && !convert->targets[0] && !convert->targets[1] && !convert->immediate &&
            !convert->type_arguments[0] && !convert->type_arguments[1]);
        CHECK(ret->op == XR_XIR_RETURN && ret->type == XR_XIR_UNIT && ret->args[0] == 1 && !ret->args[1] &&
            !ret->targets[0] && !ret->targets[1] && !ret->immediate &&
            !ret->type_arguments[0] && !ret->type_arguments[1]);
        CHECK(xr_xir_type_is_integer(pair->source) && xr_xir_type_is_integer(pair->target));
        CHECK(!xr_xir_type_is_owned(module->types, pair->source) && !xr_xir_type_is_owned(module->types, pair->target));
    }
}

static XrXirArtifact *integer_empty_admission(const XrXirCompileContext *context, unsigned mutation) {
    IntegerInput input, saved;
    integer_input(&input, mutation);
    memcpy(&saved, &input, sizeof(saved));
    size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
    XrXirConstruction *facts = NULL;
    size_t attempts = source_program_compile_attempts;
    CHECK(xr_xir_compile_construction_new(context, input.built.types, NULL, 0, &facts) == XR_XIR_OK && facts);
    CHECK(source_program_compile_attempts > attempts && !xr_xir_compile_construction_count(facts));
    CHECK(!xr_xir_compile_construction_row(facts, 0) && !memcmp(&saved, &input, sizeof(input)));
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus expected = integer_conversion_cases[mutation].expected;
    size_t prepared_live = source_program_compile_live, prepared_bytes = source_program_compile_bytes;
    XrXirStatus verified = xr_xir_compile_verify_v2(context, &input.built, facts, &diagnostic);
    printf("integer-construction-v2 case=%s operation=verify expected=%u actual=%u\n",
        integer_conversion_cases[mutation].name, (unsigned)expected, (unsigned)verified);
    CHECK(verified == expected && diagnostic.status == expected);
    CHECK(source_program_compile_live == prepared_live && source_program_compile_bytes == prepared_bytes);
    CHECK(!memcmp(&saved, &input, sizeof(input)));
    XrXirArtifact *output = NULL;
    XrXirStatus checked = xr_xir_compile_check_v2(context, &input.built, facts, &output, &diagnostic);
    printf("integer-construction-v2 case=%s operation=check expected=%u actual=%u\n",
        integer_conversion_cases[mutation].name, (unsigned)expected, (unsigned)checked);
    CHECK(checked == expected && diagnostic.status == expected && !memcmp(&saved, &input, sizeof(input)));
    if (expected == XR_XIR_OK) {
        CHECK(output && xr_xir_compile_artifact_construction(output) &&
            xr_xir_compile_artifact_construction(output) != facts);
        CHECK(xr_xir_compile_artifact_context(output)->resources == context->resources);
        CHECK(xr_xir_compile_artifact_module(output)->functions != input.functions);
    } else CHECK(!output);
    xr_xir_compile_construction_free(facts);
    facts = NULL;
    memset(&input, 0xa5, sizeof(input));
    memset(&saved, 0xa5, sizeof(saved));
    CHECK(!facts);
    if (output) integer_owner(output);
    else CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes);
    return output;
}

static void integer_occupied_admission(const XrXirCompileContext *context,
    const XrXirArtifact *keeper, unsigned mutation) {
    IntegerInput input, saved;
    integer_input(&input, mutation);
    memcpy(&saved, &input, sizeof(saved));
    const XrXirConstruction *facts = xr_xir_compile_artifact_construction(keeper);
    CHECK(facts && !xr_xir_compile_construction_count(facts));
    XrXirArtifact *output = (XrXirArtifact *)keeper;
    XrXirDiagnostic diagnostic = {XR_XIR_OK, 17, 18, 19, XR_XIR_DIAGNOSTIC_NO_SUSPEND};
    XrCompileResourceStats before = integer_stats(context);
    size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
    size_t attempts = source_program_compile_attempts;
    CHECK(xr_xir_compile_check_v2(context, &input.built, facts, &output, &diagnostic) == XR_XIR_BAD_STRUCTURE);
    CHECK(output == keeper && diagnostic.status == XR_XIR_BAD_STRUCTURE && diagnostic.function == UINT32_MAX &&
        diagnostic.block == UINT32_MAX && diagnostic.instruction == UINT32_MAX && diagnostic.reason == XR_XIR_DIAGNOSTIC_NONE);
    if (!mutation) {
        CHECK(xr_xir_compile_check_v2(context, &input.built, facts, NULL, &diagnostic) == XR_XIR_BAD_STRUCTURE);
        CHECK(diagnostic.status == XR_XIR_BAD_STRUCTURE && diagnostic.reason == XR_XIR_DIAGNOSTIC_NONE);
    }
    XrCompileResourceStats after = integer_stats(context);
    CHECK(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes &&
        before.live_bytes == after.live_bytes && before.work == after.work);
    CHECK(attempts == source_program_compile_attempts && live == source_program_compile_live &&
        bytes == source_program_compile_bytes && !memcmp(&saved, &input, sizeof(input)));
    memset(&input, 0xa5, sizeof(input));
    memset(&saved, 0xa5, sizeof(saved));
    integer_owner(keeper);
    printf("integer-construction-v2 case=%s occupied=1 expected=%u pointer-preserved=1 no-allocation=1 no-work=1\n",
        integer_conversion_cases[mutation].name, (unsigned)XR_XIR_BAD_STRUCTURE);
}

static void integer_packet_detach(const XrXirCompileContext *context, XrXirArtifact **producer) {
    CHECK(producer && *producer);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(*producer, &packet, NULL) == XR_XIR_OK && packet.bytes && packet.length >= 64);
    uint8_t *input = xr_malloc(packet.length);
    CHECK(input);
    memcpy(input, packet.bytes, packet.length);
    size_t length = packet.length;
    xr_xir_compile_artifact_free(*producer);
    *producer = NULL;
    XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_compile_checked_read(context, input, length, &decoded, NULL) == XR_XIR_OK && decoded);
    CHECK(xr_xir_compile_artifact_context(decoded)->resources == context->resources);
    CHECK(!memcmp(input, packet.bytes, length));
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(!packet.bytes && !packet.length && !*producer);
    memset(input, 0xa5, length);
    xr_free(input);
    integer_owner(decoded);
    xr_xir_compile_artifact_free(decoded);
    puts("integer-construction-v2 complete64-pair Checked body survives producer and packet input destruction");
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *keeper = integer_empty_admission(context, 0);
    CHECK(keeper);
    unsigned verified = 1, checked = 1, occupied = 0;
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        if (mutation) {
            CHECK(!integer_empty_admission(context, mutation));
            ++verified;
            ++checked;
        }
        integer_occupied_admission(context, keeper, mutation);
        ++occupied;
    }
    CHECK(verified == 8 && checked == 8 && occupied == 8);
    integer_packet_detach(context, &keeper);
    CHECK(!keeper);
    source_program_owners_free();
    CHECK(!source_program_compile_live && !source_program_compile_bytes &&
        !source_program_compile_allocations && !source_program_compile_capacity);
    puts("integer-construction-v2 pairs=64 verify=8 check=8 occupied=8 null-output=1 compiler-physical=0/0 observer=0");
    puts("Independent current wire goldens, full FI, resource axes, Source, native and runtime qualifications remain OPEN");
    return 0;
}
