/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_intrinsic_registry.c - Real library calls across compiler consumers
 *
 * KEY CONCEPT:
 *   Source creates the call. Independent receiving outlives its producer;
 *   VM, CTFE and generated C use the same owned declaration and fixed oracles.
 */
#include "lir/xlir_lower.h"
#include "../../lir/lir_source_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_intrinsic.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"

static const char source_text[] =
    "export fn anchor()->i64{return 7}\n"
    "export fn spare()->i64{return 9}\n"
    "export fn probe(text:string,search:string)->bool{return text.contains(search)}\n"
    "export fn prefix(text:string,search:string)->bool{return text.startsWith(search)}\n"
    "export fn suffix(text:string,search:string)->bool{return text.endsWith(search)}\n"
    "fn proof()->bool{return \"\".contains(\"\") && \"\".startsWith(\"\") && \"\".endsWith(\"\")}\n"
    "export fn folded()->bool{return comptime proof()}\n"
    "print(probe(\"\xc3\xa9\\0abc\",\"\\0a\"),probe(\"\xc3\xa9\\0abc\",\"z\"),folded())\n"
    "print(prefix(\"\xc3\xa9\\0abc\",\"\xc3\xa9\\0\"),suffix(\"\xc3\xa9\\0abc\",\"\\0abc\"),"
    "prefix(\"xab\",\"ab\"),suffix(\"abx\",\"ab\"),prefix(\"\",\"\"),suffix(\"abc\",\"abcd\"))\n";

typedef struct Fixture {
    XrXirCompileContext context;
    XrXirArtifact *closed;
    XrXirCheckedPacket packet;
    uint8_t identity[32];
    uint32_t probe, prefix, suffix, folded, entry;
} Fixture;

static uint32_t named(const XrXirModule *module, const char *name) {
    for (uint32_t i = 0; i < module->function_count; ++i)
        if (module->functions[i].name_length == strlen(name) &&
            !memcmp(module->functions[i].name, name, strlen(name))) return i;
    CHECK(false);
    return 0;
}
static Fixture fixture_new(void) {
    char *input = xr_malloc(sizeof(source_text));
    CHECK(input);
    memcpy(input, source_text, sizeof(source_text));
    SourceFixture source = source_fixture(input);
    xr_free(input);
    const XrXirModule *module = xr_xir_compile_artifact_module(source.source.checked);
    const char *functions[] = {"probe", "prefix", "suffix"};
    for (uint32_t c = 0; c < 3; ++c) {
        const XrXirFunction *body = &module->functions[named(module, functions[c])];
        bool found = false;
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            const XrXirInstruction *op = &body->instructions[i];
            if (op->op != XR_XIR_CALL_INTRINSIC) continue;
            CHECK(op->immediate == (int64_t)c + 1 && op->args[1] == 2 && op->type == XR_XIR_BOOL);
            found = true;
        }
        CHECK(found);
    }
    Fixture fixture = {0};
    fixture.context = *xr_xir_compile_artifact_context(source.closed);
    CHECK(xr_compile_resources_retain(fixture.context.resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_checked_write(source.closed, &fixture.packet, NULL) == XR_XIR_OK);
    xr_sha256(fixture.packet.bytes, fixture.packet.length, fixture.identity);
    source_fixture_free(&source);
    CHECK(xr_xir_compile_checked_read(&fixture.context, fixture.packet.bytes, fixture.packet.length,
        &fixture.closed, NULL) == XR_XIR_OK);
    module = xr_xir_compile_artifact_module(fixture.closed);
    fixture.probe = named(module, "probe");
    fixture.prefix = named(module, "prefix");
    fixture.suffix = named(module, "suffix");
    fixture.folded = named(module, "folded");
    fixture.entry = module->declarations->entry_function;
    return fixture;
}
static void fixture_free(Fixture *fixture) {
    xr_xir_compile_artifact_free(fixture->closed);
    xr_xir_compile_checked_packet_free(&fixture->packet);
    xr_compile_resources_release(fixture->context.resources);
    *fixture = (Fixture) {0};
}
static XrXirArtifact *lowered(Fixture *fixture) {
    XrXirArtifact *output = NULL;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(fixture->closed, &target, &output, NULL) == XR_XIR_OK);
    return output;
}
static void registry(void) {
    const XrNativeTypeDeclaration *owner = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    const XrNativeMemberDeclaration *member = xr_native_declaration_member(owner, "contains");
    const XrNativeIntrinsicDeclaration *call = xr_native_intrinsic_for_member(owner, member);
    CHECK(call && call->id == 1 && xr_native_intrinsic_by_id(1) == call);
    CHECK(call->owner == owner && call->member == member && call->ctfe);
    CHECK(call->effects == XR_NATIVE_INTRINSIC_READ && member->receiver == XR_NATIVE_RECEIVER_READ);
    CHECK(member->result == XR_NATIVE_TERM_BOOL && member->parameter_count == 1 &&
        member->parameters[0].type == XR_NATIVE_TERM_STRING && !strcmp(member->failures, "none"));
    CHECK(member->allocation == XR_NATIVE_ALLOCATION_NO_HEAP && member->ownership == XR_NATIVE_RESULT_OWNERSHIP_OWNED);
    CHECK(!strcmp(call->c_symbol, "xr_xir_string_contains"));
    CHECK(!xr_native_intrinsic_by_id(0) && !xr_native_intrinsic_by_id(UINT32_MAX));
    CHECK(!xr_native_intrinsic_for_member(NULL, member) && !xr_native_intrinsic_for_member(owner, NULL));
    CHECK(!xr_native_intrinsic_for_member(xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY), member));
    CHECK(!xr_xir_intrinsic(-1) && !xr_xir_intrinsic(INT64_C(0x100000001)));
    const char *names[] = {"startsWith", "endsWith"};
    const char *symbols[] = {"xr_xir_string_starts_with", "xr_xir_string_ends_with"};
    const XrNativeIntrinsicImplementation implementations[] = {
        XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_STARTS_WITH, XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_ENDS_WITH};
    for (uint32_t i = 0; i < 2; ++i) {
        member = xr_native_declaration_member(owner, names[i]);
        call = xr_native_intrinsic_by_id(i + 2);
        CHECK(call && xr_xir_intrinsic(i + 2) == call && call->owner == owner && call->member == member);
        CHECK(xr_native_intrinsic_for_member(owner, member) == call && call->ctfe);
        CHECK(call->implementation == implementations[i] && !strcmp(call->c_symbol, symbols[i]));
        CHECK(call->effects == XR_NATIVE_INTRINSIC_READ && member->receiver == XR_NATIVE_RECEIVER_READ);
        CHECK(member->result == XR_NATIVE_TERM_BOOL && member->parameter_count == 1 &&
            member->parameters[0].type == XR_NATIVE_TERM_STRING && !strcmp(member->failures, "none"));
        CHECK(member->allocation == XR_NATIVE_ALLOCATION_NO_HEAP && member->ownership == XR_NATIVE_RESULT_OWNERSHIP_OWNED);
    }
}
static void lir_rejected(void) {
    SourceFixture source = source_fixture(
        "fn probe(text:string,search:string)->bool{return text.contains(search)}\nprobe(\"a\",\"a\")\n");
    XrLirLowerRequest request = {source.closed, source.source.snapshot, false};
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    XrLirStatus status = xr_lir_lower_checked(&request, &output, &failure);
    source_fixture_free(&source);
    CHECK(status == XR_LIR_UNSUPPORTED && failure.diagnostic.status == status);
    CHECK(!output.module && !output.entry.id);
    printf("LIR input remains unsupported: %s\n", failure.diagnostic.message);
    xr_lir_lower_failure_free(&failure);
}
static void source_rejected(const char *text) {
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "intrinsic-rejection", NULL};
    XrXirSourceRequest request = {.session = session, .authority = &authority,
        .context = &context, .linkage_kind = XR_XIR_PROGRAM};
    XrXirSourceText input = {NULL, text, strlen(text)};
    XrXirSourceResult result = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_check_text(&request, &input, &result, &diagnostic, NULL) != XR_XIR_OK);
    CHECK(!result.checked && diagnostic.line > 0 && diagnostic.column > 0);
    xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);
    xr_compile_resources_release(context.resources);
}
static void put32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}
static uint32_t get32(const uint8_t *bytes) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)bytes[i] << (8 * i);
    return value;
}
static void predecessor_rejected(Fixture *fixture, const char *path, uint32_t schema,
    uint32_t contract, bool evaluation) {
    FILE *file = fopen(path, "rb");
    CHECK(file && !fseek(file, 0, SEEK_END));
    long end = ftell(file);
    CHECK(end >= 64 && !fseek(file, 0, SEEK_SET));
    size_t length = (size_t)end;
    uint8_t *bytes = xr_malloc(length);
    CHECK(bytes && fread(bytes, 1, length, file) == length && !fclose(file));
    CHECK(!memcmp(bytes, evaluation ? "XRCEV\0\0\0" : "XRCHK\0\0\0", 8));
    CHECK(get32(bytes + 8) == schema && get32(bytes + 12) == contract);
    CHECK(get32(fixture->packet.bytes + 12) != get32(bytes + 12));
    if (!evaluation) CHECK(get32(fixture->packet.bytes + 8) != get32(bytes + 8));
    XrSHA256Context hash;
    uint8_t digest[32];
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, bytes, 32);
    xr_sha256_update(&hash, bytes + 64, length - 64);
    xr_sha256_final(&hash, digest);
    CHECK(!memcmp(digest, bytes + 32, sizeof(digest)));
    XrXirArtifact *decoded = NULL;
    XrXirStatus status = evaluation ? xir_evaluation_read(&fixture->context, bytes, length, &decoded, NULL) :
        xr_xir_compile_checked_read(&fixture->context, bytes, length, &decoded, NULL);
    CHECK(status == XR_XIR_BAD_STRUCTURE);
    CHECK(!decoded);
    xr_free(bytes);
}
typedef struct PacketSite { Fixture *fixture; size_t offset; uint32_t function, instruction; } PacketSite;
static void packet_rejected(const PacketSite *site, unsigned field, uint32_t value, XrXirStatus expected) {
    Fixture *fixture = site->fixture;
    uint8_t *bytes = xr_malloc(fixture->packet.length);
    CHECK(bytes);
    memcpy(bytes, fixture->packet.bytes, fixture->packet.length);
    put32(bytes + site->offset + field, value);
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, bytes, 32);
    xr_sha256_update(&hash, bytes + 64, fixture->packet.length - 64);
    xr_sha256_final(&hash, bytes + 32);
    XrXirArtifact *decoded = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_checked_read(&fixture->context, bytes,
        fixture->packet.length, &decoded, &diagnostic);
    if (status != expected || diagnostic.function != site->function || diagnostic.instruction != site->instruction)
        fprintf(stderr, "packet rejection %u: status=%u function=%u instruction=%u\n",
            field, (unsigned)status, diagnostic.function, diagnostic.instruction);
    CHECK(status == expected && !decoded);
    CHECK(diagnostic.function == site->function && diagnostic.instruction == site->instruction);
    xr_free(bytes);
}
static void invalid_packets(Fixture *fixture, uint32_t function) {
    const XrXirModule *module = xr_xir_compile_artifact_module(fixture->closed);
    const XrXirFunction *probe = &module->functions[function];
    uint32_t instruction = UINT32_MAX;
    for (uint32_t i = 0; i < probe->instruction_count; ++i)
        if (probe->instructions[i].op == XR_XIR_CALL_INTRINSIC) { instruction = i; break; }
    CHECK(instruction != UINT32_MAX);
    const XrXirInstruction *op = &probe->instructions[instruction];
    uint8_t pattern[40] = {0};
    put32(pattern, (uint32_t)op->op); put32(pattern + 4, (uint32_t)op->type);
    put32(pattern + 8, op->args[0]); put32(pattern + 12, op->args[1]);
    put32(pattern + 24, (uint32_t)op->immediate);
    size_t at = 64;
    while (at + sizeof(pattern) <= fixture->packet.length &&
        memcmp(fixture->packet.bytes + at, pattern, sizeof(pattern))) ++at;
    CHECK(at + sizeof(pattern) <= fixture->packet.length);
    PacketSite site = {fixture, at, function, instruction};
    packet_rejected(&site, 24, UINT32_MAX, XR_XIR_BAD_STRUCTURE);
    packet_rejected(&site, 24, 0, XR_XIR_BAD_STRUCTURE);
    packet_rejected(&site, 28, 1, XR_XIR_BAD_STRUCTURE);
    packet_rejected(&site, 12, 1, XR_XIR_BAD_STRUCTURE);
    packet_rejected(&site, 4, XR_XIR_I64, XR_XIR_BAD_TYPE);
    packet_rejected(&site, 16, 1, XR_XIR_BAD_STRUCTURE);
    packet_rejected(&site, 32, 1, XR_XIR_BAD_STRUCTURE);
}
static void execute(XrXirInstance *instance, uint32_t function, const XrXirValue *arguments,
    uint32_t count, bool expected) {
    CHECK(xr_xir_instance_start(instance, function, arguments, count) == XR_XIR_CALL_READY);
    XrXirInstanceResult polled = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
    if (polled.outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr, "VM function %u returned status=%u type=%u payload=%lld\n", function,
            (unsigned)polled.outcome.status, polled.outcome.value.type, (long long)polled.outcome.value.payload);
    CHECK(polled.outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(result.type == XR_XIR_BOOL && !result.reserved && result.payload == expected);
    xr_xir_value_drop(&result);
}
typedef struct Output { char bytes[64]; size_t size; XrXirOutputSink sink; } Output;
static XrXirOutputStatus output_bytes(void *context, XrXirOutputStream stream,
    const char *text, size_t size) {
    Output *output = context;
    CHECK(stream == XR_XIR_STDOUT && size <= sizeof(output->bytes) - output->size);
    memcpy(output->bytes + output->size, text, size);
    output->size += size;
    return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus output_group(void *context, const XrXirOutputGroup *value) {
    return xr_xir_output_render(&((Output *)context)->sink, value);
}
typedef struct PredicateCase {
    uint32_t function;
    const char *text, *pattern;
    size_t text_length, pattern_length;
    bool expected;
} PredicateCase;
static void consumers(Fixture *fixture) {
    XrXirArtifact *code = lowered(fixture);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(UINT64_MAX, &domain) == XR_XIR_VALUE_OK);
    const char text[] = "\xc3\xa9\0abc", hit[] = "\0a";
    const char prefix[] = "\xc3\xa9\0", suffix[] = "\0abc";
#define CASE(function, value, pattern, expected) \
    {fixture->function, value, pattern, sizeof(value) - 1, sizeof(pattern) - 1, expected}
    const PredicateCase cases[] = {
        CASE(probe, text, hit, true), CASE(probe, text, "z", false), CASE(probe, text, "", true),
        CASE(probe, "abcdefghijklmnop", "cdefghijkl", true),
        CASE(prefix, text, prefix, true), CASE(prefix, text, hit, false),
        CASE(prefix, "xab", "ab", false), CASE(prefix, "", "", true), CASE(prefix, "abc", "abcd", false),
        CASE(prefix, "abcdefghijklmnop", "abcdefghij", true),
        CASE(suffix, text, "abc", true), CASE(suffix, text, hit, false), CASE(suffix, text, suffix, true),
        CASE(suffix, "abx", "ab", false), CASE(suffix, "", "", true), CASE(suffix, "abc", "abcd", false),
        CASE(suffix, "abcdefghijklmnop", "ghijklmnop", true)
    };
#undef CASE
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        const PredicateCase *test = &cases[i];
        XrXirValue arguments[2] = {{0}, {0}};
        CHECK(xr_xir_string_new(domain, test->text, test->text_length, &arguments[0]) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domain, test->pattern, test->pattern_length, &arguments[1]) == XR_XIR_VALUE_OK);
        XrXirCtfeRequest request = {.artifact = code, .function = test->function,
            .target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
            .arguments = arguments, .argument_count = 2,
            .limits = {XR_XIR_CTFE_MAX_STEPS, XR_XIR_CTFE_MAX_FRAME_BYTES, XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
        memcpy(request.checked_identity, fixture->identity, sizeof(fixture->identity));
        XrXirValue folded = {0};
        CHECK(xr_xir_compile_ctfe_leaf(&request, &folded) == XR_XIR_CTFE_OK);
        CHECK(folded.type == XR_XIR_BOOL && !folded.reserved && folded.payload == test->expected);
        xr_xir_value_drop(&folded);
        xr_xir_value_drop(&arguments[0]); xr_xir_value_drop(&arguments[1]);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&code, &program) == XR_XIR_OK && !code);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    Output output = {0};
    output.sink = (XrXirOutputSink) {XR_XIR_CALL_ABI_VERSION, 0, output_bytes, &output, sizeof(output.bytes)};
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, output_group, &output};
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        const PredicateCase *test = &cases[i];
        XrXirValue arguments[2] = {{0}, {0}};
        CHECK(xr_xir_string_new(domain, test->text, test->text_length, &arguments[0]) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domain, test->pattern, test->pattern_length, &arguments[1]) == XR_XIR_VALUE_OK);
        execute(instance, test->function, arguments, 2, test->expected);
        xr_xir_value_drop(&arguments[0]); xr_xir_value_drop(&arguments[1]);
    }
    execute(instance, fixture->folded, NULL, 0, true);
    const char expected_output[] = "true false true\ntrue true false false true false\n";
    CHECK(output.size == sizeof(expected_output) - 1 &&
        !memcmp(output.bytes, expected_output, sizeof(expected_output) - 1));
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_domain_drop(domain);
}
#include "intrinsic_evaluation.h"

static void emit(Fixture *fixture, const char *path) {
    XrXirArtifact *code = lowered(fixture);
    XrXirCSource output = {0};
    CHECK(xr_xir_compile_emit_c(code, "intrinsic_contains", (size_t)UINT64_C(16) << 20, &output) == XR_XIR_OK);
    xr_xir_compile_artifact_free(code);
    fixture_free(fixture);
    CHECK(strstr(output.text, "xr_xir_string_contains") && output.length);
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite(output.text, 1, output.length, file) == output.length && !fclose(file));
    xr_xir_compile_c_source_free(&output);
}
int main(int argc, char **argv) {
    registry();
    Fixture fixture = fixture_new();
    if (argc == 3 && !strcmp(argv[1], "--emit")) { emit(&fixture, argv[2]); return 0; }
    CHECK(argc == 6);
    predecessor_rejected(&fixture, argv[1], 33, 93, false);
    predecessor_rejected(&fixture, argv[2], 33, 93, false);
    predecessor_rejected(&fixture, argv[3], 34, 94, false);
    predecessor_rejected(&fixture, argv[4], 34, 94, false);
    predecessor_rejected(&fixture, argv[5], 1, 94, true);
    invalid_packets(&fixture, fixture.probe);
    invalid_packets(&fixture, fixture.prefix);
    invalid_packets(&fixture, fixture.suffix);
    consumers(&fixture);
    evaluation_transport(&fixture);
    fixture_free(&fixture);
    source_rejected("fn bad()->bool{return \"x\".contains(1)}\n");
    source_rejected("fn bad()->bool{return \"x\".contains(\"x\",\"y\")}\n");
    source_rejected("fn bad()->bool{return \"x\".startsWith(1)}\n");
    source_rejected("fn bad()->bool{return \"x\".endsWith(\"x\",\"y\")}\n");
    lir_rejected();
    puts("canonical string predicates Source/packet/VM/CTFE passed");
    return 0;
}
